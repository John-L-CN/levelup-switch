#pragma once

#include <atomic>

namespace levelup {

namespace detail {
// C++17 inline 变量：所有 TU 共享同一份实例，头文件内联使用，避免宿主单测（只编部分 .cpp）
// 缺符号。
inline std::atomic<bool> g_exiting{false};
inline std::atomic<int> g_criticalRequests{0};
} // namespace detail

/// 是否已经收到系统的退出请求（Horizon 的「关闭软件」）。
/// 退出路径上（析构、join、curl 中断）用它判断要不要走快速路径。
inline bool isExiting() {
    return detail::g_exiting.load();
}

/// 是否有"退出时也必须跑完"的关键请求在进行（例如云端 stopGame 释放实例）。
inline bool inCriticalRequest() {
    return detail::g_criticalRequests.load() > 0;
}

class CriticalRequestScope {
  public:
    CriticalRequestScope() {
        detail::g_criticalRequests.fetch_add(1);
    }
    ~CriticalRequestScope() {
        detail::g_criticalRequests.fetch_sub(1);
    }
    CriticalRequestScope(const CriticalRequestScope&) = delete;
    CriticalRequestScope& operator=(const CriticalRequestScope&) = delete;
};

/// 注册 applet hook：收到 AppletHookType_OnExitRequest 时
///   1) 记日志（定位卡点用）
///   2) appletSetMediaPlaybackState(false)（撤回"播放中"声明）
///   3) 停掉空闲震动看门狗 + 震动归零
///   4) 使用 libnx 原生 Thread 启动 3 秒硬退出兜底（不调用在 Switch 上恒返回 ENOSYS 的 std::thread::detach）
/// 兜底存在的必要性：串流状态下系统发来的关闭指令如果被卡住的 join / 网络 / deko3d
/// present 拖住，Horizon 会直接报「软件无法关闭」。兜底线程到点 svcExitProcess，
/// 进程一退出系统就认为关闭成功。
void installExitGuard();

/// 正常主循环返回、优雅退出已完成时调用：取消 applet hook，并通知硬退出兜底线程
/// 提前返回、回收其线程资源。
void finishExitGuard();

/// 失焦（后台）时把帧率压到 2 fps。这样在串流（NoSuspend）模式下 applet 消息（含退出请求）
/// 仍能被及时处理，同时不会把 deko3d 的三缓冲全部写满后把主循环卡死在 presentImage 上。
void installBackgroundFrameLimiter();

/// 控制 Horizon OS 在切到 HOME 桌面时是否允许直接挂起（冻结）本进程：
///   - true（大厅）：AppletFocusHandlingMode_SuspendHomeSleep，后台零功耗零发热
///   - false（串流）：AppletFocusHandlingMode_NoSuspend，保持 WebRTC/心跳不断连
void setBackgroundSuspendAllowed(bool allow);

} // namespace levelup
