#pragma once

#include "levelup/session_orchestrator.hpp"
#include "levelup/store.hpp"

#include <borealis.hpp>

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
    void setBorealisInputsBlocked(bool block);
    void onWindowFocusChanged(bool focused);
    void showFailureDialog(const std::string& stageText, const std::string& errText);
    void updateHudCache(const SessionStatsSnapshot& snap);
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

    std::chrono::steady_clock::time_point m_lastHudUpdate{};
    uint64_t m_prevDecodedFrames = 0;
    double m_fpsEstimate = 0.0;
    std::string m_hudLine1;
    std::string m_hudLine2;
};

} // namespace levelup::ui
