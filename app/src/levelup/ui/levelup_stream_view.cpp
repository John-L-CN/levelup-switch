#include "levelup/ui/levelup_stream_view.hpp"

#include "levelup/log.hpp"
#include "levelup/rumble_switch.hpp"

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

std::string formatStatsLine1(const SessionStatsSnapshot& snap, int w, int h, double fps) {
    char buf[256];
    std::snprintf(buf,
                  sizeof(buf),
                  "LevelUp | %dx%d %.1ffps | RTT %lldms | Loss %llu | PLI %llu NACK %llu",
                  w,
                  h,
                  fps,
                  static_cast<long long>(snap.rtc.rttMs),
                  static_cast<unsigned long long>(snap.rtc.rtpPacketsLost),
                  static_cast<unsigned long long>(snap.rtc.pliSent),
                  static_cast<unsigned long long>(snap.rtc.nackSent));
    return buf;
}

std::string formatStatsLine2(const SessionStatsSnapshot& snap, bool rumbleEnabled) {
    char buf[256];
    const char* rumbleState = !rumbleEnabled ? "关" : (snap.rumbleActive ? "震动中" : "待机");
    const char* linkState = !snap.rtc.signalingOpen
                                ? "信令重连中(媒体继续)"
                                : (snap.video.needKey ? "恢复IDR中"
                                                      : (snap.controlReady ? "正常" : "控制重连"));
    std::snprintf(buf,
                  sizeof(buf),
                  "vPad#%u | 震动:%s | 音频:%llu 补:%llu 延迟:%dms | 视频解码:%llu 跳:%llu | %s",
                  static_cast<unsigned>(snap.vpadId),
                  rumbleState,
                  static_cast<unsigned long long>(snap.audioDecoded),
                  static_cast<unsigned long long>(snap.audioConcealed),
                  snap.audioLatencyMs,
                  static_cast<unsigned long long>(snap.video.decodedFrames),
                  static_cast<unsigned long long>(snap.video.skippedFrames),
                  linkState);
    return buf;
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
#ifdef __SWITCH__
    appletSetMediaPlaybackState(true);
#endif

    auto [tw, th] = detectTargetResolution();
    m_targetWidth = tw;
    m_targetHeight = th;

    m_windowFocusSub = brls::Application::getWindowFocusChangedEvent()->subscribe(
        [this](bool focused) { onWindowFocusChanged(focused); });

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
    m_fpsEstimate = 0.0;
    setBorealisInputsBlocked(false);

    SessionLaunchConfig lcfg;
    lcfg.deviceId = m_cfg.deviceId;
    lcfg.cred = m_cfg.cred;
    lcfg.gameId = m_game.gameId;
    lcfg.gameName = m_game.name;
    lcfg.width = m_targetWidth;
    lcfg.height = m_targetHeight;
    lcfg.maxFps = 60;
    lcfg.videoLevel = m_cfg.videoLevel;
    lcfg.vqFirst = m_cfg.vqFirst;
    lcfg.rumbleEnabled = m_cfg.rumbleEnabled;
    lcfg.rumbleStrengthPercent = m_cfg.rumbleStrength;
    lcfg.invertStickY = m_cfg.invertStickY;
    lcfg.swapAbXy = m_cfg.swapAbXy;
    lcfg.audioVolumePercent = m_cfg.audioVolume;
    lcfg.touchMouseMode =
        (normalizeTouchMouseMode(m_cfg.touchMouseMode) == AppConfig::kTouchMouseTrackpad)
            ? input::TouchMouseMode::Trackpad
            : input::TouchMouseMode::Click;

    m_touchControlsVisible = false;
    m_justShownOnTouch = false;
    m_swkbdOpen = false;
    m_pressedOverlayBtn = 0;
    m_pressedFingerId = -1;
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
        m_orchestrator.setPausedForOverlay(true);
    } else {
        m_invalidateRendererPending = true;
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
        if (dt > 0.05 && snap.video.decodedFrames >= m_prevDecodedFrames) {
            m_fpsEstimate =
                static_cast<double>(snap.video.decodedFrames - m_prevDecodedFrames) / dt;
        }
    }
    m_prevDecodedFrames = snap.video.decodedFrames;
    m_lastHudUpdate = now;

    const int w = snap.video.width > 0 ? snap.video.width : m_targetWidth;
    const int h = snap.video.height > 0 ? snap.video.height : m_targetHeight;
    m_hudLine1 = formatStatsLine1(snap, w, h, m_fpsEstimate);
    m_hudLine2 = formatStatsLine2(snap, m_cfg.rumbleEnabled);
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
                          "目标分辨率 %dx%d@60fps  |  画质 高清  |  按 B 取消",
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
        if (font != brls::FONT_INVALID) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x + 16.0f, y + 14.0f, 670.0f, 54.0f, 8.0f);
            nvgFillColor(vg, nvgRGBA(10, 14, 22, 185));
            nvgFill(vg);

            nvgFontFaceId(vg, font);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgFontSize(vg, 16.0f);
            nvgFillColor(vg, nvgRGB(125, 211, 252));
            nvgText(vg, x + 28.0f, y + 21.0f, m_hudLine1.c_str(), nullptr);

            nvgFillColor(vg, nvgRGB(226, 232, 240));
            nvgText(vg, x + 28.0f, y + 43.0f, m_hudLine2.c_str(), nullptr);
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
        y + (hudVisible ? 76.0f : 16.0f),
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
    const bool trackpadMode =
        (normalizeTouchMouseMode(m_cfg.touchMouseMode) == AppConfig::kTouchMouseTrackpad);

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

    // 2. 右上角：鼠标模式切换半透明按钮（触控板 / 点击模式）
    {
        const auto& r = m_mouseBtnRect;
        const bool pressed = (m_pressedOverlayBtn == 2);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, r.x, r.y, r.w, r.h, 12.0f);
        nvgFillColor(vg, nvgRGBA(15, 23, 42, scaleAlpha(pressed ? 215 : 160)));
        nvgFill(vg);
        nvgStrokeWidth(vg, 1.5f);
        nvgStrokeColor(vg,
                       trackpadMode
                           ? nvgRGBA(56, 189, 248, scaleAlpha(pressed ? 230 : 165))
                           : nvgRGBA(148, 163, 184, scaleAlpha(pressed ? 200 : 120)));
        nvgStroke(vg);

        const float ix = r.x + 14.0f;
        const float iy = r.y + 11.0f;
        nvgStrokeWidth(vg, 1.6f);
        if (trackpadMode) {
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
            nvgFillColor(vg,
                         trackpadMode
                             ? nvgRGBA(125, 211, 252, scaleAlpha(245))
                             : nvgRGBA(248, 250, 252, scaleAlpha(240)));
            nvgText(vg,
                    r.x + 46.0f,
                    r.y + r.h * 0.5f,
                    trackpadMode ? "触控板模式" : "点击模式",
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

void LevelUpStreamView::toggleTouchMouseMode() {
    const bool currentlyTrackpad =
        (normalizeTouchMouseMode(m_cfg.touchMouseMode) == AppConfig::kTouchMouseTrackpad);
    const int nextMode =
        currentlyTrackpad ? AppConfig::kTouchMouseClick : AppConfig::kTouchMouseTrackpad;
    m_cfg.touchMouseMode = nextMode;
    m_store.saveConfig(m_cfg);
    if (m_onConfigChanged) {
        m_onConfigChanged(m_cfg);
    }
    const input::TouchMouseMode modeEnum =
        (nextMode == AppConfig::kTouchMouseTrackpad) ? input::TouchMouseMode::Trackpad
                                                     : input::TouchMouseMode::Click;
    m_orchestrator.setTouchMouseMode(modeEnum);
    m_lastTouchActivity = std::chrono::steady_clock::now();
    m_touchControlsVisible = true;
    logInfo(std::string("[StreamView] touch mouse mode -> ") +
            (nextMode == AppConfig::kTouchMouseTrackpad ? "trackpad" : "click"));
    brls::Application::notify(
        nextMode == AppConfig::kTouchMouseTrackpad
            ? "已切换为触控板模式（滑动移动 · 轻点左键 · 双指右键/滚轮）"
            : "已切换为点击模式（触屏直接点击）");
}

void LevelUpStreamView::openPauseOverlay() {
    if (m_overlayOpen || m_exiting || m_errorDialogOpen) {
        return;
    }

    m_overlayOpen = true;
    setBorealisInputsBlocked(false);
    m_orchestrator.setPausedForOverlay(true);

    auto* backdrop = new brls::Box(brls::Axis::COLUMN);
    backdrop->setJustifyContent(brls::JustifyContent::CENTER);
    backdrop->setAlignItems(brls::AlignItems::CENTER);
    backdrop->setBackgroundColor(nvgRGBA(8, 12, 20, 195));

    auto* panel = new brls::Box(brls::Axis::COLUMN);
    panel->setWidth(560.0f);
    panel->setPadding(24.0f);
    panel->setCornerRadius(16.0f);
    panel->setBackgroundColor(nvgRGB(24, 32, 48));
    panel->setAlignItems(brls::AlignItems::STRETCH);

    auto* title = new brls::Label();
    title->setText(m_game.name + " · 串流菜单");
    title->setFontSize(24.0f);
    title->setTextColor(nvgRGB(248, 250, 252));
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    title->setMarginBottom(6.0f);
    panel->addView(title);

    const auto snap = m_orchestrator.stats();
    char subBuf[160];
    std::snprintf(subBuf,
                  sizeof(subBuf),
                  "%dx%d | RTT %lldms | Loss %llu | vPad#%u",
                  snap.video.width > 0 ? snap.video.width : m_targetWidth,
                  snap.video.height > 0 ? snap.video.height : m_targetHeight,
                  static_cast<long long>(snap.rtc.rttMs),
                  static_cast<unsigned long long>(snap.rtc.rtpPacketsLost),
                  static_cast<unsigned>(snap.vpadId));

    auto* subtitle = new brls::Label();
    subtitle->setText(subBuf);
    subtitle->setFontSize(15.0f);
    subtitle->setTextColor(nvgRGB(148, 163, 184));
    subtitle->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    subtitle->setMarginBottom(16.0f);
    panel->addView(subtitle);

    LevelUpStreamView* self = this;
    std::shared_ptr<std::atomic<bool>> alive = m_alive;

    auto makeButton = [](const std::string& label, const brls::ButtonStyle* style) {
        auto* btn = new brls::Button();
        btn->setStyle(style);
        btn->setText(label);
        btn->setHeight(50.0f);
        btn->setMargins(5.0f, 0.0f, 5.0f, 0.0f);
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

    auto* exitBtn = makeButton("退出云游戏（安全释放实例）", &brls::BUTTONSTYLE_DEFAULT);
    exitBtn->setTextColor(nvgRGB(248, 113, 113));
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

    auto* backdrop = new brls::Box(brls::Axis::COLUMN);
    backdrop->setJustifyContent(brls::JustifyContent::CENTER);
    backdrop->setAlignItems(brls::AlignItems::CENTER);
    backdrop->setBackgroundColor(nvgRGBA(8, 12, 20, 205));

    auto* panel = new brls::Box(brls::Axis::COLUMN);
    panel->setWidth(560.0f);
    panel->setPadding(24.0f);
    panel->setCornerRadius(16.0f);
    panel->setBackgroundColor(nvgRGB(24, 32, 48));
    panel->setAlignItems(brls::AlignItems::STRETCH);

    auto* title = new brls::Label();
    title->setText("云游戏连接异常");
    title->setFontSize(24.0f);
    title->setTextColor(nvgRGB(248, 113, 113));
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    title->setMarginBottom(12.0f);
    panel->addView(title);

    auto* msgLabel = new brls::Label();
    msgLabel->setText(msg);
    msgLabel->setFontSize(17.0f);
    msgLabel->setSingleLine(false);
    msgLabel->setTextColor(nvgRGB(226, 232, 240));
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
    brls::Application::popActivity(brls::TransitionAnimation::NONE);
}

} // namespace levelup::ui
