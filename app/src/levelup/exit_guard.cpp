// NOTE (exit guard, 2026-11-25):
// 症状：按 HOME 把程序丢到后台后，从 HOME 菜单启动别的程序，系统提示「软件无法关闭」。
// 实测只在大厅可正常关闭，串流运行中关闭就失败 —— 说明卡点在串流资源的收尾
// （video/audio 停、rtc close、api->stopGame、以及各线程 join）。
//
// 成因：
//  1) borealis 用 appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend)，后台不挂起、
//     完全靠主循环里的 appletMainLoop() 收退出请求。主循环一旦卡住（线程 join / 阻塞网络 /
//     deko3d present 等三缓冲可用），退出请求就永远处理不到 → 系统报关闭失败。
//  2) 即使请求收到了，Application::exit() 里要 delete 活动栈、join 各后台线程（RTC/解码/音频/
//     封面/网络），任何一处阻塞超过系统的关闭超时，同样报关闭失败。
//
// 对策：退出请求一到就（a）撤回 media playback 声明、（b）停空闲震动看门狗并归零、
// （c）起一个 3 秒兜底线程，到点直接 svcExitProcess。正常关得快就走正常流程，
// 卡住了也有兜底，系统侧一定能完成关闭；同时 curl 的在途请求会在退出时被取消（关键请求除外）。

#include "levelup/exit_guard.hpp"

#include "levelup/log.hpp"
#include "levelup/rtc_session.hpp"
#include "levelup/rumble_switch.hpp"

#include <atomic>
#include <cstdio>
#include <string>

#if defined(__SWITCH__)
#include <switch.h>
#endif

#include <borealis/core/application.hpp>

namespace levelup {
namespace {

std::atomic<bool> g_hookInstalled{false};
std::atomic<bool> g_deadlineArmed{false};

constexpr int kExitDeadlineMs = 3000;

#if defined(__SWITCH__)
AppletHookCookie g_exitCookie;
Thread g_deadlineThread{};
std::atomic<bool> g_deadlineThreadCreated{false};
std::atomic<bool> g_exitFinished{false};
constexpr int kExitPollSliceMs = 50;

void flushAllStreams()
{
    std::fflush(nullptr);
}

void hardExitThreadEntry(void* arg)
{
    (void)arg;
    for (int elapsed = 0; elapsed < kExitDeadlineMs; elapsed += kExitPollSliceMs) {
        if (g_exitFinished.load()) {
            return;
        }
        svcSleepThread(static_cast<s64>(kExitPollSliceMs) * 1000000LL);
    }
    if (g_exitFinished.load()) {
        return;
    }
    // 走到这里说明正常退出流程没在 3 秒内结束（线程 join / 网络 / deko3d 卡住）。
    // 直接用内核 syscall 结束进程：Horizon 看到进程退出就认为「软件已关闭」，不再弹关闭失败。
    // 绝不能从后台线程调 std::_Exit(0)（会跳到 __nx_exit 切回主线程栈指针），
    // 也绝不能用 std::thread::detach()（libnx 下 pthread_detach 恒返回 ENOSYS 必触 std::terminate）。
    logWarn("[EXIT] graceful shutdown exceeded " + std::to_string(kExitDeadlineMs) +
            "ms, forcing svcExitProcess");
    levelup::net::silenceRtcLoggingForExit();
    flushAllStreams();
    appletUnlockExit();
    svcExitProcess();
}

void armHardExitDeadline()
{
    if (g_deadlineArmed.exchange(true)) {
        return;
    }
    if (g_exitFinished.load()) {
        return;
    }
    Result rc = threadCreate(&g_deadlineThread,
                             hardExitThreadEntry,
                             nullptr,
                             nullptr,
                             0x10000,
                             0x3B,
                             -2);
    if (R_FAILED(rc)) {
        logWarn("[EXIT] hard-exit watchdog threadCreate failed rc=" + std::to_string(rc));
        return;
    }
    rc = threadStart(&g_deadlineThread);
    if (R_FAILED(rc)) {
        logWarn("[EXIT] hard-exit watchdog threadStart failed rc=" + std::to_string(rc));
        threadClose(&g_deadlineThread);
        return;
    }
    g_deadlineThreadCreated.store(true);
}

void onExitAppletHook(AppletHookType hook, void* arg)
{
    (void)arg;
    if (hook != AppletHookType_OnExitRequest) {
        return;
    }
    if (detail::g_exiting.exchange(true)) {
        return;
    }
    logInfo("[EXIT] OnExitRequest received: graceful shutdown started");
    // 退出前必须撤回 media playback，否则系统一直把我们当"正在播放媒体"。
    appletSetMediaPlaybackState(false);
    // 停掉空闲震动看门狗并把手柄震动清零（退出时不该震）。
    input::stopIdleVibrationGuard();
    input::zeroAllVibrationDevices("exit-request");
    armHardExitDeadline();
}
#endif

} // namespace

void installExitGuard()
{
#if defined(__SWITCH__)
    if (g_hookInstalled.exchange(true)) {
        return;
    }
    appletHook(&g_exitCookie, onExitAppletHook, nullptr);
    logInfo("[EXIT] exit guard installed (hard-exit deadline " +
            std::to_string(kExitDeadlineMs) + "ms)");
#endif
}

void finishExitGuard()
{
#if defined(__SWITCH__)
    g_exitFinished.store(true);
    if (g_hookInstalled.exchange(false)) {
        appletUnhook(&g_exitCookie);
    }
    if (g_deadlineThreadCreated.exchange(false)) {
        threadWaitForExit(&g_deadlineThread);
        threadClose(&g_deadlineThread);
    }
#endif
}

void setBackgroundSuspendAllowed(bool allow)
{
#if defined(__SWITCH__)
    static int s_lastMode = -1;
    const int want = allow ? 1 : 0;
    if (s_lastMode == want) {
        return;
    }
    s_lastMode = want;
    appletSetFocusHandlingMode(allow ? AppletFocusHandlingMode_SuspendHomeSleep
                                     : AppletFocusHandlingMode_NoSuspend);
    logInfo(std::string("[POWER] focus handling mode = ") +
            (allow ? "SuspendHomeSleep (lobby)" : "NoSuspend (stream)"));
#else
    (void)allow;
#endif
}

void installBackgroundFrameLimiter()
{
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;
    // 默认处于大厅，允许系统在切到 HOME 时直接挂起进程以零功耗待机。
    setBackgroundSuspendAllowed(true);
    brls::Application::getWindowFocusChangedEvent()->subscribe([](bool focused) {
        // 前台保持 vsync（0 = 不额外限帧），后台（NoSuspend 串流期间）压到 2 fps。
        brls::Application::setLimitedFPS(focused ? 0 : 2);
        logInfo(std::string("[EXIT] window focus ") + (focused ? "gained" : "lost") +
                (focused ? ", frame limit = vsync" : ", frame limit = 2fps"));
    });
}

} // namespace levelup
