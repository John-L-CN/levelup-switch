#include "levelup/session_orchestrator.hpp"

#include "levelup/crypto_util.hpp"
#include "levelup/log.hpp"

#include <chrono>

namespace levelup {

SessionOrchestrator::~SessionOrchestrator() {
    stop();
}

void SessionOrchestrator::setPhaseCallback(PhaseCallback cb) {
    std::lock_guard lock(m_cbMu);
    m_phaseCb = std::move(cb);
}

void SessionOrchestrator::setOverlayComboCallback(OverlayComboCallback cb) {
    std::lock_guard lock(m_cbMu);
    m_overlayCb = std::move(cb);
}

void SessionOrchestrator::setBeforeVideoCodecCloseCallback(
    video::VideoPipeline::BeforeCodecCloseCallback cb) {
    m_video.setBeforeCodecCloseCallback(std::move(cb));
}

void SessionOrchestrator::setPhase(SessionPhase phase, const std::string& text, const std::string& err) {
    {
        std::lock_guard lock(m_stateMu);
        m_phase = phase;
        m_stageText = text;
        if (!err.empty()) {
            m_errorText = err;
        }
    }
    PhaseCallback cb;
    {
        std::lock_guard lock(m_cbMu);
        cb = m_phaseCb;
    }
    if (cb) {
        cb(phase, text);
    }
}

void SessionOrchestrator::start(const SessionLaunchConfig& cfg) {
    stop();

    m_stopRequested.store(false);
    m_active.store(true);
    m_firstFrame.store(false);
    m_vpadId.store(1);
    m_allocatedVPad.store(false);
    m_pkgName.store(0);
    {
        std::lock_guard lock(m_stateMu);
        m_errorText.clear();
    }
    {
        std::lock_guard lock(m_compMu);
        m_gameId = cfg.gameId;
    }

    m_workerThread = std::thread([this, cfg]() { runLifecycle(cfg); });
}

void SessionOrchestrator::stop() {
    m_stopRequested.store(true);
    m_stopCv.notify_all();

    // Immediately cut rumble motors and game inputs before waiting on network teardown
    m_rumble.stop();
    m_input.stop();

    if (m_vpadAllocThread.joinable()) {
        m_vpadAllocThread.join();
    }
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    std::unique_ptr<net::RtcSession> rtc;
    std::unique_ptr<ytcx::BEClient> be;
    std::unique_ptr<api::Client> api;
    std::string gid;
    {
        std::lock_guard lock(m_compMu);
        rtc = std::move(m_rtc);
        be = std::move(m_be);
        api = std::move(m_api);
        gid = m_gameId;
    }

    if (rtc) {
        rtc->close();
    }
    m_video.stop();
    m_audio.stop();

    if (be && be->isConnected()) {
        if (m_allocatedVPad.load()) {
            const int32_t vid = m_vpadId.load();
            if (vid > 0) {
                be->releaseVPad(vid);
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
            }
        }
        be->sendExit(m_pkgName.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        be->close();
    }

    if (api && !gid.empty() && !api->credentials().sessionKey.empty()) {
        std::string stopErr;
        (void)api->stopGame(gid, &stopErr);
    }

    m_active.store(false);
    SessionPhase curPhase;
    {
        std::lock_guard lock(m_stateMu);
        curPhase = m_phase;
    }
    if (curPhase != SessionPhase::Failed) {
        setPhase(SessionPhase::Stopped, "已退出云游戏");
    }
}

void SessionOrchestrator::runLifecycle(SessionLaunchConfig cfg) {
    auto aborted = [this]() -> bool { return m_stopRequested.load(); };

    auto apiClient = std::make_unique<api::Client>(cfg.deviceId, cfg.cred);
    {
        std::lock_guard lock(m_compMu);
        m_api = std::move(apiClient);
    }

    auto sleepInterruptible = [this](int ms) -> bool {
        std::unique_lock lock(m_stateMu);
        return !m_stopCv.wait_for(lock, std::chrono::milliseconds(ms),
                                  [this]() { return m_stopRequested.load(); });
    };

    // 1. Pre-cleanup stale cloud session
    setPhase(SessionPhase::StoppingPrevious, "清理上次残留云端实例...");
    if (!cfg.gameId.empty()) {
        std::string tmpErr;
        m_api->stopGame(cfg.gameId, &tmpErr);
    }
    if (!sleepInterruptible(400) || aborted()) {
        return;
    }

    // 2. startGame -> proData + pkgName
    setPhase(SessionPhase::StartingGame, "申请云端游戏实例 (startGame)...");
    api::GameStartInfo startInfo;
    std::string err;
    {
        std::lock_guard lock(m_compMu);
        if (!m_api->startGame(cfg.gameId, startInfo, &err)) {
            setPhase(SessionPhase::Failed, "申请实例失败", err);
            return;
        }
    }
    m_pkgName.store(startInfo.pkgName);
    logInfo("[Session] startGame OK pkg=" + std::to_string(startInfo.pkgName) +
            " user=" + startInfo.userId);
    if (aborted()) return;

    const std::string devInfo = ytcx::buildDeviceInfoJson(cfg.width, cfg.height);
    const std::string streamInfo = ytcx::buildStreamInfoJson(
        cfg.width, cfg.height, cfg.maxFps, startInfo.pkgName, cfg.videoLevel, true);
    const auto pbLaunch = ytcx::buildLaunchGameProto(
        startInfo.proData, startInfo.pkgName, devInfo, streamInfo);

    // 3. unionApply
    setPhase(SessionPhase::ApplyingUnion, "申请调度网关 (/v3/union/apply)...");
    ytcx::ApplyResult applyRes;
    if (!ytcx::unionApply(startInfo.proData, startInfo.pkgName, cfg.deviceId, startInfo.userId,
                          cfg.width, cfg.height, pbLaunch, applyRes, &err)) {
        setPhase(SessionPhase::Failed, "调度申请失败", err);
        return;
    }
    const std::string realUserId = ytcx::extractRealUserIdFromToken(applyRes.token);
    logInfo("[Session] unionApply OK gw=" + applyRes.accessIp + ":" +
            std::to_string(applyRes.accessPort) + " uid=" + realUserId);
    if (aborted()) return;

    // 4. BE WebSocket connect + cmd=7 + heartbeat + wait cmd=17
    setPhase(SessionPhase::WaitingBE, "连接 BE 网关等待分配边缘节点...");
    auto beClient = std::make_unique<ytcx::BEClient>();
    beClient->setVPadCallback([this](int vpadId, int errcode) {
        if (vpadId > 0) {
            m_vpadId.store(vpadId);
            m_allocatedVPad.store(true);
            m_input.setVPadId(static_cast<uint16_t>(vpadId));
            logInfo("[Session] vPad allocated: id=" + std::to_string(vpadId));
        } else {
            logWarn("[Session] vPad alloc failed errcode=" + std::to_string(errcode));
        }
    });

    if (!beClient->connect(applyRes.accessIp, applyRes.accessPort, applyRes.token,
                           cfg.deviceId, 15000, &err)) {
        setPhase(SessionPhase::Failed, "BE 连接失败", err);
        return;
    }
    if (!beClient->sendLaunch(pbLaunch, &err)) {
        beClient->close();
        setPhase(SessionPhase::Failed, "发送 LaunchGame 失败", err);
        return;
    }
    beClient->startHeartbeat();

    {
        std::lock_guard lock(m_compMu);
        m_be = std::move(beClient);
    }

    ytcx::GameResource res;
    if (!m_be->waitResource(40000, res, &err)) {
        setPhase(SessionPhase::Failed, "等待边缘资源失败", err);
        return;
    }
    logInfo("[Session] Edge resource: " + res.ipv4 + " basePort=" + std::to_string(res.basePort));
    if (aborted()) return;

    // 5. Edge /launch
    setPhase(SessionPhase::LaunchingEdge, "拉起边缘渲染容器 (/launch)...");
    ytcx::EdgeLaunchParams lp;
    lp.edgeIp = res.ipv4;
    lp.basePort = res.basePort;
    lp.flowId = applyRes.flowId;
    lp.userId = realUserId;
    lp.pkgName = startInfo.pkgName;
    lp.deviceId = cfg.deviceId;
    lp.aSessionId = res.aSessionId;
    lp.vSessionId = res.vSessionId;
    lp.cSessionId = res.cSessionId;
    lp.width = cfg.width;
    lp.height = cfg.height;
    lp.maxFps = cfg.maxFps;
    lp.clientId = unixMillisNow();
    lp.videoLevel = cfg.videoLevel;
    lp.vqFirst = cfg.vqFirst;
    lp.vibration = true;

    ytcx::EdgeLaunchResult launchRes;
    if (!ytcx::edgeLaunch(lp, launchRes, &err)) {
        setPhase(SessionPhase::Failed, "边缘容器启动失败", err);
        return;
    }
    logInfo("[Session] /launch OK sigPort=" + std::to_string(launchRes.signalingPort) +
            " enc=" + std::to_string(launchRes.encodedWidth) + "x" +
            std::to_string(launchRes.encodedHeight));
    if (aborted()) return;

    // 6. Media, Rumble, Input & WebRTC Setup
    setPhase(SessionPhase::ConnectingWebRTC, "建立 WebRTC P2P 与硬解管线...");

    net::RtcConfig rcfg;
    rcfg.edgeIp = res.ipv4;
    rcfg.signalingPort = launchRes.signalingPort;
    rcfg.uid = realUserId + "_" + res.aSessionId + "_" + res.vSessionId + "_" + res.cSessionId;
    rcfg.aSessionId = res.aSessionId;
    rcfg.vSessionId = res.vSessionId;
    rcfg.cSessionId = res.cSessionId;
    rcfg.noTlsVerify = true;
    rcfg.vqFirst = cfg.vqFirst;
    rcfg.videoLevel = cfg.videoLevel;

    auto rtcSess = std::make_unique<net::RtcSession>(rcfg);

    m_video.setKeyFrameRequester([&rtcPtr = *rtcSess]() { rtcPtr.requestKeyFrame(); });
    m_video.setNackRequester([&rtcPtr = *rtcSess](const std::vector<uint16_t>& missing) {
        rtcPtr.sendNack(missing);
    });
    m_video.setFirstFrameCallback([this, &rtcPtr = *rtcSess]() {
        rtcPtr.notifyVideoFrameDecoded();
        m_firstFrame.store(true);
        setPhase(SessionPhase::Streaming, "串流中");
        std::lock_guard lock(m_compMu);
        if (m_be && m_be->isConnected()) {
            m_be->sendStreamStatus(true);
            logInfo("[Session] sent BE cmd=37 CGCNtfStreamStatus(playing=1)");
        }
    });

    m_video.start(cfg.width, cfg.height, cfg.maxFps);

    m_audio.setVolume(cfg.audioVolumePercent);
    m_audio.start();

    m_rumble.setEnabled(cfg.rumbleEnabled);
    m_rumble.setStrengthPercent(cfg.rumbleStrengthPercent);
    m_rumble.start();

    m_input.setInvertStickY(cfg.invertStickY);
    m_input.setSwapAbXy(cfg.swapAbXy);
    m_input.setSendCallback([&rtcPtr = *rtcSess](const std::vector<uint8_t>& pkt) {
        return rtcPtr.sendControlPacket(pkt);
    });
    m_input.setOverlayComboCallback([this]() {
        OverlayComboCallback cb;
        {
            std::lock_guard lock(m_cbMu);
            cb = m_overlayCb;
        }
        if (cb) cb();
    });
    m_input.start();

    rtcSess->setVideoRtpCallback([this](uint16_t seq, uint8_t pt, const uint8_t* pl, size_t len, bool m, uint32_t ts) {
        m_video.pushRtp(seq, pt, pl, len, m, ts);
    });
    rtcSess->setAudioOpusCallback([this](uint8_t, const uint8_t* pl, size_t len, uint16_t, uint32_t) {
        m_audio.pushOpusPacket(pl, len);
    });
    rtcSess->setPacketLossCallback([this](int64_t delta) {
        m_video.onTransportLoss(delta);
    });
    rtcSess->setRumbleCallback([this](const input::RumbleEvent& ev) {
        m_rumble.submit(ev);
    });

    if (!rtcSess->start(15000, &err)) {
        m_input.stop();
        m_rumble.stop();
        m_video.stop();
        m_audio.stop();
        setPhase(SessionPhase::Failed, "WebRTC 信令连接失败", err);
        return;
    }

    {
        std::lock_guard lock(m_compMu);
        m_rtc = std::move(rtcSess);
    }

    setPhase(SessionPhase::WaitingFirstFrame, "等待云端首帧 H.264 解码...");

    // 7. Schedule 8s post-launch AllocVPad (cmd=33), retrying once after firstFrame (playing=1) if rejected
    m_vpadAllocThread = std::thread([this]() {
        auto waitMs = [this](int ms) -> bool {
            std::unique_lock lock(m_stateMu);
            return !m_stopCv.wait_for(lock, std::chrono::milliseconds(ms),
                                      [this]() { return m_stopRequested.load(); });
        };
        auto tryAllocVPad = [this](const char* reason) {
            std::lock_guard lock(m_compMu);
            if (m_be && m_be->isConnected()) {
                if (m_be->sendAllocVPad({}, 65535, 2)) {
                    logInfo(std::string("[Session] sent cmd=33 CGReqAllocVPad (") + reason + ")");
                }
            }
        };

        if (!waitMs(8000) || m_stopRequested.load()) {
            return;
        }
        tryAllocVPad("8s post-launch");

        for (int i = 0; i < 180; ++i) {
            if (!waitMs(250) || m_stopRequested.load()) {
                return;
            }
            if (m_allocatedVPad.load() || m_firstFrame.load()) {
                break;
            }
        }
        if (!m_stopRequested.load() && m_firstFrame.load() && !m_allocatedVPad.load()) {
            if (!waitMs(2000) || m_stopRequested.load()) {
                return;
            }
            if (!m_allocatedVPad.load()) {
                tryAllocVPad("2s post-firstFrame retry");
            }
        }
    });

    // Monitor connection health until stop() requested or connection dies
    while (!aborted()) {
        std::string deadReason;
        bool dead = false;
        {
            std::lock_guard lock(m_compMu);
            if (m_rtc && m_rtc->isDead(&deadReason)) {
                dead = true;
            }
        }
        if (dead) {
            setPhase(SessionPhase::Failed, "连接中断", deadReason);
            break;
        }
        if (!sleepInterruptible(250)) {
            break;
        }
    }
}

void SessionOrchestrator::requestKeyFrame() {
    m_video.requestRecoveryKeyFrame("manual UI refresh");
}

void SessionOrchestrator::setPausedForOverlay(bool paused) {
    m_input.setPaused(paused);
    if (paused) {
        m_rumble.forceZero();
    }
}

void SessionOrchestrator::setRumbleEnabled(bool enabled) {
    m_rumble.setEnabled(enabled);
}

void SessionOrchestrator::setRumbleStrengthPercent(int percent) {
    m_rumble.setStrengthPercent(percent);
}

void SessionOrchestrator::setInvertStickY(bool invert) {
    m_input.setInvertStickY(invert);
}

void SessionOrchestrator::setSwapAbXy(bool swap) {
    m_input.setSwapAbXy(swap);
}

void SessionOrchestrator::setAudioVolumePercent(int percent) {
    m_audio.setVolume(percent);
}

SessionStatsSnapshot SessionOrchestrator::stats() const {
    SessionStatsSnapshot snap;
    {
        std::lock_guard lock(m_stateMu);
        snap.phase = m_phase;
        snap.stageText = m_stageText;
        snap.errorText = m_errorText;
    }
    snap.firstFrameDecoded = m_firstFrame.load();
    snap.vpadId = static_cast<uint16_t>(m_vpadId.load());
    snap.rumbleActive = m_rumble.isActive();
    snap.video = m_video.stats();
    snap.audioDecoded = m_audio.decodedPackets();
    snap.audioDropped = m_audio.droppedPackets();
    {
        std::lock_guard lock(m_compMu);
        if (m_rtc) {
            snap.rtc = m_rtc->stats();
            snap.controlReady = snap.rtc.controlOpen;
        }
    }
    return snap;
}

} // namespace levelup
