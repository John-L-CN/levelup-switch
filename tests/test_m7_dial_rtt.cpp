// M7: 多区域测速 / 边缘节点选路（官方 lp-sdk muti_area_rtt 等价实现）
//
// 覆盖四块纯逻辑：
//   1. p95 计算与超时样本剔除（官方 math.b.a(999, 0.95f) + math.e.a(list, 0.95f)）
//   2. 测速缓存：key 形状、JSON 往返、只留最近 10 条、同 key 覆盖
//   3. apply 请求体：dialtest_history_data / area_type / try_area_type 注入
//   4. apply 响应解析：area_type / idc / area_stun_* / spare_address / uuid / client_info
//
// 编译（host，Linux）：
//   g++ -std=c++20 -DLEVELUP_NO_CURL -DLEVELUP_NO_RTC -Iapp/src
//       tests/test_m7_dial_rtt.cpp app/src/levelup/crypto_util.cpp
//       app/src/levelup/proto_lite.cpp app/src/levelup/ytcx_client.cpp -o t_m7 -lpthread
#include "levelup/crypto_util.hpp"
#include "levelup/ytcx_client.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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

    // ---- 7. 打分 / 选址 / 学习（v0.3.0）----
    {
        const int64_t now = 1700000000000LL;
        // 无数据
        {
            DialSample s = makeSample("1.1.1.1", 3, 12, kDialFailMs, now);
            assert(dialScore(s, now) == kDialScoreNoData);
        }
        // 只有探测值
        {
            DialSample s = makeSample("1.1.1.1", 3, 12, 40, now);
            assert(dialScore(s, now) == 40);
        }
        // 探测值过期（>24h）→ +1500 降权
        {
            DialSample s = makeSample("1.1.1.1", 3, 12, 40, now - kDialProbeMaxAgeMs - 1000);
            assert(dialScore(s, now) == 1540);
        }
        // live 0.6 / probe 0.4
        {
            DialSample s = makeSample("1.1.1.1", 3, 12, 50, now);
            s.liveRttMs = 20;
            s.liveTs = now;
            s.lossPermille = 0;
            assert(dialScore(s, now) == (20 * 3 + 50 * 2) / 5);
        }
        // 丢包惩罚 + 失败惩罚 + 排队惩罚 + 被拒惩罚
        {
            DialSample s = makeSample("1.1.1.1", 3, 12, 40, now);
            s.liveRttMs = 40;
            s.liveTs = now;
            s.lossPermille = 60;   // +800
            s.liveOk = false;      // +3000
            s.rejects = 2;         // +800
            s.queueRank = 3;       // +300 + 60
            assert(dialScore(s, now) == 40 + 800 + 3000 + 800 + 360);
        }
    }
    {
        const int64_t now = 1700000000000LL;
        std::vector<DialSample> cache = {
            makeSample("1.1.1.1", 3, 12, 80, now),   // 80ms
            makeSample("1.1.1.1", 7, 12, 35, now),   // 35ms ← 最优
            makeSample("1.1.1.1", 9, 12, kDialFailMs, now),
        };
        int score = 0;
        assert(pickPreferredArea(cache, now, &score) == 7);
        assert(score == 35);
        // 最优区域被服务端拒过两次 → 罚 800 后超过信任阈值，改选次优
        cache = recordAreaRejected(std::move(cache), 7, 0);
        cache = recordAreaRejected(std::move(cache), 7, 0);
        assert(pickPreferredArea(cache, now, &score) == 3);
        // 全部不可信 → 0（交给服务端）
        std::vector<DialSample> bad = {makeSample("1.1.1.1", 3, 12, kDialFailMs, now)};
        assert(pickPreferredArea(bad, now, &score) == 0);
        assert(score == kDialScoreNoData);
    }
    {
        const int64_t now = 1700000000000LL;
        std::vector<DialSample> cache = {makeSample("1.1.1.1", 3, 12, 60, now)};
        cache = recordLiveResult(std::move(cache), "1.1.1.1", 3, 12, 22, 3, 1200, true);
        assert(cache.size() == 1);
        assert(cache[0].liveRttMs == 22);
        assert(cache[0].lossPermille == 3);
        assert(cache[0].firstFrameMs == 1200);
        assert(cache[0].liveOk);
        assert(cache[0].liveTs > 0);
        // 没有对应 area/idc 条目时新建（用 apply 回显的公网 IP 作 key）
        cache = recordLiveResult(std::move(cache), "1.1.1.1", 5, 21, 45, 0, 2000, true);
        assert(cache.size() == 2);
        bool sawNew = false;
        for (const auto& s : cache) {
            if (s.areaType == 5 && s.idc == 21) {
                sawNew = true;
                assert(s.liveRttMs == 45 && s.rtt95 == 45);
            }
        }
        assert(sawNew);
    }
    {
        // 排队记账
        std::vector<DialSample> cache = {makeSample("1.1.1.1", 3, 12, 60, 1700000000000LL)};
        cache = recordQueueRank(std::move(cache), 3, 12, 7);
        assert(cache[0].queueRank == 7);
        cache = recordQueueRank(std::move(cache), 3, 12, 0);
        assert(cache[0].queueRank == 0);
    }

    // ---- 8. 旧缓存兼容：只有官方字段的文件仍可读，新字段取默认值 ----
    {
        const std::string legacy = R"({"1.2.3.4-3-12":{"ts":1700000000000,"ip":"1.2.3.4",
            "isp":"电信","country":"CN","province":"山西","city":"太原",
            "area_type":3,"idc":12,"rtt_95":42}})";
        std::vector<DialSample> back;
        assert(dialCacheFromJson(legacy, back));
        assert(back.size() == 1);
        assert(back[0].rtt95 == 42);
        assert(back[0].liveRttMs == 0);
        assert(back[0].lossPermille == -1);
        assert(back[0].firstFrameMs == -1);
        assert(back[0].liveOk);
        assert(back[0].rejects == 0);
        assert(back[0].queueRank == 0);
        // 再写回去要带上新字段（老 readers 忽略即可）
        auto root = nlohmann::json::parse(dialCacheToJson(back));
        assert(root["1.2.3.4-3-12"].contains("live_rtt"));
        assert(root["1.2.3.4-3-12"].contains("queue_rank"));
    }

    // 10. 字段类型异常（null / 错误类型）不能抛未捕获异常
    {
        std::vector<DialSample> bad;
        const bool ok = dialCacheFromJson(
            R"({"1.2.3.4-3-12":{"ip":null,"area_type":"bad","rtt_95":null}})",
            bad);
        assert(ok);
        assert(bad.size() == 1);
        assert(bad[0].areaType == 0);
        assert(bad[0].rtt95 == kDialFailMs);
    }

    // 11. resolveDialEndpoint：优先用 rtt_ping_address 的 IPv4 + rtt_ws_url 的端口
    {
        DialSample s;
        s.rttWsUrl = "wss://lt-zj-9077-st-point.yuntiancloud.com:18256/v1/dialpoint";
        s.rttPingUrl = "221.204.53.137";
        std::string host;
        int port = 0;
        assert(resolveDialEndpoint(s, host, port));
        assert(host == "221.204.53.137");
        assert(port == 18256);

        s.rttPingUrl.clear();
        assert(resolveDialEndpoint(s, host, port));
        assert(host == "lt-zj-9077-st-point.yuntiancloud.com");
        assert(port == 18256);

        s.rttWsUrl.clear();
        s.rttPingUrl = "116.136.46.148:18088";
        assert(resolveDialEndpoint(s, host, port));
        assert(host == "116.136.46.148");
        assert(port == 18088);

        DialSample empty;
        assert(!resolveDialEndpoint(empty, host, port));
    }

    // 12. pickPreferredAreas：主/备双区域选取与单可信区域回退
    {
        const int64_t now = 1700000000000LL;
        std::vector<DialSample> cache = {
            makeSample("1.1.1.1", 4090, 9077, 25, now),
            makeSample("1.1.1.1", 4100, 9505, 42, now),
            makeSample("1.1.1.1", 4110, 9600, kDialFailMs, now),
        };
        int primary = -1;
        int secondary = -1;
        int score = 0;
        pickPreferredAreas(cache, now, primary, secondary, &score);
        assert(primary == 4090);
        assert(secondary == 4100);
        assert(score == 25);

        std::vector<DialSample> single = {makeSample("1.1.1.1", 4090, 9077, 30, now)};
        pickPreferredAreas(single, now, primary, secondary, &score);
        assert(primary == 4090);
        assert(secondary == 0);
    }

    // 13. mergeDialCache：新测速写入时保留已学习的 live 质量与调度反馈字段
    {
        const int64_t now = 1700000000000LL;
        DialSample prev = makeSample("1.1.1.1", 4090, 9077, 40, now - 5000);
        prev.liveRttMs = 19;
        prev.lossPermille = 4;
        prev.firstFrameMs = 2100;
        prev.liveOk = true;
        prev.liveTs = now - 4000;
        prev.rejects = 1;
        prev.queueRank = 2;

        DialSample fresh = makeSample("1.1.1.1", 4090, 9077, 22, now);
        const auto merged = mergeDialCache({prev}, {fresh});
        assert(merged.size() == 1);
        assert(merged[0].rtt95 == 22);
        assert(merged[0].ts == now);
        assert(merged[0].liveRttMs == 19);
        assert(merged[0].lossPermille == 4);
        assert(merged[0].firstFrameMs == 2100);
        assert(merged[0].liveTs == now - 4000);
        assert(merged[0].rejects == 1);
        assert(merged[0].queueRank == 2);
    }

    // 14. parseUnionApplyResponse：body / dialpoint 兜底 area_type 与 null 防护
    {
        const std::string resp = R"({
          "ret": {"code": 0, "msg": "ok"},
          "body": {
            "flow_id": "flow-1",
            "token": "a-123-b",
            "area_type": 4090,
            "idc": 9077,
            "access_info": {
              "access_ipv4_address": "203.0.113.20",
              "access_ipv4_port": 18000,
              "area_type": null,
              "idc": 0
            },
            "dialpoint": {"rtt_ws_url": "ws://203.0.113.20:18256/probe"}
          }
        })";
        ApplyResult res;
        assert(parseUnionApplyResponse(resp, res));
        assert(res.areaType == 4090);
        assert(res.idc == 9077);

        // 字符串形式的 area_type / idc（如 "4090" / "9077"）也能正确解析；同时 schema 打印原始值
        const std::string strResp = R"({
          "ret": {"code": 0, "msg": "ok"},
          "body": {
            "flow_id": "flow-2",
            "token": "a-456-b",
            "access_info": {
              "access_ipv4_address": "203.0.113.21",
              "access_ipv4_port": 18000,
              "area_type": "4090",
              "idc": "9077"
            }
          }
        })";
        ApplyResult strRes;
        assert(parseUnionApplyResponse(strResp, strRes));
        assert(strRes.areaType == 4090);
        assert(strRes.idc == 9077);

        const std::string zeroSchema = describeApplySchema(R"({
          "access_info": {"access_ipv4_address": "1.2.3.4", "area_type": 0, "idc": 0}
        })");
        assert(zeroSchema.find("ai.area_type=0") != std::string::npos);
        assert(zeroSchema.find("ai.idc=0") != std::string::npos);
    }

#if !defined(_WIN32)
    // 15. 回环 TCP 测速验证：开放端口返回 [1, kDialFailMs - 1]，关闭端口失败并给出错误原因
    {
        const int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(listenFd >= 0);
        int reuse = 1;
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0);
        assert(::bind(listenFd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) == 0);
        assert(::listen(listenFd, 4) == 0);
        socklen_t addrLen = sizeof(addr);
        assert(::getsockname(listenFd, reinterpret_cast<struct sockaddr*>(&addr), &addrLen) == 0);
        const int openPort = static_cast<int>(ntohs(addr.sin_port));
        assert(openPort > 0);

        std::string err;
        const int openMs = dialProbeOnceMs("127.0.0.1", openPort, 500, &err);
        assert(openMs >= 1 && openMs < kDialFailMs);

        DialSample sample = makeSample("127.0.0.1", 4090, 9077, kDialFailMs, 0);
        sample.rttPingUrl = "127.0.0.1";
        sample.rttWsUrl = "wss://unused.invalid:" + std::to_string(openPort) + "/v1/dialpoint";
        const auto probed = dialProbe({sample}, 2, 500, nullptr, 1);
        assert(probed.size() == 1);
        assert(probed[0].rtt95 >= 1 && probed[0].rtt95 < kDialFailMs);

        ::close(listenFd);
        err.clear();
        const int closedMs = dialProbeOnceMs("127.0.0.1", openPort, 500, &err);
        assert(closedMs < 0);
        assert(!err.empty());
    }
#endif

    std::cout << "test_m7_dial_rtt: all assertions passed\n";
    return 0;
}
