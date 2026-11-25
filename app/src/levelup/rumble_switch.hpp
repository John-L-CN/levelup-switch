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

/// 无条件把本机所有 HID 震动设备（Handheld / No1-FullKey / No1-JoyDual）写一次 0 幅值。
/// 用途：串流退出、回到大厅、应用启动这些"不该有震动"的时刻把残留幅值冲掉 ——
/// 手柄在 Horizon 里会记住最后一次写入的幅值，只要没有新的 0 写入就一直震，
/// 直到 HID 重新枚举（这也是"按 HOME 出去再回大厅就不震了"的原因）。
void zeroAllVibrationDevices(const char* reason);

/// 空闲震动看门狗：没有串流会话时把震动维持在 0。
///
/// 存在的理由（borealis 的 bug）：extern/borealis 的
/// SwitchInputManager::clearVibration() 调错了 sendRumbleInternal 重载，把「频率」当成
/// 电机强度传进去，于是写出 amp = 320.0f（HidVibrationValue 的 amp 合法区间是 0~1，
/// 1.0 即满幅）配 0.78Hz / 1.57Hz 的次共振频率 —— 线性谐振马达被这样驱动时是「微弱而
/// 持续的小幅震动」，而不是满幅狂震。clearVibration() 在「控制器 style 变化」时被调用
/// （进大厅初始化、Joy-Con 取下/装回且无线重连、插拔底座），而 HOME 出去再进来 style
/// 不变、不再调用它，所以又不震了。
///
/// 实现要点（2026-11-25 重做）：
///  - 覆盖 Handheld + No1..No8 × {FullKey, JoyDual, Handheld, JoyLeft, JoyRight} 全部句柄；
///  - 句柄由 libnx 本地算出，hidInitializeVibrationDevices() 的返回值**不**用来决定是否发送
///    （旧实现的错误就在这：激活失败就一个 0 都没发出去，而 borealis 是不看返回值照发的）；
///  - 启动后 3 秒以 ~60Hz 突发补零（把误写压在 16ms 内），之后 4Hz 维持；检测到 style set
///    变化立刻重建句柄并把突发再来 1 秒；
///  - 每秒回读 hidGetActualVibrationValue()，仍被驱动就再补零并打日志（现场排查用）。
void startIdleVibrationGuard();
void stopIdleVibrationGuard();

/// 串流会话是否在用震动（有会话时看门狗自动让路，避免打断游戏震动）。
void setSessionRumbleActive(bool active);

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

    // 最近一次真正写入的幅值 + 重放时间戳：每 ~250ms 重放一次，用来压掉
    // borealis clearVibration() 在 style 变化时误写的 320.0f 满幅。
    float m_lastLowSent = 0.0f;
    float m_lastHighSent = 0.0f;
    int64_t m_lastResendMs = 0;

#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
    void* m_hwState = nullptr; // Opaque pointer to Switch vibration handle cache
#endif
};

} // namespace levelup::input
