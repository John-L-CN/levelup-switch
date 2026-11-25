#include "levelup/audio_player.hpp"

#include "levelup/audio_jitter.hpp"
#include "levelup/log.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
#include <malloc.h>
#include <opus/opus.h>
#include <switch.h>
#endif

namespace levelup::audio {

#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
// Supports up to a 60ms Opus frame @48kHz (WebRTC default is 960 = 20ms,
// the cloud edge used here sends 480 = 10ms).
constexpr int kMaxFrameSamples = 2880;

// Audren ring: 24 slots x 60ms of sample-offset space, far more than the ~40ms we keep queued.
constexpr int kBufferCount = 24;

// Audren 队列深度目标：喂料线程只在低于这个深度时补帧，稳态延迟 ≈ 40ms。
constexpr int kTargetQueuedMs = 40;
// 抽干后重启播放所需的最小队列深度。
constexpr int kPrebufferMs = 20;

constexpr size_t kMaxPayloadBytes = 1500;
constexpr int kFeederSleepMs = 2;

constexpr uint8_t kSinkChannels[2] = {0, 1};

const AudioRendererConfig kAudioConfig = {
    .output_rate = AudioRendererOutputRate_48kHz,
    .num_voices = 24,
    .num_effects = 0,
    .num_sinks = 1,
    .num_mix_objs = 1,
    .num_mix_buffers = 2,
};

int64_t steadyMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void applyVolume(s16* buf, size_t sampleCount, int volPercent) {
    if (volPercent >= 100) return;
    const int vol = std::clamp(volPercent, 0, 100);
    for (size_t i = 0; i < sampleCount; ++i) {
        const int scaled = (static_cast<int>(buf[i]) * vol) / 100;
        buf[i] = static_cast<s16>(std::clamp<int>(scaled, SHRT_MIN, SHRT_MAX));
    }
}

} // namespace

struct AudioPlayer::Impl {
    AudioPlayer* owner = nullptr;

    OpusDecoder* decoder = nullptr;
    AudioDriver driver{};
    AudioDriverWaveBuf wavebufs[kBufferCount]{};
    void* mempool = nullptr;
    size_t slotBytes = static_cast<size_t>(kMaxFrameSamples) * kChannels * sizeof(s16);
    std::vector<s16> pcmScratch;
    bool driverOpen = false;
    bool audrenInited = false;

    // Jitter buffer: network thread pushes, feeder thread pulls (see audio_jitter.hpp).
    mutable std::mutex qmu;
    JitterBuffer jitter;
    uint64_t audrenDrops = 0;

    std::atomic<bool> feederRunning{false};
    std::thread feeder;

    /// caller must hold qmu
    void syncCountersLocked() {
        if (!owner) {
            return;
        }
        owner->m_dropped.store(jitter.dropped() + audrenDrops, std::memory_order_relaxed);
        owner->m_concealed.store(jitter.concealed(), std::memory_order_relaxed);
        owner->m_trimmed.store(jitter.trimmed(), std::memory_order_relaxed);
    }

    int queuedMs() const {
        uint64_t samples = 0;
        for (int i = 0; i < kBufferCount; ++i) {
            const auto st = wavebufs[i].state;
            if (st == AudioDriverWaveBufState_Waiting ||
                st == AudioDriverWaveBufState_Queued ||
                st == AudioDriverWaveBufState_Playing) {
                if (wavebufs[i].end_sample_offset >= wavebufs[i].start_sample_offset) {
                    samples += static_cast<uint64_t>(wavebufs[i].end_sample_offset -
                                                     wavebufs[i].start_sample_offset);
                }
            }
        }
        return static_cast<int>((samples * 1000ULL) / kSampleRate);
    }

    int findFreeWavebuf() const {
        for (int i = 0; i < kBufferCount; ++i) {
            if (wavebufs[i].state == AudioDriverWaveBufState_Free ||
                wavebufs[i].state == AudioDriverWaveBufState_Done) {
                return i;
            }
        }
        return -1;
    }

    /// 解码后的 PCM 交给 Audren 播放。
    void enqueuePcm(s16* pcm, int samples) {
        if (!driverOpen || !pcm || samples <= 0) {
            return;
        }
        if (samples > kMaxFrameSamples) {
            samples = kMaxFrameSamples;
        }

        audrvUpdate(&driver);
        const int idx = findFreeWavebuf();
        if (idx < 0) {
            std::lock_guard lock(qmu);
            audrenDrops++;
            syncCountersLocked();
            return;
        }

        applyVolume(pcm, static_cast<size_t>(samples) * kChannels, owner ? owner->m_volume.load() : 100);

        const size_t bytes = static_cast<size_t>(samples) * kChannels * sizeof(s16);
        void* dst = static_cast<uint8_t*>(mempool) + static_cast<size_t>(idx) * slotBytes;
        std::memcpy(dst, pcm, bytes);
        armDCacheFlush(dst, bytes);

        AudioDriverWaveBuf* wb = &wavebufs[idx];
        wb->end_sample_offset = wb->start_sample_offset + samples;

        audrvVoiceAddWaveBuf(&driver, 0, wb);
        if (!audrvVoiceIsPlaying(&driver, 0) && queuedMs() >= kPrebufferMs) {
            audrvVoiceStart(&driver, 0);
        }
        audrvUpdate(&driver);

        if (owner) owner->m_latencyMs.store(queuedMs(), std::memory_order_relaxed);
    }

    void feederLoop() {
        std::vector<uint8_t> payload;
        payload.reserve(kMaxPayloadBytes);

        while (feederRunning.load(std::memory_order_relaxed)) {
            audrvUpdate(&driver);

            // Opportunistic counter mirror for the HUD/perf line.
            {
                std::lock_guard lock(qmu);
                syncCountersLocked();
            }

            if (queuedMs() >= kTargetQueuedMs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kFeederSleepMs));
                continue;
            }

            JitterBuffer::Take action = JitterBuffer::Take::None;
            {
                std::lock_guard lock(qmu);
                action = jitter.take(steadyMs(), payload);
            }
            if (action == JitterBuffer::Take::None) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kFeederSleepMs));
                continue;
            }

            s16* pcm = pcmScratch.data();
            int samples = 0;
            if (action == JitterBuffer::Take::Packet && !payload.empty()) {
                samples = opus_decode(decoder,
                                      payload.data(),
                                      static_cast<opus_int32>(payload.size()),
                                      pcm,
                                      kMaxFrameSamples,
                                      0);
                if (samples <= 0) {
                    // 坏包：用 PLC 顶一帧，保持时间轴连续
                    samples = opus_decode(decoder, nullptr, 0, pcm, kMaxFrameSamples, 0);
                } else if (owner) {
                    owner->m_decoded.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                samples = opus_decode(decoder, nullptr, 0, pcm, kMaxFrameSamples, 0);
            }

            if (samples > 0) {
                enqueuePcm(pcm, samples);
            }
        }
    }

    void teardown() {
        feederRunning.store(false, std::memory_order_relaxed);
        if (feeder.joinable()) {
            feeder.join();
        }
        {
            std::lock_guard lock(qmu);
            jitter.reset();
        }
        if (driverOpen) {
            audrvVoiceStop(&driver, 0);
            audrvClose(&driver);
            driverOpen = false;
        }
        if (audrenInited) {
            audrenExit();
            audrenInited = false;
        }
        if (decoder) {
            opus_decoder_destroy(decoder);
            decoder = nullptr;
        }
        if (mempool) {
            std::free(mempool);
            mempool = nullptr;
        }
    }
};

AudioPlayer::~AudioPlayer() {
    stop();
}

bool AudioPlayer::start() {
    std::lock_guard lock(m_mu);
    if (m_impl) {
        m_impl->teardown();
        delete m_impl;
        m_impl = nullptr;
    }

    auto* impl = new Impl();
    impl->owner = this;
    impl->pcmScratch.resize(static_cast<size_t>(kMaxFrameSamples) * kChannels);

    int opusErr = OPUS_OK;
    impl->decoder = opus_decoder_create(kSampleRate, kChannels, &opusErr);
    if (!impl->decoder || opusErr != OPUS_OK) {
        logError("[Audio] opus_decoder_create failed: " + std::to_string(opusErr));
        impl->teardown();
        delete impl;
        return false;
    }

    const size_t rawPoolBytes = impl->slotBytes * kBufferCount;
    const size_t poolBytes =
        (rawPoolBytes + (AUDREN_MEMPOOL_ALIGNMENT - 1)) & ~(AUDREN_MEMPOOL_ALIGNMENT - 1);
    impl->mempool = memalign(AUDREN_MEMPOOL_ALIGNMENT, poolBytes);
    if (!impl->mempool) {
        logError("[Audio] audren mempool allocation failed");
        impl->teardown();
        delete impl;
        return false;
    }
    std::memset(impl->mempool, 0, poolBytes);

    Result rc = audrenInitialize(&kAudioConfig);
    if (R_FAILED(rc)) {
        logError("[Audio] audrenInitialize failed: " + std::to_string(rc));
        impl->teardown();
        delete impl;
        return false;
    }
    impl->audrenInited = true;

    rc = audrvCreate(&impl->driver, &kAudioConfig, kChannels);
    if (R_FAILED(rc)) {
        logError("[Audio] audrvCreate failed: " + std::to_string(rc));
        impl->teardown();
        delete impl;
        return false;
    }
    impl->driverOpen = true;

    for (int i = 0; i < kBufferCount; ++i) {
        impl->wavebufs[i].data_raw = impl->mempool;
        impl->wavebufs[i].size = poolBytes;
        impl->wavebufs[i].start_sample_offset = i * kMaxFrameSamples;
        impl->wavebufs[i].end_sample_offset = impl->wavebufs[i].start_sample_offset;
    }

    const int mpid = audrvMemPoolAdd(&impl->driver, impl->mempool, poolBytes);
    audrvMemPoolAttach(&impl->driver, mpid);
    audrvDeviceSinkAdd(&impl->driver, AUDREN_DEFAULT_DEVICE_NAME, kChannels, kSinkChannels);

    rc = audrenStartAudioRenderer();
    if (R_FAILED(rc)) {
        logError("[Audio] audrenStartAudioRenderer failed: " + std::to_string(rc));
        impl->teardown();
        delete impl;
        return false;
    }

    audrvVoiceInit(&impl->driver, 0, kChannels, PcmFormat_Int16, kSampleRate);
    audrvVoiceSetDestinationMix(&impl->driver, 0, AUDREN_FINAL_MIX_ID);
    for (int i = 0; i < kChannels; ++i) {
        for (int j = 0; j < kChannels; ++j) {
            audrvVoiceSetMixFactor(&impl->driver, 0, i == j ? 1.0f : 0.0f, i, j);
        }
    }
    // 先起 voice（与 Moonlight-Switch 的 AudrenAudioRenderer 一致），缓冲区由 feeder
    // 线程按队列深度陆续补上；抽干后再靠 kPrebufferMs 控制重启。
    audrvVoiceStart(&impl->driver, 0);

    m_dropped.store(0);
    m_decoded.store(0);
    m_concealed.store(0);
    m_trimmed.store(0);
    m_latencyMs.store(0);

    impl->feederRunning.store(true);
    impl->feeder = std::thread([impl]() { impl->feederLoop(); });

    m_impl = impl;
    m_running.store(true);
    logInfo("[Audio] Audren 48kHz stereo Opus player started (jitter 80-140ms, audren target " +
            std::to_string(kTargetQueuedMs) + "ms)");
    return true;
}

void AudioPlayer::stop() {
    m_running.store(false);
    std::lock_guard lock(m_mu);
    if (m_impl) {
        m_impl->teardown();
        delete m_impl;
        m_impl = nullptr;
    }
}

void AudioPlayer::setVolume(int volumePercent) {
    m_volume.store(std::clamp(volumePercent, 0, 100));
}

void AudioPlayer::pushOpusPacket(const uint8_t* data, size_t len, uint16_t rtpSeq, uint32_t) {
    if (!m_running.load(std::memory_order_relaxed) || !data || len == 0) {
        return;
    }
    // m_mu guards the Impl lifetime against a concurrent stop() (which joins the feeder
    // thread while holding it); the jitter ring itself is guarded by Impl::qmu.
    std::lock_guard lifeLock(m_mu);
    Impl* impl = m_impl;
    if (!impl || !impl->feederRunning.load(std::memory_order_relaxed)) {
        return;
    }
    if (len > kMaxPayloadBytes) {
        return;
    }
    std::lock_guard ringLock(impl->qmu);
    impl->jitter.push(rtpSeq, data, len, steadyMs());
    impl->syncCountersLocked();
}

#else

struct AudioPlayer::Impl {};

AudioPlayer::~AudioPlayer() = default;

bool AudioPlayer::start() {
    m_running.store(true);
    return true;
}

void AudioPlayer::stop() {
    m_running.store(false);
}

void AudioPlayer::setVolume(int volumePercent) {
    m_volume.store(std::clamp(volumePercent, 0, 100));
}

void AudioPlayer::pushOpusPacket(const uint8_t* data, size_t len, uint16_t, uint32_t) {
    if (m_running.load() && data && len > 0) {
        m_decoded.fetch_add(1);
    }
}

#endif

} // namespace levelup::audio
