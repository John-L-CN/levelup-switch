#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace levelup::audio {

class AudioPlayer {
  public:
    AudioPlayer() = default;
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool start();
    void stop();

    void pushOpusPacket(const uint8_t* data, size_t len);
    void setVolume(int volumePercent); // 0..100
    int volume() const { return m_volume.load(); }

    uint64_t droppedPackets() const { return m_dropped.load(); }
    uint64_t decodedPackets() const { return m_decoded.load(); }

  private:
    struct Impl;
    Impl* m_impl = nullptr;
    std::mutex m_mu;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_volume{100};
    std::atomic<uint64_t> m_dropped{0};
    std::atomic<uint64_t> m_decoded{0};
};

} // namespace levelup::audio
