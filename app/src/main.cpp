#ifdef __SWITCH2__
#include "switch2_build_id.hpp"
#endif
//
//  main.cpp
//  Moonlight
//
//  Created by XITRIX on 26.05.2021.
//

// Switch include only necessary for demo videos recording
#ifdef __SWITCH__
#include <pthread.h>
#include <switch.h>
#include <sys/stat.h>
#endif

#ifdef __PSV__
extern "C" {
// PVR_PSP2 allocates its EGL/GLES state from the Sony libc heap. Match the
// memory model used by working Borealis Vita applications such as wiliwili.
unsigned int _newlib_heap_size_user      = 220 * 1024 * 1024;
unsigned int _pthread_stack_default_user = 2 * 1024 * 1024;
unsigned int sceLibcHeapSize             = 24 * 1024 * 1024;
}
#endif

#include <cstdio>
#include <cstdlib>
#include <exception>

#include <borealis.hpp>
#include <string>

#include "add_host_tab.hpp"
#include "host_tab.hpp"
#include "link_cell.hpp"
#include "main_activity.hpp"
#include "main_tabs_view.hpp"
#include "settings_tab.hpp"
#include "views/boolean_slider_cell.hpp"

#include "DiscoverManager.hpp"
#include "MoonlightSession.hpp"
#include "SwitchMoonlightSessionDecoderAndRenderProvider.hpp"

#ifdef LEVELUP_ENABLE_WEBRTC
#include <curl/curl.h>
#include "forwarder_maker.hpp"
#include "levelup/exit_guard.hpp"
#include "levelup/rtc_session.hpp"
#include "levelup/rumble_switch.hpp"
#include "levelup/ui/lobby_activity.hpp"
#include "levelup/ui/lobby_theme.hpp"
#include "levelup/webrtc_probe.hpp"
#endif


#if defined(_WIN32) && defined(__SDL2__)
#include <SDL.h>
#define SDL_MAIN
#endif

#if defined(__SDL3__)
#include <SDL3/SDL_main.h>
#elif defined(__SDL2__)
#include <SDL_main.h>
#endif
#include <main_args.hpp>

using namespace brls::literals; // for _i18n

#ifdef __SWITCH__
namespace {

s32 selectAllowedSwitchCore(u64 affinityMask, int ordinal) {
    s32 lastAllowedCore = -1;

    for (s32 core = 0; core < 64; core++)
    {
        if ((affinityMask & (1ULL << core)) == 0) {
            continue;
        }

        lastAllowedCore = core;
        if (ordinal == 0) {
            return core;
        }

        ordinal--;
    }

    return lastAllowedCore;
}

void preferSwitchCore(int ordinal) {
    s32 preferredCore = -1;
    u64 affinityMask = 0;
    if (R_FAILED(svcGetThreadCoreMask(&preferredCore, &affinityMask, CUR_THREAD_HANDLE))) {
        return;
    }

    s32 targetCore = selectAllowedSwitchCore(affinityMask, ordinal);
    if (targetCore >= 0 && targetCore != preferredCore) {
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, targetCore, static_cast<u32>(affinityMask));
    }
}

} // namespace
#endif

#ifdef LEVELUP_ENABLE_WEBRTC
namespace {

FILE* g_levelupLogFile = nullptr;

void closeLevelupLogFile() {
    if (g_levelupLogFile) {
        brls::Logger::setLogOutput(nullptr);
        std::fflush(g_levelupLogFile);
        std::fclose(g_levelupLogFile);
        g_levelupLogFile = nullptr;
    }
}

[[noreturn]] void levelupTerminateHandler() {
    FILE* out = g_levelupLogFile ? g_levelupLogFile : stderr;
    std::fprintf(out, "[FATAL] std::terminate invoked (tid=%llu)\n",
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pthread_self())));
    if (auto eptr = std::current_exception()) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            std::fprintf(out, "[FATAL] uncaught exception: %s\n", e.what());
        } catch (...) {
            std::fprintf(out, "[FATAL] uncaught non-std exception\n");
        }
    }
    std::fflush(out);
#ifdef __SWITCH__
    if (levelup::isExiting()) {
        levelup::net::silenceRtcLoggingForExit();
        appletUnlockExit();
        svcExitProcess();
    }
#endif
    std::abort();
}

} // namespace
#endif

int main(int argc, char* argv[]) {
#ifdef __SWITCH2__
    // Keep diagnostics available after a failed launch or streaming session.
    mkdir("sdmc:/switch/Moonlight-Switch", 0777);
    if (FILE* log = fopen("sdmc:/switch/Moonlight-Switch/startup.log", "w"))
    {
        setvbuf(log, nullptr, _IONBF, 0);
        brls::Logger::setLogOutput(log);
        dup2(fileno(log), fileno(stderr));
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    brls::Logger::info("SWITCH2_BUILD: id={} renderer=native-vulkan", MOONLIGHT_SWITCH2_BUILD_ID);
    brls::Logger::info(
        "Switch 2 startup: libounce, native Vulkan UI/video over OunceVK; H.264/AV1 hardware available; HEVC software");
#endif
    // Enable recording for Twitter memes
#ifdef __SWITCH__
#ifndef LEVELUP_ENABLE_WEBRTC
    appletInitializeGamePlayRecording();
#endif
    appletSetWirelessPriorityMode(AppletWirelessPriorityMode_OptimizedForWlan);

    // Keep the UI loop away from the hottest streaming worker core.
    preferSwitchCore(0);

#ifndef LEVELUP_ENABLE_WEBRTC
    // Keep the main thread above others so that the program stays responsive
    // when doing software decoding
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);
#endif

    // auto at = appletGetAppletType();
    // g_application_mode = at == AppletType_Application || at == AppletType_SystemApplication;

    // // To get access to /dev/nvhost-nvjpg, we need nvdrv:{a,s,t}
    // // However, nvdrv:{a,s} have limited address space for gpu mappings
    // extern u32 __nx_nv_service_type, __nx_nv_transfermem_size;
    // __nx_nv_service_type     = NvServiceType_Factory;
    // __nx_nv_transfermem_size = (g_application_mode ? 16 : 3) * 0x100000;
#endif

    // Set log level
    // Use INFO for normal operation to avoid flooding SD card with per-button debug traces
    brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);
#ifdef LEVELUP_ENABLE_WEBRTC
    brls::Logger::setThreadSafeLogging(true);
#endif

#if defined(__SWITCH__) && defined(LEVELUP_ENABLE_WEBRTC)
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/levelup-nx", 0777);
    if (FILE* log = fopen("sdmc:/switch/levelup-nx/levelup.log", "w")) {
        setvbuf(log, nullptr, _IONBF, 0);
        g_levelupLogFile = log;
        brls::Logger::setLogOutput(log);
    }
#endif

#ifdef LEVELUP_ENABLE_WEBRTC
    std::set_terminate(levelupTerminateHandler);
#ifdef __SWITCH__
    brls::Logger::info("[BOOT] 1. main start: hos=0x{:x} appletType={}",
                       static_cast<unsigned>(hosversionGet()),
                       static_cast<int>(appletGetAppletType()));
#else
    brls::Logger::info("[BOOT] 1. main start");
#endif
    const CURLcode curlInitRc = curl_global_init(CURL_GLOBAL_ALL);
    brls::Logger::info("[BOOT] 2. curl_global_init rc={}", static_cast<int>(curlInitRc));

    try {
#endif

    // Init the app and i18n
    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init Borealis application");
#ifdef LEVELUP_ENABLE_WEBRTC
        curl_global_cleanup();
#endif
        return EXIT_FAILURE;
    }

#ifdef LEVELUP_ENABLE_WEBRTC
    brls::Logger::info("[BOOT] 3. brls::Application::init ok");
    levelup::logWebRtcBuildProbe();
    brls::Logger::info("[BOOT] 4. webrtc probe ok");
#endif

    registerDeepLinkHandler();

#if defined(PLATFORM_VISIONOS)
    brls::Application::setMaximumUIScale(1.0f);
#endif

    MoonlightSession::set_provider(
            new SwitchMoonlightSessionDecoderAndRenderProvider());

#ifdef LEVELUP_ENABLE_WEBRTC
    levelup::ui::installLevelUpBorealisThemes();
    brls::Application::createWindow("LevelUp-NX");
    levelup::ui::syncSystemDarkTheme();
    brls::Logger::info("[BOOT] 5. createWindow ok");

    // 后台关不掉的兜底：退出请求 → 3 秒内必结束进程；失焦时降帧，避免主循环卡在 present。
    levelup::installExitGuard();
    levelup::installBackgroundFrameLimiter();
    // 空闲震动看门狗：大厅里把震动维持在 0（borealis clearVibration 在 style 变化时误发满幅）。
    levelup::input::startIdleVibrationGuard();
#else
    brls::Application::createWindow("title"_i18n);
#endif

    auto home = Application::getPlatform()->getHomeDirectory("Moonlight-Switch");
    Settings::instance().set_working_dir(home);
    Settings::instance().set_launch_path(argc > 0 ? argv[0] : "");
    brls::Logger::info("Working dir, {}", home);
#if defined(__SWITCH__) && defined(LEVELUP_ENABLE_WEBRTC)
    syncLevelUpLauncherOnStartup(Settings::instance().launch_path());
    brls::Logger::info("[BOOT] version={} launchPath={}",
                       APP_VERSION,
                       Settings::instance().launch_path());
#endif

    // Have the application register an action on every activity that will quit
    // when you press BUTTON_START
    brls::Application::setGlobalQuit(false);
    brls::Application::setFPSStatus(false);

    // Register custom views (including tabs, which are views)
    brls::Application::registerXMLView("BooleanSliderCell", BooleanSliderCell::create);
    brls::Application::registerXMLView("LinkCell", LinkCell::create);

    brls::Application::registerXMLView("MainTabs", MainTabs::create);
    brls::Application::registerXMLView("HostTab", HostTab::create);
    brls::Application::registerXMLView("AddHostTab", AddHostTab::create);
    brls::Application::registerXMLView("SettingsTab", SettingsTab::create);

    // Add custom values to the theme
    brls::Theme::getLightTheme().addColor("captioned_image/caption",
                                   nvgRGB(2, 176, 183));
    brls::Theme::getDarkTheme().addColor("captioned_image/caption",
                                  nvgRGB(51, 186, 227));

    // Add custom values to the style
    brls::getStyle().addMetric("about/padding_top_bottom", 50);
    brls::getStyle().addMetric("about/padding_sides", 75);
    brls::getStyle().addMetric("about/description_margin", 50);

    // Create and push the main activity to the stack if cannot run game from arguments
#ifdef LEVELUP_ENABLE_WEBRTC
    brls::Application::pushActivity(new levelup::ui::LevelUpLobbyActivity());
    brls::Logger::info("[BOOT] 6. pushActivity ok");
    brls::Application::enableDebuggingView(false);
    brls::Application::setSwapInputKeys(false);
#else
    if (!startFromArgs(argc, argv)) {
        brls::Application::pushActivity(new MainActivity());
    }

    brls::Application::enableDebuggingView(Settings::instance().write_log());
    brls::Application::setSwapInputKeys(Settings::instance().swap_ui_keys());
#endif

    // Run the app. The Vita development loop waits for this marker so a
    // successful launch means at least one complete Borealis frame rendered.
#ifdef __PSV__
    bool vitaHealthReported = false;
#endif
    while (brls::Application::mainLoop()) {
#ifdef LEVELUP_ENABLE_WEBRTC
        static bool firstFrameLogged = false;
        if (!firstFrameLogged) {
            firstFrameLogged = true;
            brls::Logger::info("[BOOT] 7. first mainLoop frame completed");
        }
#endif
#ifdef __SWITCH2__
        static bool switch2Ready = false;
        if (!switch2Ready)
        {
            brls::Logger::info("SWITCH2_HEALTH: first UI frame completed");
            switch2Ready = true;
        }
#endif
#ifdef __PSV__
        if (!vitaHealthReported) {
            brls::Logger::info("VITA_HEALTH: READY");
            vitaHealthReported = true;
        }
#endif
    }

#ifdef LEVELUP_ENABLE_WEBRTC
    brls::Logger::info("[EXIT] main loop returned, tearing down");
    levelup::net::silenceRtcLoggingForExit();
    levelup::input::stopIdleVibrationGuard();
    levelup::finishExitGuard();
#endif

#ifdef LEVELUP_ENABLE_WEBRTC
    } catch (const std::exception& e) {
        brls::Logger::error("[FATAL] unhandled exception in main: {}", e.what());
        if (g_levelupLogFile) {
            std::fprintf(g_levelupLogFile, "[FATAL] unhandled exception in main: %s\n", e.what());
            std::fflush(g_levelupLogFile);
        }
        levelup::net::silenceRtcLoggingForExit();
        levelup::input::stopIdleVibrationGuard();
        levelup::finishExitGuard();
        closeLevelupLogFile();
        std::fflush(nullptr);
#ifdef __SWITCH__
        if (levelup::isExiting() || levelup::net::hasActiveRtcRuntime()) {
            appletUnlockExit();
            svcExitProcess();
        }
#endif
        curl_global_cleanup();
        return EXIT_FAILURE;
    } catch (...) {
        brls::Logger::error("[FATAL] unhandled non-std exception in main");
        if (g_levelupLogFile) {
            std::fprintf(g_levelupLogFile, "[FATAL] unhandled non-std exception in main\n");
            std::fflush(g_levelupLogFile);
        }
        levelup::net::silenceRtcLoggingForExit();
        levelup::input::stopIdleVibrationGuard();
        levelup::finishExitGuard();
        closeLevelupLogFile();
        std::fflush(nullptr);
#ifdef __SWITCH__
        if (levelup::isExiting() || levelup::net::hasActiveRtcRuntime()) {
            appletUnlockExit();
            svcExitProcess();
        }
#endif
        curl_global_cleanup();
        return EXIT_FAILURE;
    }
    closeLevelupLogFile();
    std::fflush(nullptr);
#ifdef __SWITCH__
    // 收到系统退出请求（OnExitRequest）或本次运行启用过 libdatachannel（后台 PollService/
    // ThreadPool/juice/usrsctp 线程常驻）时，绝不让 main() 返回到 __call_exitprocs /
    // userAppExit(socketExit) / hbloader 卸载 NRO 代码页，否则后台线程访问已卸载的 bsd:u 或
    // 代码段必触发「由于发生错误，软件已关闭」。此时所有云端实例、日志与配置均已落盘收尾，
    // 直接解锁退出并由内核原子结束整个进程。
    if (levelup::isExiting() || levelup::net::hasActiveRtcRuntime()) {
        appletUnlockExit();
        svcExitProcess();
    }
#endif
    curl_global_cleanup();
#endif

    // Exit
#if defined(PLATFORM_TVOS)
    exit(0);
#endif
    
    return EXIT_SUCCESS;
}
