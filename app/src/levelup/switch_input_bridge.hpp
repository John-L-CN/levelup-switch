#pragma once

#include "levelup/input_packet.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace levelup::input {

// Pure Switch HID -> GamepadState mapper (host-testable; Nintendo ABXY or Xbox layout swap)
GamepadState mapSwitchHidToGamepad(uint64_t buttons,
                                   int32_t lx,
                                   int32_t ly,
                                   int32_t rx,
                                   int32_t ry,
                                   bool invertStickY = false,
                                   bool swapAbXy = false);

// Helper bit constants matching libnx HidNpadButton bits for cross-platform testing
namespace nxhid {
constexpr uint64_t kA = 1ULL << 0;
constexpr uint64_t kB = 1ULL << 1;
constexpr uint64_t kX = 1ULL << 2;
constexpr uint64_t kY = 1ULL << 3;
constexpr uint64_t kStickL = 1ULL << 4;
constexpr uint64_t kStickR = 1ULL << 5;
constexpr uint64_t kL = 1ULL << 6;
constexpr uint64_t kR = 1ULL << 7;
constexpr uint64_t kZL = 1ULL << 8;
constexpr uint64_t kZR = 1ULL << 9;
constexpr uint64_t kPlus = 1ULL << 10;
constexpr uint64_t kMinus = 1ULL << 11;
constexpr uint64_t kLeft = 1ULL << 12;
constexpr uint64_t kUp = 1ULL << 13;
constexpr uint64_t kRight = 1ULL << 14;
constexpr uint64_t kDown = 1ULL << 15;
} // namespace nxhid

class SwitchInputBridge {
  public:
    using SendPacketFn = std::function<bool(const std::vector<uint8_t>& pkt)>;
    using OverlayComboFn = std::function<void()>;

    SwitchInputBridge() = default;
    ~SwitchInputBridge();

    SwitchInputBridge(const SwitchInputBridge&) = delete;
    SwitchInputBridge& operator=(const SwitchInputBridge&) = delete;

    void setSendCallback(SendPacketFn fn);
    void setOverlayComboCallback(OverlayComboFn fn);

    void start();
    void stop();

    void setPaused(bool paused);
    void setVPadId(uint16_t vpadId);
    void setInvertStickY(bool invert);
    void setSwapAbXy(bool swap);

    uint16_t vpadId() const { return m_vpadId.load(); }
    bool swapAbXy() const { return m_swapAbXy.load(); }

  private:
    void workerLoop();
    uint32_t nextSeq() { return m_seq.fetch_add(1) + 1; }

    mutable std::mutex m_cbMu;
    SendPacketFn m_sendFn;
    OverlayComboFn m_overlayCb;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};
    std::atomic<uint16_t> m_vpadId{1}; // Default controller 1 until cmd=34 updates it
    std::atomic<bool> m_invertStickY{false};
    std::atomic<bool> m_swapAbXy{false};
    std::atomic<uint32_t> m_seq{0};

    std::mutex m_cvMu;
    std::condition_variable m_cv;
    std::thread m_thread;
};

} // namespace levelup::input
