#pragma once

#include "levelup/api_client.hpp"
#include "levelup/audio_player.hpp"
#include "levelup/rtc_session.hpp"
#include "levelup/rumble_switch.hpp"
#include "levelup/switch_input_bridge.hpp"
#include "levelup/video_pipeline.hpp"
#include "levelup/ytcx_client.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace levelup {

struct SessionLaunchConfig {
    std::string deviceId;
    api::Credentials cred;
    std::string gameId;
    std::string gameName;
    int width = 1280;
    int height = 720;
    int maxFps = 60;
    int videoLevel = 3;
    bool vqFirst = true;
    bool rumbleEnabled = true;
    int rumbleStrengthPercent = 100;
    bool invertStickY = false;
    bool swapAbXy = false;
    int audioVolumePercent = 100;
};

enum class SessionPhase {
    Idle,
    StoppingPrevious,
    StartingGame,
    ApplyingUnion,
    WaitingBE,
    LaunchingEdge,
    ConnectingWebRTC,
    WaitingFirstFrame,
    Streaming,
    Failed,
    Stopped,
};

struct SessionStatsSnapshot {
    SessionPhase phase = SessionPhase::Idle;
    std::string stageText;
    std::string errorText;
    bool firstFrameDecoded = false;
    bool controlReady = false;
    bool rumbleActive = false;
    uint16_t vpadId = 1;
    video::PipelineStatsSnapshot video{};
    net::RtcStatsSnapshot rtc{};
    uint64_t audioDecoded = 0;
    uint64_t audioDropped = 0;
    uint64_t audioConcealed = 0;
    int audioLatencyMs = 0;
};

class SessionOrchestrator {
  public:
    using PhaseCallback = std::function<void(SessionPhase phase, const std::string& text)>;
    using OverlayComboCallback = std::function<void()>;

    SessionOrchestrator() = default;
    ~SessionOrchestrator();

    SessionOrchestrator(const SessionOrchestrator&) = delete;
    SessionOrchestrator& operator=(const SessionOrchestrator&) = delete;

    void setPhaseCallback(PhaseCallback cb);
    void setOverlayComboCallback(OverlayComboCallback cb);
    void setBeforeVideoCodecCloseCallback(video::VideoPipeline::BeforeCodecCloseCallback cb);

    void start(const SessionLaunchConfig& cfg);
    void stop();

    void requestKeyFrame();
    void setPausedForOverlay(bool paused);
    void setRumbleEnabled(bool enabled);
    void setRumbleStrengthPercent(int percent);
    void setInvertStickY(bool invert);
    void setSwapAbXy(bool swap);
    void setAudioVolumePercent(int percent);

    SessionStatsSnapshot stats() const;
    bool isRunning() const { return m_active.load(); }

  private:
    void setPhase(SessionPhase phase, const std::string& text, const std::string& err = {});
    void runLifecycle(SessionLaunchConfig cfg);

    mutable std::mutex m_stateMu;
    std::condition_variable m_stopCv;
    SessionPhase m_phase = SessionPhase::Idle;
    std::string m_stageText = "准备就绪";
    std::string m_errorText;

    std::mutex m_cbMu;
    PhaseCallback m_phaseCb;
    OverlayComboCallback m_overlayCb;

    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_firstFrame{false};
    std::atomic<int32_t> m_vpadId{1};
    std::atomic<bool> m_allocatedVPad{false};
    std::atomic<int64_t> m_pkgName{0};

    std::thread m_workerThread;
    std::thread m_vpadAllocThread;

    // Owned streaming components
    std::unique_ptr<api::Client> m_api;
    std::unique_ptr<ytcx::BEClient> m_be;
    std::unique_ptr<net::RtcSession> m_rtc;
    video::VideoPipeline m_video;
    audio::AudioPlayer m_audio;
    input::RumbleSwitch m_rumble;
    input::SwitchInputBridge m_input;

    mutable std::mutex m_compMu;
    std::string m_gameId;
};

} // namespace levelup
