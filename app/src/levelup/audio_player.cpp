#include "levelup/audio_player.hpp"

#include "levelup/log.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>

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
constexpr int kMaxFrameSamples = 2880; // supports up to 60ms Opus frame @ 48kHz (WebRTC default is 960 = 20ms)
constexpr int kBufferCount = 8;
constexpr int kMaxQueuedMs = 80;
constexpr uint8_t kSinkChannels[2] = {0, 1};

const AudioRendererConfig kAudioConfig = {
    .output_rate = AudioRendererOutputRate_48kHz,
    .num_voices = 24,
    .num_effects = 0,
    .num_sinks = 1,
    .num_mix_objs = 1,
    .num_mix_buffers = 2,
};

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
    OpusDecoder* decoder = nullptr;
    AudioDriver driver{};
    AudioDriverWaveBuf wavebufs[kBufferCount]{};
    void* mempool = nullptr;
    size_t slotBytes = kMaxFrameSamples * kChannels * sizeof(s16);
    std::vector<s16> pcmScratch;
    bool driverOpen = false;
    bool audrenInited = false;

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

    void teardown() {
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
    audrvVoiceStart(&impl->driver, 0);

    m_dropped.store(0);
    m_decoded.store(0);
    m_impl = impl;
    m_running.store(true);
    logInfo("[Audio] Audren 48kHz stereo Opus player started");
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

void AudioPlayer::pushOpusPacket(const uint8_t* data, size_t len) {
    if (!m_running.load() || !data || len == 0) {
        return;
    }

    std::lock_guard lock(m_mu);
    if (!m_impl || !m_impl->driverOpen || !m_impl->decoder) {
        return;
    }

    // Always decode so Opus predictor state stays continuous even if playback drops a late burst
    const int decodedSamples = opus_decode(
        m_impl->decoder,
        data,
        static_cast<opus_int32>(len),
        m_impl->pcmScratch.data(),
        kMaxFrameSamples,
        0);
    if (decodedSamples <= 0) {
        return;
    }
    m_decoded.fetch_add(1);

    audrvUpdate(&m_impl->driver);

    int slotIdx = -1;
    if (m_impl->queuedMs() > kMaxQueuedMs || (slotIdx = m_impl->findFreeWavebuf()) < 0) {
        m_dropped.fetch_add(1);
        return;
    }

    applyVolume(m_impl->pcmScratch.data(),
                static_cast<size_t>(decodedSamples) * kChannels,
                m_volume.load());

    const size_t bytes = static_cast<size_t>(decodedSamples) * kChannels * sizeof(s16);
    void* dst = static_cast<uint8_t*>(m_impl->mempool) + slotIdx * m_impl->slotBytes;
    std::memcpy(dst, m_impl->pcmScratch.data(), bytes);
    armDCacheFlush(dst, bytes);

    AudioDriverWaveBuf* wb = &m_impl->wavebufs[slotIdx];
    wb->end_sample_offset = wb->start_sample_offset + decodedSamples;

    audrvVoiceAddWaveBuf(&m_impl->driver, 0, wb);
    if (!audrvVoiceIsPlaying(&m_impl->driver, 0)) {
        audrvVoiceStart(&m_impl->driver, 0);
    }
    audrvUpdate(&m_impl->driver);
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
void AudioPlayer::pushOpusPacket(const uint8_t* data, size_t len) {
    if (m_running.load() && data && len > 0) {
        m_decoded.fetch_add(1);
    }
}

#endif

} // namespace levelup::audio
