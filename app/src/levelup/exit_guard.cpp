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
#include "levelup/rumble_switch.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

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

void flushAllStreams()
{
    std::fflush(nullptr);
}

void armHardExitDeadline()
{
    if (g_deadlineArmed.exchange(true)) {
        return;
    }
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(kExitDeadlineMs));
        // 走到这里说明正常退出流程没在 3 秒内结束（线程 join / 网络 / deko3d 卡住）。
        // 直接结束进程：Horizon 看到进程退出就认为「软件已关闭」，不再弹关闭失败。
        logWarn("[EXIT] graceful shutdown exceeded " + std::to_string(kExitDeadlineMs) +
                "ms, forcing process exit");
        flushAllStreams();
        std::_Exit(0);
    }).detach();
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

void installBackgroundFrameLimiter()
{
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;
    brls::Application::getWindowFocusChangedEvent()->subscribe([](bool focused) {
        // 前台保持 vsync（0 = 不额外限帧），后台压到 5 fps。
        brls::Application::setLimitedFPS(focused ? 0 : 5);
        logInfo(std::string("[EXIT] window focus ") + (focused ? "gained" : "lost") +
                (focused ? ", frame limit = vsync" : ", frame limit = 5fps"));
    });
}

} // namespace levelup
