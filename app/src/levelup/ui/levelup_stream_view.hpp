#pragma once

#include "levelup/session_orchestrator.hpp"
#include "levelup/store.hpp"

#include <borealis.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#if defined(__SWITCH__) && defined(BOREALIS_USE_DEKO3D)
class DKVideoRenderer;
#endif

namespace levelup::ui {

class LevelUpStreamView : public brls::Box {
  public:
    using ConfigChangedCallback = std::function<void(const AppConfig&)>;

    LevelUpStreamView(GameEntry game,
                      AppConfig cfg,
                      Store store,
                      ConfigChangedCallback onConfigChanged = nullptr);
    ~LevelUpStreamView() override;

    void onFocusGained() override;
    void onFocusLost() override;
    void draw(NVGcontext* vg,
              float x,
              float y,
              float width,
              float height,
              brls::Style style,
              brls::FrameContext* ctx) override;

    void openPauseOverlay();
    void exitStream();

  private:
    void startSession();
    void dismissPauseOverlay(bool requestKeyFrame, bool exitAfter);
    void setBorealisInputsBlocked(bool block);
    void onWindowFocusChanged(bool focused);
    void showFailureDialog(const std::string& stageText, const std::string& errText);
    void updateHudCache(const SessionStatsSnapshot& snap);
    void updateTouchOverlayControls(float x, float y, float width, float height, bool hudVisible);
    void drawTouchOverlayControls(NVGcontext* vg, float alphaFactor);
    void openVirtualKeyboard();
    void toggleTouchMouseMode();
    void applyTouchMouseMode(int nextMode, bool showToast);
    void drawModeToast(NVGcontext* vg, float x, float y, float width, float height);
    std::pair<int, int> detectTargetResolution() const;

    GameEntry m_game;
    AppConfig m_cfg;
    Store m_store;
    ConfigChangedCallback m_onConfigChanged;

    SessionOrchestrator m_orchestrator;
#if defined(__SWITCH__) && defined(BOREALIS_USE_DEKO3D)
    std::unique_ptr<DKVideoRenderer> m_renderer;
#endif

    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> m_stopping = std::make_shared<std::atomic<bool>>(false);

    bool m_inputsBlocked = false;
    bool m_overlayOpen = false;
    bool m_windowFocused = true;
    bool m_invalidateRendererPending = false;
    bool m_exiting = false;
    bool m_popRequested = false;
    bool m_errorDialogOpen = false;

    brls::Event<bool>::Subscription m_windowFocusSub;

    SessionPhase m_uiPhase = SessionPhase::Idle;
    std::string m_uiStageText = "正在初始化云游戏...";
    int m_targetWidth = 1280;
    int m_targetHeight = 720;

    // 视频实际绘制的像素尺寸（= 内容单位 × Application::windowScale）。底座下 windowScale=1.5
    // 而内容区仍是 1280x720，直接用内容单位当像素会把视频挤到 1920x1080 framebuffer 左上角。
    int m_videoSurfaceW = 0;
    int m_videoSurfaceH = 0;

    struct LossBucket {
        uint64_t lost = 0;
        uint64_t recv = 0;
    };
    static constexpr size_t kLossWindowBuckets = 10; // 10 x 500ms = 5s 实时丢包窗口

    std::chrono::steady_clock::time_point m_lastHudUpdate{};
    uint64_t m_prevDecodedFrames = 0;
    uint64_t m_prevRtpBytesRecv = 0;
    double m_fpsEstimate = 0.0;
    double m_bitrateKbps = 0.0;
    std::array<LossBucket, kLossWindowBuckets> m_lossWindow{};
    size_t m_lossWindowIdx = 0;
    uint64_t m_prevRtpPacketsLost = 0;
    uint64_t m_prevRtpPacketsRecv = 0;
    uint64_t m_recentPacketsLost = 0;
    uint64_t m_recentPacketsRecv = 0;
    std::string m_hudLine;

    // 串流界面左上/右上半透明悬浮按钮（点击屏幕显示，3 秒无操作自动隐藏）
    bool m_touchControlsVisible = false;
    bool m_justShownOnTouch = false;
    bool m_swkbdOpen = false;
    int m_pressedOverlayBtn = 0; // 0=none, 1=virtual keyboard, 2=mouse mode
    int m_pressedFingerId = -1;
    std::chrono::steady_clock::time_point m_lastTouchActivity{};
    input::TouchHitRect m_kbBtnRect{};
    input::TouchHitRect m_mouseBtnRect{};
    std::string m_modeToastText;
    std::chrono::steady_clock::time_point m_modeToastTime{};
};

} // namespace levelup::ui
