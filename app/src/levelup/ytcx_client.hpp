#pragma once

#include "levelup/proto_lite.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace rtc {
class WebSocket;
}

namespace levelup::ytcx {

constexpr const char* kUnionAccessKey = "8f84a176b3edbe732f3126a9548ebf06";
constexpr const char* kUnionSecretKey = "97c3cfb53bb18714566fd533dde8d415";
constexpr const char* kUnionApplyURL = "https://union-access.yuntiancloud.com:18112/v3/union/apply";
constexpr const char* kUnionAppID = "mvp_114";
constexpr const char* kUnionBizID = "biz-basic";
constexpr const char* kUnionSDKVersion = "7.40.47";
constexpr int64_t kUnionSDKVersionNum = 7040047;

struct ApplyResult {
    std::string flowId;
    std::string token;
    std::string accessIp;
    int accessPort = 0;
    int64_t userId = 0;
    std::string rawJson;
};

struct EdgeLaunchParams {
    std::string edgeIp;
    int basePort = 0;
    std::string flowId;
    std::string userId;
    int64_t pkgName = 0;
    std::string deviceId;
    std::string aSessionId;
    std::string vSessionId;
    std::string cSessionId;
    int width = 1280;
    int height = 720;
    int maxFps = 60;
    int64_t clientId = 0;
    int videoLevel = 3; // 0=LOW..4=Original
    bool vqFirst = true;
    bool vibration = true;
};

struct EdgeLaunchResult {
    int signalingPort = 0;
    int videoPort = 0;
    int audioPort = 0;
    int controlPort = 0;
    std::string remoteAddr;
    std::vector<std::string> supportInputLabels;
    bool disableSctpEncrypt = false;
    int supportRtcAnswerMode = 0;
    int encodedWidth = 0;
    int encodedHeight = 0;
    int desktopWidth = 0;
    int desktopHeight = 0;
    int finalCodec = 0;
};

struct SignedUnionRequest {
    std::string url;
    std::string bodyJson;
    std::string bodyMd5Base64;
    std::string stringToSign;
    std::string signatureBase64;
};

// Pure builders & parsers (host-testable without network/curl/rtc)
std::string buildDeviceInfoJson(int width, int height);
std::string buildStreamInfoJson(int width,
                                int height,
                                int maxFps,
                                int64_t gid,
                                int videoLevel = 3,
                                bool vibration = true);

SignedUnionRequest buildSignedUnionApplyRequest(std::string_view proData,
                                                int64_t pkgName,
                                                std::string_view deviceId,
                                                std::string_view userId,
                                                int width,
                                                int height,
                                                const std::vector<uint8_t>& pbLaunchGame,
                                                std::string_view nonce = {},
                                                int64_t timestampMs = 0);

bool parseUnionApplyResponse(std::string_view rawJson,
                             ApplyResult& out,
                             std::string* errOut = nullptr);

std::string extractRealUserIdFromToken(std::string_view token);

std::string buildEdgeLaunchBodyJson(const EdgeLaunchParams& params);
bool parseEdgeLaunchResponse(std::string_view rawJson,
                             int basePort,
                             EdgeLaunchResult& out,
                             std::string* errOut = nullptr);

// Network calls (CURL)
bool unionApply(std::string_view proData,
                int64_t pkgName,
                std::string_view deviceId,
                std::string_view userId,
                int width,
                int height,
                const std::vector<uint8_t>& pbLaunchGame,
                ApplyResult& out,
                std::string* errOut = nullptr);

bool edgeLaunch(const EdgeLaunchParams& params,
                EdgeLaunchResult& out,
                std::string* errOut = nullptr);

class BEClient {
  public:
    using VPadCallback = std::function<void(int vpadId, int errcode)>;
    using DisconnectCallback = std::function<void(const std::string& reason)>;

    BEClient() = default;
    ~BEClient();

    BEClient(const BEClient&) = delete;
    BEClient& operator=(const BEClient&) = delete;

    bool connect(std::string_view host,
                 int port,
                 std::string_view token,
                 std::string_view deviceId,
                 int timeoutMs = 15000,
                 std::string* errOut = nullptr);

    void close();
    bool isConnected() const { return m_connected.load(); }

    void setVPadCallback(VPadCallback cb);
    void setDisconnectCallback(DisconnectCallback cb);

    bool sendLaunch(const std::vector<uint8_t>& pbLaunchGame, std::string* errOut = nullptr);
    void startHeartbeat();
    void stopHeartbeat();

    bool waitResource(int timeoutMs, GameResource& out, std::string* errOut = nullptr);

    bool sendStreamStatus(bool playing);
    bool sendAllocVPad(std::string_view vpadCert = {}, int padIndex = 65535, int usage = 2);
    bool releaseVPad(int vpadId);
    bool sendExit(int64_t pkgName);

  private:
    bool sendCommand(uint32_t cmd, const std::vector<uint8_t>& payload, std::string* errOut = nullptr);
    bool sendHeartbeat(int64_t seq);
    void handleIncomingBinary(const uint8_t* data, size_t len);
    void heartbeatLoop();

    std::string m_deviceId;
    std::string m_token;
    std::shared_ptr<rtc::WebSocket> m_ws;

    mutable std::mutex m_writeMu;
    mutable std::mutex m_stateMu;
    std::condition_variable m_stateCv;

    std::atomic<bool> m_openDone{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_closing{false};
    std::string m_openError;

    bool m_hasResource = false;
    GameResource m_resource;
    bool m_hasFatalLaunchError = false;
    std::string m_fatalLaunchError;

    std::atomic<int64_t> m_hbPeriodMs{5000};
    std::atomic<int64_t> m_hbAckCount{0};

    std::mutex m_hbMu;
    std::condition_variable m_hbCv;
    std::atomic<bool> m_hbStop{true};
    std::thread m_hbThread;

    std::mutex m_cbMu;
    VPadCallback m_vpadCb;
    DisconnectCallback m_disconnectCb;
};

} // namespace levelup::ytcx
