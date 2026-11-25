#include "levelup/api_client.hpp"
#include "levelup/crypto_util.hpp"
#include "levelup/proto_lite.hpp"
#include "levelup/ytcx_client.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace levelup;
using namespace levelup::api;
using namespace levelup::ytcx;

int main() {
    // 1. MD5 / SHA1 / HMAC-SHA1 / Base64 / URL encoding
    assert(md5HexLower("") == "d41d8cd98f00b204e9800998ecf8427e");
    assert(md5HexLower("abc") == "900150983cd24fb0d6963f7d28e17f72");
    assert(urlEncodeStrict("abc_.-~ /+=") == "abc_.-~%20%2F%2B%3D");
    assert(urlEncodeSlashSafe("a/b+c=d") == "a/b%2Bc%3Dd");

    {
        const std::string bodyStr = R"({"a":1,"b":"hi/there"})";
        const auto m = md5Raw(bodyStr);
        const std::string bmd5 = base64Encode(m.data(), m.size());
        assert(bmd5 == "2ZVsagcf6o49+xzegRgCgg==");

        const std::string sts = "POST&" + urlEncodeStrict("/v3/union/apply") + "&" +
                                urlEncodeStrict(bmd5) + "&" +
                                urlEncodeStrict("AccessKey=k&Timestamp=1700000000123");
        const auto mac = hmacSha1Raw(std::string(kUnionSecretKey) + "&", sts);
        assert(base64Encode(mac.data(), mac.size()) == "qd0gimJhJyyRIrGiDpD9va+ThRA=");

        const auto dec = base64Decode("data:image/jpeg;base64," + bmd5);
        assert(dec.size() == 16 && std::memcmp(dec.data(), m.data(), 16) == 0);
    }

    // 2. API clientInfo exact key order + Go golden signature vector
    {
        const std::string devId = "0123456789abcdef0123456789abcdef";
        const std::string ci = buildClientInfoJson(devId);
        const std::string expectedPrefix =
            R"({"screen":"2560*1600","os":"14","sdkVersion":34,"deviceId":"0123456789abcdef0123456789abcdef")";
        assert(ci.rfind(expectedPrefix, 0) == 0);
        assert(ci.find(R"("ipAddressIpv4":"","ipAddress":""})") != std::string::npos);

        Credentials cred;
        cred.sessionKey = "test_sess_key";
        cred.ciKey = "test_ci_key";

        // 公共参数 + 固定密钥参与签名：基准向量（改动任何公共参数都会让它失配）
        const auto params = buildSignedParams(
            nlohmann::json{{"gameId", "2zkaqmg1"}}, devId, cred, 1700000000123LL);
        assert(params.at("sig") == "41e03a44e4733400adb7b82fd9f985b9");

        const auto payloadJson = nlohmann::json::parse(
            buildSignedPayloadJson(nlohmann::json{{"gameId", "2zkaqmg1"}}, devId, cred, 1700000000123LL));
        assert(payloadJson.at("apiKey") == kApiKey);
        assert(payloadJson.at("appVer") == kAppVer);
        assert(payloadJson.at("sig") == "41e03a44e4733400adb7b82fd9f985b9");

        const std::map<std::string, std::string> prefixCase = {{"a", "9"}, {"a2", "1"}};
        assert(signParams(prefixCase) == "5515236751bece33f340b4e28ed5673a");
    }

    // 3. API UserInfo HP/MP seconds -> minutes conversion
    {
        nlohmann::json envData = {
            {"userInfo", {{"userId", 1001}, {"nickName", "Player"}}},
            {"powerInfo",
             {{"vipExpireTime", "2027-01-01"},
              {"hpPower", 10},
              {"hpPowerDuration", 3600},
              {"mpPower", 15},
              {"mpPowerDuration", 0}}},
        };
        UserInfo ui;
        assert(parseUserInfoEnvelope(envData, ui));
        assert(ui.hpMinutes() == 60);
        assert(ui.mpMinutes() == 15);
    }

    // 4. ProtoLite CGMessage + BuildLaunchGameProto + GameResource + AllocVPad + HeartbeatRsp
    {
        const auto devJson = buildDeviceInfoJson(1280, 720);
        const auto stJson = buildStreamInfoJson(1280, 720, 60, 274, 3, true);
        assert(stJson.find(R"("clientSupportHevc":0)") != std::string::npos);
        assert(stJson.find(R"("vibration":true)") != std::string::npos);
        assert(stJson.find(R"("clientType":"switch-handheld")") != std::string::npos);

        const auto pbLaunch = buildLaunchGameProto(
            "opaque-pro-data", 274, devJson, stJson, 1700000000123LL, "Req_1");
        const auto wireMsg = encodeCGMessage(kCmdReqLaunchGame, pbLaunch);
        assert(wireMsg[0] == 0x0D);
        assert(wireMsg[5] == 0x15);

        CGMessage decoded;
        assert(decodeCGMessage(wireMsg.data(), wireMsg.size(), decoded));
        assert(decoded.cmd == kCmdReqLaunchGame);
        assert(decoded.version == kCGMagicVersion);
        assert(decoded.payload == pbLaunch);

        std::vector<ProtoField> lf;
        assert(parseFields(decoded.payload.data(), decoded.payload.size(), lf));
        assert(getFieldU64(lf, 2) == 274ULL);
        assert(getFieldString(lf, 3) == "opaque-pro-data");
        assert(getFieldString(lf, 7) == devJson);
        assert(getFieldString(lf, 8) == stJson);

        // Synthetic GameResource: ipv4="203.0.113.10", portBase=123 -> basePort=12300
        std::vector<uint8_t> resPb;
        appendStringField(resPb, 1, "203.0.113.10");
        appendVarintField(resPb, 2, 123);
        appendStringField(resPb, 10, "a-sess");
        appendStringField(resPb, 11, "v-sess");
        appendStringField(resPb, 12, "c-sess");

        GameResource gr;
        assert(parseGameResource(resPb.data(), resPb.size(), gr));
        assert(gr.ipv4 == "203.0.113.10");
        assert(gr.basePort == 12300);
        assert(gr.aSessionId == "a-sess");
        assert(gr.vSessionId == "v-sess");
        assert(gr.cSessionId == "c-sess");

        // AllocVPad request & response
        const auto allocReq = buildAllocVPadReq("u-999-tok", "", 0, 2, 1700000000999LL, "AllocVPad_1");
        std::vector<ProtoField> af;
        assert(parseFields(allocReq.data(), allocReq.size(), af));
        assert(getFieldU64(af, 2) == 65535ULL);
        assert(getFieldU64(af, 5) == 2ULL);

        std::vector<uint8_t> errSub;
        appendVarintField(errSub, 1, 0);
        std::vector<uint8_t> allocRspPb;
        appendBytesField(allocRspPb, 2, errSub);
        appendVarintField(allocRspPb, 3, 2);
        const auto allocRsp = parseAllocVPadRsp(allocRspPb.data(), allocRspPb.size());
        assert(allocRsp.ok && allocRsp.errcode == 0 && allocRsp.vpadId == 2);

        // Heartbeat response
        std::vector<uint8_t> hbRspPb;
        appendBytesField(hbRspPb, 2, errSub);
        appendVarintField(hbRspPb, 3, 7500);
        const auto hbRsp = parseHeartbeatRsp(hbRspPb.data(), hbRspPb.size());
        assert(hbRsp.errcode == 0 && hbRsp.periodMs == 7500);
    }

    // 5. UnionApply signing + token extraction + EdgeLaunch JSON
    {
        const auto pbLaunch = buildLaunchGameProto("opaque", 274, "{}", "{}", 1700000000000LL, "Req_1");
        const auto ureq = buildSignedUnionApplyRequest(
            "opaque", 274, "0123456789abcdef0123456789abcdef", "uid_raw", 1280, 720,
            pbLaunch, "sig_1700000000", 1700000000123LL);
        assert(ureq.url.rfind(std::string(kUnionApplyURL) + "?AccessKey=", 0) == 0);
        assert(ureq.url.find("&Signature=") != std::string::npos);
        assert(!ureq.signatureBase64.empty());
        assert(ureq.stringToSign.rfind("POST&%2Fv3%2Funion%2Fapply&", 0) == 0);

        assert(extractRealUserIdFromToken("prefix-12345678-suffix") == "12345678");
        assert(extractRealUserIdFromToken("plain") == "plain");

        EdgeLaunchParams ep;
        ep.edgeIp = "203.0.113.10";
        ep.basePort = 12300;
        ep.flowId = "flow-1";
        ep.userId = "12345678";
        ep.pkgName = 274;
        ep.deviceId = "0123456789abcdef0123456789abcdef";
        ep.aSessionId = "a";
        ep.vSessionId = "v";
        ep.cSessionId = "c";
        ep.width = 1920;
        ep.height = 1080;
        ep.maxFps = 60;
        ep.videoLevel = 3;
        ep.vqFirst = true;
        ep.vibration = true;

        const auto ej = nlohmann::json::parse(buildEdgeLaunchBodyJson(ep));
        assert(ej.at("vibration") == true);
        assert(ej.at("clientSupportHevc") == 0);
        assert(ej.at("clientType") == "switch-handheld");
        assert(ej.at("bitrateGuidanceAdjustMode") == 1);
        assert(ej.at("desiredWidth") == 1920);
        assert(ej.at("desiredHeight") == 1080);

        EdgeLaunchResult er;
        assert(parseEdgeLaunchResponse(R"({"encodedWidth":1920,"encodedHeight":1088})", 12300, er));
        assert(er.signalingPort == 12315); // basePort + 15 fallback
    }

    // 6. Random 32-hex deviceId
    const std::string rnd1 = randomHex32();
    const std::string rnd2 = randomHex32();
    assert(rnd1.size() == 32 && rnd2.size() == 32 && rnd1 != rnd2);

    std::cout << "test_m2_protocol: ALL PASSED\n";
    return 0;
}
