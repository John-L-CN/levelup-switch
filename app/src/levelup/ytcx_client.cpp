#include "levelup/ytcx_client.hpp"

#include "levelup/crypto_util.hpp"
#include "levelup/exit_guard.hpp"
#include "levelup/log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <thread>

#ifndef LEVELUP_NO_CURL
#include <curl/curl.h>
#endif

#ifndef LEVELUP_NO_RTC
#include <rtc/rtc.hpp>
#endif

namespace levelup::ytcx {
namespace {

std::string truncateMsg(std::string_view s, size_t n = 220) {
    if (s.size() <= n) return std::string(s);
    return std::string(s.substr(0, n)) + "...";
}

#ifndef LEVELUP_NO_CURL
struct CurlBuffer {
    std::vector<uint8_t> data;
    size_t maxBytes = 4 * 1024 * 1024;
};

size_t curlWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t total = size * nmemb;
    auto* buf = static_cast<CurlBuffer*>(userdata);
    if (!buf || total == 0) return total;
    if (buf->data.size() + total > buf->maxBytes) return 0;
    const auto* u8 = reinterpret_cast<const uint8_t*>(ptr);
    buf->data.insert(buf->data.end(), u8, u8 + total);
    return total;
}

// 退出请求到达后立刻中断在途请求，避免退出路径 join 网络线程时把系统关闭超时拖爆。
int curlAbortOnExitCb(void* clientp,
                      curl_off_t dltotal,
                      curl_off_t dlnow,
                      curl_off_t ultotal,
                      curl_off_t ulnow) {
    (void)clientp;
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    return (isExiting() && !inCriticalRequest()) ? 1 : 0;
}

bool curlPostJson(const std::string& url,
                  const std::string& body,
                  const char* userAgent,
                  bool insecureTls,
                  long timeoutSec,
                  long& outHttpCode,
                  std::string& outResp,
                  CURLcode& outCurlCode,
                  std::string* errOut) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (errOut) *errOut = "curl_easy_init failed";
        return false;
    }

    CurlBuffer buf;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlAbortOnExitCb);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    if (userAgent) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent);
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));

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
        outResp.assign(reinterpret_cast<const char*>(buf.data.data()), buf.data.size());
    } else if (errOut) {
        *errOut = curl_easy_strerror(outCurlCode);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return outCurlCode == CURLE_OK;
}

// GET 文本（官方 dialpoints 走明文 http + query，无签名、无 body）
bool curlGetText(const std::string& url,
                 const char* userAgent,
                 long timeoutSec,
                 long& outHttpCode,
                 std::string& outResp,
                 CURLcode& outCurlCode,
                 std::string* errOut) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (errOut) *errOut = "curl_easy_init failed";
        return false;
    }

    CurlBuffer buf;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlAbortOnExitCb);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    if (userAgent) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent);
    }

    outCurlCode = curl_easy_perform(curl);
    outHttpCode = 0;
    if (outCurlCode == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &outHttpCode);
        outResp.assign(reinterpret_cast<const char*>(buf.data.data()), buf.data.size());
    } else if (errOut) {
        *errOut = curl_easy_strerror(outCurlCode);
    }

    curl_easy_cleanup(curl);
    return outCurlCode == CURLE_OK;
}
#endif

} // namespace

std::string buildDeviceInfoJson(int width, int height) {
    nlohmann::json j = {
        {"board", "x86_64"},
        {"brand", "Microsoft"},
        {"cpu", "x86_64"},
        {"hardware", "generic"},
        {"manufacturer", "Microsoft"},
        {"model", "Switch NX"},
        {"network", "Wifi"},
        {"os_version", "10"},
        {"produt", "levelup-switch"},
        {"resolution_height", height},
        {"resolution_width", width},
        {"screen_size", 27.0},
        {"sdk", "34"},
    };
    return j.dump();
}

std::string buildStreamInfoJson(int width,
                                int height,
                                int maxFps,
                                int64_t gid,
                                int videoLevel,
                                bool vibration) {
    nlohmann::json j = {
        {"clientType", "switch-handheld"},
        {"connectionType", "webrtcVideo"},
        {"desiredWidth", width},
        {"desiredHeight", height},
        {"decoderHeightLimited", 2160},
        {"maxFps", maxFps},
        {"clientSupportHevc", 0},
        {"videoLevel", videoLevel},
        {"withHWCursor", 0},
        {"sdkVersion", kUnionSDKVersion},
        {"vibration", vibration},
        {"gid", gid},
        {"autoEncoding", 1},
    };
    return j.dump();
}

SignedUnionRequest buildSignedUnionApplyRequest(std::string_view proData,
                                                int64_t pkgName,
                                                std::string_view deviceId,
                                                std::string_view userId,
                                                int width,
                                                int height,
                                                const std::vector<uint8_t>& pbLaunchGame,
                                                std::string_view nonce,
                                                int64_t timestampMs,
                                                const std::vector<DialSample>& dialHistory,
                                                int areaType,
                                                int tryAreaType) {
    const int64_t tMs = timestampMs > 0 ? timestampMs : unixMillisNow();
    const std::string nonceStr =
        nonce.empty() ? ("sig_" + std::to_string(tMs / 1000)) : std::string(nonce);

    nlohmann::json body = {
        {"app_user_id", std::string(userId)},
        {"account_auth_type", 2},
        {"ugid", pkgName},
        {"utoken", ""},
        {"opaque", std::string(proData)},
        {"dev_type", 2},
        {"dev_id", std::string(deviceId)},
        {"area_type", areaType},
        {"try_area_type", tryAreaType},
        {"roomid", 0},
        {"sdk_version", kUnionSDKVersionNum},
        {"os_type", 1},
        {"host_method", 0},
        {"join_method", 0},
        {"target_uuid", 0},
        {"entry_uuid", 0},
        {"prelaunch_param", base64Encode(pbLaunchGame.data(), pbLaunchGame.size())},
        {"clientDisplayWidth", width},
        {"clientDisplayHeight", height},
        {"lsNativeFullScreen", true},
        {"apply_mode", 0},
        {"vmid", 0},
        {"check_game_state", true},
        {"target_session_id", ""},
    };

    // 官方 GameStatus_prepareArea 会把本地多区域测速历史一起交给调度（dialtest_history_data），
    // 调度据此挑最近/最快的边缘机房；首次申请没有历史就与官方一致地只带 area_type=0。
    if (!dialHistory.empty()) {
        nlohmann::json history = nlohmann::json::array();
        for (const auto& s : dialHistory) {
            nlohmann::json item = {
                {"ts", s.ts},
                {"ip", s.clientIp},
                {"isp", s.isp},
                {"country", s.country},
                {"province", s.province},
                {"city", s.city},
                {"area_type", s.areaType},
                {"idc", s.idc},
                {"rtt_95", s.rtt95},
            };
            history.push_back(std::move(item));
        }
        body["dialtest_history_data"] = std::move(history);
    }

    SignedUnionRequest req;
    req.bodyJson = body.dump();

    const auto md5Bytes = md5Raw(req.bodyJson);
    req.bodyMd5Base64 = base64Encode(md5Bytes.data(), md5Bytes.size());

    std::map<std::string, std::string> params = {
        {"AccessKey", kUnionAccessKey},
        {"AppId", kUnionAppID},
        {"BizId", kUnionBizID},
        {"Format", "JSON"},
        {"SignatureMethod", "HMAC-SHA1"},
        {"SignatureNonce", nonceStr},
        {"SignatureVersion", "1.1"},
        {"Timestamp", std::to_string(tMs)},
        {"Version", "2020-11-01"},
    };

    std::string canonical;
    bool first = true;
    for (const auto& [k, v] : params) {
        if (!first) canonical.push_back('&');
        first = false;
        canonical += k;
        canonical.push_back('=');
        canonical += urlEncodeStrict(v);
    }

    req.stringToSign = "POST&" + urlEncodeStrict("/v3/union/apply") + "&" +
                       urlEncodeStrict(req.bodyMd5Base64) + "&" +
                       urlEncodeStrict(canonical);

    const std::string hmacKey = std::string(kUnionSecretKey) + "&";
    const auto mac = hmacSha1Raw(hmacKey, req.stringToSign);
    req.signatureBase64 = base64Encode(mac.data(), mac.size());
    params["Signature"] = req.signatureBase64;

    static constexpr std::array<const char*, 10> kOrderedKeys = {
        "AccessKey", "AppId", "BizId", "Format", "SignatureMethod",
        "SignatureNonce", "SignatureVersion", "Timestamp", "Version", "Signature",
    };
    std::string query;
    for (size_t i = 0; i < kOrderedKeys.size(); ++i) {
        if (i > 0) query.push_back('&');
        const char* k = kOrderedKeys[i];
        query += k;
        query.push_back('=');
        query += urlEncodeSlashSafe(params[k]);
    }
    req.url = std::string(kUnionApplyURL) + "?" + query;
    return req;
}

bool parseUnionApplyResponse(std::string_view rawJson, ApplyResult& out, std::string* errOut) {
    nlohmann::json env;
    try {
        env = nlohmann::json::parse(rawJson.begin(), rawJson.end());
    } catch (const std::exception& e) {
        if (errOut) *errOut = std::string("union apply JSON error: ") + e.what();
        return false;
    }
    if (!env.is_object() || !env.contains("ret") || !env["ret"].is_object()) {
        if (errOut) *errOut = "union apply missing ret object";
        return false;
    }
    const int retCode = env["ret"].value("code", -1);
    if (retCode != 0) {
        if (errOut) {
            *errOut = "union apply failed " + std::to_string(retCode) + ": " +
                      env["ret"].value("msg", "");
        }
        return false;
    }
    const auto body = env.contains("body") && env["body"].is_object()
                          ? env["body"]
                          : nlohmann::json::object();

    out = ApplyResult{};
    out.rawJson = body.dump();
    out.flowId = body.value("flow_id", "");
    out.token = body.value("token", "");
    out.userId = body.value("uid", int64_t{0});
    if (body.contains("access_info") && body["access_info"].is_object()) {
        const auto& ai = body["access_info"];
        out.accessIp = ai.value("access_ipv4_address", "");
        out.accessPort = ai.value("access_ipv4_port", 0);
        // 调度回显：服务端最终命中的区域/机房，以及该区域专用 STUN 和备用接入地址
        out.areaType = ai.value("area_type", 0);
        out.idc = ai.value("idc", 0);
        out.areaStunAddress = ai.value("area_stun_address", "");
        out.areaStunPort = ai.value("area_stun_port", 0);
        if (ai.contains("spare_address") && ai["spare_address"].is_array()) {
            for (const auto& s : ai["spare_address"]) {
                if (!s.is_object()) continue;
                const std::string ip = s.value("access_ipv4_address", "");
                const int port = s.value("access_ipv4_port", 0);
                if (!ip.empty() && port > 0) {
                    out.spareAddresses.emplace_back(ip, port);
                }
            }
        }
    }
    if (body.contains("client_info") && body["client_info"].is_object()) {
        out.clientAddress = body["client_info"].value("address", "");
    }
    if (body.contains("dialpoint") && body["dialpoint"].is_object()) {
        out.currentDialWsUrl = body["dialpoint"].value("rtt_ws_url", "");
    }
    auto readNumericId = [](const nlohmann::json& j) -> std::string {
        if (j.is_string()) return j.get<std::string>();
        if (j.is_number_integer()) return std::to_string(j.get<long long>());
        if (j.is_number_unsigned()) return std::to_string(j.get<unsigned long long>());
        return {};
    };
    if (body.contains("uuid")) {
        out.lpUuid = readNumericId(body["uuid"]);
    }
    if (out.lpUuid.empty() && body.contains("uid")) {
        out.lpUuid = readNumericId(body["uid"]);
    }
    if (out.token.empty() || out.accessIp.empty() || out.accessPort <= 0) {
        if (errOut) {
            *errOut = "union apply response missing token/access_info: " + truncateMsg(rawJson, 280);
        }
        return false;
    }
    return true;
}

// ---------------- 多区域测速（官方 muti_area_rtt 等价实现） ----------------

std::string dialCacheKey(std::string_view clientIp, int areaType, int idc) {
    // 官方：client_info.ip_address + "-" + area_type + "-" + idc
    return std::string(clientIp) + "-" + std::to_string(areaType) + "-" + std::to_string(idc);
}

int dialP95(std::vector<int> samples) {
    // 官方 math.b.a(999, 0.95f)：先丢掉 >=999 的超时样本，再 math.e.a(list, 0.95f)
    // 保留前 size - floor(size*0.05) 个，取最后一个 —— 最近秩 p95。
    samples.erase(std::remove_if(samples.begin(), samples.end(),
                                 [](int v) { return v < 0 || v >= kDialFailMs; }),
                  samples.end());
    if (samples.empty()) return kDialFailMs;
    std::sort(samples.begin(), samples.end());
    const int size = static_cast<int>(samples.size());
    if (size <= 1) return samples.back();
    const int keep = size - static_cast<int>(static_cast<float>(size) * 0.05f);
    if (keep <= 0 || keep >= size) return samples.back();
    return samples[keep - 1];
}

std::vector<DialSample> trimDialCache(std::vector<DialSample> samples, size_t maxEntries) {
    if (samples.size() <= maxEntries) return samples;
    std::sort(samples.begin(), samples.end(),
              [](const DialSample& a, const DialSample& b) { return a.ts < b.ts; });
    samples.erase(samples.begin(),
                  samples.begin() + static_cast<long>(samples.size() - maxEntries));
    return samples;
}

std::vector<DialSample> mergeDialCache(std::vector<DialSample> cache,
                                      const std::vector<DialSample>& fresh,
                                      size_t maxEntries) {
    for (const auto& f : fresh) {
        const std::string key = dialCacheKey(f.clientIp, f.areaType, f.idc);
        cache.erase(std::remove_if(cache.begin(), cache.end(),
                                   [&key](const DialSample& s) {
                                       return dialCacheKey(s.clientIp, s.areaType, s.idc) == key;
                                   }),
                    cache.end());
        cache.push_back(f);
    }
    return trimDialCache(std::move(cache), maxEntries);
}

std::string dialCacheToJson(const std::vector<DialSample>& samples) {
    // 与官方 Gson 落盘同构：{"<ip>-<area>-<idc>": {ts,ip,isp,country,province,city,area_type,idc,rtt_95}}
    nlohmann::json root = nlohmann::json::object();
    for (const auto& s : samples) {
        root[dialCacheKey(s.clientIp, s.areaType, s.idc)] = {
            {"ts", s.ts},
            {"ip", s.clientIp},
            {"isp", s.isp},
            {"country", s.country},
            {"province", s.province},
            {"city", s.city},
            {"area_type", s.areaType},
            {"idc", s.idc},
            {"rtt_95", s.rtt95},
        };
    }
    return root.dump(2);
}

bool dialCacheFromJson(std::string_view json, std::vector<DialSample>& out) {
    out.clear();
    if (json.empty()) return false;
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(json.begin(), json.end());
    } catch (...) {
        return false;
    }
    if (!root.is_object()) return false;
    for (auto it = root.begin(); it != root.end(); ++it) {
        const auto& v = it.value();
        if (!v.is_object()) continue;
        DialSample s;
        s.clientIp = v.value("ip", "");
        s.isp = v.value("isp", "");
        s.country = v.value("country", "");
        s.province = v.value("province", "");
        s.city = v.value("city", "");
        s.areaType = v.value("area_type", 0);
        s.idc = v.value("idc", 0);
        s.rtt95 = v.value("rtt_95", kDialFailMs);
        s.ts = v.value("ts", int64_t{0});
        out.push_back(std::move(s));
    }
    return !out.empty();
}

bool parseWsEndpoint(std::string_view url, std::string& host, int& port) {
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos) return false;
    const std::string scheme(url.substr(0, schemeEnd));
    if (scheme != "ws" && scheme != "wss" && scheme != "http" && scheme != "https") {
        return false;
    }
    std::string_view rest = url.substr(schemeEnd + 3);
    const auto slash = rest.find('/');
    if (slash != std::string_view::npos) rest = rest.substr(0, slash);
    const auto colon = rest.rfind(':');
    if (colon == std::string_view::npos) {
        host = std::string(rest);
        port = (scheme == "wss" || scheme == "https") ? 443 : 80;
        return !host.empty();
    }
    host = std::string(rest.substr(0, colon));
    const std::string portStr(rest.substr(colon + 1));
    if (host.empty() || portStr.empty()) return false;
    port = std::atoi(portStr.c_str());
    return port > 0 && port < 65536;
}

std::string buildDialPointsUrl(std::string_view uuid, int64_t ugid, std::string_view flowId) {
    return std::string(kUnionDialPointsURL) + "?uuid=" + urlEncodeStrict(uuid) +
           "&ugid=" + std::to_string(ugid) + "&flow_id=" + urlEncodeStrict(flowId);
}

std::vector<DialSample> loadDialCache(const std::string& path) {
    std::vector<DialSample> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    std::string buf;
    char chunk[512];
    size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        buf.append(chunk, n);
    }
    std::fclose(f);
    dialCacheFromJson(buf, out);
    return out;
}

bool saveDialCache(const std::string& path, const std::vector<DialSample>& samples) {
    const auto slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        std::error_code ec;
        std::filesystem::create_directories(path.substr(0, slash), ec);
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const std::string json = dialCacheToJson(trimDialCache(samples, kDialCacheEntries));
    const size_t written = std::fwrite(json.data(), 1, json.size(), f);
    std::fclose(f);
    return written == json.size();
}

std::string extractRealUserIdFromToken(std::string_view token) {
    const auto firstDash = token.find('-');
    if (firstDash == std::string_view::npos || firstDash + 1 >= token.size()) {
        return std::string(token);
    }
    const auto secondDash = token.find('-', firstDash + 1);
    if (secondDash == std::string_view::npos) {
        return std::string(token.substr(firstDash + 1));
    }
    return std::string(token.substr(firstDash + 1, secondDash - firstDash - 1));
}

std::string buildEdgeLaunchBodyJson(const EdgeLaunchParams& p) {
    const int64_t cid = p.clientId > 0 ? p.clientId : unixMillisNow();
    nlohmann::json body = {
        {"clientId", cid},
        {"flowId", p.flowId},
        {"sdkVersion", kUnionSDKVersion},
        {"gid", p.pkgName},
        {"userId", p.userId},
        {"connectionType", "webrtcVideo"},
        {"videoLevel", std::clamp(p.videoLevel, 0, 4)},
        {"desiredWidth", p.width},
        {"desiredHeight", p.height},
        {"decoderHeightLimited", 2160},
        {"maxFps", p.maxFps},
        {"gamescreen", "landscape"},
        {"clientSupportHevc", 0},
        {"withHWCursor", 0},
        {"clientType", "switch-handheld"},
        {"forceAudioDataChannel", 0},
        {"forwardIp", p.edgeIp},
        {"forwardPortType", 0},
        {"forwardPort", p.basePort},
        {"forwardPortV6", p.basePort},
        {"vibration", p.vibration},
        {"hostMethod", 0},
        {"joinMethod", 0},
        {"targetUuid", 0},
        {"aSessionId", p.aSessionId},
        {"vSessionId", p.vSessionId},
        {"cSessionId", p.cSessionId},
        {"deviceId", p.deviceId},
        {"startFlag", 0},
        {"supportVideoCodecProfiles", {4096, 8192, 1, 2, 4}},
        {"requestUseServerCursorData", true},
        {"enableVfr", 0},
        {"enableSEIInfo", 1},
        {"orientationType", 0},
        {"clientNetworkType", "2"},
        {"imeType", 0},
        {"keyboardType", 0},
        {"audioVolume", 100},
        {"screen_density", 320},
        {"localInputType", 0},
        {"recvDownloadDirChange", 0},
        {"bitrateGuidanceAdjustMode", p.vqFirst ? 1 : 0},
        {"supportSR", 0},
        {"supportPath", 1},
        {"supportCodecs", {0, 1}},
    };
    return body.dump();
}

bool parseEdgeLaunchResponse(std::string_view rawJson,
                             int basePort,
                             EdgeLaunchResult& out,
                             std::string* errOut) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(rawJson.begin(), rawJson.end());
    } catch (const std::exception& e) {
        if (errOut) *errOut = std::string("edge /launch JSON error: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        if (errOut) *errOut = "edge /launch response not JSON object";
        return false;
    }

    out = EdgeLaunchResult{};
    out.signalingPort = j.value("webrtcSignalingPort", 0);
    out.videoPort = j.value("videoPort", 0);
    out.audioPort = j.value("audioPort", 0);
    out.controlPort = j.value("controlPort", 0);
    out.remoteAddr = j.value("remoteAddr", "");
    out.disableSctpEncrypt = j.value("disableSctpEncryption", false);
    out.supportRtcAnswerMode = j.value("supportRtcAnswerMode", 0);
    out.encodedWidth = j.value("encodedWidth", 0);
    out.encodedHeight = j.value("encodedHeight", 0);
    out.desktopWidth = j.value("desktopWidth", 0);
    out.desktopHeight = j.value("desktopHeight", 0);
    out.finalCodec = j.value("finalCodec", 0);

    if (j.contains("supportInputLabelList") && j["supportInputLabelList"].is_array()) {
        for (const auto& s : j["supportInputLabelList"]) {
            if (s.is_string()) {
                out.supportInputLabels.push_back(s.get<std::string>());
            }
        }
    }
    if (out.signalingPort == 0 && basePort > 0) {
        out.signalingPort = basePort + 15;
    }
    return true;
}

bool unionApply(std::string_view proData,
                int64_t pkgName,
                std::string_view deviceId,
                std::string_view userId,
                int width,
                int height,
                const std::vector<uint8_t>& pbLaunchGame,
                ApplyResult& out,
                std::string* errOut,
                const std::vector<DialSample>& dialHistory,
                int areaType,
                int tryAreaType) {
#ifdef LEVELUP_NO_CURL
    (void)proData;
    (void)pkgName;
    (void)deviceId;
    (void)userId;
    (void)width;
    (void)height;
    (void)pbLaunchGame;
    (void)out;
    (void)dialHistory;
    (void)areaType;
    (void)tryAreaType;
    if (errOut) *errOut = "curl disabled in host test build";
    return false;
#else
    const auto req = buildSignedUnionApplyRequest(proData, pkgName, deviceId, userId, width,
                                                  height, pbLaunchGame, {}, 0, dialHistory,
                                                  areaType, tryAreaType);

    long httpCode = 0;
    std::string respStr;
    CURLcode curlCode = CURLE_OK;
    std::string transportErr;

    bool ok = curlPostJson(req.url, req.bodyJson, "okhttp/3.12.0", false, 15L,
                           httpCode, respStr, curlCode, &transportErr);
    if (!ok && (curlCode == CURLE_PEER_FAILED_VERIFICATION ||
                curlCode == CURLE_SSL_CACERT_BADFILE ||
                curlCode == CURLE_SSL_CONNECT_ERROR)) {
        logWarn("union-access TLS verify failed (" + transportErr + "), retrying insecure fallback");
        ok = curlPostJson(req.url, req.bodyJson, "okhttp/3.12.0", true, 15L,
                          httpCode, respStr, curlCode, &transportErr);
    }
    if (!ok) {
        if (errOut) *errOut = "unionApply request failed: " + transportErr;
        return false;
    }
    if (httpCode != 200) {
        if (errOut) *errOut = "unionApply HTTP " + std::to_string(httpCode) + ": " + truncateMsg(respStr);
        return false;
    }
    return parseUnionApplyResponse(respStr, out, errOut);
#endif
}

bool unionRelease(std::string_view flowId,
                  std::string_view lpUuid,
                  std::string_view deviceId,
                  int64_t pkgName,
                  bool kickRunning,
                  std::string* errOut) {
#ifdef LEVELUP_NO_CURL
    (void)flowId;
    (void)lpUuid;
    (void)deviceId;
    (void)pkgName;
    (void)kickRunning;
    if (errOut) *errOut = "curl disabled in host test build";
    return false;
#else
    if (flowId.empty()) {
        if (errOut) *errOut = "union release: empty flow id";
        return false;
    }

    // 官方 GameStatus.prepareArea.a(int,boolean,boolean) 的释放体
    nlohmann::json body = {
        {"uuid", std::string(lpUuid)},
        {"ugid", pkgName},
        {"utoken", ""},
        {"flow_id", std::string(flowId)},
        {"dev_id", std::string(deviceId)},
        {"type", kickRunning ? 1 : 0},
        {"session_id", ""},
    };
    const std::string bodyJson = body.dump();

    const std::string path = "/v3/union/release";
    const auto md5Bytes = md5Raw(bodyJson);
    const std::string bodyMd5 = base64Encode(md5Bytes.data(), md5Bytes.size());

    std::map<std::string, std::string> params = {
        {"AccessKey", kUnionAccessKey},
        {"AppId", kUnionAppID},
        {"BizId", kUnionBizID},
        {"Format", "JSON"},
        {"SignatureMethod", "HMAC-SHA1"},
        {"SignatureNonce", "sig_" + std::to_string(unixMillisNow() / 1000)},
        {"SignatureVersion", "1.1"},
        {"Timestamp", std::to_string(unixMillisNow())},
        {"Version", "2020-11-01"},
    };
    std::string canonical;
    for (const auto& [k, v] : params) {
        if (!canonical.empty()) canonical.push_back('&');
        canonical += k;
        canonical.push_back('=');
        canonical += urlEncodeStrict(v);
    }
    const std::string stringToSign = "POST&" + urlEncodeStrict(path) + "&" +
                                     urlEncodeStrict(bodyMd5) + "&" +
                                     urlEncodeStrict(canonical);
    const auto mac = hmacSha1Raw(std::string(kUnionSecretKey) + "&", stringToSign);
    params["Signature"] = base64Encode(mac.data(), mac.size());

    static constexpr std::array<const char*, 10> kOrderedKeys = {
        "AccessKey", "AppId", "BizId", "Format", "SignatureMethod",
        "SignatureNonce", "SignatureVersion", "Timestamp", "Version", "Signature",
    };
    std::string url = std::string(kUnionReleaseURL) + "?";
    for (size_t i = 0; i < kOrderedKeys.size(); ++i) {
        if (i > 0) url.push_back('&');
        const char* k = kOrderedKeys[i];
        url += k;
        url.push_back('=');
        url += urlEncodeSlashSafe(params[k]);
    }

    long httpCode = 0;
    std::string resp;
    CURLcode curlCode = CURLE_OK;
    std::string transportErr;
    if (!curlPostJson(url, bodyJson, "okhttp/3.12.0", false, 10L, httpCode, resp, curlCode,
                      &transportErr)) {
        if (errOut) *errOut = "union release request failed: " + transportErr;
        return false;
    }
    if (httpCode != 200) {
        if (errOut) {
            *errOut = "union release HTTP " + std::to_string(httpCode) + ": " +
                      truncateMsg(resp);
        }
        return false;
    }
    nlohmann::json env;
    try {
        env = nlohmann::json::parse(resp.begin(), resp.end());
    } catch (...) {
        if (errOut) *errOut = "union release JSON error: " + truncateMsg(resp);
        return false;
    }
    const int code = env.contains("ret") ? env["ret"].value("code", -1) : -1;
    if (code != 0) {
        if (errOut) {
            *errOut = "union release failed " + std::to_string(code) + ": " +
                      (env.contains("ret") ? env["ret"].value("msg", "") : "");
        }
        return false;
    }
    return true;
#endif
}

bool dialFetchPoints(std::string_view lpUuid,
                     int64_t pkgName,
                     std::string_view flowId,
                     std::vector<DialSample>& out,
                     std::string* clientInfoOut,
                     std::string* errOut) {
    out.clear();
    if (clientInfoOut) clientInfoOut->clear();
#ifdef LEVELUP_NO_CURL
    (void)lpUuid;
    (void)pkgName;
    (void)flowId;
    if (errOut) *errOut = "curl disabled in host test build";
    return false;
#else
    if (lpUuid.empty()) {
        if (errOut) *errOut = "dialpoints: empty uuid";
        return false;
    }
    const std::string url = buildDialPointsUrl(lpUuid, pkgName, flowId);
    long httpCode = 0;
    std::string resp;
    CURLcode curlCode = CURLE_OK;
    std::string transportErr;
    if (!curlGetText(url, "okhttp/3.12.0", 8L, httpCode, resp, curlCode, &transportErr)) {
        if (errOut) *errOut = "dialpoints request failed: " + transportErr;
        return false;
    }
    if (httpCode != 200) {
        if (errOut) *errOut = "dialpoints HTTP " + std::to_string(httpCode) + ": " + truncateMsg(resp);
        return false;
    }
    nlohmann::json env;
    try {
        env = nlohmann::json::parse(resp.begin(), resp.end());
    } catch (...) {
        if (errOut) *errOut = "dialpoints JSON error: " + truncateMsg(resp);
        return false;
    }
    const int code = env.contains("ret") ? env["ret"].value("code", -1) : -1;
    if (code != 0) {
        if (errOut) {
            *errOut = "dialpoints failed " + std::to_string(code) + ": " +
                      (env.contains("ret") ? env["ret"].value("msg", "") : "");
        }
        return false;
    }
    const nlohmann::json body = env.contains("body") && env["body"].is_object()
                                    ? env["body"]
                                    : nlohmann::json::object();
    std::string clientIp;
    if (body.contains("client_info") && body["client_info"].is_object()) {
        clientIp = body["client_info"].value("ip_address", "");
    }
    if (clientInfoOut) *clientInfoOut = clientIp;
    if (!body.contains("dial_points") || !body["dial_points"].is_array()) {
        if (errOut) *errOut = "dialpoints: missing dial_points";
        return false;
    }
    for (const auto& p : body["dial_points"]) {
        if (!p.is_object()) continue;
        DialSample s;
        s.clientIp = clientIp;
        s.areaType = p.value("area_type", 0);
        s.idc = p.value("idc", 0);
        s.isp = p.value("dial_isp", "");
        s.rttWsUrl = p.value("rtt_ws_url", "");
        s.rttPingUrl = p.value("rtt_ping_address", "");
        if (s.rttWsUrl.empty() && s.rttPingUrl.empty()) continue;
        if (body.contains("client_info") && body["client_info"].is_object()) {
            const auto& ci = body["client_info"];
            s.country = ci.value("country", "");
            s.province = ci.value("province", "");
            s.city = ci.value("city", "");
        }
        out.push_back(std::move(s));
    }
    return !out.empty();
#endif
}

int dialProbeOnceMs(std::string_view host, int port, int timeoutMs) {
#ifdef LEVELUP_NO_CURL
    (void)host;
    (void)port;
    (void)timeoutMs;
    return -1;
#else
    if (host.empty() || port <= 0) return -1;
    CURL* curl = curl_easy_init();
    if (!curl) return -1;
    const std::string url = "http://" + std::string(host) + ":" + std::to_string(port) + "/";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeoutMs));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 600L);
    const CURLcode rc = curl_easy_perform(curl);
    int ms = -1;
    if (rc == CURLE_OK) {
        double connectTime = 0.0;
        if (curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connectTime) == CURLE_OK) {
            ms = static_cast<int>(connectTime * 1000.0 + 0.5);
        }
        if (ms < 0) ms = 0;
    }
    curl_easy_cleanup(curl);
    return ms;
#endif
}

std::vector<DialSample> dialProbe(std::vector<DialSample> points,
                                  int attempts,
                                  int timeoutMs,
                                  const std::atomic<bool>* stopFlag) {
    if (attempts < 1) attempts = 1;
    for (auto& p : points) {
        std::string host;
        int port = 0;
        if (!parseWsEndpoint(p.rttWsUrl, host, port)) {
            // 兜底：rtt_ping_address 形如 "ip:port"
            const auto colon = p.rttPingUrl.rfind(':');
            if (colon == std::string::npos) {
                p.rtt95 = kDialFailMs;
                p.ts = unixMillisNow();
                continue;
            }
            host = p.rttPingUrl.substr(0, colon);
            port = std::atoi(p.rttPingUrl.c_str() + static_cast<long>(colon) + 1);
        }
        std::vector<int> samples;
        samples.reserve(static_cast<size_t>(attempts));
        for (int i = 0; i < attempts; ++i) {
            if (stopFlag && stopFlag->load()) break;
            const int ms = dialProbeOnceMs(host, port, timeoutMs);
            samples.push_back(ms < 0 || ms >= kDialFailMs ? kDialFailMs : ms);
            if (i + 1 < attempts) {
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
            }
        }
        p.rtt95 = dialP95(std::move(samples));
        p.ts = unixMillisNow();
        logInfo("[Dial] area=" + std::to_string(p.areaType) + " idc=" + std::to_string(p.idc) +
                " " + host + ":" + std::to_string(port) + " p95=" +
                std::to_string(p.rtt95) + "ms");
    }
    return points;
}

bool edgeLaunch(const EdgeLaunchParams& params, EdgeLaunchResult& out, std::string* errOut) {
#ifdef LEVELUP_NO_CURL
    (void)params;
    (void)out;
    if (errOut) *errOut = "curl disabled in host test build";
    return false;
#else
    const std::string url = "https://" + params.edgeIp + ":" +
                            std::to_string(params.basePort + 4) + "/launch";
    const std::string bodyJson = buildEdgeLaunchBodyJson(params);

    long httpCode = 0;
    std::string respStr;
    CURLcode curlCode = CURLE_OK;
    std::string transportErr;

    if (!curlPostJson(url, bodyJson, "okhttp/3.12.0", true, 20L,
                      httpCode, respStr, curlCode, &transportErr)) {
        if (errOut) *errOut = "edge /launch failed: " + transportErr;
        return false;
    }
    if (httpCode != 200) {
        if (errOut) *errOut = "edge /launch HTTP " + std::to_string(httpCode) + ": " + truncateMsg(respStr);
        return false;
    }
    return parseEdgeLaunchResponse(respStr, params.basePort, out, errOut);
#endif
}

// ---------------- BEClient ----------------

BEClient::~BEClient() {
    m_alive->store(false);
    close();
}

void BEClient::setVPadCallback(VPadCallback cb) {
    std::lock_guard lock(m_cbMu);
    m_vpadCb = std::move(cb);
}

void BEClient::setDisconnectCallback(DisconnectCallback cb) {
    std::lock_guard lock(m_cbMu);
    m_disconnectCb = std::move(cb);
}

bool BEClient::connect(std::string_view host,
                       int port,
                       std::string_view token,
                       std::string_view deviceId,
                       int timeoutMs,
                       std::string* errOut) {
#ifdef LEVELUP_NO_RTC
    (void)host;
    (void)port;
    (void)token;
    (void)deviceId;
    (void)timeoutMs;
    if (errOut) *errOut = "libdatachannel disabled in host test build";
    return false;
#else
    close();

    m_deviceId = std::string(deviceId);
    m_token = std::string(token);
    m_closing.store(false);
    m_connected.store(false);
    m_openDone.store(false);
    m_openError.clear();
    m_hbPeriodMs.store(5000);
    m_hbAckCount.store(0);
    {
        std::lock_guard lock(m_stateMu);
        m_hasResource = false;
        m_resource = GameResource{};
        m_hasFatalLaunchError = false;
        m_fatalLaunchError.clear();
    }

    const std::string connId = "1_" + std::to_string(unixMillisNow()) + "_99";
    const std::string url = "ws://" + std::string(host) + ":" + std::to_string(port) +
                            "/v1/endpoint?token=" + urlEncodeStrict(m_token) +
                            "&dev_type=2&dev_id=" + urlEncodeStrict(m_deviceId) +
                            "&conn_id=" + connId + "&conn_seq=1";

    rtc::WebSocket::Configuration cfg;
    cfg.pingInterval = std::chrono::milliseconds(0); // App sends cmd=5 CGReqHeartbeat manually
    cfg.connectionTimeout = std::chrono::milliseconds(timeoutMs);

    m_ws = std::make_shared<rtc::WebSocket>(cfg);
    std::shared_ptr<std::atomic<bool>> alive = m_alive;

    m_ws->onOpen([this, alive]() {
        if (!alive->load()) return;
        m_connected.store(true);
        m_openDone.store(true);
        m_stateCv.notify_all();
    });

    m_ws->onError([this, alive](std::string err) {
        if (!alive->load()) return;
        {
            std::lock_guard lock(m_stateMu);
            if (!m_openDone.load() && m_openError.empty()) {
                m_openError = err;
            }
        }
        m_openDone.store(true);
        m_stateCv.notify_all();
    });

    m_ws->onClosed([this, alive]() {
        if (!alive->load()) return;
        const bool wasConnected = m_connected.exchange(false);
        const bool expectedClose = m_closing.load();
        if (!m_openDone.load()) {
            std::lock_guard lock(m_stateMu);
            if (m_openError.empty()) {
                m_openError = "WebSocket closed before open";
            }
            m_openDone.store(true);
        }
        {
            std::lock_guard lock(m_stateMu);
            if (!m_hasResource && !m_hasFatalLaunchError) {
                m_hasFatalLaunchError = true;
                m_fatalLaunchError = m_openError.empty() ? "BE WebSocket closed" : m_openError;
            }
        }
        m_stateCv.notify_all();
        m_hbCv.notify_all();

        if (wasConnected && !expectedClose) {
            DisconnectCallback cb;
            {
                std::lock_guard lock(m_cbMu);
                cb = m_disconnectCb;
            }
            if (cb) {
                cb(m_openError.empty() ? "BE WebSocket closed" : m_openError);
            }
        }
    });

    m_ws->onMessage([this, alive](rtc::message_variant msg) {
        if (!alive->load()) return;
        if (std::holds_alternative<rtc::binary>(msg)) {
            const auto& bin = std::get<rtc::binary>(msg);
            handleIncomingBinary(reinterpret_cast<const uint8_t*>(bin.data()), bin.size());
        }
    });

    try {
        m_ws->open(url);
    } catch (const std::exception& e) {
        if (errOut) *errOut = std::string("BE WebSocket open exception: ") + e.what();
        m_ws.reset();
        return false;
    }

    std::unique_lock lock(m_stateMu);
    const bool ready = m_stateCv.wait_for(
        lock, std::chrono::milliseconds(timeoutMs),
        [this]() { return m_openDone.load(); });
    if (!ready) {
        if (errOut) *errOut = "BE WebSocket connect timeout";
        lock.unlock();
        close();
        return false;
    }
    if (!m_connected.load()) {
        if (errOut) *errOut = m_openError.empty() ? "BE WebSocket connect failed" : m_openError;
        lock.unlock();
        close();
        return false;
    }
    return true;
#endif
}

void BEClient::close() {
    m_closing.store(true);
    stopHeartbeat();
#ifndef LEVELUP_NO_RTC
    std::shared_ptr<rtc::WebSocket> ws;
    {
        std::lock_guard lock(m_writeMu);
        ws = std::move(m_ws);
    }
    if (ws) {
        try {
            ws->onOpen(nullptr);
            ws->onError(nullptr);
            ws->onClosed(nullptr);
            ws->onMessage(nullptr);
            ws->forceClose();
        } catch (...) {
        }
    }
#endif
    m_connected.store(false);
    m_stateCv.notify_all();
}

bool BEClient::sendCommand(uint32_t cmd, const std::vector<uint8_t>& payload, std::string* errOut) {
#ifdef LEVELUP_NO_RTC
    (void)cmd;
    (void)payload;
    if (errOut) *errOut = "libdatachannel disabled";
    return false;
#else
    std::lock_guard lock(m_writeMu);
    if (!m_ws || !m_connected.load()) {
        if (errOut) *errOut = "BE WebSocket not connected";
        return false;
    }
    const auto packet = encodeCGMessage(cmd, payload);
    try {
        return m_ws->send(reinterpret_cast<const std::byte*>(packet.data()), packet.size());
    } catch (const std::exception& e) {
        if (errOut) *errOut = e.what();
        return false;
    }
#endif
}

bool BEClient::sendLaunch(const std::vector<uint8_t>& pbLaunchGame, std::string* errOut) {
    return sendCommand(kCmdReqLaunchGame, pbLaunchGame, errOut);
}

bool BEClient::sendHeartbeat(int64_t seq) {
    return sendCommand(kCmdReqHeartbeat, buildHeartbeatReq(seq, m_token));
}

void BEClient::startHeartbeat() {
    stopHeartbeat();
    m_hbStop.store(false);
    m_hbThread = std::thread([this]() { heartbeatLoop(); });
}

void BEClient::stopHeartbeat() {
    m_hbStop.store(true);
    m_hbCv.notify_all();
    if (m_hbThread.joinable()) {
        m_hbThread.join();
    }
}

void BEClient::heartbeatLoop() {
    int64_t seq = 0;
    while (!m_hbStop.load() && !m_closing.load()) {
        ++seq;
        if (!sendHeartbeat(seq)) {
            logWarn("[BE] heartbeat send failed");
            break;
        }
        int64_t ms = m_hbPeriodMs.load();
        if (ms < 2000) ms = 5000;
        if (ms > 30000) ms = 30000;

        std::unique_lock lock(m_hbMu);
        m_hbCv.wait_for(lock, std::chrono::milliseconds(ms),
                        [this]() { return m_hbStop.load() || m_closing.load(); });
    }
}

void BEClient::handleIncomingBinary(const uint8_t* data, size_t len) {
    CGMessage msg;
    std::string decodeErr;
    const bool decoded = decodeCGMessage(data, len, msg, &decodeErr);
    if (!decoded && msg.cmd == 0) {
        return;
    }

    if (msg.cmd == kCmdRspHeartbeat) {
        const auto rsp = parseHeartbeatRsp(msg.payload.data(), msg.payload.size());
        const int64_t count = m_hbAckCount.fetch_add(1) + 1;
        if (rsp.periodMs >= 1000 && rsp.periodMs <= 60000) {
            m_hbPeriodMs.store(rsp.periodMs);
        }
        if (count == 1 || rsp.errcode != 0 || count % 12 == 0) {
            logInfo("[BE] heartbeat ACK #" + std::to_string(count) +
                    " err=" + std::to_string(rsp.errcode) +
                    " period=" + std::to_string(rsp.periodMs) + "ms");
        }
        return;
    }

    if (msg.cmd == kCmdCfmStreamStatus) {
        const auto rsp = parseHeartbeatRsp(msg.payload.data(), msg.payload.size());
        logInfo("[BE] StreamStatus ACK (cmd=38) err=" + std::to_string(rsp.errcode));
        return;
    }

    if (msg.cmd == kCmdRspAllocVPad) {
        const auto rsp = parseAllocVPadRsp(msg.payload.data(), msg.payload.size());
        logInfo("[BE] AllocVPad Rsp (cmd=34) ok=" + std::to_string(rsp.ok ? 1 : 0) +
                " vpadId=" + std::to_string(rsp.vpadId) +
                " err=" + std::to_string(rsp.errcode));
        if (rsp.ok) {
            VPadCallback cb;
            {
                std::lock_guard lock(m_cbMu);
                cb = m_vpadCb;
            }
            if (cb) {
                cb(rsp.vpadId, rsp.errcode);
            }
        }
        return;
    }

    if (msg.cmd == kCmdNtfGameStatus) {
        std::vector<ProtoField> fields;
        if (!parseFields(msg.payload.data(), msg.payload.size(), fields)) {
            return;
        }
        const uint64_t st = getFieldU64(fields, 3, 0);
        const auto* resBytes = getFieldBytes(fields, 6);
        logDebug("[BE] cmd=17 gameStatus=" + std::to_string(st) +
                 " resLen=" + std::to_string(resBytes ? resBytes->size() : 0));
        if (resBytes && !resBytes->empty()) {
            GameResource res;
            if (parseGameResource(resBytes->data(), resBytes->size(), res) &&
                !res.ipv4.empty() && res.basePort > 0) {
                std::lock_guard lock(m_stateMu);
                if (!m_hasResource) {
                    m_resource = std::move(res);
                    m_hasResource = true;
                    m_stateCv.notify_all();
                }
            }
        }
        return;
    }

    if (msg.cmd == kCmdRspLaunchGame) {
        const std::string text(reinterpret_cast<const char*>(msg.payload.data()), msg.payload.size());
        if (text.find("another game running") != std::string::npos ||
            text.find("running") != std::string::npos) {
            logWarn("[BE] cmd=8 indicates another game running; sending ExitGame");
            sendExit(0);
            {
                std::lock_guard lock(m_stateMu);
                m_hasFatalLaunchError = true;
                m_fatalLaunchError = "云端已有游戏在运行 (another game running)";
            }
            m_stateCv.notify_all();
        }
        return;
    }
}

bool BEClient::waitResource(int timeoutMs, GameResource& out, std::string* errOut) {
    std::unique_lock lock(m_stateMu);
    const bool ok = m_stateCv.wait_for(
        lock, std::chrono::milliseconds(timeoutMs),
        [this]() { return m_hasResource || m_hasFatalLaunchError || m_closing.load(); });
    if (m_hasResource) {
        out = m_resource;
        return true;
    }
    if (m_hasFatalLaunchError) {
        if (errOut) *errOut = m_fatalLaunchError;
        return false;
    }
    if (!ok) {
        if (errOut) *errOut = "等待边缘节点资源超时";
        return false;
    }
    if (errOut) *errOut = "BE 连接已关闭";
    return false;
}

bool BEClient::sendStreamStatus(bool playing) {
    return sendCommand(kCmdNtfStreamStatus, buildStreamStatusNtf(playing, m_token));
}

bool BEClient::sendAllocVPad(std::string_view vpadCert, int padIndex, int usage) {
    return sendCommand(kCmdReqAllocVPad, buildAllocVPadReq(m_token, vpadCert, padIndex, usage));
}

bool BEClient::releaseVPad(int vpadId) {
    return sendCommand(kCmdReqRecycleVPad, buildRecycleVPadReq(m_token, vpadId));
}

bool BEClient::sendExit(int64_t pkgName) {
    return sendCommand(kCmdReqExitGame, buildExitGameReq(m_token, pkgName));
}

} // namespace levelup::ytcx
