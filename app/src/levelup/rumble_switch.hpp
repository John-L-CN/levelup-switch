#pragma once

#include "levelup/input_packet.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace levelup::input {

/// Pure testable HD Rumble coalescing & 3-second idle watchdog state machine.
class RumbleCoalescer {
  public:
    struct Config {
        int64_t coalesceIntervalMs = 16; // ~60Hz max HID send rate
        int64_t idleTimeoutMs = 3000;    // 3s failsafe auto-stop if downlink stop packet lost
    };

    RumbleCoalescer() : m_cfg{} {}
    explicit RumbleCoalescer(const Config& cfg) : m_cfg(cfg) {}

    void reset();
    void setEnabled(bool enabled);
    void setStrength(float strength01);
    bool isEnabled() const { return m_enabled; }
    float strength() const { return m_strength; }

    /// Enqueues latest motor intensities; returns true when immediate wakeup needed (start/stop edge).
    bool submit(uint16_t rawLow, uint16_t rawHigh, int64_t nowMs);

    /// Evaluates whether a HID update should be sent at nowMs; outputs clamped [0.0f, 1.0f] amplitudes.
    bool tick(int64_t nowMs, float& outLowAmp, float& outHighAmp);

    /// Returns wait duration in ms before next scheduled coalesce flush or watchdog check.
    int64_t nextWakeDelayMs(int64_t nowMs) const;

    bool isActive() const { return m_lastSentLow > 0.0f || m_lastSentHigh > 0.0f; }

  private:
    Config m_cfg;
    bool m_enabled = true;
    float m_strength = 1.0f;

    float m_pendingLow = 0.0f;
    float m_pendingHigh = 0.0f;
    bool m_pendingDirty = false;

    float m_lastSentLow = 0.0f;
    float m_lastSentHigh = 0.0f;
    bool m_hasSent = false;
    int64_t m_lastSentMs = 0;
    int64_t m_lastNonZeroSubmitMs = 0;
};

/// Direct libnx HD Rumble bridge (bypasses Borealis SwitchInputManager amp=320.0f binary bug).
class RumbleSwitch {
  public:
    RumbleSwitch() = default;
    ~RumbleSwitch();

    RumbleSwitch(const RumbleSwitch&) = delete;
    RumbleSwitch& operator=(const RumbleSwitch&) = delete;

    void start();
    void stop();

    void submit(const RumbleEvent& ev);
    void forceZero();

    void setEnabled(bool enabled);
    void setStrengthPercent(int percent); // 0..100
    bool isActive() const { return m_active.load(); }

  private:
    void workerLoop();
    void applyHardwareVibration(float lowAmp, float highAmp);

    mutable std::mutex m_mu;
    std::condition_variable m_cv;
    RumbleCoalescer m_coalescer;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_active{false};
    std::thread m_thread;

#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
    void* m_hwState = nullptr; // Opaque pointer to Switch vibration handle cache
#endif
};

} // namespace levelup::input
