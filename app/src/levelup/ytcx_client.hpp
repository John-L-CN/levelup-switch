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
#include <utility>
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

// 官方的另外两个 union 端点（lp-sdk com.light.play.config.a）：
//   dialpoints: 同一 union host、端口固定 18088、明文 http、无签名，按用户查全量测速点
//   release:    /v3/union/release，签名同 apply
constexpr const char* kUnionDialPointsURL =
    "http://union-access.yuntiancloud.com:18088/v1/speed/dialpoints";
constexpr const char* kUnionReleaseURL =
    "http://union-access.yuntiancloud.com:18108/v3/union/release";

// 官方多区域测速常量（qualityReport/collection/e.java + areaping_socket/d.java）
constexpr int kDialTimeoutMs = 200;       // 单次探测 >200ms 记失败
constexpr int kDialFailMs = 999;          // 官方失败哨兵值
constexpr size_t kDialCacheEntries = 10;  // 本地缓存只留最近 10 条

struct ApplyResult {
    std::string flowId;
    std::string token;
    std::string accessIp;
    int accessPort = 0;
    int64_t userId = 0;
    // 调度回显：服务端最终把实例放在哪个区域 / 备用边缘入口 / 该区域专用 STUN
    int areaType = 0;
    int idc = 0;
    std::string areaStunAddress;
    int areaStunPort = 0;
    std::vector<std::pair<std::string, int>> spareAddresses;
    // body.client_info.address = 本机出口公网 IP（测速缓存 key 的第一段）
    std::string clientAddress;
    // body.uuid = LP 侧用户号，dialpoints 的 uuid 参数用它
    std::string lpUuid;
    // body.dialpoint.rtt_ws_url = 当前实例自身的测速地址（后台测速时要跳过自己）
    std::string currentDialWsUrl;
    std::string rawJson;
};

// 一条多区域测速记录，对应官方 CacheIpRttBean / dialtest_history_data 的一项。
struct DialSample {
    std::string clientIp;   // 缓存 key 第一段：客户端公网 IP
    int areaType = 0;
    int idc = 0;
    std::string isp;
    std::string country;
    std::string province;
    std::string city;
    std::string rttWsUrl;   // 官方探测目标 ws://host:port/...
    std::string rttPingUrl; // 服务的 rtt_ping_address（curl 不可用时的兜底探测目标）
    int rtt95 = kDialFailMs;
    int64_t ts = 0;         // 毫秒时间戳
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
                                                int64_t timestampMs = 0,
                                                const std::vector<DialSample>& dialHistory = {},
                                                int areaType = 0,
                                                int tryAreaType = 0);

bool parseUnionApplyResponse(std::string_view rawJson,
                             ApplyResult& out,
                             std::string* errOut = nullptr);

std::string extractRealUserIdFromToken(std::string_view token);

// ---- 多区域测速（官方 muti_area_rtt 等价实现，纯函数部分主机可测）----
std::string dialCacheKey(std::string_view clientIp, int areaType, int idc);
int dialP95(std::vector<int> samples);
std::vector<DialSample> trimDialCache(std::vector<DialSample> samples, size_t maxEntries);
std::string dialCacheToJson(const std::vector<DialSample>& samples);
bool dialCacheFromJson(std::string_view json, std::vector<DialSample>& out);
bool parseWsEndpoint(std::string_view url, std::string& host, int& port);
std::string buildDialPointsUrl(std::string_view uuid, int64_t ugid, std::string_view flowId);

std::vector<DialSample> loadDialCache(const std::string& path);
bool saveDialCache(const std::string& path, const std::vector<DialSample>& samples);
// 把新测得的样本并进旧缓存（同 key 覆盖），按 ts 保留最近 maxEntries 条
std::vector<DialSample> mergeDialCache(std::vector<DialSample> cache,
                                      const std::vector<DialSample>& fresh,
                                      size_t maxEntries = kDialCacheEntries);

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
                std::string* errOut = nullptr,
                const std::vector<DialSample>& dialHistory = {},
                int areaType = 0,
                int tryAreaType = 0);

// 释放 union flow（官方 /v3/union/release）。type=1 会顺带踢掉当前在跑的实例。
bool unionRelease(std::string_view flowId,
                  std::string_view lpUuid,
                  std::string_view deviceId,
                  int64_t pkgName,
                  bool kickRunning,
                  std::string* errOut = nullptr);

// 拉全量测速点（body.dial_points）。clientInfoOut 回填 body.client_info.ip_address。
bool dialFetchPoints(std::string_view lpUuid,
                     int64_t pkgName,
                     std::string_view flowId,
                     std::vector<DialSample>& out,
                     std::string* clientInfoOut = nullptr,
                     std::string* errOut = nullptr);

// 单点探测一次 TCP 建连耗时（毫秒），失败返回 -1。走 libcurl CONNECT_ONLY，
// 复用与 BE/边缘 /launch 完全相同的网络栈（Switch 上没有裸 socket 依赖）。
int dialProbeOnceMs(std::string_view host, int port, int timeoutMs = kDialTimeoutMs);

// 逐点探测 attempts 次、丢 >=999 的超时样本后取 p95，写回 sample.rtt95。
// stopFlag 非空时每轮检查一次，退出流程可立刻打断。
std::vector<DialSample> dialProbe(std::vector<DialSample> points,
                                  int attempts = 5,
                                  int timeoutMs = kDialTimeoutMs,
                                  const std::atomic<bool>* stopFlag = nullptr);

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
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
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
