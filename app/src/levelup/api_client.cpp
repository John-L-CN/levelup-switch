#include "levelup/api_client.hpp"

#include "levelup/crypto_util.hpp"
#include "levelup/log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <vector>

#ifndef LEVELUP_NO_CURL
#include <curl/curl.h>
#endif

namespace levelup::api {
namespace {

std::string jsonEscapeInline(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c >= 0x20) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string jsonScalarToParamString(const nlohmann::json& v) {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "true" : "false";
    }
    if (v.is_number_integer()) {
        return std::to_string(v.get<int64_t>());
    }
    if (v.is_number_unsigned()) {
        return std::to_string(v.get<uint64_t>());
    }
    if (v.is_number_float()) {
        return v.dump();
    }
    if (v.is_null()) {
        return {};
    }
    return v.dump();
}

std::string anyIdToString(const nlohmann::json& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_number_integer()) return std::to_string(j.get<int64_t>());
    if (j.is_number_unsigned()) return std::to_string(j.get<uint64_t>());
    return {};
}

std::string truncateMsg(std::string_view s, size_t n = 200) {
    if (s.size() <= n) return std::string(s);
    return std::string(s.substr(0, n)) + "...";
}

#ifndef LEVELUP_NO_CURL
std::atomic<bool> g_preferInsecureTls{false};

struct CurlBuffer {
    std::vector<uint8_t> data;
    size_t maxBytes = 8 * 1024 * 1024;
    bool overflow = false;
};

size_t curlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t total = size * nmemb;
    auto* buf = static_cast<CurlBuffer*>(userdata);
    if (!buf || total == 0) return total;
    if (buf->data.size() + total > buf->maxBytes) {
        buf->overflow = true;
        return 0;
    }
    const auto* u8 = reinterpret_cast<const uint8_t*>(ptr);
    buf->data.insert(buf->data.end(), u8, u8 + total);
    return total;
}

bool performCurlRequest(const std::string& url,
                        const std::string* postBody,
                        const char* userAgent,
                        bool insecureTls,
                        long timeoutSec,
                        size_t maxBytes,
                        long& outHttpCode,
                        std::vector<uint8_t>& outBody,
                        CURLcode& outCurlCode,
                        std::string* errOut) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (errOut) *errOut = "curl_easy_init failed";
        return false;
    }

    CurlBuffer buf;
    buf.maxBytes = maxBytes;

    struct curl_slist* headers = nullptr;
    if (postBody != nullptr) {
        headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    if (userAgent && userAgent[0] != '\0') {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent);
    }
    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    if (postBody != nullptr) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody->data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(postBody->size()));
    }
    if (insecureTls) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    }

    outCurlCode = curl_easy_perform(curl);
    outHttpCode = 0;
    if (outCurlCode == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &outHttpCode);
        outBody = std::move(buf.data);
    } else if (errOut) {
        *errOut = buf.overflow ? "response exceeded maxBytes" : curl_easy_strerror(outCurlCode);
    }

    if (headers) {
        curl_slist_free_all(headers);
    }
    curl_easy_cleanup(curl);
    return outCurlCode == CURLE_OK;
}
#endif

} // namespace

std::string buildClientInfoJson(std::string_view deviceId) {
    const std::string escaped = jsonEscapeInline(deviceId);
    std::string s;
    s.reserve(380);
    s += R"({"screen":"2560*1600","os":"14","sdkVersion":34,"deviceId":")";
    s += escaped;
    s += R"(","androidId":")";
    s += escaped;
    s += R"(","brand":"Lenovo","model":"TB320FC","manufacturer":"LENOVO","device":"TB320FC","product":"TB320FC_PRC","board":"kalama","hardware":"qcom","incremental":"V1","isAdbEnabled":true,"isDeviceRooted":false,"cpuAbi":"arm64-v8a","networkType":"WIFI","carrier":"","ipAddressIpv4":"","ipAddress":""})";
    return s;
}

std::string signParams(const std::map<std::string, std::string>& params, std::string_view signKey) {
    std::vector<std::string> items;
    items.reserve(params.size());
    for (const auto& [k, val] : params) {
        if (k == "sig" || val.empty()) {
            continue;
        }
        std::string truncated = val.size() > 50 ? val.substr(0, 50) : val;
        items.push_back(k + "=" + truncated);
    }
    std::sort(items.begin(), items.end());
    std::string joined;
    for (const auto& item : items) {
        joined += item;
    }
    joined.append(signKey.data(), signKey.size());
    return md5HexLower(joined);
}

std::map<std::string, std::string> buildSignedParams(const nlohmann::json& body,
                                                     std::string_view deviceId,
                                                     const Credentials& cred,
                                                     int64_t timestampMs) {
    std::map<std::string, std::string> params;
    auto add = [&](const std::string& k, const std::string& v) {
        if (!v.empty()) {
            params[k] = v;
        }
    };

    if (body.is_object()) {
        for (auto it = body.begin(); it != body.end(); ++it) {
            add(it.key(), jsonScalarToParamString(it.value()));
        }
    }

    const int64_t tMs = timestampMs > 0 ? timestampMs : unixMillisNow();
    add("t", std::to_string(tMs));
    add("appVer", kAppVer);
    add("appVerCode", kAppVerCode);
    add("streamingVer", kStreamVer);
    add("streamingVerCode", kStreamVerCode);
    add("clientInfo", buildClientInfoJson(deviceId));
    add("apiKey", kApiKey);
    add("channelCode", kChannelCode);
    add("sessionKey", cred.sessionKey);
    add("ciKey", cred.ciKey);
    params["sig"] = signParams(params, kSignKey);
    return params;
}

std::string buildSignedPayloadJson(const nlohmann::json& body,
                                   std::string_view deviceId,
                                   const Credentials& cred,
                                   int64_t timestampMs) {
    const auto params = buildSignedParams(body, deviceId, cred, timestampMs);
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& [k, v] : params) {
        obj[k] = v;
    }
    return obj.dump();
}

bool parseApiEnvelope(std::string_view rawResp, nlohmann::json& outData, std::string* errOut) {
    nlohmann::json env;
    try {
        env = nlohmann::json::parse(rawResp.begin(), rawResp.end());
    } catch (const std::exception& e) {
        if (errOut) {
            *errOut = std::string("response parse error: ") + e.what() + " (" + truncateMsg(rawResp) + ")";
        }
        return false;
    }
    if (!env.is_object()) {
        if (errOut) *errOut = "response is not JSON object";
        return false;
    }
    const int code = env.value("code", -1);
    if (code != 0) {
        const std::string msg = env.value("msg", "");
        if (errOut) {
            *errOut = "API error " + std::to_string(code) + ": " + msg;
        }
        return false;
    }
    if (env.contains("data")) {
        outData = env["data"];
    } else {
        outData = nlohmann::json{};
    }
    return true;
}

bool parseUserInfoEnvelope(const nlohmann::json& data, UserInfo& out, std::string* errOut) {
    if (!data.is_object()) {
        if (errOut) *errOut = "userInfo data is not object";
        return false;
    }
    out = UserInfo{};
    const auto ui = data.contains("userInfo") && data["userInfo"].is_object() ? data["userInfo"] : nlohmann::json::object();
    const auto pi = data.contains("powerInfo") && data["powerInfo"].is_object() ? data["powerInfo"] : nlohmann::json::object();

    out.userId = ui.value("userId", int64_t{0});
    out.nickName = ui.value("nickName", "");
    out.mobile = ui.value("mobile", "");
    out.headImg = ui.value("headImg", "");

    out.vipExpireTime = pi.value("vipExpireTime", "");
    out.idleTime = pi.value("idleTime", false);
    out.hpTotalPower = pi.value("hpTotalPower", 0);
    out.mpTotalPower = pi.value("mpTotalPower", 0);
    out.hpPower = pi.value("hpPower", 0);
    out.hpPowerDuration = pi.value("hpPowerDuration", 0);
    out.mpPower = pi.value("mpPower", 0);
    out.mpPowerDuration = pi.value("mpPowerDuration", 0);
    return true;
}

bool parseGameStartEnvelope(const nlohmann::json& data,
                            std::string_view rawDataJson,
                            GameStartInfo& out,
                            std::string* errOut) {
    if (!data.is_object()) {
        if (errOut) *errOut = "startGame data is not object";
        return false;
    }
    out = GameStartInfo{};
    out.proData = data.value("proData", "");
    out.gameName = data.value("gameName", "");
    out.rawJson = rawDataJson.empty() ? data.dump() : std::string(rawDataJson);

    if (data.contains("platformInfo") && data["platformInfo"].is_object()) {
        const auto& pi = data["platformInfo"];
        out.userId = pi.value("userId", "");
        if (pi.contains("pkgName")) {
            const auto& pn = pi["pkgName"];
            if (pn.is_number_integer()) {
                out.pkgName = pn.get<int64_t>();
            } else if (pn.is_string()) {
                out.pkgName = std::strtoll(pn.get<std::string>().c_str(), nullptr, 10);
            }
        }
    }
    return !out.proData.empty();
}

namespace {

GameListItem parseGameItem(const nlohmann::json& j, bool detailSchema) {
    GameListItem item;
    item.gid = j.value("gid", 0);
    item.gameId = detailSchema ? j.value("id", "") : j.value("gameId", "");
    if (item.gameId.empty()) {
        item.gameId = detailSchema ? j.value("gameId", "") : j.value("id", "");
    }
    item.gameName = j.value("gameName", "");
    item.gameFullName = j.value("gameFullName", "");
    item.coverUrl = j.value("coverUrl", "");
    item.titleUrl = j.value("titleUrl", "");
    item.description = j.value("description", "");
    item.collected = j.value("collected", false);

    if (j.contains("categories") && j["categories"].is_array()) {
        for (const auto& c : j["categories"]) {
            if (!c.is_object()) continue;
            GameCategoryTag tag;
            if (c.contains("id")) tag.id = anyIdToString(c["id"]);
            tag.name = c.value("name", "");
            if (!tag.name.empty()) {
                item.categories.push_back(std::move(tag));
            }
        }
    }
    return item;
}

} // namespace

bool parseGameListEnvelope(const nlohmann::json& data, ListGamesResult& out, std::string* errOut) {
    if (!data.is_object()) {
        if (errOut) *errOut = "game list data is not object";
        return false;
    }
    out = ListGamesResult{};
    out.count = data.value("count", 0);
    if (data.contains("list") && data["list"].is_array()) {
        for (const auto& j : data["list"]) {
            if (j.is_object()) {
                out.list.push_back(parseGameItem(j, false));
            }
        }
    }
    return true;
}

Client::Client(std::string deviceId, Credentials cred)
    : m_deviceId(std::move(deviceId)), m_cred(std::move(cred)) {}

bool Client::call(std::string_view path,
                  const nlohmann::json& body,
                  nlohmann::json* outData,
                  std::string* errOut,
                  std::string* rawDataOut) const {
#ifdef LEVELUP_NO_CURL
    (void)path;
    (void)body;
    (void)outData;
    (void)rawDataOut;
    if (errOut) *errOut = "curl disabled in host test build";
    return false;
#else
    const std::string payload = buildSignedPayloadJson(body, m_deviceId, m_cred);
    const std::string url = std::string(kBaseURL) + std::string(path);

    long httpCode = 0;
    std::vector<uint8_t> rawBytes;
    CURLcode curlCode = CURLE_OK;
    std::string transportErr;

    const bool startInsecure = g_preferInsecureTls.load(std::memory_order_relaxed);
    bool ok = performCurlRequest(url, &payload, "okhttp/4.12.0", startInsecure, 25L, 8 * 1024 * 1024,
                                 httpCode, rawBytes, curlCode, &transportErr);
    if (!ok && !startInsecure &&
        (curlCode == CURLE_PEER_FAILED_VERIFICATION ||
         curlCode == CURLE_SSL_CACERT_BADFILE ||
         curlCode == CURLE_SSL_CONNECT_ERROR)) {
        g_preferInsecureTls.store(true, std::memory_order_relaxed);
        logWarn("TLS verify failed for " + url + " (" + transportErr + "), switching to insecure fallback");
        ok = performCurlRequest(url, &payload, "okhttp/4.12.0", true, 25L, 8 * 1024 * 1024,
                                httpCode, rawBytes, curlCode, &transportErr);
    }

    if (!ok) {
        if (errOut) *errOut = transportErr;
        return false;
    }

    std::string rawStr(reinterpret_cast<const char*>(rawBytes.data()), rawBytes.size());
    if (httpCode != 200) {
        if (errOut) *errOut = "HTTP " + std::to_string(httpCode) + ": " + truncateMsg(rawStr);
        return false;
    }

    nlohmann::json data;
    if (!parseApiEnvelope(rawStr, data, errOut)) {
        return false;
    }
    if (rawDataOut) {
        *rawDataOut = data.is_null() ? "" : data.dump();
    }
    if (outData) {
        *outData = std::move(data);
    }
    return true;
#endif
}

bool Client::getQRCode(QRCodeResult& out, std::string* errOut) const {
    nlohmann::json data;
    if (!call("v1/app/login/getQRCode", nlohmann::json::object(), &data, errOut)) {
        return false;
    }
    out.type = data.value("type", 0);
    out.codeId = data.value("codeId", "");
    out.buffer = data.value("buffer", "");
    if (out.codeId.empty()) {
        if (errOut) *errOut = "missing codeId in getQRCode response";
        return false;
    }
    return true;
}

bool Client::pollQRCodeState(std::string_view codeId, LoginStateResult& out, std::string* errOut) const {
    nlohmann::json data;
    if (!call("v1/app/login/getQRCodeState", nlohmann::json{{"codeId", std::string(codeId)}}, &data, errOut)) {
        return false;
    }
    out.state = data.value("state", 0);
    out.sessionKey = data.value("sessionKey", "");
    out.ciKey = data.value("ciKey", "");
    out.secretKey = data.value("secretKey", "");
    return true;
}

bool Client::getUserInfo(UserInfo& out, std::string* errOut) const {
    nlohmann::json data;
    if (!call("v1/app/user/getUserInfo", nlohmann::json::object(), &data, errOut)) {
        return false;
    }
    return parseUserInfoEnvelope(data, out, errOut);
}

bool Client::listGames(int page, int size, ListGamesResult& out, std::string* errOut) const {
    nlohmann::json data;
    nlohmann::json body = {
        {"channel", "all"},
        {"categoryId", ""},
        {"searchKey", ""},
        {"page", std::to_string(page)},
        {"size", std::to_string(size)},
    };
    if (!call("v1/app/game/list", body, &data, errOut)) {
        return false;
    }
    return parseGameListEnvelope(data, out, errOut);
}

bool Client::getGameDetail(std::string_view gameId, GameListItem& out, std::string* errOut) const {
    nlohmann::json data;
    if (!call("v1/app/game/detail", nlohmann::json{{"gameId", std::string(gameId)}}, &data, errOut)) {
        return false;
    }
    if (!data.contains("gameInfo") || !data["gameInfo"].is_object()) {
        if (errOut) *errOut = "missing gameInfo in detail response";
        return false;
    }
    out = parseGameItem(data["gameInfo"], true);
    if (out.gameId.empty() && out.gameName.empty()) {
        if (errOut) *errOut = "empty game detail";
        return false;
    }
    return true;
}

bool Client::startGame(std::string_view gameId, GameStartInfo& out, std::string* errOut) const {
    nlohmann::json data;
    std::string rawData;
    if (!call("v1/app/game/startGame", nlohmann::json{{"gameId", std::string(gameId)}},
              &data, errOut, &rawData)) {
        return false;
    }
    return parseGameStartEnvelope(data, rawData, out, errOut);
}

bool Client::stopGame(std::string_view gameId, std::string* errOut) const {
    nlohmann::json body = nlohmann::json::object();
    if (!gameId.empty()) {
        body["gameId"] = std::string(gameId);
    }
    return call("v1/app/game/ytcx/stopGame", body, nullptr, errOut);
}

bool Client::downloadBinary(std::string_view url,
                            std::vector<uint8_t>& outBytes,
                            std::string* errOut,
                            size_t maxBytes) {
#ifdef LEVELUP_NO_CURL
    (void)url;
    (void)outBytes;
    (void)maxBytes;
    if (errOut) *errOut = "curl disabled in host test build";
    return false;
#else
    long httpCode = 0;
    CURLcode curlCode = CURLE_OK;
    std::string transportErr;
    const std::string urlStr(url);

    const bool startInsecure = g_preferInsecureTls.load(std::memory_order_relaxed);
    bool ok = performCurlRequest(urlStr, nullptr, "okhttp/4.12.0", startInsecure, 15L, maxBytes,
                                 httpCode, outBytes, curlCode, &transportErr);
    if (!ok && !startInsecure &&
        (curlCode == CURLE_PEER_FAILED_VERIFICATION ||
         curlCode == CURLE_SSL_CACERT_BADFILE ||
         curlCode == CURLE_SSL_CONNECT_ERROR)) {
        g_preferInsecureTls.store(true, std::memory_order_relaxed);
        ok = performCurlRequest(urlStr, nullptr, "okhttp/4.12.0", true, 15L, maxBytes,
                                httpCode, outBytes, curlCode, &transportErr);
    }
    if (!ok) {
        if (errOut) *errOut = transportErr;
        return false;
    }
    if (httpCode != 200) {
        if (errOut) *errOut = "HTTP " + std::to_string(httpCode);
        outBytes.clear();
        return false;
    }
    return true;
#endif
}

} // namespace levelup::api
