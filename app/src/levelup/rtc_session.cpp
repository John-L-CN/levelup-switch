#include "levelup/rtc_session.hpp"

#include "levelup/crypto_util.hpp"
#include "levelup/h264_depacketizer.hpp"
#include "levelup/log.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

#ifndef LEVELUP_NO_RTC
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>
#include <rtc/rtcpreceivingsession.hpp>
#endif

namespace levelup::net {

std::string ensureCandidatePrefix(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) {
        s.pop_back();
    }
    if (s.rfind("a=", 0) == 0) {
        s.erase(0, 2);
    }
    if (s.rfind("candidate:", 0) == 0) {
        return s;
    }
    return "candidate:" + s;
}

std::string injectH264FeedbackAndProfile(std::string sdp, bool preferHighProfile) {
    // Normalizes H.264 rtcp-fb lines in generated SDP answer if any are missing.
    std::vector<std::string> lines;
    {
        std::istringstream iss(sdp);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(std::move(line));
        }
    }

    auto iequalsPrefix = [](const std::string& s, const std::string& p) {
        if (s.size() < p.size()) return false;
        for (size_t i = 0; i < p.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(s[i])) !=
                std::tolower(static_cast<unsigned char>(p[i]))) {
                return false;
            }
        }
        return true;
    };

    std::vector<int> h264Pts;
    bool inVideo = false;
    for (const auto& line : lines) {
        if (line.rfind("m=", 0) == 0) {
            inVideo = (line.rfind("m=video ", 0) == 0);
            continue;
        }
        if (inVideo && line.rfind("a=rtpmap:", 0) == 0) {
            const auto sp = line.find(' ');
            if (sp != std::string::npos) {
                const int pt = std::atoi(line.substr(9, sp - 9).c_str());
                const std::string codec = line.substr(sp + 1);
                if (iequalsPrefix(codec, "H264/")) {
                    h264Pts.push_back(pt);
                }
            }
        }
    }

    if (h264Pts.empty()) {
        return sdp;
    }

    std::vector<std::string> outLines;
    outLines.reserve(lines.size() + h264Pts.size() * 4);
    inVideo = false;
    std::unordered_set<int> addedFeedbackForPt;

    for (const auto& line : lines) {
        if (line.rfind("m=", 0) == 0) {
            inVideo = (line.rfind("m=video ", 0) == 0);
            outLines.push_back(line);
            continue;
        }

        std::string cur = line;
        if (preferHighProfile && inVideo && cur.rfind("a=fmtp:", 0) == 0) {
            const auto sp = cur.find(' ');
            if (sp != std::string::npos) {
                const int pt = std::atoi(cur.substr(7, sp - 7).c_str());
                if (std::find(h264Pts.begin(), h264Pts.end(), pt) != h264Pts.end()) {
                    // Only normalize profile if it isn't RTX apt= line
                    if (cur.find("packetization-mode=1") != std::string::npos &&
                        cur.find("profile-level-id=") == std::string::npos) {
                        cur += ";profile-level-id=640032";
                    }
                }
            }
        }

        outLines.push_back(cur);

        if (inVideo && cur.rfind("a=rtpmap:", 0) == 0) {
            const auto sp = cur.find(' ');
            if (sp != std::string::npos) {
                const int pt = std::atoi(cur.substr(9, sp - 9).c_str());
                if (std::find(h264Pts.begin(), h264Pts.end(), pt) != h264Pts.end() &&
                    addedFeedbackForPt.insert(pt).second) {
                    static constexpr const char* kFbs[] = {"nack", "nack pli", "ccm fir", "goog-remb"};
                    for (const char* fb : kFbs) {
                        const std::string target = "a=rtcp-fb:" + std::to_string(pt) + " " + fb;
                        bool exists = false;
                        for (const auto& existing : lines) {
                            if (existing == target) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
                            outLines.push_back(target);
                        }
                    }
                }
            }
        }
    }

    std::string result;
    for (const auto& l : outLines) {
        result += l;
        result += "\r\n";
    }
    return result;
}

#ifdef LEVELUP_NO_RTC

struct RtcSession::Impl {
    RtcConfig cfg;
};

RtcSession::RtcSession(RtcConfig cfg) : m_impl(std::make_unique<Impl>()) {
    m_impl->cfg = std::move(cfg);
}
RtcSession::~RtcSession() = default;
void RtcSession::setVideoRtpCallback(VideoRtpCallback) {}
void RtcSession::setAudioOpusCallback(AudioOpusCallback) {}
void RtcSession::setStateCallback(StateCallback) {}
void RtcSession::setPacketLossCallback(PacketLossCallback) {}
void RtcSession::setRumbleCallback(RumbleCallback) {}
bool RtcSession::start(int, std::string* errOut) {
    if (errOut) *errOut = "libdatachannel disabled in host test";
    return false;
}
void RtcSession::close() {}
void RtcSession::notifyVideoFrameDecoded() {}
void RtcSession::requestKeyFrame() {}
void RtcSession::sendNack(const std::vector<uint16_t>&) {}
bool RtcSession::sendControlPacket(const uint8_t*, size_t) { return false; }
bool RtcSession::sendControlPacket(const std::vector<uint8_t>&) { return false; }
bool RtcSession::isControlConnected() const { return false; }
bool RtcSession::isDead(std::string*) const { return false; }
RtcStatsSnapshot RtcSession::stats() const { return {}; }

#else

namespace {

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

/// Custom RTCP handler replacing upstream RtcpReceivingSession:
///  1. Captures raw transportSend callback so NACK/PLI/FIR can be sent on RecvOnly tracks
///  2. Forces BOTH PLI and FIR on requestKeyframe (cloud encoder coverage)
///  3. Implements explicit RTCP NACK (PT=205, FMT=1)
///  4. Tracks exact RTP packet loss cleanly from sequence 1 (immune to RtcpReceivingSession probation=0)
class LevelUpRtcpHandler final : public rtc::RtcpReceivingSession {
  public:
    void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override {
        {
            std::lock_guard lock(m_sendMu);
            m_sendCb = send;
        }

        // First track incoming binary RTP sequence numbers before/after RtcpReceivingSession (including RTX unwrap)
        rtc::RtcpReceivingSession::incoming(messages, send);

        const uint32_t ssrc = mSsrc;
        if (ssrc != 0) {
            m_videoSsrc.store(ssrc);
        }

        std::lock_guard seqLock(m_seqMu);
        for (const auto& m : messages) {
            if (!m || m->type != rtc::Message::Binary || m->size() < sizeof(rtc::RtpHeader)) {
                continue;
            }
            const auto* rtp = reinterpret_cast<const rtc::RtpHeader*>(m->data());
            recordSeqLocked(rtp->seqNumber());
        }
    }

    bool requestKeyframe(const std::vector<rtc::SSRC>& targetSSRCs,
                         bool retransmit,
                         const rtc::message_callback& send) override {
        const rtc::message_callback activeSend = send ? send : copySendCb();
        if (!activeSend || mSsrc == 0) {
            return false;
        }
        pushPLI(activeSend);
        m_pliSent.fetch_add(1);
        pushFIR(activeSend, targetSSRCs, retransmit);
        m_firSent.fetch_add(1);
        return true;
    }

    void sendPliAndFir() {
        auto send = copySendCb();
        const uint32_t ssrc = m_videoSsrc.load();
        if (!send || ssrc == 0) {
            return;
        }
        std::lock_guard lock(mMutex);
        mSsrc = ssrc;
        pushPLI(send);
        m_pliSent.fetch_add(1);
        pushFIR(send, {ssrc}, false);
        m_firSent.fetch_add(1);
    }

    void sendNack(const std::vector<uint16_t>& missingSeqs) {
        if (missingSeqs.empty()) return;
        auto send = copySendCb();
        const uint32_t ssrc = m_videoSsrc.load();
        if (!send || ssrc == 0) return;

        std::vector<uint16_t> sorted = missingSeqs;
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        if (sorted.size() > 32) {
            sorted.resize(32);
        }

        auto msg = rtc::make_message(
            rtc::RtcpNack::Size(static_cast<unsigned int>(sorted.size())),
            rtc::Message::Control);
        std::fill(msg->begin(), msg->end(), std::byte{0});
        auto* nack = reinterpret_cast<rtc::RtcpNack*>(msg->data());
        nack->preparePacket(ssrc, static_cast<unsigned int>(sorted.size()));

        unsigned int fciCount = 0;
        uint16_t fciPid = 0;
        for (uint16_t seq : sorted) {
            nack->addMissingPacket(&fciCount, &fciPid, seq);
        }
        nack->header.header.prepareHeader(205, 1, static_cast<uint16_t>(2 + fciCount));
        msg->resize(rtc::RtcpNack::Size(fciCount));
        send(std::move(msg));
        m_nackSent.fetch_add(sorted.size());
    }

    uint32_t videoSsrc() const { return m_videoSsrc.load(); }
    uint64_t pliSent() const { return m_pliSent.load(); }
    uint64_t firSent() const { return m_firSent.load(); }
    uint64_t nackSent() const { return m_nackSent.load(); }
    uint64_t packetsReceived() const { return m_packetsRecv.load(); }

    uint64_t totalLostEstimate() const {
        std::lock_guard lock(m_seqMu);
        if (!m_seqInit) return 0;
        const uint64_t expected = (m_cycles + m_maxSeq) - m_baseSeq + 1ULL;
        const uint64_t uniqueRecv = m_uniqueRecv;
        return expected > uniqueRecv ? (expected - uniqueRecv) : 0;
    }

  private:
    rtc::message_callback copySendCb() const {
        std::lock_guard lock(m_sendMu);
        return m_sendCb;
    }

    void recordSeqLocked(uint16_t seq) {
        m_packetsRecv.fetch_add(1);
        if (!m_seqInit) {
            m_seqInit = true;
            m_baseSeq = seq;
            m_maxSeq = seq;
            m_cycles = 0;
            m_uniqueRecv = 1;
            return;
        }
        const int16_t delta = static_cast<int16_t>(seq - m_maxSeq);
        if (delta > 0 && delta < 3000) {
            if (seq < m_maxSeq) {
                m_cycles += 65536ULL;
            }
            m_maxSeq = seq;
            m_uniqueRecv++;
        } else if (delta > -3000 && delta <= 0) {
            // Retransmitted / reordered earlier packet arrived cleanly
            m_uniqueRecv++;
        } else if (delta >= 3000) {
            // Stream reset jump
            m_baseSeq = seq;
            m_maxSeq = seq;
            m_cycles = 0;
            m_uniqueRecv = 1;
        }
    }

    mutable std::mutex m_sendMu;
    rtc::message_callback m_sendCb;

    mutable std::mutex m_seqMu;
    bool m_seqInit = false;
    uint32_t m_baseSeq = 0;
    uint16_t m_maxSeq = 0;
    uint64_t m_cycles = 0;
    uint64_t m_uniqueRecv = 0;

    std::atomic<uint32_t> m_videoSsrc{0};
    std::atomic<uint64_t> m_packetsRecv{0};
    std::atomic<uint64_t> m_pliSent{0};
    std::atomic<uint64_t> m_firSent{0};
    std::atomic<uint64_t> m_nackSent{0};
};

} // namespace

struct RtcSession::Impl {
    RtcConfig cfg;

    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::mutex trackMu;
    std::shared_ptr<rtc::Track> videoTrack;
    std::shared_ptr<LevelUpRtcpHandler> rtcpHandler;

    std::shared_ptr<rtc::Track> getVideoTrack() {
        std::lock_guard lock(trackMu);
        return videoTrack;
    }

    std::shared_ptr<LevelUpRtcpHandler> getRtcpHandler() {
        std::lock_guard lock(trackMu);
        return rtcpHandler;
    }

    std::mutex wsWriteMu;
    std::mutex stateMu;
    std::condition_variable stateCv;
    std::atomic<bool> wsOpenDone{false};
    std::atomic<bool> wsConnected{false};
    std::atomic<bool> closing{false};
    std::string wsOpenError;

    std::mutex peerMu;
    std::string peer;

    std::mutex dcMu;
    std::shared_ptr<rtc::DataChannel> controlDc;
    std::atomic<bool> dcReadyFired{false};
    std::atomic<bool> dead{false};
    std::string deadReason;

    std::atomic<bool> gotFirstFrame{false};
    std::atomic<int64_t> startMs{0};
    std::atomic<int64_t> firstVideoTrackMs{0};
    std::atomic<int64_t> lastBootPliMs{0};
    std::atomic<int64_t> lastKeyReqMs{0};
    std::atomic<int64_t> lastRttMs{-1};
    std::atomic<uint64_t> dcRecvCount{0};
    std::atomic<uint64_t> dcPingEchoCount{0};

    std::mutex cbMu;
    VideoRtpCallback onVideoRtp;
    AudioOpusCallback onAudioOpus;
    StateCallback onState;
    PacketLossCallback onPacketLoss;
    RumbleCallback onRumble;

    std::mutex bgMu;
    std::condition_variable bgCv;
    std::atomic<bool> bgStop{true};
    std::thread bgThread;

    void markDead(const std::string& reason) {
        if (!dead.exchange(true)) {
            std::lock_guard lock(stateMu);
            deadReason = reason;
        }
        stateCv.notify_all();
    }

    bool sendSignalJson(const nlohmann::json& j) {
        std::lock_guard lock(wsWriteMu);
        if (!ws || !ws->isOpen()) return false;
        try {
            return ws->send(j.dump());
        } catch (const std::exception& e) {
            logWarn(std::string("[RTC] signal send error: ") + e.what());
            return false;
        }
    }

    bool sendControl(const uint8_t* data, size_t len) {
        std::shared_ptr<rtc::DataChannel> dc;
        {
            std::lock_guard lock(dcMu);
            dc = controlDc;
        }
        if (!dc || !dc->isOpen() || !data || len == 0) {
            return false;
        }
        try {
            return dc->send(reinterpret_cast<const std::byte*>(data), len);
        } catch (const std::exception&) {
            return false;
        }
    }

    void sendControlReadyHandshake() {
        if (dcReadyFired.exchange(true)) {
            return;
        }
        if (!cfg.cSessionId.empty()) {
            const auto pkt = input::buildHandshakeSession(cfg.cSessionId);
            sendControl(pkt.data(), pkt.size());
        }
        {
            const auto h773 = input::buildHandshake773();
            sendControl(h773.data(), h773.size());
            const auto h775 = input::buildHandshake775();
            sendControl(h775.data(), h775.size());
        }
        {
            const auto vq = input::buildBitrateModePacket(cfg.vqFirst);
            const bool okVq = sendControl(vq.data(), vq.size());
            const auto cur = input::buildCursorPacket(true);
            sendControl(cur.data(), cur.size());
            logInfo("[RTC] control handshake sent (VQ_FIRST=" +
                    std::string(cfg.vqFirst ? "1" : "0") +
                    " ok=" + std::string(okVq ? "1" : "0") + ")");
        }
    }

    void handleOffer(const std::string& offerSdp) {
        if (!pc) return;
        try {
            rtc::Description offer(offerSdp, rtc::Description::Type::Offer);
            pc->setRemoteDescription(offer);

            if (auto vt = getVideoTrack()) {
                auto desc = vt->description();
                bool modified = false;
                for (int pt : desc.payloadTypes()) {
                    auto* rm = desc.rtpMap(pt);
                    if (!rm || !equalsIgnoreCase(rm->format, "H264")) {
                        continue;
                    }
                    static constexpr const char* kFeedbacks[] = {
                        "nack", "nack pli", "ccm fir", "goog-remb",
                    };
                    for (const char* fb : kFeedbacks) {
                        if (!rm->hasFeedback(fb)) {
                            rm->addFeedback(fb);
                            modified = true;
                        }
                    }
                }
                if (modified) {
                    vt->setDescription(std::move(desc));
                }
            }

            pc->setLocalDescription(rtc::Description::Type::Answer);
        } catch (const std::exception& e) {
            logError(std::string("[RTC] handleOffer failed: ") + e.what());
        }
    }

    void handleSignalingText(const std::string& text) {
        nlohmann::json msg;
        try {
            msg = nlohmann::json::parse(text);
        } catch (...) {
            return;
        }
        if (!msg.is_object()) return;

        if (msg.contains("peer") && msg["peer"].is_string()) {
            const std::string p = msg["peer"].get<std::string>();
            if (!p.empty()) {
                std::lock_guard lock(peerMu);
                peer = p;
            }
        }

        const std::string mtype = msg.value("type", "");
        if (mtype == "onOffer") {
            if (msg.contains("offer") && msg["offer"].is_object()) {
                const std::string sdp = msg["offer"].value("sdp", "");
                if (!sdp.empty()) {
                    handleOffer(sdp);
                }
            }
        } else if (mtype == "onCandidate") {
            if (msg.contains("candidate") && msg["candidate"].is_object() && pc) {
                const auto& c = msg["candidate"];
                const std::string cs = c.value("candidate", "");
                if (cs.empty()) return;
                std::string mid = c.value("sdpMid", "");
                if (mid.empty()) {
                    const int idx = c.value("sdpMLineIndex", 0);
                    mid = std::to_string(idx);
                }
                try {
                    pc->addRemoteCandidate(rtc::Candidate(cs, mid));
                } catch (const std::exception& e) {
                    logWarn(std::string("[RTC] addRemoteCandidate rejected (") + cs + "): " + e.what());
                }
            }
        }
    }

    void bgLoop() {
        int64_t tick = 0;
        uint64_t prevLost = 0;
        int64_t prevHbMs = 0;
        int64_t prevBootPliMs = 0;
        int cursorKickIndex = 0;
        static constexpr int kCursorKickMs[] = {1000, 3000, 6000, 10000, 16000};

        while (!bgStop.load() && !closing.load()) {
            const int64_t nowMs = unixMillisNow();

            // Pre-first-frame boost: send PLI+FIR every 500ms for first 8s until first video frame arrives
            const int64_t trackMs = firstVideoTrackMs.load();
            if (trackMs > 0 && !gotFirstFrame.load() && (nowMs - trackMs) < 8000 &&
                (prevBootPliMs == 0 || (nowMs - prevBootPliMs) >= 500)) {
                prevBootPliMs = nowMs;
                if (auto rh = getRtcpHandler()) {
                    rh->sendPliAndFir();
                }
            }

            // Control DataChannel 1s heartbeat (1537) + delayed cursor SHOW kicks
            if (dcReadyFired.load() && (nowMs - prevHbMs) >= 1000) {
                prevHbMs = nowMs;
                const uint64_t uptimeMs = static_cast<uint64_t>(std::max<int64_t>(0, nowMs - startMs.load()));
                const auto hb = input::buildHeartbeatPacket(uptimeMs);
                sendControl(hb.data(), hb.size());

                while (cursorKickIndex < 5 &&
                       static_cast<int64_t>(uptimeMs) >= kCursorKickMs[cursorKickIndex]) {
                    const auto cur = input::buildCursorPacket(true);
                    sendControl(cur.data(), cur.size());
                    cursorKickIndex++;
                }
            }

            // Every 1s (5x200ms ticks): poll RTP packet loss delta -> Layer 1 callback
            if ((tick % 5) == 0) {
                if (auto rh = getRtcpHandler()) {
                    const uint64_t lost = rh->totalLostEstimate();
                    const int64_t delta = static_cast<int64_t>(lost) - static_cast<int64_t>(prevLost);
                    prevLost = lost;
                    if (delta > 0) {
                        PacketLossCallback cb;
                        {
                            std::lock_guard lock(cbMu);
                            cb = onPacketLoss;
                        }
                        if (cb) {
                            cb(delta);
                        }
                    }
                }
            }

            tick++;
            std::unique_lock lock(bgMu);
            bgCv.wait_for(lock, std::chrono::milliseconds(200),
                          [this]() { return bgStop.load() || closing.load(); });
        }
    }
};

RtcSession::RtcSession(RtcConfig cfg) : m_impl(std::make_unique<Impl>()) {
    m_impl->cfg = std::move(cfg);
}

RtcSession::~RtcSession() {
    close();
}

void RtcSession::setVideoRtpCallback(VideoRtpCallback cb) {
    std::lock_guard lock(m_impl->cbMu);
    m_impl->onVideoRtp = std::move(cb);
}

void RtcSession::setAudioOpusCallback(AudioOpusCallback cb) {
    std::lock_guard lock(m_impl->cbMu);
    m_impl->onAudioOpus = std::move(cb);
}

void RtcSession::setStateCallback(StateCallback cb) {
    std::lock_guard lock(m_impl->cbMu);
    m_impl->onState = std::move(cb);
}

void RtcSession::setPacketLossCallback(PacketLossCallback cb) {
    std::lock_guard lock(m_impl->cbMu);
    m_impl->onPacketLoss = std::move(cb);
}

void RtcSession::setRumbleCallback(RumbleCallback cb) {
    std::lock_guard lock(m_impl->cbMu);
    m_impl->onRumble = std::move(cb);
}

bool RtcSession::start(int timeoutMs, std::string* errOut) {
    close();

    m_impl->closing.store(false);
    m_impl->dead.store(false);
    m_impl->deadReason.clear();
    m_impl->wsOpenDone.store(false);
    m_impl->wsConnected.store(false);
    m_impl->wsOpenError.clear();
    m_impl->dcReadyFired.store(false);
    m_impl->gotFirstFrame.store(false);
    m_impl->firstVideoTrackMs.store(0);
    m_impl->startMs.store(unixMillisNow());
    m_impl->lastKeyReqMs.store(0);

    rtc::Configuration pcCfg;
    pcCfg.disableAutoNegotiation = true;
    pcCfg.iceServers.emplace_back("stun:111.206.174.2:3478");
    pcCfg.iceServers.emplace_back("stun:stun.hitv.com:3478");
    pcCfg.iceServers.emplace_back("stun:stun.chat.bilibili.com:3478");

    m_impl->pc = std::make_shared<rtc::PeerConnection>(pcCfg);

    m_impl->pc->onLocalDescription([this](rtc::Description desc) {
        const std::string sdp = injectH264FeedbackAndProfile(desc.generateSdp(), true);
        std::string curPeer;
        {
            std::lock_guard lock(m_impl->peerMu);
            curPeer = m_impl->peer;
        }
        const nlohmann::json msg = {
            {"uid", m_impl->cfg.uid},
            {"peer", curPeer},
            {"token", ""},
            {"answer", {{"type", "answer"}, {"sdp", sdp}}},
        };
        m_impl->sendSignalJson(msg);
        logInfo("[RTC] SDP Answer sent (" + std::to_string(sdp.size()) + " bytes)");
    });

    m_impl->pc->onLocalCandidate([this](rtc::Candidate cand) {
        const std::string candLine = ensureCandidatePrefix(cand.candidate());
        std::string curPeer;
        {
            std::lock_guard lock(m_impl->peerMu);
            curPeer = m_impl->peer;
        }
        static constexpr const char* kMids[] = {"0", "1", "2"};
        for (int idx = 0; idx < 3; ++idx) {
            const nlohmann::json msg = {
                {"candidate",
                 {{"sdpMLineIndex", idx}, {"sdpMid", kMids[idx]}, {"candidate", candLine}}},
                {"uid", m_impl->cfg.uid},
                {"peer", curPeer},
                {"token", ""},
            };
            m_impl->sendSignalJson(msg);
        }
    });

    m_impl->pc->onStateChange([this](rtc::PeerConnection::State st) {
        const char* name = "unknown";
        switch (st) {
        case rtc::PeerConnection::State::New: name = "new"; break;
        case rtc::PeerConnection::State::Connecting: name = "connecting"; break;
        case rtc::PeerConnection::State::Connected: name = "connected"; break;
        case rtc::PeerConnection::State::Disconnected:
            name = "disconnected";
            m_impl->markDead("WebRTC PeerConnection disconnected");
            break;
        case rtc::PeerConnection::State::Failed:
            name = "failed";
            m_impl->markDead("WebRTC PeerConnection failed");
            break;
        case rtc::PeerConnection::State::Closed:
            name = "closed";
            m_impl->markDead("WebRTC PeerConnection closed");
            break;
        }
        logInfo(std::string("[RTC] PC state: ") + name);
        StateCallback cb;
        {
            std::lock_guard lock(m_impl->cbMu);
            cb = m_impl->onState;
        }
        if (cb) cb(std::string("pc:") + name);
    });

    m_impl->pc->onIceStateChange([this](rtc::PeerConnection::IceState st) {
        const char* name = "unknown";
        switch (st) {
        case rtc::PeerConnection::IceState::New: name = "new"; break;
        case rtc::PeerConnection::IceState::Checking: name = "checking"; break;
        case rtc::PeerConnection::IceState::Connected: name = "connected"; break;
        case rtc::PeerConnection::IceState::Completed: name = "completed"; break;
        case rtc::PeerConnection::IceState::Failed: name = "failed"; break;
        case rtc::PeerConnection::IceState::Disconnected: name = "disconnected"; break;
        case rtc::PeerConnection::IceState::Closed: name = "closed"; break;
        }
        logInfo(std::string("[RTC] ICE state: ") + name);
        StateCallback cb;
        {
            std::lock_guard lock(m_impl->cbMu);
            cb = m_impl->onState;
        }
        if (cb) cb(std::string("ice:") + name);
    });

    m_impl->pc->onTrack([this](std::shared_ptr<rtc::Track> track) {
        if (!track) return;
        const auto desc = track->description();
        const std::string kind = desc.type();
        logInfo("[RTC] Track mid=" + track->mid() + " kind=" + kind);

        if (kind == "video") {
            auto rh = std::make_shared<LevelUpRtcpHandler>();
            {
                std::lock_guard lock(m_impl->trackMu);
                m_impl->videoTrack = track;
                m_impl->rtcpHandler = rh;
            }
            track->setMediaHandler(rh);
            m_impl->firstVideoTrackMs.store(unixMillisNow());

            track->onMessage(
                [this](rtc::binary bin) {
                    levelup::video::ParsedRtpPacket rtp;
                    if (!levelup::video::parseRtpPacket(
                            reinterpret_cast<const uint8_t*>(bin.data()), bin.size(), rtp)) {
                        return;
                    }
                    VideoRtpCallback cb;
                    {
                        std::lock_guard lock(m_impl->cbMu);
                        cb = m_impl->onVideoRtp;
                    }
                    if (cb) {
                        cb(rtp.seq, rtp.payloadType, rtp.payload, rtp.payloadLen,
                           rtp.marker, rtp.timestamp);
                    }
                },
                nullptr);
        } else if (kind == "audio") {
            auto audioRtcp = std::make_shared<rtc::RtcpReceivingSession>();
            track->setMediaHandler(audioRtcp);
            track->onMessage(
                [this](rtc::binary bin) {
                    levelup::video::ParsedRtpPacket rtp;
                    if (!levelup::video::parseRtpPacket(
                            reinterpret_cast<const uint8_t*>(bin.data()), bin.size(), rtp)) {
                        return;
                    }
                    AudioOpusCallback cb;
                    {
                        std::lock_guard lock(m_impl->cbMu);
                        cb = m_impl->onAudioOpus;
                    }
                    if (cb && rtp.payloadLen > 0) {
                        cb(rtp.payloadType, rtp.payload, rtp.payloadLen, rtp.seq, rtp.timestamp);
                    }
                },
                nullptr);
        }
    });

    m_impl->pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
        if (!dc) return;
        logInfo("[RTC] DataChannel: " + dc->label());
        if (dc->label() != "control") {
            return;
        }
        {
            std::lock_guard lock(m_impl->dcMu);
            m_impl->controlDc = dc;
        }

        dc->onMessage([this](rtc::message_variant msg) {
            if (!std::holds_alternative<rtc::binary>(msg)) {
                m_impl->dcRecvCount.fetch_add(1);
                return;
            }
            const auto& bin = std::get<rtc::binary>(msg);
            const auto* data = reinterpret_cast<const uint8_t*>(bin.data());
            const size_t len = bin.size();

            uint64_t sentMs = 0;
            if (input::isHeartbeatEcho1537(data, len, &sentMs)) {
                const uint64_t n = m_impl->dcPingEchoCount.fetch_add(1) + 1;
                const int64_t uptimeMs = std::max<int64_t>(0, unixMillisNow() - m_impl->startMs.load());
                const int64_t rtt = uptimeMs - static_cast<int64_t>(sentMs);
                if (rtt >= 0 && rtt < 10000) {
                    m_impl->lastRttMs.store(rtt);
                }
                if (n == 1) {
                    logInfo("[RTC] DC 1537 first echo RTT=" + std::to_string(rtt) + "ms");
                }
                return;
            }

            input::RumbleEvent rumble;
            if (input::parseRumble(data, len, rumble)) {
                RumbleCallback cb;
                {
                    std::lock_guard lock(m_impl->cbMu);
                    cb = m_impl->onRumble;
                }
                if (cb) cb(rumble);
                return;
            }

            m_impl->dcRecvCount.fetch_add(1);
        });

        dc->onOpen([this]() {
            logInfo("[RTC] control DataChannel open");
            m_impl->sendControlReadyHandshake();
        });

        if (dc->isOpen()) {
            logInfo("[RTC] control DataChannel already open");
            m_impl->sendControlReadyHandshake();
        }
    });

    rtc::WebSocket::Configuration wsCfg;
    wsCfg.disableTlsVerification = m_impl->cfg.noTlsVerify;
    wsCfg.pingInterval = std::chrono::seconds(20);
    wsCfg.connectionTimeout = std::chrono::milliseconds(timeoutMs);

    m_impl->ws = std::make_shared<rtc::WebSocket>(wsCfg);

    m_impl->ws->onOpen([this]() {
        m_impl->wsConnected.store(true);
        m_impl->wsOpenDone.store(true);
        m_impl->stateCv.notify_all();
    });

    m_impl->ws->onError([this](std::string err) {
        {
            std::lock_guard lock(m_impl->stateMu);
            if (!m_impl->wsOpenDone.load() && m_impl->wsOpenError.empty()) {
                m_impl->wsOpenError = err;
            }
        }
        m_impl->wsOpenDone.store(true);
        m_impl->stateCv.notify_all();
    });

    m_impl->ws->onClosed([this]() {
        const bool wasConn = m_impl->wsConnected.exchange(false);
        if (!m_impl->wsOpenDone.load()) {
            std::lock_guard lock(m_impl->stateMu);
            if (m_impl->wsOpenError.empty()) {
                m_impl->wsOpenError = "signaling WebSocket closed before open";
            }
            m_impl->wsOpenDone.store(true);
        }
        if (wasConn && !m_impl->closing.load()) {
            m_impl->markDead("边缘信令 WebSocket 已断开");
        }
        m_impl->stateCv.notify_all();
    });

    m_impl->ws->onMessage([this](rtc::message_variant msg) {
        if (std::holds_alternative<std::string>(msg)) {
            m_impl->handleSignalingText(std::get<std::string>(msg));
        }
    });

    const std::string wsUrl = "wss://" + m_impl->cfg.edgeIp + ":" +
                              std::to_string(m_impl->cfg.signalingPort) + "/";
    try {
        m_impl->ws->open(wsUrl);
    } catch (const std::exception& e) {
        if (errOut) *errOut = std::string("signaling WS open exception: ") + e.what();
        close();
        return false;
    }

    {
        std::unique_lock lock(m_impl->stateMu);
        const bool ok = m_impl->stateCv.wait_for(
            lock, std::chrono::milliseconds(timeoutMs),
            [this]() { return m_impl->wsOpenDone.load(); });
        if (!ok || !m_impl->wsConnected.load()) {
            if (errOut) {
                *errOut = m_impl->wsOpenError.empty() ? "signaling WebSocket connect timeout/failed"
                                                      : m_impl->wsOpenError;
            }
            lock.unlock();
            close();
            return false;
        }
    }

    const nlohmann::json joinMsg = {
        {"uid", m_impl->cfg.uid},
        {"join", "{}"},
        {"aSessionId", m_impl->cfg.aSessionId},
        {"vSessionId", m_impl->cfg.vSessionId},
        {"cSessionId", m_impl->cfg.cSessionId},
        {"token", ""},
        {"disableEncryption", false},
        {"clientSupportHevc", 0},
    };
    if (!m_impl->sendSignalJson(joinMsg)) {
        if (errOut) *errOut = "failed to send signaling join";
        close();
        return false;
    }

    m_impl->bgStop.store(false);
    m_impl->bgThread = std::thread([this]() { m_impl->bgLoop(); });
    return true;
}

void RtcSession::close() {
    m_impl->closing.store(true);
    m_impl->bgStop.store(true);
    m_impl->bgCv.notify_all();
    if (m_impl->bgThread.joinable()) {
        m_impl->bgThread.join();
    }

    {
        std::lock_guard lock(m_impl->trackMu);
        m_impl->videoTrack.reset();
        m_impl->rtcpHandler.reset();
    }
    {
        std::lock_guard lock(m_impl->dcMu);
        if (m_impl->controlDc) {
            try {
                m_impl->controlDc->close();
            } catch (...) {
            }
            m_impl->controlDc.reset();
        }
    }
    if (m_impl->ws) {
        std::lock_guard lock(m_impl->wsWriteMu);
        try {
            m_impl->ws->forceClose();
        } catch (...) {
        }
        m_impl->ws.reset();
    }
    if (m_impl->pc) {
        try {
            m_impl->pc->close();
        } catch (...) {
        }
        m_impl->pc.reset();
    }
    m_impl->wsConnected.store(false);
}

void RtcSession::notifyVideoFrameDecoded() {
    m_impl->gotFirstFrame.store(true);
}

void RtcSession::requestKeyFrame() {
    const int64_t nowMs = unixMillisNow();
    int64_t prev = m_impl->lastKeyReqMs.load();
    if (prev > 0 && (nowMs - prev) < 300) {
        return;
    }
    if (!m_impl->lastKeyReqMs.compare_exchange_strong(prev, nowMs)) {
        return;
    }
    if (auto rh = m_impl->getRtcpHandler()) {
        rh->sendPliAndFir();
    }
}

void RtcSession::sendNack(const std::vector<uint16_t>& missingSeqs) {
    if (auto rh = m_impl->getRtcpHandler()) {
        rh->sendNack(missingSeqs);
    }
}

bool RtcSession::sendControlPacket(const uint8_t* data, size_t len) {
    return m_impl->sendControl(data, len);
}

bool RtcSession::sendControlPacket(const std::vector<uint8_t>& pkt) {
    return m_impl->sendControl(pkt.data(), pkt.size());
}

bool RtcSession::isControlConnected() const {
    std::lock_guard lock(m_impl->dcMu);
    return m_impl->controlDc && m_impl->controlDc->isOpen();
}

bool RtcSession::isDead(std::string* reasonOut) const {
    if (!m_impl->dead.load()) {
        return false;
    }
    if (reasonOut) {
        std::lock_guard lock(m_impl->stateMu);
        *reasonOut = m_impl->deadReason;
    }
    return true;
}

RtcStatsSnapshot RtcSession::stats() const {
    RtcStatsSnapshot snap;
    if (auto rh = m_impl->getRtcpHandler()) {
        snap.rtpPacketsRecv = rh->packetsReceived();
        snap.rtpPacketsLost = rh->totalLostEstimate();
        snap.nackSent = rh->nackSent();
        snap.pliSent = rh->pliSent();
        snap.firSent = rh->firSent();
        snap.videoSsrc = rh->videoSsrc();
    }
    snap.dcRecvCount = m_impl->dcRecvCount.load();
    snap.rttMs = m_impl->lastRttMs.load();
    snap.controlOpen = isControlConnected();
    return snap;
}

#endif // LEVELUP_NO_RTC

} // namespace levelup::net
