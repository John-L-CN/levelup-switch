#include "levelup/rumble_switch.hpp"

#include "levelup/crypto_util.hpp"
#include "levelup/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

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

// ---------------- Switch HID vibration primitives ----------------

#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
namespace {

// NOTE (wireless-JoyCon idle-rumble fix, 2026-11-25)
// 症状：掌机/主机模式下把 Joy-Con 取下来无线使用，进入程序后手柄立刻开始"小幅持续震动"；
// 按 HOME 回桌面再进来就不震了。
//
// 成因（已按 libnx/Horizon 语义核对）：
//  extern/borealis 的 SwitchInputManager::clearVibration() 调错了重载 ——
//      sendRumbleInternal(handles, values, 160.0f, 320.0f, 0.0f, 0.0f)
//  这个重载把第 3/4 个参数当"低频/高频电机强度"，于是 amp 被写成 320.0f（HidVibrationValue
//  的 amp 合法区间是 0.0f~1.0f，1.0 即满幅；libnx 只做透传，不 clamp），而 freq 又被算成
//  160/204≈0.78Hz、320/204≈1.57Hz —— 远低于线性谐振马达的工作频段，于是马达被一个"满幅但
//  频率几乎为 0"的信号驱动：听起来/手上感觉就是**微弱而持续的小幅震动**（所以不是"满幅狂震"）。
//  clearVibration() 在「控制器 style set 变化」时被调用，而程序启动时 padConfigureInput +
//  hidSetNpadJoyHoldType + 无线 Joy-Con 重新枚举恰好会触发一轮 style 变化 → 一起动就震；
//  HOME 出去再进来时 style 不再变化，clearVibration 不再被调用 → 不震。
//
// 为什么旧看门狗（只写 Handheld + No1，且只在 hidInitializeVibrationDevices 成功后才发）
// 压不住：HidVibrationDeviceHandle 是 libnx 本地算出来的三元组（npad_style_index / player_number /
// device_idx），hidInitializeVibrationDevices() 先填句柄、再去"激活表"里激活；激活失败会返回
// 错误，但句柄其实已经填好了 —— 旧代码收到错误就放弃发送（no1Style=0 / handheldInit=false），
// 结果一个 0 都没发出去，而 borealis 那边是不看返回值直接 send 的，于是"它写得进、我们压不住"。
// 另外旧代码只覆盖 Handheld + No1，如果 Joy-Con 落在 No2..No5 或落在 FullKey/JoyLeft/JoyRight
// 样式句柄上，也完全漏掉。
//
// 现在的策略：
//  1) 覆盖 Handheld + No1..No8 × {Handheld, FullKey, JoyDual, JoyLeft, JoyRight} 全部句柄组合，
//     句柄一律由 hidInitializeVibrationDevices() 填（返回值只看日志，不阻断发送）。
//  2) 启动后 3 秒内以 ~60Hz 连续补零（把 clearVibration 的误写压在 16ms 之内），之后 4Hz 维持；
//     检测到 npad style set 变化立刻重新激活句柄 + 让 60Hz 突发再来 1 秒。
//  3) 闭环回读 hidGetActualVibrationValue()：还在震就立刻再补零，并把幅值打进日志，
//     下次现场测试可以直接从 levelup.log 判断是哪个设备/哪个句柄在震。

constexpr int kSweepIdCount    = 9; // Handheld 优先，然后 No1..No8
constexpr int kSweepStyleCount = 5;
constexpr int kSweepSlotCount  = kSweepIdCount * kSweepStyleCount;

// 顺序有讲究：最可能承载无线 Joy-Con 的组合排在前面（激活表万一有上限也能覆盖到）。
const HidNpadIdType kSweepIds[kSweepIdCount] = {
    HidNpadIdType_Handheld,
    HidNpadIdType_No1,
    HidNpadIdType_No2,
    HidNpadIdType_No3,
    HidNpadIdType_No4,
    HidNpadIdType_No5,
    HidNpadIdType_No6,
    HidNpadIdType_No7,
    HidNpadIdType_No8,
};

const HidNpadStyleTag kSweepStyles[kSweepStyleCount] = {
    HidNpadStyleTag_NpadFullKey,
    HidNpadStyleTag_NpadJoyDual,
    HidNpadStyleTag_NpadHandheld,
    HidNpadStyleTag_NpadJoyLeft,
    HidNpadStyleTag_NpadJoyRight,
};

struct SweepSlot {
    HidVibrationDeviceHandle handles[2]{};
    s32 count = 0;
    HidNpadIdType id = HidNpadIdType_No1;
    HidNpadStyleTag style = HidNpadStyleTag_NpadJoyDual;
};

HidVibrationValue zeroVibrationValue() {
    // amp 0（真·静音）；频率字段保持 Joy-Con 的常用区间，避免某些系统版本把 0Hz 当成"沿用上次"。
    return HidVibrationValue{0.0f, 160.0f, 0.0f, 320.0f};
}

/// 句柄 = (style_index, player_number, device_idx) 三元组，libnx 本地计算；激活失败也会填好。
void fillSlotHandles(SweepSlot& slot) {
    const bool single = (slot.style == HidNpadStyleTag_NpadJoyLeft ||
                         slot.style == HidNpadStyleTag_NpadJoyRight);
    const s32 count = single ? 1 : 2;
    HidVibrationDeviceHandle filled[2]{};
    // 返回值故意不看：激活失败不影响句柄有效性，发送失败会被 sweepZeros 统计。
    hidInitializeVibrationDevices(filled, count, slot.id, slot.style);
    slot.handles[0] = filled[0];
    slot.handles[1] = (count > 1) ? filled[1] : filled[0];
    slot.count = count;
}

void buildSweepSlots(SweepSlot* slots) {
    int n = 0;
    for (int i = 0; i < kSweepIdCount; ++i) {
        for (int s = 0; s < kSweepStyleCount; ++s) {
            SweepSlot& slot = slots[n++];
            slot.id = kSweepIds[i];
            slot.style = kSweepStyles[s];
            fillSlotHandles(slot);
        }
    }
}

/// 向所有槽位写 0；返回发送成功的次数。失败不重试（句柄本身仍然有效，下一轮再发）。
int sweepZeros(SweepSlot* slots, int slotCount) {
    const HidVibrationValue zeros[2] = {zeroVibrationValue(), zeroVibrationValue()};
    int ok = 0;
    for (int i = 0; i < slotCount; ++i) {
        if (slots[i].count <= 0) {
            continue;
        }
        if (R_SUCCEEDED(hidSendVibrationValues(slots[i].handles, zeros, slots[i].count))) {
            ++ok;
        }
    }
    return ok;
}

u32 npadStyleFingerprint() {
    u32 fp = 0x811c9dc5u;
    for (int i = 0; i < kSweepIdCount; ++i) {
        fp = (fp ^ hidGetNpadStyleSet(kSweepIds[i])) * 16777619u;
    }
    return fp;
}

std::string styleSetSummary() {
    std::string out;
    for (int i = 0; i < kSweepIdCount; ++i) {
        const u32 style = hidGetNpadStyleSet(kSweepIds[i]);
        if (style == 0) {
            continue;
        }
        out += (out.empty() ? "" : " ");
        out += "id" + std::to_string(static_cast<int>(kSweepIds[i])) + "=0x" +
               std::to_string(static_cast<unsigned>(style));
    }
    return out.empty() ? std::string("<none>") : out;
}

/// 回读每个"真实存在"的震动设备当前的幅值；非零就写日志（这就是手柄还在震的直接证据）。
int logNonZeroActualValues(const SweepSlot* slots, int slotCount, const char* tag) {
    int noisy = 0;
    for (int i = 0; i < slotCount; ++i) {
        const SweepSlot& slot = slots[i];
        if (slot.count <= 0) {
            continue;
        }
        HidVibrationDeviceInfo info{};
        if (R_FAILED(hidGetVibrationDeviceInfo(slot.handles[0], &info))) {
            continue; // 该 (id, style, device) 当前没有实体设备
        }
        for (s32 dev = 0; dev < slot.count; ++dev) {
            HidVibrationValue actual{};
            if (R_FAILED(hidGetActualVibrationValue(slot.handles[dev], &actual))) {
                continue;
            }
            if (actual.amp_low <= 0.001f && actual.amp_high <= 0.001f) {
                continue;
            }
            ++noisy;
            logWarn(std::string("[Rumble] ") + (tag ? tag : "?") + " device still driven: id=" +
                    std::to_string(static_cast<int>(slot.id)) + " style=0x" +
                    std::to_string(static_cast<unsigned>(slot.style)) + " dev=" +
                    std::to_string(static_cast<int>(dev)) + " ampLow=" +
                    std::to_string(actual.amp_low) + " freqLow=" +
                    std::to_string(actual.freq_low) + " ampHigh=" +
                    std::to_string(actual.amp_high) + " freqHigh=" +
                    std::to_string(actual.freq_high));
        }
    }
    return noisy;
}

} // namespace

namespace {

struct SwitchVibrationState {
    bool handheldInit = false;
    HidVibrationDeviceHandle handheld[2]{};

    u32 no1Style = 0;
    HidVibrationDeviceHandle no1[2]{};

    // NOTE (docked rumble fix): Horizon 在插/拔底座的瞬间会重新枚举 HID 震动设备。旧句柄上
    // 最后一次写入的非零幅值会被新枚举的设备沿用，而我们的合批器以为「已经发过同样的值」
    // 直接 dedupe 掉，于是手柄在入坞后一直震、再也收不到归零指令。这里记录 applet 模式，
    // 检测到切换就先把所有已知句柄显式清零并作废，让后续流程重新初始化。
    AppletOperationMode lastMode = AppletOperationMode_Handheld;
    bool modeKnown = false;

    // 诊断用：只在 0 -> 非0 的边沿打一条日志，避免 60Hz 刷屏。
    bool activeLogged = false;
};

void writeVibrationZero(const HidVibrationDeviceHandle* handles, int count) {
    if (!handles || count <= 0) {
        return;
    }
    HidVibrationValue zeros[2] = {
        {0.0f, 160.0f, 0.0f, 320.0f},
        {0.0f, 160.0f, 0.0f, 320.0f},
    };
    hidSendVibrationValues(handles, zeros, static_cast<s32>(std::min(count, 2)));
}

void hardZeroAll(SwitchVibrationState* hw) {
    if (!hw) {
        return;
    }
    if (hw->handheldInit) {
        writeVibrationZero(hw->handheld, 2);
        hw->handheldInit = false;
    }
    if (hw->no1Style != 0) {
        writeVibrationZero(hw->no1, 2);
        hw->no1Style = 0;
    }
}

/// 返回 true 表示刚刚发生了掌机 <-> 底座切换（已把旧句柄清零作废）。
bool pollOperationModeChange(SwitchVibrationState* hw) {
    if (!hw) {
        return false;
    }
    const AppletOperationMode mode = appletGetOperationMode();
    if (!hw->modeKnown) {
        hw->modeKnown = true;
        hw->lastMode = mode;
        return false;
    }
    if (mode == hw->lastMode) {
        return false;
    }
    hw->lastMode = mode;
    hardZeroAll(hw);
    return true;
}

} // namespace
#endif

void zeroAllVibrationDevices(const char* reason) {
#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
    SweepSlot slots[kSweepSlotCount];
    buildSweepSlots(slots);
    const int ok = sweepZeros(slots, kSweepSlotCount);
    const int noisy = logNonZeroActualValues(slots, kSweepSlotCount, "zero-all");

    logInfo(std::string("[Rumble] zeroAllVibrationDevices(") + (reason ? reason : "?") +
            ") sent=" + std::to_string(ok) + "/" + std::to_string(kSweepSlotCount) +
            " stillDriven=" + std::to_string(noisy) + " styles=[" + styleSetSummary() + "]");
#else
    (void)reason;
#endif
}

namespace {

std::atomic<int> g_sessionRumbleActive{0};

} // namespace

void setSessionRumbleActive(bool active) {
    g_sessionRumbleActive.store(active ? 1 : 0);
}

#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
namespace {

struct IdleGuardState {
    SweepSlot slots[kSweepSlotCount];
    u32 fingerprint = 0;
    bool fingerprintKnown = false;
    int64_t burstUntilMs = 0; // 该时刻之前按 ~60Hz 补零
    int64_t nextReadbackMs = 0;
    int64_t startMs = 0;
};

std::mutex g_guardMu;
std::condition_variable g_guardCv;
bool g_guardRunning = false;
bool g_guardStop = false;
std::thread g_guardThread;
IdleGuardState g_guardState;

constexpr int kStartupBurstMs = 3000;
constexpr int kStyleChangeBurstMs = 1000;
constexpr int kBurstIntervalMs = 16;  // ~60Hz
constexpr int kSteadyIntervalMs = 250; // 4Hz

void idleGuardTick(int64_t nowMs) {
    auto& st = g_guardState;

    const u32 fp = npadStyleFingerprint();
    if (!st.fingerprintKnown || fp != st.fingerprint) {
        const bool reinit = st.fingerprintKnown;
        st.fingerprintKnown = true;
        st.fingerprint = fp;
        if (reinit) {
            // style 变化 = Horizon 重新枚举了震动设备，句柄要重新激活（borealis 的
            // clearVibration 也正是在这些时刻误写 amp=320 的）。
            for (int i = 0; i < kSweepSlotCount; ++i) {
                fillSlotHandles(st.slots[i]);
            }
            logInfo("[Rumble] npad style set changed -> re-init vibration handles, styles=[" +
                    styleSetSummary() + "]");
        }
        st.burstUntilMs = nowMs + (reinit ? kStyleChangeBurstMs : kStartupBurstMs);
        st.nextReadbackMs = nowMs + 200;
    }

    const int ok = sweepZeros(st.slots, kSweepSlotCount);

    // 闭环回读：设备还在被驱动就说明有别的代码（borealis clearVibration / 上一局的残留幅值）
    // 又写了非零值 —— 立刻再补一轮零，并把现场打进日志。
    if (nowMs >= st.nextReadbackMs) {
        st.nextReadbackMs = nowMs + 1000;
        const int noisy = logNonZeroActualValues(st.slots, kSweepSlotCount, "guard-readback");
        if (noisy > 0) {
            sweepZeros(st.slots, kSweepSlotCount);
            logWarn("[Rumble] guard found " + std::to_string(noisy) +
                    " driven device(s); re-zeroed (sent=" + std::to_string(ok) + "/" +
                    std::to_string(kSweepSlotCount) + ")");
        } else if (nowMs - st.startMs < 5000) {
            logInfo("[Rumble] guard readback clean (sent=" + std::to_string(ok) + "/" +
                    std::to_string(kSweepSlotCount) + ") styles=[" + styleSetSummary() + "]");
        }
    }
}

void idleGuardLoop() {
    for (;;) {
        std::unique_lock lock(g_guardMu);
        if (g_guardStop) {
            return;
        }
        const int64_t nowMs = unixMillisNow();
        int waitMs = kSteadyIntervalMs;
        // 有串流会话时让路（会话自己按云端包驱动震动），但要继续跟踪 style 变化。
        if (g_sessionRumbleActive.load() == 0) {
            idleGuardTick(nowMs);
            waitMs = (nowMs < g_guardState.burstUntilMs) ? kBurstIntervalMs : kSteadyIntervalMs;
        } else {
            g_guardState.fingerprintKnown = false; // 会话结束后重新建一遍句柄
        }
        g_guardCv.wait_for(lock, std::chrono::milliseconds(waitMs));
    }
}

} // namespace
#endif

void startIdleVibrationGuard() {
#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
    std::lock_guard lock(g_guardMu);
    if (g_guardRunning) {
        return;
    }
    g_guardStop = false;
    g_guardRunning = true;

    auto& st = g_guardState;
    buildSweepSlots(st.slots);
    st.fingerprintKnown = false;
    st.startMs = unixMillisNow();
    st.burstUntilMs = st.startMs + kStartupBurstMs;
    st.nextReadbackMs = st.startMs + 200;

    const int ok = sweepZeros(st.slots, kSweepSlotCount);
    logInfo("[Rumble] idle vibration guard started: slots=" + std::to_string(kSweepSlotCount) +
            " sent=" + std::to_string(ok) + " styles=[" + styleSetSummary() + "]");

    g_guardThread = std::thread([]() { idleGuardLoop(); });
#endif
}

void stopIdleVibrationGuard() {
#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
    std::thread toJoin;
    {
        std::lock_guard lock(g_guardMu);
        if (!g_guardRunning) {
            return;
        }
        g_guardStop = true;
        g_guardCv.notify_all();
        toJoin = std::move(g_guardThread);
        g_guardRunning = false;
    }
    if (toJoin.joinable()) {
        toJoin.join();
    }
    zeroAllVibrationDevices("guard-stop");
#endif
}

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
    // 会话开始：让空闲看门狗停止补零，交给云端震动包驱动。
    setSessionRumbleActive(true);
    m_thread = std::thread([this]() { workerLoop(); });
}

void RumbleSwitch::stop() {
    m_running.store(false);
    // 会话结束：恢复空闲看门狗（borealis 的 clearVibration 会在 style 变化时误发满幅）。
    setSessionRumbleActive(false);
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    applyHardwareVibration(0.0f, 0.0f);
    m_active.store(false);

#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
    std::lock_guard lock(m_mu);
    if (m_hwState) {
        auto* hw = static_cast<SwitchVibrationState*>(m_hwState);
        // 退出串流时把所有登记过的句柄都显式清零（含上一次 style 的 No1 句柄），
        // 否则拔插底座/换手柄后旧句柄上的残留幅值会让手柄继续震。
        hardZeroAll(hw);
        delete hw;
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
#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
        // 入坞/出坞会重新枚举震动设备，旧句柄残留的非零幅值会变成「一直在震」。检测到模式
        // 切换立刻清零并作废句柄，同时重置合批器基线，让下一帧以 0 幅值重新建连。
        if (pollOperationModeChange(static_cast<SwitchVibrationState*>(m_hwState))) {
            {
                std::lock_guard lock(m_mu);
                m_coalescer.reset();
            }
            applyHardwareVibration(0.0f, 0.0f);
            m_active.store(false);
        }
#endif

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
            m_lastLowSent = low;
            m_lastHighSent = high;
            m_lastResendMs = unixMillisNow();
        } else if (m_lastResendMs > 0 &&
                   unixMillisNow() - m_lastResendMs >= 250) {
            // 每 250ms 重放一次最近写入的幅值。手柄会记住最后一次写入的幅值，
            // borealis 的 clearVibration() 在手柄 style 变化（取下/装回 Joy-Con、插拔底座）
            // 时会误写 amp=320 的次共振信号；重放能把它在 250ms 内压回去。
            applyHardwareVibration(m_lastLowSent, m_lastHighSent);
            m_lastResendMs = unixMillisNow();
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
        const Result rc = hidInitializeVibrationDevices(
            hw->handheld, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
        // 句柄由 libnx 本地算出，激活失败也不影响发送 —— 标记为已初始化，别每帧重来。
        hw->handheldInit = true;
        if (R_FAILED(rc)) {
            logWarn("[Rumble] handheld vibrator activate rc=0x" +
                    std::to_string(static_cast<unsigned>(rc)) + " (handles still usable)");
        }
    }
    hidSendVibrationValues(hw->handheld, vals, 2);

    const u32 curStyle = hidGetNpadStyleSet(HidNpadIdType_No1);
    u32 desiredTag = 0;
    if (curStyle & HidNpadStyleTag_NpadFullKey) {
        desiredTag = HidNpadStyleTag_NpadFullKey;
    } else if (curStyle & HidNpadStyleTag_NpadJoyDual) {
        desiredTag = HidNpadStyleTag_NpadJoyDual;
    }

    if (desiredTag != 0 && desiredTag != hw->no1Style) {
        if (hw->no1Style != 0) {
            // style 变了（例如入坞 JoyDual -> FullKey）：先把旧句柄残留幅值清零，否则它一直震。
            writeVibrationZero(hw->no1, 2);
            hw->no1Style = 0;
        }
        const Result rc = hidInitializeVibrationDevices(
            hw->no1, 2, HidNpadIdType_No1, static_cast<HidNpadStyleTag>(desiredTag));
        // 同上：句柄已经填好，激活失败只记日志，不能因此放弃发送（否则流内震动全哑）。
        hw->no1Style = desiredTag;
        if (R_FAILED(rc)) {
            logWarn("[Rumble] no1 vibrator activate rc=0x" +
                    std::to_string(static_cast<unsigned>(rc)) + " style=0x" +
                    std::to_string(static_cast<unsigned>(desiredTag)) + " (handles still usable)");
        }
        writeVibrationZero(hw->no1, 2); // 新句柄先落一次 0，再写入本次目标幅值
    } else if (desiredTag == 0) {
        if (hw->no1Style != 0) {
            writeVibrationZero(hw->no1, 2);
            hw->no1Style = 0;
        }
    }

    if (hw->no1Style != 0) {
        hidSendVibrationValues(hw->no1, vals, 2);
    }

    // 诊断：只在开始震的那一帧记一条（含句柄状态），方便从日志判断震动是本地发起的
    // 还是 Horizon 里残留的旧幅值。
    const bool active = (cLow > 0.0f || cHigh > 0.0f);
    if (active && !hw->activeLogged) {
        hw->activeLogged = true;
        logInfo("[Rumble] drive low=" + std::to_string(cLow) + " high=" +
                std::to_string(cHigh) + " handheldInit=" +
                std::to_string(hw->handheldInit ? 1 : 0) + " no1Style=0x" +
                std::to_string(static_cast<unsigned>(hw->no1Style)));
    } else if (!active) {
        hw->activeLogged = false;
    }
#else
    (void)lowAmp;
    (void)highAmp;
#endif
}

} // namespace levelup::input
