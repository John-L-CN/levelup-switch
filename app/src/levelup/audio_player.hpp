#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace levelup::audio {

/// Opus -> Audren 播放器。
///
/// 网络侧（libdatachannel 收包线程）只做「入抖动缓冲区」的 memcpy，
/// 解码 + Audren 喂料全部放在独立的 feeder 线程里，按 Audren 队列深度
/// 定速输出。这样：
///  * RTP 突发/乱序不会再让 Audren 队列涨到上限后连续丢包（旧实现的爆音来源）；
///  * 网络抖动被 80ms 级抖动缓冲吸收，不会把播放器饿死（旧实现的断续来源）；
///  * 真正丢包时用 Opus PLC 补一帧，避免硬缺口。
class AudioPlayer {
  public:
    AudioPlayer() = default;
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool start();
    void stop();

    /// rtpSeq 用于抖动缓冲排序（缺省 0 表示不考虑顺序，按到达顺序播放）。
    void pushOpusPacket(const uint8_t* data, size_t len, uint16_t rtpSeq = 0, uint32_t rtpTs = 0);
    void setPaused(bool paused);
    bool isPaused() const { return m_paused.load(std::memory_order_relaxed); }
    void setVolume(int volumePercent); // 0..100
    int volume() const { return m_volume.load(); }

    uint64_t droppedPackets() const { return m_dropped.load(); }
    uint64_t decodedPackets() const { return m_decoded.load(); }
    uint64_t concealedPackets() const { return m_concealed.load(); }
    uint64_t trimmedPackets() const { return m_trimmed.load(); }
    int latencyMs() const { return m_latencyMs.load(); }

  private:
    struct Impl;
    Impl* m_impl = nullptr;
    std::mutex m_mu;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};
    std::atomic<int> m_volume{100};
    std::atomic<uint64_t> m_dropped{0};
    std::atomic<uint64_t> m_decoded{0};
    std::atomic<uint64_t> m_concealed{0};
    std::atomic<uint64_t> m_trimmed{0};
    std::atomic<int> m_latencyMs{0};
};

} // namespace levelup::audio
