#include "levelup/switch_input_bridge.hpp"

#include "levelup/crypto_util.hpp"
#include "levelup/log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>

#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
#include <switch.h>
#endif

namespace levelup::input {
namespace {

inline int16_t clampAxis16(int32_t v) {
    return static_cast<int16_t>(std::clamp<int32_t>(v, -32768, 32767));
}

inline int16_t invertAxis16(int32_t v) {
    if (v <= -32768) return 32767;
    if (v >= 32767) return -32768;
    return static_cast<int16_t>(-v);
}

} // namespace

GamepadState mapSwitchHidToGamepad(uint64_t buttons,
                                   int32_t lx,
                                   int32_t ly,
                                   int32_t rx,
                                   int32_t ry,
                                   bool invertStickY,
                                   bool swapAbXy) {
    GamepadState st;
    uint16_t mask = 0;

    if (swapAbXy) {
        // Xbox layout: Switch B -> Xbox A, Switch A -> Xbox B, Switch Y -> Xbox X, Switch X -> Xbox Y
        if (buttons & nxhid::kA) mask |= kPadB;
        if (buttons & nxhid::kB) mask |= kPadA;
        if (buttons & nxhid::kX) mask |= kPadY;
        if (buttons & nxhid::kY) mask |= kPadX;
    } else {
        // Native Nintendo ABXY passthrough
        if (buttons & nxhid::kA) mask |= kPadA;
        if (buttons & nxhid::kB) mask |= kPadB;
        if (buttons & nxhid::kX) mask |= kPadX;
        if (buttons & nxhid::kY) mask |= kPadY;
    }

    if (buttons & nxhid::kUp) mask |= kPadDPadUp;
    if (buttons & nxhid::kDown) mask |= kPadDPadDown;
    if (buttons & nxhid::kLeft) mask |= kPadDPadLeft;
    if (buttons & nxhid::kRight) mask |= kPadDPadRight;

    if (buttons & nxhid::kPlus) mask |= kPadStart;
    if (buttons & nxhid::kMinus) mask |= kPadBack;
    if (buttons & nxhid::kStickL) mask |= kPadL3;
    if (buttons & nxhid::kStickR) mask |= kPadR3;
    if (buttons & nxhid::kL) mask |= kPadLB;
    if (buttons & nxhid::kR) mask |= kPadRB;

    st.buttons = mask;
    st.leftTrigger = (buttons & nxhid::kZL) ? 255 : 0;
    st.rightTrigger = (buttons & nxhid::kZR) ? 255 : 0;
    st.lx = clampAxis16(lx);
    st.ly = invertStickY ? invertAxis16(ly) : clampAxis16(ly);
    st.rx = clampAxis16(rx);
    st.ry = invertStickY ? invertAxis16(ry) : clampAxis16(ry);
    return st;
}

SwitchInputBridge::~SwitchInputBridge() {
    stop();
}

void SwitchInputBridge::setSendCallback(SendPacketFn fn) {
    std::lock_guard lock(m_cbMu);
    m_sendFn = std::move(fn);
}

void SwitchInputBridge::setOverlayComboCallback(OverlayComboFn fn) {
    std::lock_guard lock(m_cbMu);
    m_overlayCb = std::move(fn);
}

void SwitchInputBridge::start() {
    stop();
    m_paused.store(false);
    m_vpadId.store(1);
    m_seq.store(0);
    m_running.store(true);
    m_thread = std::thread([this]() { workerLoop(); });
}

void SwitchInputBridge::stop() {
    m_running.store(false);
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }

    SendPacketFn fn;
    {
        std::lock_guard lock(m_cbMu);
        fn = m_sendFn;
    }
    if (fn) {
        GamepadState zero;
        fn(buildGamepadPacket(nextSeq(), static_cast<uint64_t>(unixMillisNow()),
                              m_vpadId.load(), zero));
    }
}

void SwitchInputBridge::setPaused(bool paused) {
    const bool prev = m_paused.exchange(paused);
    if (!prev && paused) {
        SendPacketFn fn;
        {
            std::lock_guard lock(m_cbMu);
            fn = m_sendFn;
        }
        if (fn) {
            GamepadState zero;
            fn(buildGamepadPacket(nextSeq(), static_cast<uint64_t>(unixMillisNow()),
                                  m_vpadId.load(), zero));
        }
    }
}

void SwitchInputBridge::setVPadId(uint16_t vpadId) {
    if (vpadId > 0) {
        m_vpadId.store(vpadId);
    }
}

void SwitchInputBridge::setInvertStickY(bool invert) {
    m_invertStickY.store(invert);
}

void SwitchInputBridge::setSwapAbXy(bool swap) {
    m_swapAbXy.store(swap);
}

void SwitchInputBridge::workerLoop() {
#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
    PadState padHandheld{};
    PadState padNo1{};
    padInitialize(&padHandheld, HidNpadIdType_Handheld);
    padInitialize(&padNo1, HidNpadIdType_No1);
    hidInitializeTouchScreen();

    static_assert(nxhid::kA == HidNpadButton_A, "A bit mismatch");
    static_assert(nxhid::kB == HidNpadButton_B, "B bit mismatch");
    static_assert(nxhid::kX == HidNpadButton_X, "X bit mismatch");
    static_assert(nxhid::kY == HidNpadButton_Y, "Y bit mismatch");
    static_assert(nxhid::kZL == HidNpadButton_ZL, "ZL bit mismatch");
    static_assert(nxhid::kZR == HidNpadButton_ZR, "ZR bit mismatch");
    static_assert(nxhid::kPlus == HidNpadButton_Plus, "Plus bit mismatch");
    static_assert(nxhid::kMinus == HidNpadButton_Minus, "Minus bit mismatch");

    GamepadState lastState{};
    int64_t lastSendMs = 0;
    int64_t comboStartMs = 0;
    bool comboFired = false;

    bool touchActive = false;
    uint16_t lastTouchX = 0;
    uint16_t lastTouchY = 0;

    auto sendPkt = [&](const std::vector<uint8_t>& pkt) {
        SendPacketFn fn;
        {
            std::lock_guard lock(m_cbMu);
            fn = m_sendFn;
        }
        if (fn) {
            fn(pkt);
        }
    };

    auto fireOverlay = [&]() {
        OverlayComboFn cb;
        {
            std::lock_guard lock(m_cbMu);
            cb = m_overlayCb;
        }
        if (cb) cb();
    };

    while (m_running.load()) {
        padUpdate(&padHandheld);
        padUpdate(&padNo1);

        // Merge Handheld + No1 controller state seamlessly
        u64 btns = 0;
        HidAnalogStickState stL{};
        HidAnalogStickState stR{};
        if (padHandheld.active_handheld) {
            btns |= padGetButtons(&padHandheld);
            stL = padGetStickPos(&padHandheld, 0);
            stR = padGetStickPos(&padHandheld, 1);
        }
        if (padNo1.style_set != 0) {
            btns |= padGetButtons(&padNo1);
            const auto nL = padGetStickPos(&padNo1, 0);
            const auto nR = padGetStickPos(&padNo1, 1);
            if (std::abs(nL.x) > std::abs(stL.x)) stL.x = nL.x;
            if (std::abs(nL.y) > std::abs(stL.y)) stL.y = nL.y;
            if (std::abs(nR.x) > std::abs(stR.x)) stR.x = nR.x;
            if (std::abs(nR.y) > std::abs(stR.y)) stR.y = nR.y;
        }

        const int64_t nowMs = unixMillisNow();

        // Check in-stream menu combo: L+R+Minus+Plus instant OR Minus+Plus hold 400ms
        const u64 kMenuHold = HidNpadButton_Minus | HidNpadButton_Plus;
        const u64 kMenuInstant = kMenuHold | HidNpadButton_L | HidNpadButton_R;
        if ((btns & kMenuHold) == kMenuHold) {
            if (!comboFired) {
                if ((btns & kMenuInstant) == kMenuInstant) {
                    comboFired = true;
                    fireOverlay();
                } else {
                    if (comboStartMs == 0) {
                        comboStartMs = nowMs;
                    } else if ((nowMs - comboStartMs) >= 400) {
                        comboFired = true;
                        fireOverlay();
                    }
                }
            }
            // Suppress Start+Select spam while holding menu combo
            btns &= ~(HidNpadButton_Minus | HidNpadButton_Plus | HidNpadButton_L | HidNpadButton_R);
        } else {
            comboStartMs = 0;
            comboFired = false;
        }

        if (!m_paused.load()) {
            const GamepadState cur = mapSwitchHidToGamepad(
                btns, stL.x, stL.y, stR.x, stR.y, m_invertStickY.load(), m_swapAbXy.load());
            if (cur != lastState || (nowMs - lastSendMs) >= 100) {
                lastState = cur;
                lastSendMs = nowMs;
                sendPkt(buildGamepadPacket(
                    nextSeq(), static_cast<uint64_t>(nowMs), m_vpadId.load(), cur));
            }

            // Single-finger touchscreen -> absolute mouse move/click
            HidTouchScreenState touchState{};
            if (hidGetTouchScreenStates(&touchState, 1) > 0 && touchState.count > 0) {
                const auto& t = touchState.touches[0];
                const uint16_t nx = static_cast<uint16_t>(
                    (static_cast<uint32_t>(std::clamp<u32>(t.x, 0, 1280)) * 65535U) / 1280U);
                const uint16_t ny = static_cast<uint16_t>(
                    (static_cast<uint32_t>(std::clamp<u32>(t.y, 0, 720)) * 65535U) / 720U);
                if (!touchActive) {
                    touchActive = true;
                    lastTouchX = nx;
                    lastTouchY = ny;
                    sendPkt(buildMouseMoveAbsolute(nextSeq(), static_cast<uint64_t>(nowMs), nx, ny));
                    sendPkt(buildMouseClickAt(nextSeq(), static_cast<uint64_t>(nowMs),
                                              nx, ny, kMouseLeft, true));
                } else if (std::abs(static_cast<int>(nx) - static_cast<int>(lastTouchX)) >= 64 ||
                           std::abs(static_cast<int>(ny) - static_cast<int>(lastTouchY)) >= 64) {
                    lastTouchX = nx;
                    lastTouchY = ny;
                    sendPkt(buildMouseMoveAbsolute(nextSeq(), static_cast<uint64_t>(nowMs), nx, ny));
                }
            } else if (touchActive) {
                touchActive = false;
                sendPkt(buildMouseClickAt(nextSeq(), static_cast<uint64_t>(nowMs),
                                          lastTouchX, lastTouchY, kMouseLeft, false));
            }
        } else if (touchActive) {
            touchActive = false;
        }

        std::unique_lock lock(m_cvMu);
        m_cv.wait_for(lock, std::chrono::milliseconds(16), [this]() { return !m_running.load(); });
    }
#else
    std::unique_lock lock(m_cvMu);
    m_cv.wait(lock, [this]() { return !m_running.load(); });
#endif
}

} // namespace levelup::input
