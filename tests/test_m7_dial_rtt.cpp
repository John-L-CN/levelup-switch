// M7: 多区域测速 / 边缘节点选路（官方 lp-sdk muti_area_rtt 等价实现）
//
// 覆盖四块纯逻辑：
//   1. p95 计算与超时样本剔除（官方 math.b.a(999, 0.95f) + math.e.a(list, 0.95f)）
//   2. 测速缓存：key 形状、JSON 往返、只留最近 10 条、同 key 覆盖
//   3. apply 请求体：dialtest_history_data / area_type / try_area_type 注入
//   4. apply 响应解析：area_type / idc / area_stun_* / spare_address / uuid / client_info
//
// 编译（host，Linux）：
//   g++ -std=c++20 -DLEVELUP_NO_CURL -DLEVELUP_NO_RTC -Iapp/src \
//       tests/test_m7_dial_rtt.cpp app/src/levelup/crypto_util.cpp \
//       app/src/levelup/proto_lite.cpp app/src/levelup/ytcx_client.cpp -o t_m7 -lpthread
#include "levelup/crypto_util.hpp"
#include "levelup/ytcx_client.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace levelup;
using namespace levelup::ytcx;

namespace {

DialSample makeSample(const std::string& ip,
                      int areaType,
                      int idc,
                      int rtt95,
                      int64_t ts,
                      const std::string& ws = "ws://10.0.0.1:18090/probe") {
    DialSample s;
    s.clientIp = ip;
    s.areaType = areaType;
    s.idc = idc;
    s.isp = "电信";
    s.country = "CN";
    s.province = "山西";
    s.city = "太原";
    s.rttWsUrl = ws;
    s.rttPingUrl = "10.0.0.1:8000";
    s.rtt95 = rtt95;
    s.ts = ts;
    return s;
}

} // namespace

int main() {
    // ---- 1. p95 ----
    assert(dialP95({}) == kDialFailMs);
    assert(dialP95({kDialFailMs, kDialFailMs}) == kDialFailMs);
    assert(dialP95({42}) == 42);
    // 20 个样本 10..200：丢掉 5% (1 个最大) 后取最后一个 → 第 19 小 = 190
    {
        std::vector<int> v;
        for (int i = 1; i <= 20; ++i) v.push_back(i * 10);
        assert(dialP95(v) == 190);
    }
    // 超时样本先被剔除，不参与分位
    {
        std::vector<int> v = {30, 40, kDialFailMs, 35, kDialFailMs};
        assert(dialP95(v) == 40);
    }
    // 负数（connect 失败）也当超时丢掉
    assert(dialP95({-1, 25}) == 25);

    // ---- 2. 缓存 ----
    assert(dialCacheKey("1.2.3.4", 3, 12) == "1.2.3.4-3-12");

    {
        std::vector<DialSample> cache = {
            makeSample("1.2.3.4", 3, 12, 38, 1000),
            makeSample("1.2.3.4", 7, 21, 55, 2000),
        };
        const std::string json = dialCacheToJson(cache);
        auto root = nlohmann::json::parse(json);
        assert(root.is_object());
        assert(root.contains("1.2.3.4-3-12"));
        assert(root["1.2.3.4-3-12"]["rtt_95"] == 38);
        assert(root["1.2.3.4-3-12"]["area_type"] == 3);
        assert(root["1.2.3.4-3-12"]["idc"] == 12);
        assert(root["1.2.3.4-3-12"]["isp"] == "电信");

        std::vector<DialSample> back;
        assert(dialCacheFromJson(json, back));
        assert(back.size() == 2);
        bool sawArea3 = false;
        for (const auto& s : back) {
            if (s.areaType == 3) {
                sawArea3 = true;
                assert(s.idc == 12 && s.rtt95 == 38 && s.ts == 1000);
                assert(s.city == "太原" && s.country == "CN");
            }
        }
        assert(sawArea3);
    }
    {
        std::vector<DialSample> bad;
        assert(!dialCacheFromJson("not json", bad));
        assert(!dialCacheFromJson("[]", bad));
    }

    // 只留最近 10 条（按 ts）
    {
        std::vector<DialSample> many;
        for (int i = 0; i < 15; ++i) {
            many.push_back(makeSample("9.9.9.9", i, i, 10 + i, 100 + i));
        }
        const auto trimmed = trimDialCache(many, kDialCacheEntries);
        assert(trimmed.size() == kDialCacheEntries);
        assert(trimmed.front().ts == 105);  // 丢掉最旧的 5 条
        assert(trimmed.back().ts == 114);
    }
    // 同 key 覆盖 + 保持上限
    {
        std::vector<DialSample> cache = {makeSample("1.1.1.1", 3, 12, 90, 1000)};
        std::vector<DialSample> fresh = {makeSample("1.1.1.1", 3, 12, 21, 5000)};
        const auto merged = mergeDialCache(cache, fresh);
        assert(merged.size() == 1);
        assert(merged[0].rtt95 == 21);
        assert(merged[0].ts == 5000);
    }
    {
        std::vector<DialSample> cache;
        for (int i = 0; i < 9; ++i) cache.push_back(makeSample("1.1.1.1", i, i, 50, 1000 + i));
        std::vector<DialSample> fresh;
        for (int i = 9; i < 12; ++i) fresh.push_back(makeSample("1.1.1.1", i, i, 50, 1000 + i));
        const auto merged = mergeDialCache(cache, fresh);
        assert(merged.size() == kDialCacheEntries);
        assert(merged.back().areaType == 11);
    }

    // ---- 3. ws endpoint 解析 ----
    {
        std::string host;
        int port = 0;
        assert(parseWsEndpoint("ws://1.2.3.4:18090/probe?flow_id=x", host, port));
        assert(host == "1.2.3.4" && port == 18090);
        assert(parseWsEndpoint("wss://edge.example.com/ws", host, port));
        assert(host == "edge.example.com" && port == 443);
        assert(parseWsEndpoint("http://1.2.3.4:8000", host, port));
        assert(host == "1.2.3.4" && port == 8000);
        assert(!parseWsEndpoint("", host, port));
        assert(!parseWsEndpoint("1.2.3.4:18090", host, port));
    }

    // ---- 4. dialpoints URL ----
    {
        const std::string url = buildDialPointsUrl("123456789", 10086, "flow-abc");
        assert(url.rfind(std::string(kUnionDialPointsURL) + "?uuid=123456789&ugid=10086&flow_id=flow-abc", 0) == 0);
        // flow_id 为空时也合法（首次申请前测速）
        const std::string url2 = buildDialPointsUrl("1", 2, "");
        assert(url2.find("&flow_id=") != std::string::npos);
    }

    // ---- 5. apply body 注入 ----
    const std::vector<uint8_t> pbLaunch = {1, 2, 3};
    {
        const auto req = buildSignedUnionApplyRequest(
            "opaque-data", 10086, "dev-1", "app-user-1", 1280, 720, pbLaunch,
            "sig_1700000000", 1700000000000LL, {}, 0, 0);
        auto body = nlohmann::json::parse(req.bodyJson);
        assert(body["area_type"] == 0);
        assert(body["try_area_type"] == 0);
        assert(!body.contains("dialtest_history_data"));  // 首次申请与官方一致：不带历史
        assert(req.url.rfind(std::string(kUnionApplyURL) + "?AccessKey=", 0) == 0);
    }
    {
        std::vector<DialSample> history = {
            makeSample("203.0.113.7", 3, 12, 38, 1700000000000LL),
            makeSample("203.0.113.7", 7, 21, 61, 1700000001000LL),
        };
        const auto req = buildSignedUnionApplyRequest(
            "opaque-data", 10086, "dev-1", "app-user-1", 1280, 720, pbLaunch,
            "sig_1700000000", 1700000000000LL, history, 3, 7);
        auto body = nlohmann::json::parse(req.bodyJson);
        assert(body["area_type"] == 3);
        assert(body["try_area_type"] == 7);
        assert(body.contains("dialtest_history_data"));
        const auto& hist = body["dialtest_history_data"];
        assert(hist.is_array() && hist.size() == 2);
        assert(hist[0]["ip"] == "203.0.113.7");
        assert(hist[0]["area_type"] == 3);
        assert(hist[0]["idc"] == 12);
        assert(hist[0]["rtt_95"] == 38);
        assert(hist[0]["ts"] == 1700000000000LL);
        assert(hist[1]["rtt_95"] == 61);
        // 签名仍然走 apply 的 path/算法
        assert(req.stringToSign.rfind("POST&" + urlEncodeStrict("/v3/union/apply") + "&", 0) == 0);
    }

    // ---- 6. apply 响应解析（含调度回显与备用地址） ----
    {
        const std::string resp = R"({
          "ret": {"code": 0, "msg": ""},
          "body": {
            "flow_id": "flow-xyz",
            "token": "abc-def-ghi",
            "uid": 555,
            "uuid": 987654321,
            "access_info": {
              "access_ipv4_address": "203.0.113.20",
              "access_ipv4_port": 18000,
              "area_type": 3,
              "idc": 12,
              "area_stun_address": "203.0.113.30",
              "area_stun_port": 3478,
              "spare_address": [
                {"access_ipv4_address": "203.0.113.21", "access_ipv4_port": 18000},
                {"access_ipv4_address": "203.0.113.22", "access_ipv4_port": 18001}
              ]
            },
            "client_info": {"address": "198.51.100.9", "city": "太原", "province": "山西", "country": "CN", "isp": "电信"},
            "dialpoint": {"rtt_ws_url": "ws://203.0.113.20:18090/probe"}
          }
        })";
        ApplyResult res;
        std::string err;
        assert(parseUnionApplyResponse(resp, res, &err));
        assert(res.flowId == "flow-xyz");
        assert(res.token == "abc-def-ghi");
        assert(res.accessIp == "203.0.113.20");
        assert(res.accessPort == 18000);
        assert(res.areaType == 3);
        assert(res.idc == 12);
        assert(res.areaStunAddress == "203.0.113.30");
        assert(res.areaStunPort == 3478);
        assert(res.spareAddresses.size() == 2);
        assert(res.spareAddresses[0].first == "203.0.113.21");
        assert(res.spareAddresses[1].second == 18001);
        assert(res.clientAddress == "198.51.100.9");
        assert(res.lpUuid == "987654321");
        assert(res.currentDialWsUrl == "ws://203.0.113.20:18090/probe");
    }
    {
        // 缺 token / access_info 仍要判定失败
        ApplyResult res;
        std::string err;
        assert(!parseUnionApplyResponse(R"({"ret":{"code":0},"body":{}})", res, &err));
    }

    std::cout << "test_m7_dial_rtt: all assertions passed\n";
    return 0;
}
