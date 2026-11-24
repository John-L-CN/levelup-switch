#include "levelup/rumble_switch.hpp"

#include "levelup/crypto_util.hpp"
#include "levelup/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
#include <switch.h>
#endif

namespace levelup::input {

// ---------------- RumbleCoalescer ----------------

void RumbleCoalescer::reset() {
    m_pendingLow = 0.0f;
    m_pendingHigh = 0.0f;
    m_pendingDirty = false;
    m_lastSentLow = 0.0f;
    m_lastSentHigh = 0.0f;
    m_hasSent = false;
    m_lastSentMs = 0;
    m_lastNonZeroSubmitMs = 0;
}

void RumbleCoalescer::setEnabled(bool enabled) {
    m_enabled = enabled;
    if (!m_enabled && (m_pendingLow > 0.0f || m_pendingHigh > 0.0f ||
                       m_lastSentLow > 0.0f || m_lastSentHigh > 0.0f)) {
        m_pendingLow = 0.0f;
        m_pendingHigh = 0.0f;
        m_pendingDirty = true;
    }
}

void RumbleCoalescer::setStrength(float strength01) {
    m_strength = std::clamp(strength01, 0.0f, 1.0f);
    if (m_strength <= 0.001f && (m_lastSentLow > 0.0f || m_lastSentHigh > 0.0f)) {
        m_pendingLow = 0.0f;
        m_pendingHigh = 0.0f;
        m_pendingDirty = true;
    }
}

bool RumbleCoalescer::submit(uint16_t rawLow, uint16_t rawHigh, int64_t nowMs) {
    float ampLow = 0.0f;
    float ampHigh = 0.0f;
    if (m_enabled) {
        ampLow = scaleMotorNorm(rawLow, m_strength);
        ampHigh = scaleMotorNorm(rawHigh, m_strength);
    }

    const bool wasActive = (m_lastSentLow > 0.0f || m_lastSentHigh > 0.0f);
    const bool nowActive = (ampLow > 0.0f || ampHigh > 0.0f);

    if (nowActive) {
        m_lastNonZeroSubmitMs = nowMs;
    }

    const bool changedPending =
        !m_hasSent ||
        std::fabs(ampLow - m_pendingLow) > 1e-4f ||
        std::fabs(ampHigh - m_pendingHigh) > 1e-4f ||
        std::fabs(ampLow - m_lastSentLow) > 1e-4f ||
        std::fabs(ampHigh - m_lastSentHigh) > 1e-4f;

    if (changedPending) {
        m_pendingLow = ampLow;
        m_pendingHigh = ampHigh;
        m_pendingDirty = true;
    }

    // Immediate wakeup on start (0 -> >0) or stop (>0 -> 0) transitions
    return (!wasActive && nowActive) || (wasActive && !nowActive);
}

bool RumbleCoalescer::tick(int64_t nowMs, float& outLowAmp, float& outHighAmp) {
    // 1. Failsafe 3-second watchdog auto-stop if active vibration receives no keepalive/update
    if ((m_lastSentLow > 0.0f || m_lastSentHigh > 0.0f) &&
        m_lastNonZeroSubmitMs > 0 &&
        (nowMs - m_lastNonZeroSubmitMs) >= m_cfg.idleTimeoutMs) {
        m_pendingLow = 0.0f;
        m_pendingHigh = 0.0f;
        m_pendingDirty = false;
        m_lastSentLow = 0.0f;
        m_lastSentHigh = 0.0f;
        m_hasSent = true;
        m_lastSentMs = nowMs;
        outLowAmp = 0.0f;
        outHighAmp = 0.0f;
        return true;
    }

    if (!m_pendingDirty) {
        return false;
    }

    // Deduplicate against currently active motor state unless this is the initial baseline send
    if (m_hasSent &&
        std::fabs(m_pendingLow - m_lastSentLow) <= 1e-4f &&
        std::fabs(m_pendingHigh - m_lastSentHigh) <= 1e-4f) {
        m_pendingDirty = false;
        return false;
    }

    const bool wasActive = (m_lastSentLow > 0.0f || m_lastSentHigh > 0.0f);
    const bool nowActive = (m_pendingLow > 0.0f || m_pendingHigh > 0.0f);
    const bool edge = !m_hasSent || (!wasActive && nowActive) || (wasActive && !nowActive);
    const int64_t sinceLast = nowMs - m_lastSentMs;

    if (!edge && sinceLast < m_cfg.coalesceIntervalMs) {
        return false;
    }

    m_lastSentLow = m_pendingLow;
    m_lastSentHigh = m_pendingHigh;
    m_hasSent = true;
    m_lastSentMs = nowMs;
    m_pendingDirty = false;
    outLowAmp = m_lastSentLow;
    outHighAmp = m_lastSentHigh;
    return true;
}

int64_t RumbleCoalescer::nextWakeDelayMs(int64_t nowMs) const {
    if (m_pendingDirty) {
        const int64_t rem = m_cfg.coalesceIntervalMs - (nowMs - m_lastSentMs);
        return std::max<int64_t>(1, rem);
    }
    if ((m_lastSentLow > 0.0f || m_lastSentHigh > 0.0f) && m_lastNonZeroSubmitMs > 0) {
        const int64_t rem = m_cfg.idleTimeoutMs - (nowMs - m_lastNonZeroSubmitMs);
        return std::max<int64_t>(1, rem);
    }
    return 1000;
}

// ---------------- RumbleSwitch ----------------

#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
namespace {

struct SwitchVibrationState {
    bool handheldInit = false;
    HidVibrationDeviceHandle handheld[2]{};

    u32 no1Style = 0;
    HidVibrationDeviceHandle no1[2]{};
};

} // namespace
#endif

RumbleSwitch::~RumbleSwitch() {
    stop();
}

void RumbleSwitch::start() {
    stop();
    {
        std::lock_guard lock(m_mu);
        m_coalescer.reset();
#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
        if (!m_hwState) {
            m_hwState = new SwitchVibrationState();
        }
#endif
    }
    m_active.store(false);
    m_running.store(true);
    m_thread = std::thread([this]() { workerLoop(); });
}

void RumbleSwitch::stop() {
    m_running.store(false);
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    applyHardwareVibration(0.0f, 0.0f);
    m_active.store(false);

#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
    std::lock_guard lock(m_mu);
    if (m_hwState) {
        delete static_cast<SwitchVibrationState*>(m_hwState);
        m_hwState = nullptr;
    }
#endif
}

void RumbleSwitch::submit(const RumbleEvent& ev) {
    if (!m_running.load()) return;
    bool urgent = false;
    {
        std::lock_guard lock(m_mu);
        urgent = m_coalescer.submit(ev.lowFreq, ev.highFreq, unixMillisNow());
    }
    if (urgent) {
        m_cv.notify_one();
    }
}

void RumbleSwitch::forceZero() {
    {
        std::lock_guard lock(m_mu);
        m_coalescer.submit(0, 0, unixMillisNow());
    }
    m_cv.notify_one();
    applyHardwareVibration(0.0f, 0.0f);
    m_active.store(false);
}

void RumbleSwitch::setEnabled(bool enabled) {
    {
        std::lock_guard lock(m_mu);
        m_coalescer.setEnabled(enabled);
    }
    m_cv.notify_one();
}

void RumbleSwitch::setStrengthPercent(int percent) {
    {
        std::lock_guard lock(m_mu);
        m_coalescer.setStrength(static_cast<float>(std::clamp(percent, 0, 100)) / 100.0f);
    }
    m_cv.notify_one();
}

void RumbleSwitch::workerLoop() {
    while (m_running.load()) {
        float low = 0.0f;
        float high = 0.0f;
        bool shouldEmit = false;
        int64_t waitMs = 100;

        {
            std::lock_guard lock(m_mu);
            const int64_t nowMs = unixMillisNow();
            shouldEmit = m_coalescer.tick(nowMs, low, high);
            waitMs = m_coalescer.nextWakeDelayMs(nowMs);
        }

        if (shouldEmit) {
            applyHardwareVibration(low, high);
            m_active.store(low > 0.0f || high > 0.0f);
        }

        std::unique_lock lock(m_mu);
        if (!m_running.load()) break;
        const int64_t nowMs = unixMillisNow();
        waitMs = m_coalescer.nextWakeDelayMs(nowMs);
        m_cv.wait_for(lock, std::chrono::milliseconds(waitMs));
    }
}

void RumbleSwitch::applyHardwareVibration(float lowAmp, float highAmp) {
#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
    auto* hw = static_cast<SwitchVibrationState*>(m_hwState);
    if (!hw) return;

    const float cLow = std::clamp(lowAmp, 0.0f, 1.0f);
    const float cHigh = std::clamp(highAmp, 0.0f, 1.0f);
    const HidVibrationValue vals[2] = {
        {cLow, 160.0f, cHigh, 320.0f},
        {cLow, 160.0f, cHigh, 320.0f},
    };

    if (!hw->handheldInit) {
        if (R_SUCCEEDED(hidInitializeVibrationDevices(
                hw->handheld, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld))) {
            hw->handheldInit = true;
        }
    }
    if (hw->handheldInit) {
        hidSendVibrationValues(hw->handheld, vals, 2);
    }

    const u32 curStyle = hidGetNpadStyleSet(HidNpadIdType_No1);
    u32 desiredTag = 0;
    if (curStyle & HidNpadStyleTag_NpadFullKey) {
        desiredTag = HidNpadStyleTag_NpadFullKey;
    } else if (curStyle & HidNpadStyleTag_NpadJoyDual) {
        desiredTag = HidNpadStyleTag_NpadJoyDual;
    }

    if (desiredTag != 0 && desiredTag != hw->no1Style) {
        if (R_SUCCEEDED(hidInitializeVibrationDevices(
                hw->no1, 2, HidNpadIdType_No1, static_cast<HidNpadStyleTag>(desiredTag)))) {
            hw->no1Style = desiredTag;
        }
    } else if (desiredTag == 0) {
        hw->no1Style = 0;
    }

    if (hw->no1Style != 0) {
        hidSendVibrationValues(hw->no1, vals, 2);
    }
#else
    (void)lowAmp;
    (void)highAmp;
#endif
}

} // namespace levelup::input
