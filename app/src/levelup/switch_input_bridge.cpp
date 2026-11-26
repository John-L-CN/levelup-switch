#include "levelup/switch_input_bridge.hpp"

#include "levelup/crypto_util.hpp"
#include "levelup/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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

void TouchMouseProcessor::setOverlayRects(bool visible, TouchHitRect leftBtn, TouchHitRect rightBtn) {
    m_overlayVisible = visible;
    m_leftBtn = leftBtn;
    m_rightBtn = rightBtn;
}

void TouchMouseProcessor::reset(int64_t nowMs, const SendPacketFn& sendFn, const NextSeqFn& nextSeq) {
    if (m_clickActive && sendFn && nextSeq) {
        sendFn(buildMouseClickAt(nextSeq(), static_cast<uint64_t>(nowMs),
                                 m_lastClickX, m_lastClickY, kMouseLeft, false));
    }
    m_clickActive = false;
    m_trackpadActive = false;
    m_trackpadMoved = false;
    m_hadTwoFingers = false;
    m_twoFingerScrolled = false;
    m_suppressed = false;
    m_anyTouchDown = false;
    m_accumX = 0.0f;
    m_accumY = 0.0f;
}

void TouchMouseProcessor::setMode(TouchMouseMode mode,
                                  int64_t nowMs,
                                  const SendPacketFn& sendFn,
                                  const NextSeqFn& nextSeq) {
    if (m_mode == mode) {
        return;
    }
    const bool wasTouching = m_clickActive || m_trackpadActive || m_anyTouchDown;
    reset(nowMs, sendFn, nextSeq);
    m_mode = mode;
    if (wasTouching) {
        m_suppressed = true;
    }
}

void TouchMouseProcessor::process(const TouchPoint* touches,
                                  size_t count,
                                  int64_t nowMs,
                                  const SendPacketFn& sendFn,
                                  const NextSeqFn& nextSeq) {
    if (!touches || count == 0) {
        m_anyTouchDown = false;
        if (m_suppressed) {
            m_suppressed = false;
        }
        if (m_clickActive) {
            m_clickActive = false;
            if (sendFn && nextSeq) {
                sendFn(buildMouseClickAt(nextSeq(), static_cast<uint64_t>(nowMs),
                                         m_lastClickX, m_lastClickY, kMouseLeft, false));
            }
        }
        if (m_trackpadActive) {
            if (sendFn && nextSeq) {
                if (m_hadTwoFingers) {
                    if (!m_twoFingerScrolled && (nowMs - m_downTimeMs) <= 350) {
                        sendFn(buildMouseButton(nextSeq(), static_cast<uint64_t>(nowMs), kMouseRight, true));
                        sendFn(buildMouseButton(nextSeq(), static_cast<uint64_t>(nowMs), kMouseRight, false));
                    }
                } else if (!m_trackpadMoved && (nowMs - m_downTimeMs) <= 350) {
                    sendFn(buildMouseButton(nextSeq(), static_cast<uint64_t>(nowMs), kMouseLeft, true));
                    sendFn(buildMouseButton(nextSeq(), static_cast<uint64_t>(nowMs), kMouseLeft, false));
                }
            }
            m_trackpadActive = false;
            m_trackpadMoved = false;
            m_hadTwoFingers = false;
            m_twoFingerScrolled = false;
            m_accumX = 0.0f;
            m_accumY = 0.0f;
        }
        return;
    }

    m_anyTouchDown = true;

    if (m_suppressed) {
        return;
    }

    if (m_mode == TouchMouseMode::Locked) {
        return;
    }

    const uint32_t x0 = std::clamp<uint32_t>(touches[0].x, 0, 1280);
    const uint32_t y0 = std::clamp<uint32_t>(touches[0].y, 0, 720);

    if (!m_clickActive && !m_trackpadActive && m_overlayVisible) {
        const float fx = static_cast<float>(x0);
        const float fy = static_cast<float>(y0);
        if (m_leftBtn.contains(fx, fy) || m_rightBtn.contains(fx, fy)) {
            m_suppressed = true;
            return;
        }
    }

    if (m_mode == TouchMouseMode::Click) {
        const uint16_t nx = static_cast<uint16_t>((x0 * 65535U) / 1280U);
        const uint16_t ny = static_cast<uint16_t>((y0 * 65535U) / 720U);
        if (!m_clickActive) {
            m_clickActive = true;
            m_lastClickX = nx;
            m_lastClickY = ny;
            if (sendFn && nextSeq) {
                sendFn(buildMouseMoveAbsolute(nextSeq(), static_cast<uint64_t>(nowMs), nx, ny));
                sendFn(buildMouseClickAt(nextSeq(), static_cast<uint64_t>(nowMs),
                                         nx, ny, kMouseLeft, true));
            }
        } else if (std::abs(static_cast<int>(nx) - static_cast<int>(m_lastClickX)) >= 64 ||
                   std::abs(static_cast<int>(ny) - static_cast<int>(m_lastClickY)) >= 64) {
            m_lastClickX = nx;
            m_lastClickY = ny;
            if (sendFn && nextSeq) {
                sendFn(buildMouseMoveAbsolute(nextSeq(), static_cast<uint64_t>(nowMs), nx, ny));
            }
        }
        return;
    }

    // Trackpad mode: single-finger drag = relative move, tap = left click,
    // two-finger tap = right click, two-finger vertical drag = mouse wheel.
    if (count >= 2) {
        const uint32_t y1 = std::clamp<uint32_t>(touches[1].y, 0, 720);
        const float avgY = (static_cast<float>(y0) + static_cast<float>(y1)) * 0.5f;
        if (!m_trackpadActive) {
            m_trackpadActive = true;
            m_downTimeMs = nowMs;
        }
        if (!m_hadTwoFingers) {
            m_hadTwoFingers = true;
            m_twoFingerScrolled = false;
            m_twoFingerPrevY = avgY;
        } else {
            const float dy = avgY - m_twoFingerPrevY;
            if (std::fabs(dy) >= 28.0f) {
                m_twoFingerScrolled = true;
                const int dir = (dy > 0.0f) ? -1 : 1;
                if (sendFn && nextSeq) {
                    sendFn(buildMouseScroll(nextSeq(), static_cast<uint64_t>(nowMs), dir));
                }
                m_twoFingerPrevY = avgY;
            }
        }
        return;
    }

    if (m_hadTwoFingers) {
        return;
    }

    if (!m_trackpadActive) {
        m_trackpadActive = true;
        m_trackpadMoved = false;
        m_startX = x0;
        m_startY = y0;
        m_prevX = x0;
        m_prevY = y0;
        m_downTimeMs = nowMs;
        m_accumX = 0.0f;
        m_accumY = 0.0f;
        return;
    }

    const int dxRaw = static_cast<int>(x0) - static_cast<int>(m_prevX);
    const int dyRaw = static_cast<int>(y0) - static_cast<int>(m_prevY);
    m_prevX = x0;
    m_prevY = y0;

    if (!m_trackpadMoved) {
        const int totalDist = std::max(
            std::abs(static_cast<int>(x0) - static_cast<int>(m_startX)),
            std::abs(static_cast<int>(y0) - static_cast<int>(m_startY)));
        if (totalDist >= 10) {
            m_trackpadMoved = true;
        }
    }

    if (m_trackpadMoved && (dxRaw != 0 || dyRaw != 0)) {
        constexpr float kTrackpadSensitivity = 1.5f;
        m_accumX += static_cast<float>(dxRaw) * kTrackpadSensitivity;
        m_accumY += static_cast<float>(dyRaw) * kTrackpadSensitivity;
        int ix = static_cast<int>(std::trunc(m_accumX));
        int iy = static_cast<int>(std::trunc(m_accumY));
        if (ix != 0 || iy != 0) {
            m_accumX -= static_cast<float>(ix);
            m_accumY -= static_cast<float>(iy);
            ix = std::clamp(ix, -32767, 32767);
            iy = std::clamp(iy, -32767, 32767);
            if (sendFn && nextSeq) {
                sendFn(buildMouseMoveRelative(nextSeq(),
                                              static_cast<uint64_t>(nowMs),
                                              static_cast<int16_t>(ix),
                                              static_cast<int16_t>(iy)));
            }
        }
    }
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

void SwitchInputBridge::setTouchMouseMode(TouchMouseMode mode) {
    m_touchMouseMode.store(mode);
}

void SwitchInputBridge::setOverlayButtonRects(bool visible, TouchHitRect leftBtn, TouchHitRect rightBtn) {
    std::lock_guard lock(m_overlayRectMu);
    m_overlayButtonsVisible = visible;
    m_overlayLeftBtn = leftBtn;
    m_overlayRightBtn = rightBtn;
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

    TouchMouseProcessor touchProc;

    auto sendPkt = [&](const std::vector<uint8_t>& pkt) -> bool {
        SendPacketFn fn;
        {
            std::lock_guard lock(m_cbMu);
            fn = m_sendFn;
        }
        return fn ? fn(pkt) : false;
    };
    auto seqFn = [this]() -> uint32_t { return nextSeq(); };

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

            {
                std::lock_guard rectLock(m_overlayRectMu);
                touchProc.setOverlayRects(m_overlayButtonsVisible, m_overlayLeftBtn, m_overlayRightBtn);
            }
            touchProc.setMode(m_touchMouseMode.load(), nowMs, sendPkt, seqFn);

            HidTouchScreenState touchState{};
            TouchPoint pts[2]{};
            size_t ptCount = 0;
            if (hidGetTouchScreenStates(&touchState, 1) > 0 && touchState.count > 0) {
                ptCount = std::min<size_t>(static_cast<size_t>(touchState.count), 2);
                for (size_t i = 0; i < ptCount; ++i) {
                    pts[i].x = touchState.touches[i].x;
                    pts[i].y = touchState.touches[i].y;
                }
            }
            touchProc.process(pts, ptCount, nowMs, sendPkt, seqFn);
        } else {
            touchProc.reset(nowMs, sendPkt, seqFn);
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
