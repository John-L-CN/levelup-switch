#include "levelup/ui/levelup_stream_view.hpp"

#include "levelup/exit_guard.hpp"
#include "levelup/log.hpp"
#include "levelup/rumble_switch.hpp"
#include "levelup/ui/lobby_theme.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <utility>

#ifdef __SWITCH__
#include <switch.h>
#endif

#if defined(__SWITCH__) && defined(BOREALIS_USE_DEKO3D)
#include "AVFrameHolder.hpp"
#include "DKVideoRenderer.hpp"
#endif

namespace levelup::ui {

namespace {

class TranslucentOverlayActivity : public brls::Activity {
  public:
    explicit TranslucentOverlayActivity(brls::View* view) : brls::Activity(view) {}

    bool isTranslucent() override {
        return true;
    }

    float getShowAnimationDuration(brls::TransitionAnimation) override {
        return 0.0f;
    }
};

std::string formatCompactHud(const SessionStatsSnapshot& snap,
                             int w,
                             int h,
                             double fps,
                             double bitrateKbps,
                             uint64_t recentLost,
                             uint64_t recentRecv) {
    char bwBuf[32];
    const double safeKbps = std::max(0.0, bitrateKbps);
    if (safeKbps < 1000.0) {
        std::snprintf(bwBuf, sizeof(bwBuf), "%.0fKbps", safeKbps);
    } else {
        std::snprintf(bwBuf, sizeof(bwBuf), "%.1fMbps", safeKbps / 1000.0);
    }

    char netBuf[32];
    if (snap.rtc.rttMs >= 0) {
        std::snprintf(netBuf, sizeof(netBuf), "网络 %lldms", static_cast<long long>(snap.rtc.rttMs));
    } else {
        std::snprintf(netBuf, sizeof(netBuf), "网络 --");
    }

    char decBuf[32];
    if (snap.video.decodedFrames > 0) {
        std::snprintf(decBuf,
                      sizeof(decBuf),
                      "解码 %.1fms",
                      static_cast<double>(snap.video.decodeTimeUs) / 1000.0);
    } else {
        std::snprintf(decBuf, sizeof(decBuf), "解码 --");
    }

    char lossBuf[48];
    if (recentLost == 0) {
        std::snprintf(lossBuf, sizeof(lossBuf), "丢包 0");
    } else {
        const double total = static_cast<double>(recentRecv + recentLost);
        const double pct =
            total > 0.0 ? (100.0 * static_cast<double>(recentLost) / total) : 100.0;
        std::snprintf(lossBuf,
                      sizeof(lossBuf),
                      "丢包 %llu(%.1f%%)",
                      static_cast<unsigned long long>(recentLost),
                      pct);
    }

    char buf[256];
    std::snprintf(buf,
                  sizeof(buf),
                  "%dx%d | %.1ffps | %s | %s | %s | %s",
                  w,
                  h,
                  std::max(0.0, fps),
                  bwBuf,
                  netBuf,
                  decBuf,
                  lossBuf);
    return buf;
}

input::TouchMouseMode toInputTouchMouseMode(int cfgMode) {
    switch (normalizeTouchMouseMode(cfgMode)) {
    case AppConfig::kTouchMouseTrackpad:
        return input::TouchMouseMode::Trackpad;
    case AppConfig::kTouchMouseLocked:
        return input::TouchMouseMode::Locked;
    default:
        return input::TouchMouseMode::Click;
    }
}

const char* touchMouseModeShortLabel(int cfgMode) {
    switch (normalizeTouchMouseMode(cfgMode)) {
    case AppConfig::kTouchMouseTrackpad:
        return "触控板模式";
    case AppConfig::kTouchMouseLocked:
        return "鼠标锁定";
    default:
        return "点击模式";
    }
}

const char* touchMouseModeToastText(int cfgMode) {
    switch (normalizeTouchMouseMode(cfgMode)) {
    case AppConfig::kTouchMouseTrackpad:
        return "触控板模式 · 滑动移动 · 轻点左键 · 双指右键/滚轮";
    case AppConfig::kTouchMouseLocked:
        return "鼠标锁定 · 触屏不传递到游戏，仅响应悬浮按钮";
    default:
        return "点击模式 · 触屏直接点击";
    }
}

} // namespace

LevelUpStreamView::LevelUpStreamView(GameEntry game,
                                     AppConfig cfg,
                                     Store store,
                                     ConfigChangedCallback onConfigChanged)
    : m_game(std::move(game)),
      m_cfg(std::move(cfg)),
      m_store(std::move(store)),
      m_onConfigChanged(std::move(onConfigChanged)) {
    setFocusable(true);
    setHideHighlight(true);
    // Keep background transparent: Borealis flushes View::drawBackground() via NanoVG at
    // nvgEndFrame() AFTER DKVideoRenderer::draw() flushes deko3d, which would cover live video.

    brls::Application::getPlatform()->disableScreenDimming(true);
    levelup::setBackgroundSuspendAllowed(false);
#ifdef __SWITCH__
    appletSetMediaPlaybackState(true);
#endif

    auto [tw, th] = detectTargetResolution();
    m_targetWidth = tw;
    m_targetHeight = th;

    m_windowFocusSub = brls::Application::getWindowFocusChangedEvent()->subscribe(
        [this](bool focused) { onWindowFocusChanged(focused); });

    m_runLoopSub = brls::Application::getRunLoopEvent()->subscribe([this]() {
        if (m_windowFocused || m_exiting || m_popRequested) {
            return;
        }
        if (m_backgroundLostTime.time_since_epoch().count() == 0) {
            return;
        }
        constexpr auto kBackgroundKeepaliveTimeout = std::chrono::minutes(5);
        const auto elapsed = std::chrono::steady_clock::now() - m_backgroundLostTime;
        if (elapsed < kBackgroundKeepaliveTimeout) {
            return;
        }
        m_backgroundLostTime = {};
        logInfo("[StreamView] background keepalive timeout (5m): disconnecting back to lobby");
        if (m_overlayOpen || m_errorDialogOpen) {
            m_overlayOpen = false;
            m_errorDialogOpen = false;
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
        }
        exitStream();
    });

    registerAction(
        "取消连接",
        brls::BUTTON_B,
        [this](brls::View*) {
            if (m_exiting || m_overlayOpen || m_errorDialogOpen) {
                return false;
            }
            if (m_uiPhase != SessionPhase::Streaming) {
                exitStream();
                return true;
            }
            return false;
        },
        true);

    std::shared_ptr<std::atomic<bool>> alive = m_alive;
    std::shared_ptr<std::atomic<bool>> stopping = m_stopping;
#if defined(__SWITCH__) && defined(BOREALIS_USE_DEKO3D)
    m_orchestrator.setBeforeVideoCodecCloseCallback([this, alive, stopping]() {
        if (!alive->load() || stopping->load()) {
            return;
        }
        struct SyncState {
            std::mutex mu;
            std::condition_variable cv;
            bool done = false;
        };
        auto st = std::make_shared<SyncState>();
        brls::sync([this, alive, stopping, st]() {
            if (alive->load() && !stopping->load()) {
                if (m_renderer) {
                    m_renderer->invalidateHardwareResources();
                }
                AVFrameHolder::instance().cleanup();
            }
            {
                std::lock_guard lk(st->mu);
                st->done = true;
            }
            st->cv.notify_all();
        });
        std::unique_lock lk(st->mu);
        for (int i = 0; i < 30; ++i) {
            if (st->done || !alive->load() || stopping->load()) {
                break;
            }
            st->cv.wait_for(lk, std::chrono::milliseconds(10));
        }
    });
#endif
    m_orchestrator.setPhaseCallback([this, alive](SessionPhase phase, const std::string& text) {
        brls::sync([this, alive, phase, text]() {
            if (!alive->load() || m_exiting) {
                return;
            }
            m_uiPhase = phase;
            m_uiStageText = text;
            if (phase == SessionPhase::Streaming) {
                if (!m_overlayOpen && m_windowFocused && isFocused()) {
                    setBorealisInputsBlocked(true);
                }
            } else if (phase == SessionPhase::Failed && !m_errorDialogOpen) {
                const auto snap = m_orchestrator.stats();
                showFailureDialog(text, snap.errorText);
            }
        });
    });

    m_orchestrator.setOverlayComboCallback([this, alive]() {
        brls::sync([this, alive]() {
            if (!alive->load() || m_exiting || m_overlayOpen || m_errorDialogOpen) {
                return;
            }
            openPauseOverlay();
        });
    });

    // union 选址学到的 LP uuid / pkgName 落盘：大厅预热测速（/v1/speed/dialpoints）需要它们
    m_orchestrator.setLearnedUnionCallback([this, alive](const std::string& lpUuid, int64_t pkg) {
        brls::sync([this, alive, lpUuid, pkg]() {
            if (!alive->load()) {
                return;
            }
            if (m_cfg.lpUuid == lpUuid && m_cfg.lastPkgName == pkg) {
                return;
            }
            m_cfg.lpUuid = lpUuid;
            m_cfg.lastPkgName = pkg;
            m_store.saveConfig(m_cfg);
            if (m_onConfigChanged) {
                m_onConfigChanged(m_cfg);
            }
            logInfo("[StreamView] learned union ids lpUuid=" + lpUuid +
                    " pkg=" + std::to_string(pkg));
        });
    });

    startSession();
}

LevelUpStreamView::~LevelUpStreamView() {
    m_alive->store(false);
    m_exiting = true;
    m_stopping->store(true);

    brls::Application::getWindowFocusChangedEvent()->unsubscribe(m_windowFocusSub);
    brls::Application::getRunLoopEvent()->unsubscribe(m_runLoopSub);
    setBorealisInputsBlocked(false);

    try {
        m_orchestrator.stop();
    } catch (const std::exception& e) {
        logWarn(std::string("[StreamView] ~LevelUpStreamView: stop threw: ") + e.what());
    } catch (...) {
        logWarn("[StreamView] ~LevelUpStreamView: stop threw (unknown)");
    }

#if defined(__SWITCH__) && defined(BOREALIS_USE_DEKO3D)
    if (m_renderer) {
        m_renderer->invalidateHardwareResources();
        m_renderer.reset();
    }
    AVFrameHolder::instance().cleanup();
#endif

    brls::Application::getPlatform()->disableScreenDimming(false);
    // 退出串流后把震动彻底归零：云端震动包可能在收尾阶段还在路上，
    // 且 borealis 的 clearVibration() 在 style 变化时会把满幅写进手柄。
    input::zeroAllVibrationDevices("stream-exit");
#ifdef __SWITCH__
    appletSetMediaPlaybackState(false);
#endif
    levelup::setBackgroundSuspendAllowed(true);
}

std::pair<int, int> LevelUpStreamView::detectTargetResolution() const {
    bool docked = false;
#ifdef __SWITCH__
    docked = (appletGetOperationMode() == AppletOperationMode_Console);
#endif
    return resolveTargetResolution(docked, m_cfg.handheldResolution);
}

void LevelUpStreamView::startSession() {
    m_orchestrator.stop();

#if defined(__SWITCH__) && defined(BOREALIS_USE_DEKO3D)
    if (m_renderer) {
        m_renderer->invalidateHardwareResources();
        m_renderer.reset();
    }
    AVFrameHolder::instance().cleanup();
#endif

    auto [tw, th] = detectTargetResolution();
    m_targetWidth = tw;
    m_targetHeight = th;
#ifdef __SWITCH__
    const bool isDocked = (appletGetOperationMode() == AppletOperationMode_Console);
#else
    const bool isDocked = false;
#endif
    logInfo("[StreamView] target resolution " + std::to_string(m_targetWidth) + "x" +
            std::to_string(m_targetHeight) + " (mode=" + (isDocked ? "docked" : "handheld") +
            ", handheldSetting=" +
            std::to_string(normalizeHandheldResolution(m_cfg.handheldResolution)) + "p)");

    m_uiPhase = SessionPhase::Idle;
    m_uiStageText = "正在启动云游戏连接...";
    m_errorDialogOpen = false;
    m_lastHudUpdate = {};
    m_prevDecodedFrames = 0;
    m_prevRtpBytesRecv = 0;
    m_fpsEstimate = 0.0;
    m_bitrateKbps = 0.0;
    m_lossWindow.fill({});
    m_lossWindowIdx = 0;
    m_prevRtpPacketsLost = 0;
    m_prevRtpPacketsRecv = 0;
    m_recentPacketsLost = 0;
    m_recentPacketsRecv = 0;
    m_hudLine.clear();
    setBorealisInputsBlocked(false);

    SessionLaunchConfig lcfg;
    lcfg.deviceId = m_cfg.deviceId;
    lcfg.cred = m_cfg.cred;
    lcfg.gameId = m_game.gameId;
    lcfg.gameName = m_game.name;
    lcfg.width = m_targetWidth;
    lcfg.height = m_targetHeight;
    lcfg.maxFps = 60;
    lcfg.videoLevel = resolveEffectiveVideoLevel(m_targetHeight, m_cfg.videoLevelOverride);
    lcfg.vqFirst = m_cfg.vqFirst;
    lcfg.rumbleEnabled = m_cfg.rumbleEnabled;
    lcfg.rumbleStrengthPercent = m_cfg.rumbleStrength;
    lcfg.invertStickY = m_cfg.invertStickY;
    lcfg.swapAbXy = m_cfg.swapAbXy;
    lcfg.audioVolumePercent = m_cfg.audioVolume;
    lcfg.touchMouseMode = toInputTouchMouseMode(m_cfg.touchMouseMode);

    m_touchControlsVisible = false;
    m_justShownOnTouch = false;
    m_swkbdOpen = false;
    m_pressedOverlayBtn = 0;
    m_pressedFingerId = -1;
    m_modeToastText.clear();
    m_modeToastTime = {};
    m_orchestrator.setOverlayButtonRects(false, {}, {});

    m_orchestrator.start(lcfg);
}

void LevelUpStreamView::setBorealisInputsBlocked(bool block) {
    if (block == m_inputsBlocked) {
        return;
    }
    m_inputsBlocked = block;
    if (block) {
        brls::Application::blockInputs(true);
    } else {
        brls::Application::unblockInputs();
    }
}

void LevelUpStreamView::onFocusGained() {
    Box::onFocusGained();
    if (!m_exiting && !m_overlayOpen && !m_errorDialogOpen && m_windowFocused &&
        m_uiPhase == SessionPhase::Streaming) {
        setBorealisInputsBlocked(true);
    }
}

void LevelUpStreamView::onFocusLost() {
    Box::onFocusLost();
    setBorealisInputsBlocked(false);
}

void LevelUpStreamView::onWindowFocusChanged(bool focused) {
    if (m_windowFocused == focused) {
        return;
    }
    m_windowFocused = focused;
    logInfo(std::string("[StreamView] window focus ") + (focused ? "gained" : "lost"));

    if (!focused) {
        m_backgroundLostTime = std::chrono::steady_clock::now();
        m_orchestrator.setPausedForOverlay(true);
        m_orchestrator.setBackgroundPowerSave(true);
#ifdef __SWITCH__
        appletSetMediaPlaybackState(false);
#endif
    } else {
        m_backgroundLostTime = {};
        m_invalidateRendererPending = true;
#ifdef __SWITCH__
        appletSetMediaPlaybackState(true);
#endif
        m_orchestrator.setBackgroundPowerSave(false);
        if (!m_overlayOpen && !m_errorDialogOpen && !m_exiting) {
            m_orchestrator.setPausedForOverlay(false);
        }
    }
}

void LevelUpStreamView::updateHudCache(const SessionStatsSnapshot& snap) {
    const auto now = std::chrono::steady_clock::now();
    if (m_lastHudUpdate != std::chrono::steady_clock::time_point{} &&
        now - m_lastHudUpdate < std::chrono::milliseconds(500)) {
        return;
    }

    if (m_lastHudUpdate != std::chrono::steady_clock::time_point{}) {
        const double dt = std::chrono::duration<double>(now - m_lastHudUpdate).count();
        if (dt > 0.05) {
            if (snap.video.decodedFrames >= m_prevDecodedFrames) {
                m_fpsEstimate =
                    static_cast<double>(snap.video.decodedFrames - m_prevDecodedFrames) / dt;
            }
            if (snap.rtc.rtpBytesRecv >= m_prevRtpBytesRecv) {
                const double instKbps =
                    static_cast<double>(snap.rtc.rtpBytesRecv - m_prevRtpBytesRecv) * 8.0 /
                    (dt * 1000.0);
                m_bitrateKbps = (m_bitrateKbps <= 0.0)
                                    ? instKbps
                                    : (m_bitrateKbps * 0.4 + instKbps * 0.6);
            }
        }
        const uint64_t dLost = (snap.rtc.rtpPacketsLost >= m_prevRtpPacketsLost)
                                   ? (snap.rtc.rtpPacketsLost - m_prevRtpPacketsLost)
                                   : 0;
        const uint64_t dRecv = (snap.rtc.rtpPacketsRecv >= m_prevRtpPacketsRecv)
                                   ? (snap.rtc.rtpPacketsRecv - m_prevRtpPacketsRecv)
                                   : 0;
        m_lossWindow[m_lossWindowIdx] = LossBucket{dLost, dRecv};
        m_lossWindowIdx = (m_lossWindowIdx + 1) % kLossWindowBuckets;

        uint64_t sumLost = 0;
        uint64_t sumRecv = 0;
        for (const auto& b : m_lossWindow) {
            sumLost += b.lost;
            sumRecv += b.recv;
        }
        m_recentPacketsLost = sumLost;
        m_recentPacketsRecv = sumRecv;
    }
    m_prevDecodedFrames = snap.video.decodedFrames;
    m_prevRtpBytesRecv = snap.rtc.rtpBytesRecv;
    m_prevRtpPacketsLost = snap.rtc.rtpPacketsLost;
    m_prevRtpPacketsRecv = snap.rtc.rtpPacketsRecv;
    m_lastHudUpdate = now;

    const int w = snap.video.width > 0 ? snap.video.width : m_targetWidth;
    const int h = snap.video.height > 0 ? snap.video.height : m_targetHeight;
    m_hudLine = formatCompactHud(
        snap, w, h, m_fpsEstimate, m_bitrateKbps, m_recentPacketsLost, m_recentPacketsRecv);
}

void LevelUpStreamView::draw(NVGcontext* vg,
                             float x,
                             float y,
                             float width,
                             float height,
                             brls::Style style,
                             brls::FrameContext* ctx) {
    if (!m_windowFocused || m_exiting) {
        return;
    }

#if defined(__SWITCH__) && defined(BOREALIS_USE_DEKO3D)
    if (m_invalidateRendererPending) {
        m_invalidateRendererPending = false;
        if (m_renderer) {
            m_renderer->invalidateHardwareResources();
        }
    }
    if (!m_renderer) {
        m_renderer = std::make_unique<DKVideoRenderer>();
    }

    // NOTE (docked fullscreen fix): draw() 收到的 width/height 是 Borealis 的**内容单位**，
    // 不是像素。底座模式下 Application::windowScale = 1.5，内容区仍是 1280x720，而真正的
    // framebuffer 是 1920x1080。DKVideoRenderer 会把这两个数字当成 deko3d 视口尺寸（EASU /
    // RCAS / dithering 的后处理 present pass 以及离屏 target 分配全部用它），于是底座下视频
    // 只画进 1920x1080 画面的左上角 1280x720 区域 —— 也就是「大厅全屏、游戏不全屏」。
    // 掌机下 windowScale=1.0，内容单位 == 像素，所以只有底座暴露这个问题。
    const float uiScale = brls::Application::windowScale > 0.0f ? brls::Application::windowScale : 1.0f;
    const int surfaceW = std::max(1, static_cast<int>(std::lround(width * uiScale)));
    const int surfaceH = std::max(1, static_cast<int>(std::lround(height * uiScale)));
    if (surfaceW != m_videoSurfaceW || surfaceH != m_videoSurfaceH) {
        m_videoSurfaceW = surfaceW;
        m_videoSurfaceH = surfaceH;
        logInfo("[StreamView] video surface " + std::to_string(surfaceW) + "x" +
                std::to_string(surfaceH) + " px (content " +
                std::to_string(static_cast<int>(width)) + "x" +
                std::to_string(static_cast<int>(height)) + ", uiScale " +
                std::to_string(uiScale) + ")");
    }

    const int surfaceWidth = m_videoSurfaceW;
    const int surfaceHeight = m_videoSurfaceH;
    AVFrameHolder::instance().get([this, vg, surfaceWidth, surfaceHeight](AVFrame* frame) {
        if (m_renderer) {
            m_renderer->draw(vg, surfaceWidth, surfaceHeight, frame, 0);
        }
    });
#endif

    const auto snap = m_orchestrator.stats();
    if (m_uiPhase == SessionPhase::Streaming && snap.firstFrameDecoded) {
        static auto s_lastDraw = std::chrono::steady_clock::time_point{};
        static auto s_lastWarn = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (s_lastDraw.time_since_epoch().count() != 0) {
            const auto gapMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastDraw).count();
            if (gapMs >= 45 && gapMs < 5000 &&
                (s_lastWarn.time_since_epoch().count() == 0 ||
                 now - s_lastWarn >= std::chrono::seconds(2))) {
                s_lastWarn = now;
                logWarn("[Perf] UI draw stall " + std::to_string(gapMs) + "ms");
            }
        }
        s_lastDraw = now;
    }
    const int font = brls::Application::getFont(brls::FONT_REGULAR);

    if (!snap.firstFrameDecoded || m_uiPhase != SessionPhase::Streaming) {
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillColor(vg, nvgRGBA(12, 16, 24, 232));
        nvgFill(vg);

        if (font != brls::FONT_INVALID) {
            nvgFontFaceId(vg, font);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

            nvgFontSize(vg, 34.0f);
            nvgFillColor(vg, nvgRGB(248, 250, 252));
            nvgText(vg, x + width * 0.5f, y + height * 0.40f, m_game.name.c_str(), nullptr);

            nvgFontSize(vg, 22.0f);
            nvgFillColor(vg, nvgRGB(56, 189, 248));
            nvgText(vg, x + width * 0.5f, y + height * 0.50f, m_uiStageText.c_str(), nullptr);

            char infoBuf[128];
            std::snprintf(infoBuf,
                          sizeof(infoBuf),
                          "目标分辨率 %dx%d@60fps  |  按 B 取消",
                          m_targetWidth,
                          m_targetHeight);
            nvgFontSize(vg, 18.0f);
            nvgFillColor(vg, nvgRGB(148, 163, 184));
            nvgText(vg, x + width * 0.5f, y + height * 0.58f, infoBuf, nullptr);

            nvgText(vg,
                    x + width * 0.5f,
                    y + height * 0.66f,
                    "提示：游戏中长按 - + + (或同时按 L+R+-++) 可呼出串流菜单",
                    nullptr);
        }
    } else if (m_cfg.showStatsHud) {
        updateHudCache(snap);
        if (font != brls::FONT_INVALID && !m_hudLine.empty()) {
            nvgFontFaceId(vg, font);
            nvgFontSize(vg, 15.5f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            float bounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            nvgTextBounds(vg, 0.0f, 0.0f, m_hudLine.c_str(), nullptr, bounds);
            const float textW = std::max(0.0f, bounds[2] - bounds[0]);
            const float maxBoxW = std::max(180.0f, width - 196.0f);
            const float boxX = x + 16.0f;
            const float boxY = y + 14.0f;
            const float boxW = std::clamp(textW + 24.0f, 180.0f, maxBoxW);
            const float boxH = 34.0f;

            nvgBeginPath(vg);
            nvgRoundedRect(vg, boxX, boxY, boxW, boxH, 8.0f);
            nvgFillColor(vg, nvgRGBA(10, 14, 22, 185));
            nvgFill(vg);

            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, nvgRGB(226, 232, 240));
            nvgText(vg, boxX + 12.0f, boxY + boxH * 0.5f, m_hudLine.c_str(), nullptr);
        }
    }

    if (snap.firstFrameDecoded && m_uiPhase == SessionPhase::Streaming) {
        updateTouchOverlayControls(x, y, width, height, m_cfg.showStatsHud);
        if (m_touchControlsVisible) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastTouchActivity).count();
            float alphaFactor = 1.0f;
            if (elapsedMs > 2750) {
                alphaFactor = std::clamp(1.0f - static_cast<float>(elapsedMs - 2750) / 250.0f, 0.0f, 1.0f);
            }
            drawTouchOverlayControls(vg, alphaFactor);
        }
        if (!m_overlayOpen && !m_errorDialogOpen && !m_swkbdOpen) {
            drawModeToast(vg, x, y, width, height);
        }
    } else {
        if (m_touchControlsVisible) {
            m_touchControlsVisible = false;
            m_orchestrator.setOverlayButtonRects(false, {}, {});
        }
    }

    Box::draw(vg, x, y, width, height, style, ctx);
}

void LevelUpStreamView::updateTouchOverlayControls(
    float x, float y, float width, float height, bool hudVisible) {
    (void)height;
    if (m_overlayOpen || m_errorDialogOpen || m_exiting || m_swkbdOpen) {
        if (m_touchControlsVisible) {
            m_touchControlsVisible = false;
            m_pressedOverlayBtn = 0;
            m_pressedFingerId = -1;
            m_orchestrator.setOverlayButtonRects(false, {}, {});
        }
        return;
    }

    m_kbBtnRect = input::TouchHitRect{
        x + 16.0f,
        y + (hudVisible ? 56.0f : 16.0f),
        136.0f,
        44.0f,
    };
    m_mouseBtnRect = input::TouchHitRect{
        x + width - 16.0f - 156.0f,
        y + 16.0f,
        156.0f,
        44.0f,
    };

    const auto now = std::chrono::steady_clock::now();
    const auto& touches = brls::Application::currentTouchState;

    bool anyTouchDown = false;
    bool triggerKeyboard = false;
    bool triggerMouseToggle = false;

    for (const auto& t : touches) {
        if (t.phase == brls::TouchPhase::START || t.phase == brls::TouchPhase::STAY) {
            anyTouchDown = true;
            m_lastTouchActivity = now;
        }
        if (t.phase == brls::TouchPhase::START) {
            if (!m_touchControlsVisible) {
                m_touchControlsVisible = true;
                m_justShownOnTouch = true;
                m_pressedOverlayBtn = 0;
                m_pressedFingerId = -1;
            } else if (!m_justShownOnTouch && m_pressedOverlayBtn == 0) {
                if (m_kbBtnRect.contains(t.position.x, t.position.y)) {
                    m_pressedOverlayBtn = 1;
                    m_pressedFingerId = t.fingerId;
                } else if (m_mouseBtnRect.contains(t.position.x, t.position.y)) {
                    m_pressedOverlayBtn = 2;
                    m_pressedFingerId = t.fingerId;
                }
            }
        } else if (t.phase == brls::TouchPhase::END) {
            m_lastTouchActivity = now;
            if (m_pressedOverlayBtn != 0 && t.fingerId == m_pressedFingerId) {
                constexpr float kSlop = 12.0f;
                auto inWithSlop = [kSlop](const input::TouchHitRect& r, float px, float py) {
                    return px >= (r.x - kSlop) && px <= (r.x + r.w + kSlop) &&
                           py >= (r.y - kSlop) && py <= (r.y + r.h + kSlop);
                };
                if (m_pressedOverlayBtn == 1 && inWithSlop(m_kbBtnRect, t.position.x, t.position.y)) {
                    triggerKeyboard = true;
                } else if (m_pressedOverlayBtn == 2 &&
                           inWithSlop(m_mouseBtnRect, t.position.x, t.position.y)) {
                    triggerMouseToggle = true;
                }
                m_pressedOverlayBtn = 0;
                m_pressedFingerId = -1;
            }
        } else if (t.phase == brls::TouchPhase::NONE) {
            if (m_pressedOverlayBtn != 0 && t.fingerId == m_pressedFingerId) {
                m_pressedOverlayBtn = 0;
                m_pressedFingerId = -1;
            }
        }
    }

    if (!anyTouchDown) {
        m_justShownOnTouch = false;
        if (m_touchControlsVisible &&
            (now - m_lastTouchActivity) >= std::chrono::seconds(3)) {
            m_touchControlsVisible = false;
            m_pressedOverlayBtn = 0;
            m_pressedFingerId = -1;
        }
    }

    m_orchestrator.setOverlayButtonRects(m_touchControlsVisible, m_kbBtnRect, m_mouseBtnRect);

    if (triggerKeyboard) {
        std::shared_ptr<std::atomic<bool>> alive = m_alive;
        brls::sync([this, alive]() {
            if (alive->load() && !m_exiting) {
                openVirtualKeyboard();
            }
        });
    } else if (triggerMouseToggle) {
        toggleTouchMouseMode();
    }
}

void LevelUpStreamView::drawTouchOverlayControls(NVGcontext* vg, float alphaFactor) {
    if (alphaFactor <= 0.01f) {
        return;
    }
    const auto scaleAlpha = [alphaFactor](int a) -> unsigned char {
        return static_cast<unsigned char>(std::clamp(static_cast<int>(a * alphaFactor), 0, 255));
    };

    const int font = brls::Application::getFont(brls::FONT_REGULAR);
    const int mouseMode = normalizeTouchMouseMode(m_cfg.touchMouseMode);
    const bool trackpadMode = (mouseMode == AppConfig::kTouchMouseTrackpad);
    const bool lockedMode = (mouseMode == AppConfig::kTouchMouseLocked);

    // 1. 左上角：虚拟键盘半透明按钮
    {
        const auto& r = m_kbBtnRect;
        const bool pressed = (m_pressedOverlayBtn == 1);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, r.x, r.y, r.w, r.h, 12.0f);
        nvgFillColor(vg, nvgRGBA(15, 23, 42, scaleAlpha(pressed ? 215 : 160)));
        nvgFill(vg);
        nvgStrokeWidth(vg, 1.5f);
        nvgStrokeColor(vg, nvgRGBA(148, 163, 184, scaleAlpha(pressed ? 200 : 120)));
        nvgStroke(vg);

        // 矢量键盘图标
        const float ix = r.x + 14.0f;
        const float iy = r.y + 12.0f;
        const float iw = 26.0f;
        const float ih = 20.0f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, ix, iy, iw, ih, 3.0f);
        nvgStrokeWidth(vg, 1.6f);
        nvgStrokeColor(vg, nvgRGBA(226, 232, 240, scaleAlpha(235)));
        nvgStroke(vg);

        nvgFillColor(vg, nvgRGBA(226, 232, 240, scaleAlpha(235)));
        for (int col = 0; col < 4; ++col) {
            nvgBeginPath(vg);
            nvgRect(vg, ix + 4.0f + col * 5.0f, iy + 4.5f, 3.2f, 3.2f);
            nvgRect(vg, ix + 4.0f + col * 5.0f, iy + 9.5f, 3.2f, 3.2f);
            nvgFill(vg);
        }
        nvgBeginPath(vg);
        nvgRect(vg, ix + 6.5f, iy + 14.5f, 13.0f, 2.5f);
        nvgFill(vg);

        if (font != brls::FONT_INVALID) {
            nvgFontFaceId(vg, font);
            nvgFontSize(vg, 16.0f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, nvgRGBA(248, 250, 252, scaleAlpha(240)));
            nvgText(vg, r.x + 48.0f, r.y + r.h * 0.5f, "虚拟键盘", nullptr);
        }
    }

    // 2. 右上角：鼠标模式切换半透明按钮（点击模式 / 触控板模式 / 鼠标锁定）
    {
        const auto& r = m_mouseBtnRect;
        const bool pressed = (m_pressedOverlayBtn == 2);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, r.x, r.y, r.w, r.h, 12.0f);
        nvgFillColor(vg, nvgRGBA(15, 23, 42, scaleAlpha(pressed ? 215 : 160)));
        nvgFill(vg);
        nvgStrokeWidth(vg, 1.5f);
        NVGcolor borderCol = nvgRGBA(148, 163, 184, scaleAlpha(pressed ? 200 : 120));
        if (lockedMode) {
            borderCol = nvgRGBA(245, 158, 11, scaleAlpha(pressed ? 235 : 175));
        } else if (trackpadMode) {
            borderCol = nvgRGBA(56, 189, 248, scaleAlpha(pressed ? 230 : 165));
        }
        nvgStrokeColor(vg, borderCol);
        nvgStroke(vg);

        const float ix = r.x + 14.0f;
        const float iy = r.y + 11.0f;
        nvgStrokeWidth(vg, 1.6f);
        if (lockedMode) {
            // 挂锁图标：U 形锁梁 + 圆角锁体 + 锁孔点
            const NVGcolor lockCol = nvgRGBA(251, 191, 36, scaleAlpha(240));
            nvgBeginPath(vg);
            nvgMoveTo(vg, ix + 7.0f, iy + 11.0f);
            nvgLineTo(vg, ix + 7.0f, iy + 7.5f);
            nvgArc(vg, ix + 12.0f, iy + 7.5f, 5.0f, 3.14159265f, 0.0f, NVG_CW);
            nvgLineTo(vg, ix + 17.0f, iy + 11.0f);
            nvgStrokeColor(vg, lockCol);
            nvgStroke(vg);

            nvgBeginPath(vg);
            nvgRoundedRect(vg, ix + 5.0f, iy + 11.0f, 14.0f, 11.0f, 2.5f);
            nvgStrokeColor(vg, lockCol);
            nvgStroke(vg);

            nvgBeginPath(vg);
            nvgCircle(vg, ix + 12.0f, iy + 16.5f, 1.8f);
            nvgFillColor(vg, lockCol);
            nvgFill(vg);
        } else if (trackpadMode) {
            // 触控板图标：矩形板 + 底部左右按键分隔线
            nvgBeginPath(vg);
            nvgRoundedRect(vg, ix, iy, 24.0f, 22.0f, 3.5f);
            nvgMoveTo(vg, ix, iy + 15.0f);
            nvgLineTo(vg, ix + 24.0f, iy + 15.0f);
            nvgMoveTo(vg, ix + 12.0f, iy + 15.0f);
            nvgLineTo(vg, ix + 12.0f, iy + 22.0f);
            nvgStrokeColor(vg, nvgRGBA(125, 211, 252, scaleAlpha(240)));
            nvgStroke(vg);
        } else {
            // 点击模式图标：准心圆环 + 中心点
            const float cx = ix + 12.0f;
            const float cy = iy + 11.0f;
            nvgBeginPath(vg);
            nvgCircle(vg, cx, cy, 8.5f);
            nvgStrokeColor(vg, nvgRGBA(226, 232, 240, scaleAlpha(235)));
            nvgStroke(vg);

            nvgBeginPath(vg);
            nvgCircle(vg, cx, cy, 3.0f);
            nvgFillColor(vg, nvgRGBA(226, 232, 240, scaleAlpha(235)));
            nvgFill(vg);
        }

        if (font != brls::FONT_INVALID) {
            nvgFontFaceId(vg, font);
            nvgFontSize(vg, 16.0f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            NVGcolor textCol = nvgRGBA(248, 250, 252, scaleAlpha(240));
            if (lockedMode) {
                textCol = nvgRGBA(253, 224, 71, scaleAlpha(245));
            } else if (trackpadMode) {
                textCol = nvgRGBA(125, 211, 252, scaleAlpha(245));
            }
            nvgFillColor(vg, textCol);
            nvgText(vg,
                    r.x + 46.0f,
                    r.y + r.h * 0.5f,
                    touchMouseModeShortLabel(mouseMode),
                    nullptr);
        }
    }
}

void LevelUpStreamView::openVirtualKeyboard() {
    if (m_swkbdOpen || m_exiting || m_overlayOpen || m_errorDialogOpen) {
        return;
    }
    auto* ime = brls::Application::getPlatform()->getImeManager();
    if (!ime) {
        return;
    }

    m_swkbdOpen = true;
    m_touchControlsVisible = false;
    m_orchestrator.setOverlayButtonRects(false, {}, {});
    m_orchestrator.setPausedForOverlay(true);

    std::shared_ptr<std::atomic<bool>> alive = m_alive;
    ime->openForText(
        [this, alive](std::string text) {
            if (!alive->load() || m_exiting) {
                return;
            }
            if (!text.empty()) {
                const bool ok = m_orchestrator.sendTextInput(text);
                logInfo("[StreamView] virtual keyboard sent text bytes=" +
                        std::to_string(text.size()) + " ok=" + std::string(ok ? "1" : "0"));
                if (!ok) {
                    brls::Application::notify("发送文本失败：控制通道未就绪");
                }
            }
        },
        "虚拟键盘输入",
        "输入文字将直接发送到云端游戏",
        128,
        "");

    m_swkbdOpen = false;
    if (!alive->load() || m_exiting) {
        return;
    }
    if (!m_overlayOpen && !m_errorDialogOpen && m_windowFocused) {
        m_orchestrator.setPausedForOverlay(false);
    }
    m_lastTouchActivity = std::chrono::steady_clock::now();
    m_touchControlsVisible = true;
    m_orchestrator.setOverlayButtonRects(true, m_kbBtnRect, m_mouseBtnRect);
}

void LevelUpStreamView::drawModeToast(
    NVGcontext* vg, float x, float y, float width, float height) {
    if (m_modeToastText.empty() || m_modeToastTime == std::chrono::steady_clock::time_point{}) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_modeToastTime).count();
    if (elapsedMs >= 1900) {
        m_modeToastText.clear();
        m_modeToastTime = {};
        return;
    }

    const int font = brls::Application::getFont(brls::FONT_REGULAR);
    if (font == brls::FONT_INVALID) {
        return;
    }

    float alphaFactor = 1.0f;
    if (elapsedMs > 1600) {
        alphaFactor = std::clamp(1.0f - static_cast<float>(elapsedMs - 1600) / 300.0f, 0.0f, 1.0f);
    }
    if (alphaFactor <= 0.01f) {
        return;
    }

    const auto scaleAlpha = [alphaFactor](int a) -> unsigned char {
        return static_cast<unsigned char>(std::clamp(static_cast<int>(a * alphaFactor), 0, 255));
    };

    nvgFontFaceId(vg, font);
    nvgFontSize(vg, 16.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    float bounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    nvgTextBounds(vg, 0.0f, 0.0f, m_modeToastText.c_str(), nullptr, bounds);
    const float textW = std::max(0.0f, bounds[2] - bounds[0]);
    const float maxPillW = std::max(180.0f, width - 48.0f);
    const float pillW = std::clamp(textW + 40.0f, 180.0f, maxPillW);
    const float pillH = 38.0f;
    const float pillX = x + (width - pillW) * 0.5f;
    const float pillY = y + height - 58.0f;

    const int mouseMode = normalizeTouchMouseMode(m_cfg.touchMouseMode);
    NVGcolor borderCol = nvgRGBA(148, 163, 184, scaleAlpha(160));
    NVGcolor textCol = nvgRGBA(248, 250, 252, scaleAlpha(245));
    if (mouseMode == AppConfig::kTouchMouseLocked) {
        borderCol = nvgRGBA(245, 158, 11, scaleAlpha(190));
        textCol = nvgRGBA(253, 224, 71, scaleAlpha(245));
    } else if (mouseMode == AppConfig::kTouchMouseTrackpad) {
        borderCol = nvgRGBA(56, 189, 248, scaleAlpha(185));
        textCol = nvgRGBA(125, 211, 252, scaleAlpha(245));
    }

    nvgBeginPath(vg);
    nvgRoundedRect(vg, pillX, pillY, pillW, pillH, 19.0f);
    nvgFillColor(vg, nvgRGBA(15, 23, 42, scaleAlpha(210)));
    nvgFill(vg);
    nvgStrokeWidth(vg, 1.5f);
    nvgStrokeColor(vg, borderCol);
    nvgStroke(vg);

    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, textCol);
    nvgText(vg, pillX + pillW * 0.5f, pillY + pillH * 0.5f, m_modeToastText.c_str(), nullptr);
}

void LevelUpStreamView::applyTouchMouseMode(int nextMode, bool showToast) {
    const int normalized = normalizeTouchMouseMode(nextMode);
    m_cfg.touchMouseMode = normalized;
    m_store.saveConfig(m_cfg);
    if (m_onConfigChanged) {
        m_onConfigChanged(m_cfg);
    }
    m_orchestrator.setTouchMouseMode(toInputTouchMouseMode(normalized));

    const char* logName = "click";
    if (normalized == AppConfig::kTouchMouseTrackpad) {
        logName = "trackpad";
    } else if (normalized == AppConfig::kTouchMouseLocked) {
        logName = "locked";
    }
    logInfo(std::string("[StreamView] touch mouse mode -> ") + logName);

    if (showToast) {
        m_modeToastText = touchMouseModeToastText(normalized);
        m_modeToastTime = std::chrono::steady_clock::now();
    }
}

void LevelUpStreamView::toggleTouchMouseMode() {
    const int cur = normalizeTouchMouseMode(m_cfg.touchMouseMode);
    const int nextMode = (cur + 1) % 3;
    applyTouchMouseMode(nextMode, true);
    m_lastTouchActivity = std::chrono::steady_clock::now();
    m_touchControlsVisible = true;
}

void LevelUpStreamView::openPauseOverlay() {
    if (m_overlayOpen || m_exiting || m_errorDialogOpen) {
        return;
    }

    m_overlayOpen = true;
    setBorealisInputsBlocked(false);
    m_orchestrator.setPausedForOverlay(true);

    const bool isDark = syncSystemDarkTheme();
    const LobbyPalette& pal = currentLobbyPalette(isDark);

    auto* backdrop = new brls::Box(brls::Axis::COLUMN);
    backdrop->setJustifyContent(brls::JustifyContent::CENTER);
    backdrop->setAlignItems(brls::AlignItems::CENTER);
    backdrop->setBackgroundColor(pal.modalBackdrop);

    auto* panel = new StyledSurfaceBox(brls::Axis::COLUMN);
    panel->setWidth(560.0f);
    panel->setPadding(24.0f, 26.0f, 22.0f, 26.0f);
    panel->setSurfaceRadius(20.0f);
    panel->setSurfaceColors(pal.modalBg, pal.modalBorder);
    panel->setSurfaceBorderWidth(1.0f);
    panel->setSurfaceShadow(true, pal.modalShadowInner, nvgRGBA(0, 0, 0, 0), 10.0f, 28.0f, 2.0f);
    panel->setAlignItems(brls::AlignItems::STRETCH);

    auto* titleRow = new brls::Box(brls::Axis::ROW);
    titleRow->setAlignItems(brls::AlignItems::CENTER);
    titleRow->setJustifyContent(brls::JustifyContent::CENTER);
    titleRow->setMarginBottom(10.0f);

    auto* menuIcon = new VectorIconView(VectorIconKind::Menu, 19.0f, pal.accent);
    menuIcon->setMarginRight(9.0f);
    titleRow->addView(menuIcon);

    auto* title = new brls::Label();
    title->setText(m_game.name + " · 串流菜单");
    title->setFontSize(22.0f);
    title->setSingleLine(true);
    title->setTextColor(pal.inkPrimary);
    titleRow->addView(title);
    panel->addView(titleRow);

    const auto snap = m_orchestrator.stats();
    const int subW = snap.video.width > 0 ? snap.video.width : m_targetWidth;
    const int subH = snap.video.height > 0 ? snap.video.height : m_targetHeight;

    auto* statsCard = new StyledSurfaceBox(brls::Axis::ROW);
    statsCard->setPadding(8.0f, 12.0f, 8.0f, 12.0f);
    statsCard->setMarginBottom(14.0f);
    statsCard->setJustifyContent(brls::JustifyContent::CENTER);
    statsCard->setAlignItems(brls::AlignItems::CENTER);
    statsCard->setSurfaceRadius(11.0f);
    statsCard->setSurfaceColors(pal.modalSurface, pal.hairline);
    statsCard->setSurfaceBorderWidth(1.0f);

    auto* subtitle = new brls::Label();
    subtitle->setText(formatCompactHud(
        snap, subW, subH, m_fpsEstimate, m_bitrateKbps, m_recentPacketsLost, m_recentPacketsRecv));
    subtitle->setFontSize(13.5f);
    subtitle->setSingleLine(true);
    subtitle->setTextColor(pal.inkSecondary);
    subtitle->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    statsCard->addView(subtitle);
    panel->addView(statsCard);

    LevelUpStreamView* self = this;
    std::shared_ptr<std::atomic<bool>> alive = m_alive;

    auto makeButton = [](const std::string& label, const brls::ButtonStyle* style) {
        auto* btn = new brls::Button();
        btn->setStyle(style);
        btn->setText(label);
        btn->setHeight(44.0f);
        btn->setMargins(4.0f, 0.0f, 4.0f, 0.0f);
        return btn;
    };

    auto* resumeBtn = makeButton("继续游戏", &brls::BUTTONSTYLE_PRIMARY);
    resumeBtn->registerClickAction([self, alive](brls::View*) {
        brls::sync([self, alive]() {
            if (alive->load()) {
                self->dismissPauseOverlay(false, false);
            }
        });
        return true;
    });
    panel->addView(resumeBtn);

    auto* hudBtn = makeButton(
        std::string("性能 HUD：") + (m_cfg.showStatsHud ? "开" : "关"),
        &brls::BUTTONSTYLE_DEFAULT);
    hudBtn->registerClickAction([self, alive, hudBtn](brls::View*) {
        if (!alive->load()) {
            return true;
        }
        self->m_cfg.showStatsHud = !self->m_cfg.showStatsHud;
        self->m_store.saveConfig(self->m_cfg);
        if (self->m_onConfigChanged) {
            self->m_onConfigChanged(self->m_cfg);
        }
        hudBtn->setText(std::string("性能 HUD：") + (self->m_cfg.showStatsHud ? "开" : "关"));
        return true;
    });
    panel->addView(hudBtn);

    auto* idrBtn = makeButton("强制刷新关键帧 (PLI+FIR)", &brls::BUTTONSTYLE_DEFAULT);
    idrBtn->registerClickAction([self, alive](brls::View*) {
        brls::sync([self, alive]() {
            if (alive->load()) {
                self->dismissPauseOverlay(true, false);
            }
        });
        return true;
    });
    panel->addView(idrBtn);

    auto* rumbleBtn = makeButton(
        std::string("HD 手柄震动：") + (m_cfg.rumbleEnabled ? "开" : "关"),
        &brls::BUTTONSTYLE_DEFAULT);
    rumbleBtn->registerClickAction([self, alive, rumbleBtn](brls::View*) {
        if (!alive->load()) {
            return true;
        }
        self->m_cfg.rumbleEnabled = !self->m_cfg.rumbleEnabled;
        self->m_orchestrator.setRumbleEnabled(self->m_cfg.rumbleEnabled);
        self->m_store.saveConfig(self->m_cfg);
        if (self->m_onConfigChanged) {
            self->m_onConfigChanged(self->m_cfg);
        }
        rumbleBtn->setText(std::string("HD 手柄震动：") + (self->m_cfg.rumbleEnabled ? "开" : "关"));
        return true;
    });
    panel->addView(rumbleBtn);

    auto* layoutBtn = makeButton(
        std::string("按键布局：") + (m_cfg.swapAbXy ? "Xbox (AB/XY互换)" : "Switch (原生)"),
        &brls::BUTTONSTYLE_DEFAULT);
    layoutBtn->registerClickAction([self, alive, layoutBtn](brls::View*) {
        if (!alive->load()) {
            return true;
        }
        self->m_cfg.swapAbXy = !self->m_cfg.swapAbXy;
        self->m_orchestrator.setSwapAbXy(self->m_cfg.swapAbXy);
        self->m_store.saveConfig(self->m_cfg);
        if (self->m_onConfigChanged) {
            self->m_onConfigChanged(self->m_cfg);
        }
        layoutBtn->setText(std::string("按键布局：") + (self->m_cfg.swapAbXy ? "Xbox (AB/XY互换)" : "Switch (原生)"));
        return true;
    });
    panel->addView(layoutBtn);

    auto* mouseModeBtn = makeButton(
        std::string("触屏鼠标：") + touchMouseModeShortLabel(m_cfg.touchMouseMode),
        &brls::BUTTONSTYLE_DEFAULT);
    mouseModeBtn->registerClickAction([self, alive, mouseModeBtn](brls::View*) {
        if (!alive->load()) {
            return true;
        }
        const int nextMode = (normalizeTouchMouseMode(self->m_cfg.touchMouseMode) + 1) % 3;
        self->applyTouchMouseMode(nextMode, false);
        mouseModeBtn->setText(std::string("触屏鼠标：") + touchMouseModeShortLabel(nextMode));
        return true;
    });
    panel->addView(mouseModeBtn);

    auto* exitBtn = makeButton("退出云游戏（安全释放实例）", &brls::BUTTONSTYLE_DEFAULT);
    exitBtn->setTextColor(pal.dangerText);
    exitBtn->registerClickAction([self, alive](brls::View*) {
        brls::sync([self, alive]() {
            if (alive->load()) {
                self->dismissPauseOverlay(false, true);
            }
        });
        return true;
    });
    panel->addView(exitBtn);

    backdrop->addView(panel);
    backdrop->registerAction("返回游戏", brls::BUTTON_B, [self, alive](brls::View*) {
        brls::sync([self, alive]() {
            if (alive->load()) {
                self->dismissPauseOverlay(false, false);
            }
        });
        return true;
    });

    brls::Application::pushActivity(new TranslucentOverlayActivity(backdrop),
                                    brls::TransitionAnimation::NONE);
    brls::Application::giveFocus(resumeBtn);
}

void LevelUpStreamView::dismissPauseOverlay(bool requestKeyFrame, bool exitAfter) {
    if (!m_overlayOpen) {
        return;
    }
    m_overlayOpen = false;

    if (requestKeyFrame && !m_exiting) {
        m_orchestrator.requestKeyFrame();
        brls::Application::notify("已请求云端发送恢复关键帧");
    }

    if (exitAfter) {
        logInfo("[StreamView] dismissPauseOverlay: exitAfter=true");
        m_exiting = true;
        m_stopping->store(true);
        setBorealisInputsBlocked(false);
        std::shared_ptr<std::atomic<bool>> alive = m_alive;
        LevelUpStreamView* self = this;
        brls::Application::popActivity(brls::TransitionAnimation::NONE, [self, alive]() {
            brls::sync([self, alive]() {
                if (alive->load()) {
                    self->exitStream();
                }
            });
        });
        return;
    }

    brls::Application::popActivity(brls::TransitionAnimation::NONE);
    if (!m_exiting) {
        m_orchestrator.setPausedForOverlay(false);
        if (m_uiPhase == SessionPhase::Streaming && m_windowFocused) {
            setBorealisInputsBlocked(true);
        }
    }
}

void LevelUpStreamView::showFailureDialog(const std::string& stageText, const std::string& errText) {
    if (m_errorDialogOpen || m_exiting) {
        return;
    }
    m_errorDialogOpen = true;
    setBorealisInputsBlocked(false);
    m_orchestrator.setPausedForOverlay(true);

    std::string msg = stageText;
    if (!errText.empty()) {
        msg += "\n" + errText;
    }

    const bool isDark = syncSystemDarkTheme();
    const LobbyPalette& pal = currentLobbyPalette(isDark);

    auto* backdrop = new brls::Box(brls::Axis::COLUMN);
    backdrop->setJustifyContent(brls::JustifyContent::CENTER);
    backdrop->setAlignItems(brls::AlignItems::CENTER);
    backdrop->setBackgroundColor(pal.modalBackdrop);

    auto* panel = new StyledSurfaceBox(brls::Axis::COLUMN);
    panel->setWidth(560.0f);
    panel->setPadding(26.0f, 28.0f, 24.0f, 28.0f);
    panel->setSurfaceRadius(20.0f);
    panel->setSurfaceColors(pal.modalBg, pal.modalBorder);
    panel->setSurfaceBorderWidth(1.0f);
    panel->setSurfaceShadow(true, pal.modalShadowInner, nvgRGBA(0, 0, 0, 0), 10.0f, 28.0f, 2.0f);
    panel->setAlignItems(brls::AlignItems::STRETCH);

    auto* title = new brls::Label();
    title->setText("云游戏连接异常");
    title->setFontSize(22.0f);
    title->setTextColor(pal.dangerText);
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    title->setMarginBottom(12.0f);
    panel->addView(title);

    auto* msgLabel = new brls::Label();
    msgLabel->setText(msg);
    msgLabel->setFontSize(16.0f);
    msgLabel->setSingleLine(false);
    msgLabel->setTextColor(pal.inkSecondary);
    msgLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    msgLabel->setMarginBottom(20.0f);
    panel->addView(msgLabel);

    LevelUpStreamView* self = this;
    std::shared_ptr<std::atomic<bool>> alive = m_alive;
    auto closed = std::make_shared<std::atomic<bool>>(false);

    auto closeFailureOverlay = [closed](std::function<void()> after) {
        if (!closed->exchange(true)) {
            brls::Application::popActivity(brls::TransitionAnimation::NONE, [fn = std::move(after)]() {
                brls::sync([fn2 = std::move(fn)]() {
                    if (fn2) fn2();
                });
            });
        }
    };

    auto* retryBtn = new brls::Button();
    retryBtn->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    retryBtn->setText("重试");
    retryBtn->setHeight(48.0f);
    retryBtn->setMarginBottom(8.0f);
    retryBtn->registerClickAction([self, alive, closeFailureOverlay](brls::View*) {
        closeFailureOverlay([self, alive]() {
            if (!alive->load() || self->m_exiting) {
                return;
            }
            self->m_errorDialogOpen = false;
            self->startSession();
        });
        return true;
    });
    panel->addView(retryBtn);

    auto* backBtn = new brls::Button();
    backBtn->setStyle(&brls::BUTTONSTYLE_DEFAULT);
    backBtn->setText("返回大厅");
    backBtn->setHeight(48.0f);
    backBtn->registerClickAction([self, alive, closeFailureOverlay](brls::View*) {
        self->m_exiting = true;
        self->m_stopping->store(true);
        closeFailureOverlay([self, alive]() {
            if (!alive->load()) {
                return;
            }
            self->m_errorDialogOpen = false;
            self->exitStream();
        });
        return true;
    });
    panel->addView(backBtn);

    backdrop->addView(panel);
    backdrop->registerAction("返回大厅", brls::BUTTON_B, [self, alive, closeFailureOverlay](brls::View*) {
        self->m_exiting = true;
        self->m_stopping->store(true);
        closeFailureOverlay([self, alive]() {
            if (!alive->load()) {
                return;
            }
            self->m_errorDialogOpen = false;
            self->exitStream();
        });
        return true;
    });

    brls::Application::pushActivity(new TranslucentOverlayActivity(backdrop),
                                    brls::TransitionAnimation::NONE);
    brls::Application::giveFocus(retryBtn);
}

void LevelUpStreamView::exitStream() {
    if (m_popRequested) {
        return;
    }
    logInfo("[StreamView] exitStream: stopping orchestrator");
    m_popRequested = true;
    m_exiting = true;
    m_stopping->store(true);
    setBorealisInputsBlocked(false);

    try {
        m_orchestrator.stop();
    } catch (const std::exception& e) {
        logWarn(std::string("[StreamView] exitStream: stop threw: ") + e.what());
    } catch (...) {
        logWarn("[StreamView] exitStream: stop threw (unknown)");
    }
    logInfo("[StreamView] exitStream: orchestrator stopped, releasing renderer");

#if defined(__SWITCH__) && defined(BOREALIS_USE_DEKO3D)
    if (m_renderer) {
        m_renderer->invalidateHardwareResources();
        m_renderer.reset();
    }
    AVFrameHolder::instance().cleanup();
#endif

    logInfo("[StreamView] exitStream: popping StreamActivity");
    levelup::setBackgroundSuspendAllowed(true);
    brls::Application::popActivity(brls::TransitionAnimation::NONE);
}

} // namespace levelup::ui
