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

enum class TouchMouseMode : uint8_t {
    Click = 0,
    Trackpad = 1,
};

struct TouchPoint {
    uint32_t x = 0;
    uint32_t y = 0;
};

struct TouchHitRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    bool contains(float px, float py) const {
        return w > 0.0f && h > 0.0f && px >= x && px <= (x + w) && py >= y && py <= (y + h);
    }
};

class TouchMouseProcessor {
  public:
    using SendPacketFn = std::function<bool(const std::vector<uint8_t>& pkt)>;
    using NextSeqFn = std::function<uint32_t()>;

    void setMode(TouchMouseMode mode,
                 int64_t nowMs,
                 const SendPacketFn& sendFn,
                 const NextSeqFn& nextSeq);
    TouchMouseMode mode() const { return m_mode; }

    void setOverlayRects(bool visible, TouchHitRect leftBtn, TouchHitRect rightBtn);
    void reset(int64_t nowMs, const SendPacketFn& sendFn, const NextSeqFn& nextSeq);

    void process(const TouchPoint* touches,
                 size_t count,
                 int64_t nowMs,
                 const SendPacketFn& sendFn,
                 const NextSeqFn& nextSeq);

  private:
    TouchMouseMode m_mode = TouchMouseMode::Click;
    bool m_overlayVisible = false;
    TouchHitRect m_leftBtn{};
    TouchHitRect m_rightBtn{};
    bool m_suppressed = false;

    // Click mode state
    bool m_clickActive = false;
    uint16_t m_lastClickX = 0;
    uint16_t m_lastClickY = 0;

    // Trackpad mode state
    bool m_trackpadActive = false;
    bool m_trackpadMoved = false;
    bool m_hadTwoFingers = false;
    bool m_twoFingerScrolled = false;
    uint32_t m_startX = 0;
    uint32_t m_startY = 0;
    uint32_t m_prevX = 0;
    uint32_t m_prevY = 0;
    float m_twoFingerPrevY = 0.0f;
    float m_accumX = 0.0f;
    float m_accumY = 0.0f;
    int64_t m_downTimeMs = 0;
};

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
    void setTouchMouseMode(TouchMouseMode mode);
    void setOverlayButtonRects(bool visible, TouchHitRect leftBtn, TouchHitRect rightBtn);

    uint16_t vpadId() const { return m_vpadId.load(); }
    bool swapAbXy() const { return m_swapAbXy.load(); }
    TouchMouseMode touchMouseMode() const { return m_touchMouseMode.load(); }

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
    std::atomic<TouchMouseMode> m_touchMouseMode{TouchMouseMode::Click};
    std::atomic<uint32_t> m_seq{0};

    std::mutex m_overlayRectMu;
    bool m_overlayButtonsVisible = false;
    TouchHitRect m_overlayLeftBtn{};
    TouchHitRect m_overlayRightBtn{};

    std::mutex m_cvMu;
    std::condition_variable m_cv;
    std::thread m_thread;
};

} // namespace levelup::input
