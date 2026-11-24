#pragma once

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace levelup::api {

constexpr const char* kBaseURL = "https://client-api.yunshungame.com/api/";
constexpr const char* kSignKey = "84c1f93f61b0c0053952f444853afc4b";
constexpr const char* kApiKey = "8d5e1cf4465e90091ef0029b4618e404";
constexpr const char* kChannelCode = "main";
constexpr const char* kAppVer = "2.2.6";
constexpr const char* kAppVerCode = "35";
constexpr const char* kStreamVer = "1.1.8";
constexpr const char* kStreamVerCode = "18";

struct Credentials {
    std::string sessionKey;
    std::string ciKey;
    std::string secretKey;
    int64_t userId = 0;
    std::string nickName;
    std::string mobile;
};

struct QRCodeResult {
    int type = 0;
    std::string codeId;
    std::string buffer;
};

struct LoginStateResult {
    int state = 0; // 0=unscanned, 1=scanned pending confirm, 2=logged in
    std::string sessionKey;
    std::string ciKey;
    std::string secretKey;
};

struct UserInfo {
    int64_t userId = 0;
    std::string nickName;
    std::string mobile;
    std::string headImg;
    std::string vipExpireTime;
    bool idleTime = false;
    int hpTotalPower = 0;
    int mpTotalPower = 0;
    int hpPower = 0;
    int hpPowerDuration = 0; // seconds
    int mpPower = 0;
    int mpPowerDuration = 0; // seconds

    int hpMinutes() const { return hpPowerDuration > 0 ? (hpPowerDuration / 60) : hpPower; }
    int mpMinutes() const { return mpPowerDuration > 0 ? (mpPowerDuration / 60) : mpPower; }
};

struct GameStartInfo {
    std::string proData;
    int64_t pkgName = 0;
    std::string userId;
    std::string gameName;
    std::string rawJson;
};

struct GameCategoryTag {
    std::string id;
    std::string name;
};

struct GameListItem {
    int gid = 0;
    std::string gameId;
    std::string gameName;
    std::string gameFullName;
    std::string coverUrl;
    std::string titleUrl;
    std::string description;
    std::vector<GameCategoryTag> categories;
    bool collected = false;
};

struct ListGamesResult {
    std::vector<GameListItem> list;
    int count = 0;
};

// Pure protocol / signing helpers (host-testable without network/curl)
std::string buildClientInfoJson(std::string_view deviceId);
std::string signParams(const std::map<std::string, std::string>& params,
                       std::string_view signKey = kSignKey);
std::map<std::string, std::string> buildSignedParams(const nlohmann::json& body,
                                                     std::string_view deviceId,
                                                     const Credentials& cred,
                                                     int64_t timestampMs = 0);
std::string buildSignedPayloadJson(const nlohmann::json& body,
                                   std::string_view deviceId,
                                   const Credentials& cred,
                                   int64_t timestampMs = 0);
bool parseApiEnvelope(std::string_view rawResp,
                      nlohmann::json& outData,
                      std::string* errOut = nullptr);
bool parseUserInfoEnvelope(const nlohmann::json& data, UserInfo& out, std::string* errOut = nullptr);
bool parseGameStartEnvelope(const nlohmann::json& data,
                            std::string_view rawDataJson,
                            GameStartInfo& out,
                            std::string* errOut = nullptr);
bool parseGameListEnvelope(const nlohmann::json& data,
                           ListGamesResult& out,
                           std::string* errOut = nullptr);

class Client {
  public:
    Client() = default;
    Client(std::string deviceId, Credentials cred = {});

    void setDeviceId(std::string deviceId) { m_deviceId = std::move(deviceId); }
    void setCredentials(Credentials cred) { m_cred = std::move(cred); }
    const std::string& deviceId() const { return m_deviceId; }
    const Credentials& credentials() const { return m_cred; }

    bool call(std::string_view path,
              const nlohmann::json& body,
              nlohmann::json* outData,
              std::string* errOut = nullptr,
              std::string* rawDataOut = nullptr) const;

    bool getQRCode(QRCodeResult& out, std::string* errOut = nullptr) const;
    bool pollQRCodeState(std::string_view codeId,
                         LoginStateResult& out,
                         std::string* errOut = nullptr) const;
    bool getUserInfo(UserInfo& out, std::string* errOut = nullptr) const;
    bool listGames(int page, int size, ListGamesResult& out, std::string* errOut = nullptr) const;
    bool getGameDetail(std::string_view gameId, GameListItem& out, std::string* errOut = nullptr) const;
    bool startGame(std::string_view gameId, GameStartInfo& out, std::string* errOut = nullptr) const;
    bool stopGame(std::string_view gameId = {}, std::string* errOut = nullptr) const;

    static bool downloadBinary(std::string_view url,
                               std::vector<uint8_t>& outBytes,
                               std::string* errOut = nullptr,
                               size_t maxBytes = 4 * 1024 * 1024);

  private:
    std::string m_deviceId;
    Credentials m_cred;
};

} // namespace levelup::api
