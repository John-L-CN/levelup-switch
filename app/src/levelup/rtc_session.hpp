#pragma once

#include "levelup/input_packet.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace levelup::net {

struct RtcConfig {
    std::string edgeIp;
    int signalingPort = 0;
    std::string uid;
    std::string aSessionId;
    std::string vSessionId;
    std::string cSessionId;
    bool noTlsVerify = true;
    bool vqFirst = true;
    int videoLevel = 3;
    // union apply 回显的「该区域专用 STUN」（access_info.area_stun_address/port）。
    // 非空时作为首个 ICE 服务器，打洞命中率与区域一致性都比公共 STUN 好。
    std::string areaStunHost;
    int areaStunPort = 0;
};

struct RtcStatsSnapshot {
    uint64_t rtpPacketsRecv = 0;
    uint64_t rtpBytesRecv = 0;
    uint64_t rtpPacketsLost = 0;
    uint64_t nackSent = 0;
    uint64_t pliSent = 0;
    uint64_t firSent = 0;
    uint64_t dcRecvCount = 0;
    int64_t rttMs = -1;
    uint32_t videoSsrc = 0;
    bool controlOpen = false;
    bool signalingOpen = false;
    int signalingReconnects = 0;
    bool mediaAlive = true;
};

/// Pure helpers (host-testable)
std::string ensureCandidatePrefix(std::string s);
std::string injectH264FeedbackAndProfile(std::string sdp, bool preferHighProfile = true);

/// 退出收尾时静默 libdatachannel 日志回调，避免后台线程在日志文件关闭后继续访问 logger。
void silenceRtcLoggingForExit();
/// 本次运行是否已初始化过 libdatachannel（存在常驻后台线程，退出不能走 post-main 卸载路径）。
bool hasActiveRtcRuntime();

class RtcSession {
  public:
    using VideoRtpCallback =
        std::function<void(uint16_t seq, uint8_t pt, const uint8_t* payload, size_t len, bool marker, uint32_t ts)>;
    using AudioOpusCallback =
        std::function<void(uint8_t pt, const uint8_t* payload, size_t len, uint16_t seq, uint32_t ts)>;
    using StateCallback = std::function<void(const std::string& state)>;
    using PacketLossCallback = std::function<void(int64_t delta)>;
    using RumbleCallback = std::function<void(const input::RumbleEvent& ev)>;

    explicit RtcSession(RtcConfig cfg);
    ~RtcSession();

    RtcSession(const RtcSession&) = delete;
    RtcSession& operator=(const RtcSession&) = delete;

    void setVideoRtpCallback(VideoRtpCallback cb);
    void setAudioOpusCallback(AudioOpusCallback cb);
    void setStateCallback(StateCallback cb);
    void setPacketLossCallback(PacketLossCallback cb);
    void setRumbleCallback(RumbleCallback cb);

    bool start(int timeoutMs = 15000, std::string* errOut = nullptr);
    /// Fast, allocation/exception-free shutdown: stops media delivery and the background
    /// loop so the orchestrator can release the cloud instance. The heavy libdatachannel
    /// teardown stays in close(); it never destroys the transport objects (see close()).
    void beginShutdown();
    void close();

    void notifyVideoFrameDecoded();
    void requestKeyFrame();
    void sendNack(const std::vector<uint16_t>& missingSeqs);
    bool sendControlPacket(const uint8_t* data, size_t len);
    bool sendControlPacket(const std::vector<uint8_t>& pkt);

    bool isControlConnected() const;
    bool isDead(std::string* reasonOut = nullptr) const;
    RtcStatsSnapshot stats() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

} // namespace levelup::net
