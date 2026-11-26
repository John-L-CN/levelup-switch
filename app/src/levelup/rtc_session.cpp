#include "levelup/rtc_session.hpp"

#include "levelup/crypto_util.hpp"
#include "levelup/h264_depacketizer.hpp"
#include "levelup/log.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>
#include <vector>

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

RtcSession::RtcSession(RtcConfig cfg) : m_impl(std::make_shared<Impl>()) {
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
void RtcSession::beginShutdown() {}
void RtcSession::notifyVideoFrameDecoded() {}
void RtcSession::requestKeyFrame() {}
void RtcSession::sendNack(const std::vector<uint16_t>&) {}
bool RtcSession::sendControlPacket(const uint8_t*, size_t) { return false; }
bool RtcSession::sendControlPacket(const std::vector<uint8_t>&) { return false; }
bool RtcSession::isControlConnected() const { return false; }
bool RtcSession::isDead(std::string*) const { return false; }
RtcStatsSnapshot RtcSession::stats() const { return {}; }
void silenceRtcLoggingForExit() {}
bool hasActiveRtcRuntime() { return false; }

#else

namespace {

std::atomic<bool> g_rtcLogSilenced{false};
std::atomic<bool> g_rtcRuntimeActive{false};

void initRtcLoggingOnce() {
    g_rtcRuntimeActive.store(true, std::memory_order_relaxed);
    static std::once_flag s_once;
    std::call_once(s_once, []() {
        // Info level preserves ICE/DTLS/SCTP/DataChannel lifecycle logs while avoiding
        // per-heartbeat Debug spam on Switch SD card.
        rtc::InitLogger(rtc::LogLevel::Info, [](rtc::LogLevel level, std::string message) {
            if (g_rtcLogSilenced.load(std::memory_order_relaxed)) {
                return;
            }
            while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
                message.pop_back();
            }
            if (message.empty()) {
                return;
            }
            if (message.find("dropping incoming message, no receive callback") != std::string::npos) {
                return;
            }
            switch (level) {
            case rtc::LogLevel::Fatal:
            case rtc::LogLevel::Error:
                logError("[libRTC] " + message);
                break;
            case rtc::LogLevel::Warning:
                logWarn("[libRTC] " + message);
                break;
            default:
                logInfo("[libRTC] " + message);
                break;
            }
        });
    });
}

/// libdatachannel objects are never destroyed on Horizon OS.
///
/// Session teardown used to hand the retired PeerConnection/WebSocket to a detached
/// thread that reset them 250ms later. On Switch that path is a landmine: the
/// destructors run inside libdatachannel's own noexcept teardown code (usrsctp /
/// juice / mbedtls), and any failure there ends up in std::terminate -> the
/// "[FATAL] std::terminate invoked" abort seen right after "[RTC] close(): ws.reset()
/// deferred", which killed the process and left the cloud instance running.
///
/// close() therefore only *stops* the transports (close()/forceClose(), callbacks
/// detached) and parks the shared_ptrs here for the lifetime of the process. A few
/// hundred KB per session is a cheap price for a teardown that cannot abort the app.
namespace {

std::mutex& graveyardMu() {
    static auto* mu = new std::mutex();
    return *mu;
}

std::vector<std::shared_ptr<void>>& graveyard() {
    // Intentionally heap-allocated and leaked: __cxa_finalize must never
    // destroy retired libdatachannel objects at process exit, because
    // rtc::impl::Init teardown calls std::thread::detach() -> ENOSYS -> std::terminate on Switch.
    static auto* parked = new std::vector<std::shared_ptr<void>>();
    return *parked;
}

template <typename T>
void retire(std::shared_ptr<T> obj) {
    g_rtcRuntimeActive.store(true, std::memory_order_relaxed);
    if (!obj) {
        return;
    }
    std::shared_ptr<void> keep = std::move(obj);
    try {
        std::lock_guard lock(graveyardMu());
        graveyard().push_back(keep);
    } catch (...) {
        // Out of memory while parking: leak the reference on purpose, never destroy it.
        new std::shared_ptr<void>(keep);
    }
}

size_t graveyardSize() {
    std::lock_guard lock(graveyardMu());
    return graveyard().size();
}

constexpr int kMaxSignalingReconnects = 12;
constexpr int kSignalingReconnectMs = 4000;
constexpr int kMediaStallMs = 8000;

} // namespace

void logAnswerSummary(const std::string& sdp) {
    std::istringstream iss(sdp);
    std::string line;
    std::unordered_set<std::string> h264Pts;
    std::unordered_set<std::string> rtxPts;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("m=", 0) == 0 || line.rfind("a=setup:", 0) == 0 ||
            line.rfind("a=group:", 0) == 0 || line.rfind("a=extmap:", 0) == 0) {
            logInfo("[RTC] SDP " + line);
        } else if (line.rfind("a=rtpmap:", 0) == 0) {
            const auto sp = line.find(' ');
            if (sp != std::string::npos && sp > 9) {
                const std::string pt = line.substr(9, sp - 9);
                const std::string codec = line.substr(sp + 1);
                if (codec.find("H264") != std::string::npos) {
                    h264Pts.insert(pt);
                    logInfo("[RTC] SDP " + line);
                } else if (codec.find("rtx/") != std::string::npos) {
                    rtxPts.insert(pt);
                    logInfo("[RTC] SDP " + line);
                }
            }
        } else if (line.rfind("a=rtcp-fb:", 0) == 0) {
            const auto sp = line.find(' ');
            if (sp != std::string::npos && sp > 10 &&
                h264Pts.count(line.substr(10, sp - 10))) {
                logInfo("[RTC] SDP " + line);
            }
        } else if (line.rfind("a=fmtp:", 0) == 0) {
            const auto sp = line.find(' ');
            if (sp != std::string::npos && sp > 7) {
                const std::string pt = line.substr(7, sp - 7);
                if (h264Pts.count(pt) || rtxPts.count(pt)) {
                    logInfo("[RTC] SDP " + line);
                }
            }
        }
    }
}

void logOfferSummary(const std::string& sdp) {
    std::istringstream iss(sdp);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("m=", 0) == 0 || line.rfind("a=group:", 0) == 0 ||
            line.rfind("a=mid:", 0) == 0 || line.rfind("a=setup:", 0) == 0 ||
            line.rfind("a=fingerprint:", 0) == 0 || line.rfind("a=ice-ufrag:", 0) == 0) {
            logInfo("[RTC] Offer " + line);
        }
    }
}

/// Post-Answer sanity audit: with a correct Answer every mid must be present in
/// a=group:BUNDLE and no m-line may carry port 0. Any violation means the
/// libdatachannel side already dropped a track (weak_ptr expiry) and DTLS will
/// never come up, so make that failure loud instead of silent.
void auditAnswerBundle(const std::string& sdp) {
    std::istringstream iss(sdp);
    std::string line;
    std::string bundle;
    std::vector<std::string> mids;
    bool audioRejected = false;
    bool videoRejected = false;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("a=group:BUNDLE", 0) == 0) {
            bundle = line;
        } else if (line.rfind("a=mid:", 0) == 0) {
            mids.push_back(line.substr(6));
        } else if (line.rfind("m=audio 0 ", 0) == 0) {
            audioRejected = true;
        } else if (line.rfind("m=video 0 ", 0) == 0) {
            videoRejected = true;
        }
    }
    if (audioRejected) {
        logError("[RTC] Answer rejected audio (m=audio 0) -> BUNDLE/DTLS transport will fail");
    }
    if (videoRejected) {
        logError("[RTC] Answer rejected video (m=video 0) -> no media will flow");
    }
    if (bundle.empty()) {
        logWarn("[RTC] Answer has no a=group:BUNDLE line");
        return;
    }
    for (const auto& mid : mids) {
        if (bundle.find(" " + mid) == std::string::npos) {
            logError("[RTC] Answer BUNDLE omits mid=" + mid + " (" + bundle + ")");
        }
    }
}

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
///  1. Captures raw transportSend callback so NACK/PLI/FIR/RR/REMB work on RecvOnly tracks
///  2. Forces BOTH PLI and FIR on requestKeyframe (cloud encoder coverage)
///  3. Implements explicit RTCP NACK (PT=205, FMT=1)
///  4. Replaces broken upstream pushRR() with accurate RFC 3550 RR + 20 Mbps REMB
class LevelUpRtcpHandler final : public rtc::RtcpReceivingSession {
  public:
    explicit LevelUpRtcpHandler(bool enableRemb = true, uint32_t rembBitrateBps = 20000000U)
        : m_enableRemb(enableRemb), m_rembBitrateBps(rembBitrateBps) {}

    void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override {
        {
            std::lock_guard lock(m_sendMu);
            m_sendCb = send;
        }

        // Split binary RTP and control RTCP so upstream RtcpReceivingSession keeps RTX unwrapping
        // and mSsrc detection, while never emitting its buggy pushRR() on incoming SRs.
        rtc::message_vector binaryMsgs;
        rtc::message_vector controlMsgs;
        binaryMsgs.reserve(messages.size());
        for (auto& m : messages) {
            if (!m) continue;
            if (m->type == rtc::Message::Control) {
                controlMsgs.push_back(std::move(m));
            } else {
                binaryMsgs.push_back(std::move(m));
            }
        }

        rtc::RtcpReceivingSession::incoming(binaryMsgs, send);

        const uint32_t ssrc = mSsrc;
        if (ssrc != 0) {
            m_videoSsrc.store(ssrc);
        }

        const int64_t nowMs = unixMillisNow();
        {
            std::lock_guard seqLock(m_seqMu);
            for (const auto& m : binaryMsgs) {
                if (!m || m->type != rtc::Message::Binary || m->size() < sizeof(rtc::RtpHeader)) {
                    continue;
                }
                m_bytesRecv.fetch_add(m->size());
                const auto* rtp = reinterpret_cast<const rtc::RtpHeader*>(m->data());
                recordSeqAndJitterLocked(rtp->seqNumber(), rtp->timestamp(), nowMs);
            }
        }

        messages.swap(binaryMsgs);

        for (const auto& m : controlMsgs) {
            if (!m || m->size() < sizeof(rtc::RtcpHeader)) {
                continue;
            }
            const auto* hdr = reinterpret_cast<const rtc::RtcpHeader*>(m->data());
            if (hdr->payloadType() == 200 && m->size() >= rtc::RtcpSr::Size(0)) {
                const auto* sr = reinterpret_cast<const rtc::RtcpSr*>(m->data());
                const uint32_t srSsrc = sr->senderSSRC();
                if (srSsrc != 0) {
                    uint32_t expectedZero = 0;
                    m_videoSsrc.compare_exchange_strong(expectedZero, srSsrc);
                }
                {
                    std::lock_guard seqLock(m_seqMu);
                    m_lastSrNtp = sr->ntpTimestamp();
                    m_lastSrRecvMs = nowMs;
                }
                sendRrAndRemb(send);
            }
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
        uint64_t pli = 0;
        uint64_t fir = 0;
        try {
            std::lock_guard lock(mMutex);
            mSsrc = ssrc;
            pushPLI(send);
            pli = m_pliSent.fetch_add(1) + 1;
            pushFIR(send, {ssrc}, false);
            fir = m_firSent.fetch_add(1) + 1;
        } catch (const std::exception&) {
            return;
        }
        logInfo("[RTC] sent PLI+FIR ssrc=" + std::to_string(ssrc) +
                " pli=" + std::to_string(pli) + " fir=" + std::to_string(fir));
    }

    void sendRrAndRemb(const rtc::message_callback& sendOverride = nullptr) {
        auto send = sendOverride ? sendOverride : copySendCb();
        const uint32_t ssrc = m_videoSsrc.load();
        if (!send || ssrc == 0) {
            return;
        }

        uint8_t fraction = 0;
        uint32_t cumulativeLost = 0;
        uint16_t highestSeq = 0;
        uint16_t seqCycles = 0;
        uint32_t jitter = 0;
        uint64_t lastSrNtp = 0;
        uint32_t dlsr = 0;
        {
            std::lock_guard seqLock(m_seqMu);
            if (!m_seqInit) {
                return;
            }
            const uint64_t expected = (m_cycles + m_maxSeq) - m_baseSeq + 1ULL;
            const uint64_t uniqueRecv = m_uniqueRecv;
            cumulativeLost = static_cast<uint32_t>(
                std::min<uint64_t>(0x7FFFFFULL, expected > uniqueRecv ? (expected - uniqueRecv) : 0ULL));

            const uint64_t expectedDelta = expected >= m_expectedPrior ? (expected - m_expectedPrior) : 0ULL;
            const uint64_t recvDelta = uniqueRecv >= m_receivedPrior ? (uniqueRecv - m_receivedPrior) : 0ULL;
            const uint64_t lostDelta = expectedDelta > recvDelta ? (expectedDelta - recvDelta) : 0ULL;
            if (expectedDelta > 0 && lostDelta > 0) {
                fraction = static_cast<uint8_t>(
                    std::min<uint64_t>(255ULL, (lostDelta * 256ULL) / expectedDelta));
            }
            m_expectedPrior = expected;
            m_receivedPrior = uniqueRecv;

            highestSeq = m_maxSeq;
            seqCycles = static_cast<uint16_t>(m_cycles >> 16);
            jitter = static_cast<uint32_t>(std::max(0.0, m_jitter));
            lastSrNtp = m_lastSrNtp;
            if (m_lastSrRecvMs > 0) {
                const int64_t dtMs = std::max<int64_t>(0, unixMillisNow() - m_lastSrRecvMs);
                dlsr = static_cast<uint32_t>(
                    std::min<uint64_t>(0xFFFFFFFFULL, (static_cast<uint64_t>(dtMs) * 65536ULL) / 1000ULL));
            }
        }

        try {
            auto rrMsg = rtc::make_message(rtc::RtcpRr::SizeWithReportBlocks(1), rtc::Message::Control);
            std::fill(rrMsg->begin(), rrMsg->end(), std::byte{0});
            auto* rr = reinterpret_cast<rtc::RtcpRr*>(rrMsg->data());
            rr->preparePacket(ssrc, 1);
            if (auto* rb = rr->getReportBlock(0)) {
                rb->preparePacket(ssrc, fraction, cumulativeLost, highestSeq, seqCycles, jitter, lastSrNtp, dlsr);
            }
            send(std::move(rrMsg));
            m_rrSent.fetch_add(1);

            if (m_enableRemb && m_rembBitrateBps > 0) {
                auto rembMsg = rtc::make_message(rtc::RtcpRemb::SizeWithSSRCs(1), rtc::Message::Control);
                std::fill(rembMsg->begin(), rembMsg->end(), std::byte{0});
                auto* remb = reinterpret_cast<rtc::RtcpRemb*>(rembMsg->data());
                remb->preparePacket(ssrc, 1, m_rembBitrateBps);
                remb->setSSRC(0, ssrc);
                send(std::move(rembMsg));
                m_rembSent.fetch_add(1);
            }
        } catch (const std::exception&) {
        }
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
        try {
            send(std::move(msg));
            m_nackSent.fetch_add(sorted.size());
        } catch (const std::exception&) {
        }
    }

    uint32_t videoSsrc() const { return m_videoSsrc.load(); }
    uint64_t pliSent() const { return m_pliSent.load(); }
    uint64_t firSent() const { return m_firSent.load(); }
    uint64_t nackSent() const { return m_nackSent.load(); }
    uint64_t packetsReceived() const { return m_packetsRecv.load(); }
    uint64_t bytesReceived() const { return m_bytesRecv.load(); }
    uint64_t rrSent() const { return m_rrSent.load(); }
    uint64_t rembSent() const { return m_rembSent.load(); }

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

    void recordSeqAndJitterLocked(uint16_t seq, uint32_t rtpTs, int64_t arrivalMs) {
        m_packetsRecv.fetch_add(1);

        const int64_t arrivalTicks = arrivalMs * (m_enableRemb ? 90LL : 48LL);
        const int64_t transit = arrivalTicks - static_cast<int64_t>(rtpTs);
        if (m_hasJitterPrev) {
            int64_t d = transit - m_prevTransit;
            if (d < 0) d = -d;
            m_jitter += (static_cast<double>(d) - m_jitter) / 16.0;
        }
        m_prevTransit = transit;
        m_hasJitterPrev = true;

        if (!m_seqInit) {
            m_seqInit = true;
            m_baseSeq = seq;
            m_maxSeq = seq;
            m_cycles = 0;
            m_uniqueRecv = 1;
            m_expectedPrior = 0;
            m_receivedPrior = 0;
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
            m_expectedPrior = 0;
            m_receivedPrior = 0;
        }
    }

    const bool m_enableRemb;
    const uint32_t m_rembBitrateBps;

    mutable std::mutex m_sendMu;
    rtc::message_callback m_sendCb;

    mutable std::mutex m_seqMu;
    bool m_seqInit = false;
    uint32_t m_baseSeq = 0;
    uint16_t m_maxSeq = 0;
    uint64_t m_cycles = 0;
    uint64_t m_uniqueRecv = 0;
    uint64_t m_expectedPrior = 0;
    uint64_t m_receivedPrior = 0;
    bool m_hasJitterPrev = false;
    int64_t m_prevTransit = 0;
    double m_jitter = 0.0;
    uint64_t m_lastSrNtp = 0;
    int64_t m_lastSrRecvMs = 0;

    std::atomic<uint32_t> m_videoSsrc{0};
    std::atomic<uint64_t> m_packetsRecv{0};
    std::atomic<uint64_t> m_bytesRecv{0};
    std::atomic<uint64_t> m_pliSent{0};
    std::atomic<uint64_t> m_firSent{0};
    std::atomic<uint64_t> m_nackSent{0};
    std::atomic<uint64_t> m_rrSent{0};
    std::atomic<uint64_t> m_rembSent{0};
};

} // namespace

struct RtcSession::Impl : std::enable_shared_from_this<RtcSession::Impl> {
    RtcConfig cfg;

    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::mutex trackMu;
    std::shared_ptr<rtc::Track> videoTrack;
    std::shared_ptr<LevelUpRtcpHandler> rtcpHandler;
    // NOTE: libdatachannel's PeerConnection only keeps a weak_ptr to every received
    // track, so the track object dies the instant onTrack returns unless we hold a
    // strong reference. Dropping the audio track made populateLocalDescription()
    // treat mid=0 as removed -> "a=group:BUNDLE 1 2" + "m=audio 0", which killed
    // the DTLS/SRTP transport (cloud never answered). Keep both audio handles alive.
    std::shared_ptr<rtc::Track> audioTrack;
    std::shared_ptr<LevelUpRtcpHandler> audioRtcpHandler;

    std::shared_ptr<rtc::Track> getVideoTrack() {
        std::lock_guard lock(trackMu);
        return videoTrack;
    }

    std::shared_ptr<LevelUpRtcpHandler> getRtcpHandler() {
        std::lock_guard lock(trackMu);
        return rtcpHandler;
    }

    std::shared_ptr<rtc::Track> getAudioTrack() {
        std::lock_guard lock(trackMu);
        return audioTrack;
    }

    std::shared_ptr<LevelUpRtcpHandler> getAudioRtcpHandler() {
        std::lock_guard lock(trackMu);
        return audioRtcpHandler;
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
    std::atomic<bool> iceConnected{false};
    std::atomic<bool> loggedFirstVideoRtp{false};
    std::atomic<bool> loggedFirstAudioRtp{false};
    std::atomic<int> remoteCandCount{0};
    std::atomic<int> localCandCount{0};
    std::atomic<int64_t> startMs{0};
    std::atomic<int64_t> firstVideoTrackMs{0};
    std::atomic<int64_t> lastBootPliMs{0};
    std::atomic<int64_t> lastKeyReqMs{0};
    std::atomic<int64_t> lastRttMs{-1};
    std::atomic<uint64_t> dcRecvCount{0};
    std::atomic<uint64_t> dcPingEchoCount{0};

    // Signaling resilience: the edge WebSocket is only used for SDP/candidate exchange.
    // Once SRTP media + the control DataChannel are up, losing it must NOT kill the
    // session (previously it flipped the whole stream to "连接中断" while audio kept
    // playing). We keep streaming and reconnect the socket in the background.
    std::atomic<bool> signalingLost{false};
    std::atomic<int> reconnectAttempts{0};
    std::atomic<bool> joinedOnce{false};
    std::atomic<int64_t> lastReconnectMs{0};
    std::atomic<int64_t> lastVideoRtpMs{0};
    std::atomic<bool> ignoreOffers{false};
    std::string joinPayload;

    std::mutex localCandMu;
    std::vector<std::string> localCandidates;

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

    bool sendJoinPayload() {
        std::lock_guard lock(wsWriteMu);
        if (!ws || !ws->isOpen() || joinPayload.empty()) return false;
        try {
            return ws->send(joinPayload);
        } catch (const std::exception&) {
            return false;
        }
    }

    /// Creates a fresh signaling WebSocket and starts connecting.
    /// Used by start() (with waitForOpen=true) and by the reconnect path in bgLoop().
    bool openSignaling(bool waitForOpen, int timeoutMs, std::string* errOut) {
        std::weak_ptr<Impl> weakImpl = weak_from_this();

        std::shared_ptr<rtc::WebSocket> wsNew;
        try {
            rtc::WebSocket::Configuration wsCfg;
            wsCfg.disableTlsVerification = cfg.noTlsVerify;
            wsCfg.pingInterval = std::chrono::seconds(20);
            wsCfg.connectionTimeout = std::chrono::milliseconds(timeoutMs);
            wsNew = std::make_shared<rtc::WebSocket>(wsCfg);
        } catch (const std::exception& e) {
            if (errOut) *errOut = std::string("signaling WS create exception: ") + e.what();
            return false;
        }

        wsNew->onOpen([weakImpl]() {
            auto impl = weakImpl.lock();
            if (!impl || impl->closing.load()) return;
            impl->wsConnected.store(true);
            impl->wsOpenDone.store(true);
            impl->signalingLost.store(false);
            if (impl->joinedOnce.exchange(true)) {
                // Reconnect: the edge rebinds this signaling socket to the existing
                // media session, so re-send the same join payload.
                impl->stateCv.notify_all();
                if (impl->sendJoinPayload()) {
                    logInfo("[RTC] signaling reconnected (attempt " +
                            std::to_string(impl->reconnectAttempts.load()) + "), join re-sent");
                } else {
                    logWarn("[RTC] signaling reconnected but join re-send failed");
                }
                return;
            }
            impl->stateCv.notify_all();
        });

        wsNew->onError([weakImpl](std::string err) {
            auto impl = weakImpl.lock();
            if (!impl || impl->closing.load()) return;
            if (!impl->wsConnected.load()) {
                std::lock_guard lock(impl->stateMu);
                if (!impl->wsOpenDone.load() && impl->wsOpenError.empty()) {
                    impl->wsOpenError = err;
                }
            }
            impl->wsOpenDone.store(true);
            impl->stateCv.notify_all();
        });

        wsNew->onClosed([weakImpl]() {
            auto impl = weakImpl.lock();
            if (!impl || impl->closing.load()) return;
            const bool wasConn = impl->wsConnected.exchange(false);
            if (!impl->wsOpenDone.load()) {
                std::lock_guard lock(impl->stateMu);
                if (impl->wsOpenError.empty()) {
                    impl->wsOpenError = "signaling WebSocket closed before open";
                }
                impl->wsOpenDone.store(true);
            }
            if (wasConn) {
                // Signaling death is only fatal when there is no media yet.
                const bool mediaUp = impl->iceConnected.load() && impl->gotFirstFrame.load();
                if (mediaUp) {
                    impl->signalingLost.store(true);
                    logWarn("[RTC] signaling WebSocket closed; media path keeps running, "
                            "scheduling reconnect");
                } else {
                    impl->markDead("边缘信令 WebSocket 已断开");
                }
            }
            impl->stateCv.notify_all();
        });

        wsNew->onMessage([weakImpl](rtc::message_variant msg) {
            auto impl = weakImpl.lock();
            if (!impl || impl->closing.load()) return;
            if (std::holds_alternative<std::string>(msg)) {
                impl->handleSignalingText(std::get<std::string>(msg));
            }
        });

        const std::string wsUrl =
            "wss://" + cfg.edgeIp + ":" + std::to_string(cfg.signalingPort) + "/";
        try {
            wsNew->open(wsUrl);
        } catch (const std::exception& e) {
            if (errOut) *errOut = std::string("signaling WS open exception: ") + e.what();
            return false;
        }

        {
            std::shared_ptr<rtc::WebSocket> previous;
            {
                std::lock_guard lock(wsWriteMu);
                previous = std::move(ws);
                ws = wsNew;
            }
            if (previous) {
                try {
                    previous->onOpen(nullptr);
                    previous->onError(nullptr);
                    previous->onClosed(nullptr);
                    previous->onMessage(nullptr);
                    previous->forceClose();
                } catch (...) {
                }
                retire(std::move(previous));
            }
        }

        if (!waitForOpen) {
            return true;
        }

        std::unique_lock lock(stateMu);
        const bool ok = stateCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                        [this]() { return wsOpenDone.load(); });
        if (!ok || !wsConnected.load()) {
            if (errOut) {
                *errOut = wsOpenError.empty() ? "signaling WebSocket connect timeout/failed"
                                              : wsOpenError;
            }
            return false;
        }
        return true;
    }

    void sendLocalCandidateToAllMids(const std::string& candLine) {
        std::string curPeer;
        {
            std::lock_guard lock(peerMu);
            curPeer = peer;
        }
        static constexpr const char* kMids[] = {"0", "1", "2"};
        for (int idx = 0; idx < 3; ++idx) {
            const nlohmann::json msg = {
                {"candidate",
                 {{"sdpMLineIndex", idx}, {"sdpMid", kMids[idx]}, {"candidate", candLine}}},
                {"uid", cfg.uid},
                {"peer", curPeer},
                {"token", ""},
            };
            sendSignalJson(msg);
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
            logOfferSummary(offerSdp);
            rtc::Description offer(offerSdp, rtc::Description::Type::Offer);
            pc->setRemoteDescription(offer);
            {
                int mLines = 0;
                int candLines = 0;
                std::istringstream ss(offerSdp);
                std::string l;
                while (std::getline(ss, l)) {
                    if (l.rfind("m=", 0) == 0) ++mLines;
                    else if (l.rfind("a=candidate:", 0) == 0) ++candLines;
                }
                logInfo("[RTC] remote offer applied: m-lines=" + std::to_string(mLines) +
                        " candidates=" + std::to_string(candLines));
            }

            auto stripTwccExtmaps = [](rtc::Description::Media& media) {
                std::vector<int> rmIds;
                for (int id : media.extIds()) {
                    const auto* em = media.extMap(id);
                    if (em && em->uri.find("transport-wide-cc") != std::string::npos) {
                        rmIds.push_back(id);
                    }
                }
                for (int id : rmIds) {
                    media.removeExtMap(id);
                }
            };

            if (auto vt = getVideoTrack()) {
                auto desc = vt->description();
                bool hasMode1H264 = false;
                for (int pt : desc.payloadTypes()) {
                    const auto* rm = desc.rtpMap(pt);
                    if (!rm || !equalsIgnoreCase(rm->format, "H264")) {
                        continue;
                    }
                    for (const auto& f : rm->fmtps) {
                        if (f.find("packetization-mode=1") != std::string::npos) {
                            hasMode1H264 = true;
                            break;
                        }
                    }
                    if (hasMode1H264) break;
                }

                std::vector<int> ptsToRemove;
                for (int pt : desc.payloadTypes()) {
                    const auto* rm = desc.rtpMap(pt);
                    if (!rm) continue;
                    if (equalsIgnoreCase(rm->format, "rtx")) {
                        continue;
                    }
                    if (!equalsIgnoreCase(rm->format, "H264")) {
                        ptsToRemove.push_back(pt);
                        continue;
                    }
                    if (hasMode1H264) {
                        bool isMode1 = false;
                        for (const auto& f : rm->fmtps) {
                            if (f.find("packetization-mode=1") != std::string::npos) {
                                isMode1 = true;
                                break;
                            }
                        }
                        if (!isMode1) {
                            ptsToRemove.push_back(pt);
                        }
                    }
                }
                for (int pt : ptsToRemove) {
                    desc.removeRtpMap(pt);
                }

                for (int pt : desc.payloadTypes()) {
                    auto* rm = desc.rtpMap(pt);
                    if (!rm || !equalsIgnoreCase(rm->format, "H264")) {
                        continue;
                    }
                    rm->removeFeedback("transport-cc");
                    rm->removeFeedback("rrtr");
                    static constexpr const char* kFeedbacks[] = {
                        "nack", "nack pli", "ccm fir", "goog-remb",
                    };
                    for (const char* fb : kFeedbacks) {
                        if (!rm->hasFeedback(fb)) {
                            rm->addFeedback(fb);
                        }
                    }
                }
                desc.removeFeedback("transport-cc");
                desc.removeFeedback("rrtr");
                stripTwccExtmaps(desc);
                vt->setDescription(std::move(desc));
            }

            if (auto at = getAudioTrack()) {
                auto desc = at->description();
                for (int pt : desc.payloadTypes()) {
                    if (auto* rm = desc.rtpMap(pt)) {
                        rm->removeFeedback("transport-cc");
                        rm->removeFeedback("rrtr");
                    }
                }
                desc.removeFeedback("transport-cc");
                desc.removeFeedback("rrtr");
                stripTwccExtmaps(desc);
                at->setDescription(std::move(desc));
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
                    if (ignoreOffers.load()) {
                        // Media is already flowing; a re-offer after signaling reconnect
                        // would disturb the established transport, so ignore it.
                        logInfo("[RTC] re-offer ignored (media already established)");
                    } else {
                        handleOffer(sdp);
                    }
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
                    rtc::Candidate rcand(cs, mid);
                    const int n = remoteCandCount.fetch_add(1) + 1;
                    pc->addRemoteCandidate(rcand);
                    // Note: the Candidate object built here is always "unresolved" until
                    // libjuice resolves it inside addRemoteCandidate, so printing
                    // isResolved() before the call only ever yielded a misleading 0.
                    if (n <= 16) {
                        logInfo("[RTC] remote candidate #" + std::to_string(n) +
                                " mid=" + mid + " applied " + cs);
                    }
                } catch (const std::exception& e) {
                    logWarn(std::string("[RTC] addRemoteCandidate rejected (") + cs + "): " + e.what());
                }
            }
        }
    }

    void bgLoop() {
        int64_t tick = 0;
        uint64_t prevLost = 0;
        uint64_t prevAliveBytes = 0;
        int64_t prevAliveTs = 0;
        int64_t prevHbMs = 0;
        int64_t prevBootPliMs = 0;
        int64_t prevAliveMs = 0;
        int cursorKickIndex = 0;
        int candResendIdx = 0;
        static constexpr int kCursorKickMs[] = {1000, 3000, 6000, 10000, 16000};
        static constexpr int kCandResendMs[] = {1500, 4000, 7500};

        while (!bgStop.load() && !closing.load()) {
            const int64_t nowMs = unixMillisNow();

            // Liveness heartbeat: distinguishes "app hung" from "app crashed" when the
            // tail of levelup.log is all we get back from the console.
            if (prevAliveMs == 0 || (nowMs - prevAliveMs) >= 5000) {
                prevAliveMs = nowMs;
                const int64_t uptime = std::max<int64_t>(0, nowMs - startMs.load());
                bool ctlOpen = false;
                {
                    std::lock_guard lock(dcMu);
                    ctlOpen = controlDc && controlDc->isOpen();
                }
                uint64_t rtpRecv = 0;
                uint64_t lost = 0;
                uint64_t rrSent = 0;
                uint64_t rembSent = 0;
                uint64_t pliSent = 0;
                uint64_t nackSent = 0;
                uint64_t bytesRecv = 0;
                if (auto rh = getRtcpHandler()) {
                    rtpRecv = rh->packetsReceived();
                    lost = rh->totalLostEstimate();
                    rrSent = rh->rrSent();
                    rembSent = rh->rembSent();
                    pliSent = rh->pliSent();
                    nackSent = rh->nackSent();
                    bytesRecv = rh->bytesReceived();
                }
                uint64_t kbps = 0;
                if (prevAliveTs > 0 && nowMs > prevAliveTs && bytesRecv >= prevAliveBytes) {
                    kbps = ((bytesRecv - prevAliveBytes) * 8ULL) /
                           static_cast<uint64_t>(nowMs - prevAliveTs);
                }
                prevAliveBytes = bytesRecv;
                prevAliveTs = nowMs;
                logInfo("[RTC] alive t=" + std::to_string(uptime) +
                        "ms ice=" + std::string(iceConnected.load() ? "1" : "0") +
                        " ctl=" + std::string(ctlOpen ? "1" : "0") +
                        " rtt=" + std::to_string(lastRttMs.load()) +
                        "ms echo=" + std::to_string(dcPingEchoCount.load()) +
                        " dcRecv=" + std::to_string(dcRecvCount.load()) +
                        " rtp=" + std::to_string(rtpRecv) +
                        " kbps=" + std::to_string(kbps) +
                        " lost=" + std::to_string(lost) +
                        " rr=" + std::to_string(rrSent) +
                        " remb=" + std::to_string(rembSent) +
                        " pli=" + std::to_string(pliSent) +
                        " nack=" + std::to_string(nackSent) +
                        " sig=" + std::string(wsConnected.load() ? "1" : "0") +
                        " rec=" + std::to_string(reconnectAttempts.load()) +
                        " frame=" + std::string(gotFirstFrame.load() ? "1" : "0"));
            }

            // Retry cached local ICE candidates across all m-lines while ICE is still connecting
            if (!iceConnected.load() && candResendIdx < 3 &&
                (nowMs - startMs.load()) >= kCandResendMs[candResendIdx]) {
                std::vector<std::string> snapshot;
                {
                    std::lock_guard lock(localCandMu);
                    snapshot = localCandidates;
                }
                if (!snapshot.empty()) {
                    logInfo("[RTC] resending " + std::to_string(snapshot.size()) +
                            " local candidates (round " + std::to_string(candResendIdx + 1) + ")");
                    for (const auto& c : snapshot) {
                        sendLocalCandidateToAllMids(c);
                    }
                }
                candResendIdx++;
            }

            // Pre-first-frame boost: send PLI+FIR every 500ms for first 8s, then every 1500ms until first frame decodes
            const int64_t trackMs = firstVideoTrackMs.load();
            if (trackMs > 0 && !gotFirstFrame.load()) {
                const int64_t pliInterval = ((nowMs - trackMs) < 8000) ? 500 : 1500;
                if (prevBootPliMs == 0 || (nowMs - prevBootPliMs) >= pliInterval) {
                    prevBootPliMs = nowMs;
                    if (auto rh = getRtcpHandler()) {
                        rh->sendPliAndFir();
                    }
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

            // Every 1s (5x200ms ticks): send accurate RR+REMB and poll RTP packet loss delta -> Layer 1 callback
            if ((tick % 5) == 0) {
                if (auto rh = getRtcpHandler()) {
                    rh->sendRrAndRemb();
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
                if (auto arh = getAudioRtcpHandler()) {
                    arh->sendRrAndRemb();
                }
            }

            // Signaling reconnect: the edge WebSocket only carries SDP/candidates, so a
            // drop must not kill a session whose SRTP media is still flowing.
            if (!closing.load() && !dead.load() && !wsConnected.load()) {
                const int attempt = reconnectAttempts.load();
                const int64_t lastTry = lastReconnectMs.load();
                if (attempt < kMaxSignalingReconnects &&
                    (lastTry == 0 || (nowMs - lastTry) >= kSignalingReconnectMs)) {
                    lastReconnectMs.store(nowMs);
                    reconnectAttempts.store(attempt + 1);
                    wsOpenDone.store(false);
                    {
                        std::lock_guard lock(stateMu);
                        wsOpenError.clear();
                    }
                    logInfo("[RTC] signaling reconnect attempt " + std::to_string(attempt + 1) +
                            "/" + std::to_string(kMaxSignalingReconnects) +
                            " (media=" + std::string(iceConnected.load() ? "up" : "down") + ")");
                    std::string reconErr;
                    if (!openSignaling(false, 8000, &reconErr)) {
                        logWarn("[RTC] signaling reconnect failed: " + reconErr);
                    }
                }
            }

            // Media stall watchdog: signaling drops are survivable, a starved media path is not.
            if (gotFirstFrame.load() && !dead.load()) {
                const int64_t lastRtp = lastVideoRtpMs.load();
                if (lastRtp > 0 && (nowMs - lastRtp) >= kMediaStallMs) {
                    markDead("视频流已中断 (" + std::to_string((nowMs - lastRtp) / 1000) + "s 无 RTP)");
                }
            }

            tick++;
            std::unique_lock lock(bgMu);
            bgCv.wait_for(lock, std::chrono::milliseconds(200),
                          [this]() { return bgStop.load() || closing.load(); });
        }
    }
};

RtcSession::RtcSession(RtcConfig cfg) : m_impl(std::make_shared<Impl>()) {
    m_impl->cfg = std::move(cfg);
}

RtcSession::~RtcSession() {
    close();
    if (m_impl) {
        std::lock_guard lock(m_impl->cbMu);
        m_impl->onVideoRtp = nullptr;
        m_impl->onAudioOpus = nullptr;
        m_impl->onState = nullptr;
        m_impl->onPacketLoss = nullptr;
        m_impl->onRumble = nullptr;
    }
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
    initRtcLoggingOnce();

    m_impl->closing.store(false);
    m_impl->dead.store(false);
    m_impl->deadReason.clear();
    m_impl->wsOpenDone.store(false);
    m_impl->wsConnected.store(false);
    m_impl->wsOpenError.clear();
    m_impl->dcReadyFired.store(false);
    m_impl->gotFirstFrame.store(false);
    m_impl->iceConnected.store(false);
    m_impl->loggedFirstVideoRtp.store(false);
    m_impl->loggedFirstAudioRtp.store(false);
    m_impl->remoteCandCount.store(0);
    m_impl->localCandCount.store(0);
    m_impl->firstVideoTrackMs.store(0);
    m_impl->startMs.store(unixMillisNow());
    m_impl->lastKeyReqMs.store(0);
    m_impl->signalingLost.store(false);
    m_impl->reconnectAttempts.store(0);
    m_impl->joinedOnce.store(false);
    m_impl->lastReconnectMs.store(0);
    m_impl->lastVideoRtpMs.store(0);
    m_impl->ignoreOffers.store(false);
    m_impl->joinPayload.clear();
    {
        std::lock_guard lock(m_impl->localCandMu);
        m_impl->localCandidates.clear();
    }

    rtc::Configuration pcCfg;
    pcCfg.disableAutoNegotiation = true;
    // 优先用 union apply 下发的区域专用 STUN，再退回公共 STUN（与原客户端一致）
    if (!m_impl->cfg.areaStunHost.empty() && m_impl->cfg.areaStunPort > 0) {
        pcCfg.iceServers.emplace_back("stun:" + m_impl->cfg.areaStunHost + ":" +
                                      std::to_string(m_impl->cfg.areaStunPort));
    }
    pcCfg.iceServers.emplace_back("stun:111.206.174.2:3478");
    pcCfg.iceServers.emplace_back("stun:stun.hitv.com:3478");
    pcCfg.iceServers.emplace_back("stun:stun.chat.bilibili.com:3478");

    m_impl->pc = std::make_shared<rtc::PeerConnection>(pcCfg);
    std::weak_ptr<Impl> weakImpl = m_impl;

    m_impl->pc->onLocalDescription([weakImpl](rtc::Description desc) {
        auto impl = weakImpl.lock();
        if (!impl || impl->closing.load()) return;
        const std::string sdp = injectH264FeedbackAndProfile(desc.generateSdp(), true);
        std::string curPeer;
        {
            std::lock_guard lock(impl->peerMu);
            curPeer = impl->peer;
        }
        const nlohmann::json msg = {
            {"uid", impl->cfg.uid},
            {"peer", curPeer},
            {"token", ""},
            {"answer", {{"type", "answer"}, {"sdp", sdp}}},
        };
        impl->sendSignalJson(msg);
        logInfo("[RTC] SDP Answer sent (" + std::to_string(sdp.size()) + " bytes)");
        logAnswerSummary(sdp);
        auditAnswerBundle(sdp);
    });

    m_impl->pc->onLocalCandidate([weakImpl](rtc::Candidate cand) {
        auto impl = weakImpl.lock();
        if (!impl || impl->closing.load()) return;
        const std::string candLine = ensureCandidatePrefix(cand.candidate());
        const int n = impl->localCandCount.fetch_add(1) + 1;
        logInfo("[RTC] local candidate #" + std::to_string(n) +
                " resolved=" + std::string(cand.isResolved() ? "1" : "0") +
                " " + candLine);
        {
            std::lock_guard lock(impl->localCandMu);
            impl->localCandidates.push_back(candLine);
        }
        impl->sendLocalCandidateToAllMids(candLine);
    });

    m_impl->pc->onGatheringStateChange([weakImpl](rtc::PeerConnection::GatheringState st) {
        auto impl = weakImpl.lock();
        if (!impl || impl->closing.load()) return;
        const char* name = "unknown";
        switch (st) {
        case rtc::PeerConnection::GatheringState::New: name = "new"; break;
        case rtc::PeerConnection::GatheringState::InProgress: name = "inprogress"; break;
        case rtc::PeerConnection::GatheringState::Complete: name = "complete"; break;
        }
        logInfo(std::string("[RTC] ICE gathering: ") + name);
    });

    m_impl->pc->onStateChange([weakImpl](rtc::PeerConnection::State st) {
        auto impl = weakImpl.lock();
        if (!impl || impl->closing.load()) return;
        const char* name = "unknown";
        switch (st) {
        case rtc::PeerConnection::State::New: name = "new"; break;
        case rtc::PeerConnection::State::Connecting: name = "connecting"; break;
        case rtc::PeerConnection::State::Connected: name = "connected"; break;
        case rtc::PeerConnection::State::Disconnected:
            name = "disconnected";
            impl->markDead("WebRTC PeerConnection disconnected");
            break;
        case rtc::PeerConnection::State::Failed:
            name = "failed";
            impl->markDead("WebRTC PeerConnection failed");
            break;
        case rtc::PeerConnection::State::Closed:
            name = "closed";
            impl->markDead("WebRTC PeerConnection closed");
            break;
        }
        logInfo(std::string("[RTC] PC state: ") + name);
        StateCallback cb;
        {
            std::lock_guard lock(impl->cbMu);
            cb = impl->onState;
        }
        if (cb) cb(std::string("pc:") + name);
    });

    m_impl->pc->onIceStateChange([weakImpl](rtc::PeerConnection::IceState st) {
        auto impl = weakImpl.lock();
        if (!impl || impl->closing.load()) return;
        const char* name = "unknown";
        switch (st) {
        case rtc::PeerConnection::IceState::New: name = "new"; break;
        case rtc::PeerConnection::IceState::Checking: name = "checking"; break;
        case rtc::PeerConnection::IceState::Connected:
            name = "connected";
            impl->iceConnected.store(true);
            break;
        case rtc::PeerConnection::IceState::Completed:
            name = "completed";
            impl->iceConnected.store(true);
            break;
        case rtc::PeerConnection::IceState::Failed: name = "failed"; break;
        case rtc::PeerConnection::IceState::Disconnected: name = "disconnected"; break;
        case rtc::PeerConnection::IceState::Closed: name = "closed"; break;
        }
        logInfo(std::string("[RTC] ICE state: ") + name);
        StateCallback cb;
        {
            std::lock_guard lock(impl->cbMu);
            cb = impl->onState;
        }
        if (cb) cb(std::string("ice:") + name);
    });

    m_impl->pc->onTrack([weakImpl](std::shared_ptr<rtc::Track> track) {
        auto impl = weakImpl.lock();
        if (!impl || impl->closing.load() || !track) return;
        const auto desc = track->description();
        const std::string kind = desc.type();
        logInfo("[RTC] Track mid=" + track->mid() + " kind=" + kind);

        if (kind == "video") {
            auto rh = std::make_shared<LevelUpRtcpHandler>(true, 20000000U);
            {
                std::lock_guard lock(impl->trackMu);
                impl->videoTrack = track;
                impl->rtcpHandler = rh;
            }
            track->setMediaHandler(rh);
            impl->firstVideoTrackMs.store(unixMillisNow());
            track->onOpen([weakImpl, mid = track->mid()]() {
                auto impl2 = weakImpl.lock();
                if (!impl2 || impl2->closing.load()) return;
                logInfo("[RTC] video track open mid=" + mid);
            });
            track->onError([weakImpl, mid = track->mid()](std::string err) {
                auto impl2 = weakImpl.lock();
                if (!impl2 || impl2->closing.load()) return;
                logWarn("[RTC] video track error mid=" + mid + ": " + err);
            });

            track->onMessage(
                [weakImpl](rtc::binary bin) {
                    auto impl2 = weakImpl.lock();
                    if (!impl2 || impl2->closing.load()) return;
                    levelup::video::ParsedRtpPacket rtp;
                    if (!levelup::video::parseRtpPacket(
                            reinterpret_cast<const uint8_t*>(bin.data()), bin.size(), rtp)) {
                        return;
                    }
                    VideoRtpCallback cb;
                    {
                        std::lock_guard lock(impl2->cbMu);
                        cb = impl2->onVideoRtp;
                    }
                    impl2->lastVideoRtpMs.store(unixMillisNow());
                    if (!impl2->loggedFirstVideoRtp.exchange(true)) {
                        logInfo("[RTC] first video RTP seq=" + std::to_string(rtp.seq) +
                                " pt=" + std::to_string(rtp.payloadType) +
                                " len=" + std::to_string(rtp.payloadLen) +
                                " cb=" + std::to_string(cb ? 1 : 0));
                    }
                    if (cb) {
                        cb(rtp.seq, rtp.payloadType, rtp.payload, rtp.payloadLen,
                           rtp.marker, rtp.timestamp);
                    }
                },
                nullptr);
        } else if (kind == "audio") {
            auto audioRtcp = std::make_shared<LevelUpRtcpHandler>(false, 0);
            {
                std::lock_guard lock(impl->trackMu);
                impl->audioTrack = track;
                impl->audioRtcpHandler = audioRtcp;
            }
            track->setMediaHandler(audioRtcp);
            track->onOpen([weakImpl, mid = track->mid()]() {
                auto impl2 = weakImpl.lock();
                if (!impl2 || impl2->closing.load()) return;
                logInfo("[RTC] audio track open mid=" + mid);
            });
            track->onError([weakImpl, mid = track->mid()](std::string err) {
                auto impl2 = weakImpl.lock();
                if (!impl2 || impl2->closing.load()) return;
                logWarn("[RTC] audio track error mid=" + mid + ": " + err);
            });
            track->onMessage(
                [weakImpl](rtc::binary bin) {
                    auto impl2 = weakImpl.lock();
                    if (!impl2 || impl2->closing.load()) return;
                    levelup::video::ParsedRtpPacket rtp;
                    if (!levelup::video::parseRtpPacket(
                            reinterpret_cast<const uint8_t*>(bin.data()), bin.size(), rtp)) {
                        return;
                    }
                    if (!impl2->loggedFirstAudioRtp.exchange(true)) {
                        logInfo("[RTC] first audio RTP seq=" + std::to_string(rtp.seq) +
                                " pt=" + std::to_string(rtp.payloadType) +
                                " len=" + std::to_string(rtp.payloadLen));
                    }
                    AudioOpusCallback cb;
                    {
                        std::lock_guard lock(impl2->cbMu);
                        cb = impl2->onAudioOpus;
                    }
                    if (cb && rtp.payloadLen > 0) {
                        cb(rtp.payloadType, rtp.payload, rtp.payloadLen, rtp.seq, rtp.timestamp);
                    }
                },
                nullptr);
        }
    });

    m_impl->pc->onDataChannel([weakImpl](std::shared_ptr<rtc::DataChannel> dc) {
        auto impl = weakImpl.lock();
        if (!impl || impl->closing.load() || !dc) return;
        logInfo("[RTC] DataChannel: " + dc->label());
        if (dc->label() != "control") {
            return;
        }
        {
            std::lock_guard lock(impl->dcMu);
            impl->controlDc = dc;
        }

        dc->onMessage([weakImpl](rtc::message_variant msg) {
            auto impl2 = weakImpl.lock();
            if (!impl2 || impl2->closing.load()) return;
            if (!std::holds_alternative<rtc::binary>(msg)) {
                impl2->dcRecvCount.fetch_add(1);
                return;
            }
            const auto& bin = std::get<rtc::binary>(msg);
            const auto* data = reinterpret_cast<const uint8_t*>(bin.data());
            const size_t len = bin.size();

            uint64_t sentMs = 0;
            if (input::isHeartbeatEcho1537(data, len, &sentMs)) {
                const uint64_t n = impl2->dcPingEchoCount.fetch_add(1) + 1;
                const int64_t uptimeMs = std::max<int64_t>(0, unixMillisNow() - impl2->startMs.load());
                const int64_t rtt = uptimeMs - static_cast<int64_t>(sentMs);
                if (rtt >= 0 && rtt < 10000) {
                    impl2->lastRttMs.store(rtt);
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
                    std::lock_guard lock(impl2->cbMu);
                    cb = impl2->onRumble;
                }
                if (cb) cb(rumble);
                return;
            }

            impl2->dcRecvCount.fetch_add(1);
        });

        dc->onOpen([weakImpl]() {
            auto impl2 = weakImpl.lock();
            if (!impl2 || impl2->closing.load()) return;
            logInfo("[RTC] control DataChannel open");
            impl2->sendControlReadyHandshake();
        });

        if (dc->isOpen()) {
            logInfo("[RTC] control DataChannel already open");
            impl->sendControlReadyHandshake();
        }
    });

    if (!m_impl->openSignaling(true, timeoutMs, errOut)) {
        close();
        return false;
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
    m_impl->joinPayload = joinMsg.dump();
    if (!m_impl->sendJoinPayload()) {
        if (errOut) *errOut = "failed to send signaling join";
        close();
        return false;
    }

    m_impl->bgStop.store(false);
    m_impl->bgThread = std::thread([this]() { m_impl->bgLoop(); });
    return true;
}

void RtcSession::close() {
    if (!m_impl) {
        return;
    }
    m_impl->closing.store(true);
    m_impl->bgStop.store(true);
    m_impl->bgCv.notify_all();
    if (m_impl->bgThread.joinable()) {
        m_impl->bgThread.join();
    }

    std::shared_ptr<rtc::PeerConnection> pcToRetire;
    if (m_impl->pc) {
        logInfo("[RTC] close(): pc->close()");
        try {
            m_impl->pc->onLocalDescription(nullptr);
            m_impl->pc->onLocalCandidate(nullptr);
            m_impl->pc->onGatheringStateChange(nullptr);
            m_impl->pc->onStateChange(nullptr);
            m_impl->pc->onIceStateChange(nullptr);
            m_impl->pc->onTrack(nullptr);
            m_impl->pc->onDataChannel(nullptr);
            m_impl->pc->close();
        } catch (...) {
        }
        pcToRetire = std::move(m_impl->pc);
        logInfo("[RTC] close(): pc retired (kept alive)");
    }
    {
        std::shared_ptr<rtc::Track> retiredVideo;
        std::shared_ptr<rtc::Track> retiredAudio;
        std::shared_ptr<LevelUpRtcpHandler> retiredVideoRtcp;
        std::shared_ptr<LevelUpRtcpHandler> retiredAudioRtcp;
        {
            std::lock_guard lock(m_impl->trackMu);
            retiredVideo = std::move(m_impl->videoTrack);
            retiredVideoRtcp = std::move(m_impl->rtcpHandler);
            retiredAudio = std::move(m_impl->audioTrack);
            retiredAudioRtcp = std::move(m_impl->audioRtcpHandler);
            m_impl->videoTrack = nullptr;
            m_impl->rtcpHandler = nullptr;
            m_impl->audioTrack = nullptr;
            m_impl->audioRtcpHandler = nullptr;
        }
        if (retiredVideo) {
            try {
                retiredVideo->onOpen(nullptr);
                retiredVideo->onError(nullptr);
                retiredVideo->onMessage(nullptr);
                retiredVideo->close();
            } catch (...) {
            }
        }
        if (retiredAudio) {
            try {
                retiredAudio->onOpen(nullptr);
                retiredAudio->onError(nullptr);
                retiredAudio->onMessage(nullptr);
                retiredAudio->close();
            } catch (...) {
            }
        }
        // Keep every rtc:: object alive: their destructors abort the app on Horizon OS.
        retire(std::move(retiredVideo));
        retire(std::move(retiredVideoRtcp));
        retire(std::move(retiredAudio));
        retire(std::move(retiredAudioRtcp));
    }
    {
        std::lock_guard lock(m_impl->dcMu);
        if (m_impl->controlDc) {
            try {
                m_impl->controlDc->onOpen(nullptr);
                m_impl->controlDc->onMessage(nullptr);
                m_impl->controlDc->close();
            } catch (...) {
            }
            retire(std::move(m_impl->controlDc));
            m_impl->controlDc = nullptr;
        }
    }
    if (m_impl->ws) {
        logInfo("[RTC] close(): ws->forceClose()");
        std::shared_ptr<rtc::WebSocket> wsToRetire;
        {
            std::lock_guard lock(m_impl->wsWriteMu);
            try {
                m_impl->ws->onOpen(nullptr);
                m_impl->ws->onError(nullptr);
                m_impl->ws->onClosed(nullptr);
                m_impl->ws->onMessage(nullptr);
                m_impl->ws->forceClose();
            } catch (...) {
            }
            wsToRetire = std::move(m_impl->ws);
        }
        m_impl->ws = nullptr;
        retire(std::move(wsToRetire));
        logInfo("[RTC] close(): ws retired (kept alive, no destructor on Switch)");
    }
    m_impl->wsConnected.store(false);
    retire(std::move(pcToRetire));
    logInfo("[RTC] close(): transports parked, graveyard=" + std::to_string(graveyardSize()));
}

void RtcSession::beginShutdown() {
    if (!m_impl) {
        return;
    }
    // Hard-stop media delivery first: no more video/audio callbacks into the pipelines.
    m_impl->closing.store(true);
    m_impl->bgStop.store(true);
    m_impl->bgCv.notify_all();
    if (m_impl->bgThread.joinable()) {
        m_impl->bgThread.join();
    }
    m_impl->wsConnected.store(false);
}

void RtcSession::notifyVideoFrameDecoded() {
    m_impl->gotFirstFrame.store(true);
    // Media is up: a signaling re-offer must no longer touch this transport.
    m_impl->ignoreOffers.store(true);
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
        snap.rtpBytesRecv = rh->bytesReceived();
        snap.rtpPacketsLost = rh->totalLostEstimate();
        snap.nackSent = rh->nackSent();
        snap.pliSent = rh->pliSent();
        snap.firSent = rh->firSent();
        snap.videoSsrc = rh->videoSsrc();
    }
    if (auto arh = m_impl->getAudioRtcpHandler()) {
        snap.rtpBytesRecv += arh->bytesReceived();
    }
    snap.dcRecvCount = m_impl->dcRecvCount.load();
    snap.rttMs = m_impl->lastRttMs.load();
    snap.controlOpen = isControlConnected();
    snap.signalingOpen = m_impl->wsConnected.load();
    snap.signalingReconnects = m_impl->reconnectAttempts.load();
    snap.mediaAlive = m_impl->gotFirstFrame.load() && !m_impl->dead.load();
    return snap;
}

void silenceRtcLoggingForExit() {
    g_rtcLogSilenced.store(true, std::memory_order_relaxed);
}

bool hasActiveRtcRuntime() {
    return g_rtcRuntimeActive.load(std::memory_order_relaxed) || graveyardSize() > 0;
}

#endif // LEVELUP_NO_RTC

} // namespace levelup::net
