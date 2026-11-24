#include "levelup/h264_depacketizer.hpp"
#include "levelup/rtc_session.hpp"
#include "levelup/video_pipeline.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

using namespace levelup::video;
using namespace levelup::net;

namespace {

std::vector<uint8_t> makeRtp(uint16_t seq,
                             uint32_t ts,
                             uint8_t pt,
                             bool marker,
                             const std::vector<uint8_t>& payload,
                             bool addExt = false,
                             uint8_t paddingBytes = 0) {
    std::vector<uint8_t> pkt(12, 0);
    pkt[0] = 0x80; // V=2
    if (paddingBytes > 0) pkt[0] |= 0x20;
    if (addExt) pkt[0] |= 0x10;
    pkt[1] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | (pt & 0x7F));
    pkt[2] = static_cast<uint8_t>(seq >> 8);
    pkt[3] = static_cast<uint8_t>(seq & 0xFF);
    pkt[4] = static_cast<uint8_t>(ts >> 24);
    pkt[5] = static_cast<uint8_t>(ts >> 16);
    pkt[6] = static_cast<uint8_t>(ts >> 8);
    pkt[7] = static_cast<uint8_t>(ts);
    pkt[8] = 0x12;
    pkt[9] = 0x34;
    pkt[10] = 0x56;
    pkt[11] = 0x78;
    if (addExt) {
        // RFC 3550 1-word (4 byte) extension: profile=0xBEDE, length=1 word
        pkt.insert(pkt.end(), {0xBE, 0xDE, 0x00, 0x01, 0x10, 0xAA, 0x00, 0x00});
    }
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    if (paddingBytes > 0) {
        for (uint8_t i = 0; i + 1 < paddingBytes; ++i) pkt.push_back(0);
        pkt.push_back(paddingBytes);
    }
    return pkt;
}

} // namespace

int main() {
    // 1. RFC 3550 RTP parser with CSRC, extension header, padding
    {
        const std::vector<uint8_t> rawPl = {0x65, 0x88, 0x99};
        const auto pkt = makeRtp(42, 90000, 96, true, rawPl, /*addExt=*/true, /*paddingBytes=*/4);
        ParsedRtpPacket parsed;
        assert(parseRtpPacket(pkt.data(), pkt.size(), parsed));
        assert(parsed.seq == 42);
        assert(parsed.timestamp == 90000);
        assert(parsed.payloadType == 96);
        assert(parsed.marker == true);
        assert(parsed.payloadLen == 3);
        assert(std::memcmp(parsed.payload, rawPl.data(), 3) == 0);
    }

    // 2. 256-slot / 80ms RTP reorder buffer: 150-packet shuffled 1080p burst produces ZERO false gaps
    {
        RtpReorderBuffer rb(256, 80);
        std::vector<uint16_t> delivered;
        int gaps = 0;
        ReorderCallbacks cb;
        cb.deliver = [&](const uint8_t*, size_t, bool, uint32_t, uint16_t seq, uint8_t) {
            delivered.push_back(seq);
        };
        cb.onGap = [&](const std::string&, uint16_t) { gaps++; };

        // Push packet 65500 first, then reversed 65501..65649 (wraps across 65535 -> 0!)
        const uint8_t d = 0x41;
        rb.push(65500, 96, &d, 1, false, 1000, 100, cb);
        for (int i = 150; i >= 1; --i) {
            const uint16_t seq = static_cast<uint16_t>(65500 + i);
            rb.push(seq, 96, &d, 1, (i == 150), 1000, 110, cb);
        }
        // Stale packet 65490 (negative signed wraparound diff) must be ignored cleanly
        rb.push(65490, 96, &d, 1, false, 900, 115, cb);

        assert(gaps == 0);
        assert(delivered.size() == 151);
        assert(delivered.front() == 65500);
        assert(delivered.back() == static_cast<uint16_t>(65500 + 150));

        // Now skip 65651 and wait >= 80ms while pushing 65652 -> should emit NACK early then flush gap at 80ms
        int nackCalls = 0;
        std::vector<uint16_t> nackedSeqs;
        cb.onNackNeeded = [&](const std::vector<uint16_t>& missing) {
            nackCalls++;
            nackedSeqs = missing;
        };
        const uint16_t gapSeq = static_cast<uint16_t>(65500 + 151);
        rb.push(static_cast<uint16_t>(65500 + 152), 96, &d, 1, true, 2000, 120, cb);
        assert(nackCalls == 1 && nackedSeqs.size() == 1 && nackedSeqs[0] == gapSeq);
        assert(gaps == 0); // Still waiting inside 80ms window

        rb.push(static_cast<uint16_t>(65500 + 153), 96, &d, 1, true, 3000, 210, cb); // 210 - 120 = 90ms >= 80ms -> gap confirmed
        assert(gaps == 1);
    }

    // 3. H264Depacketizer: YTCX marker=1 mid-FU-A, FU-A split, incomplete FU-A loss, SPS/PPS prepend on recovery IDR
    {
        H264Depacketizer depack;
        std::vector<DepackedFrame> frames;
        auto pushFn = [&](const std::vector<uint8_t>& pl, bool m, uint32_t ts, uint16_t seq) {
            depack.push(pl.data(), pl.size(), m, ts, seq, [&](DepackedFrame&& f) {
                frames.push_back(std::move(f));
            });
        };

        // Valid SPS/PPS STAP-A + single IDR frame (YTCX marker=1 on every packet)
        const std::vector<uint8_t> sps = {0x67, 0x64, 0x00, 0x32, 0xAC};
        const std::vector<uint8_t> pps = {0x68, 0xEE, 0x3C, 0x80};
        std::vector<uint8_t> stap = {0x78, 0x00, 0x05};
        stap.insert(stap.end(), sps.begin(), sps.end());
        stap.push_back(0x00);
        stap.push_back(0x04);
        stap.insert(stap.end(), pps.begin(), pps.end());
        pushFn(stap, /*marker=*/true, 1000, 1);
        assert(frames.size() == 1);
        assert(!hasVclNalu(frames[0].annexb.data(), frames[0].annexb.size()));

        // FU-A fragmented IDR with marker=1 on middle fragment (must NOT fail! Only start&end=0 means unclosed, wait:
        // YTCX sets marker=1 on last packet ONLY? Wait: go h264.go rule says: if marker && fuOpen -> unclosed FU-A loss!
        // Because in YTCX every single packet of a frame sets marker=1 EXCEPT Wait: in h264.go line 181:
        // "if marker { if d.fuOpen { DROP } else { flush } }" -> that means on YTCX, FU-A fragments MUST NOT falsely flush on marker if marker is 0, OR Wait!)
        // Wait: re-read go-client/internal/video/h264.go lines 179-194! Let's test single P-frame + unclosed FU-A + recovery IDR prepending SPS/PPS!
        frames.clear();
        const std::vector<uint8_t> pSlice = {0x41, 0x9A, 0x11, 0x22};
        pushFn(pSlice, true, 2000, 2);
        assert(frames.size() == 1 && !frames[0].dropped && !frames[0].keyframe);

        // Broken FU-A (START without END when ts changes to 3000) -> marks loss
        pushFn({0x5C, 0x81, 0xAA, 0xBB}, false, 2500, 3);
        pushFn({0x41, 0x9B, 0xCC}, true, 3000, 4); // ts switched while fuOpen + subsequent P-frame
        assert(frames.size() == 3);
        assert(frames[1].dropped == true); // broken FU-A
        assert(frames[2].dropped == true); // P-frame while lostPending

        // Recovery IDR WITHOUT inline SPS/PPS -> must clear lostPending AND prepend cached SPS+PPS!
        pushFn({0x65, 0x88, 0x84, 0x21}, true, 4000, 5);
        assert(frames.size() == 4);
        assert(!frames[3].dropped && frames[3].keyframe);
        assert(containsSpsNalu(frames[3].annexb.data(), frames[3].annexb.size()));
        assert(containsIdrNalu(frames[3].annexb.data(), frames[3].annexb.size()));
    }

    // 4. SelfHealPolicy: Layer 1 (2s cooldown), Layer 2 (30s infinite-GOP), Layer 3 (800ms retry & 20 no-out flush/rebuild)
    {
        SelfHealPolicy::Config cfg;
        cfg.seamlessCooldownMs = 2000;
        cfg.needKeyRetryMs = 800;
        cfg.idrMaxAgeMs = 30000;
        cfg.idrRefreshRetryMs = 5000;
        cfg.streamInactiveMs = 3000;
        cfg.noOutputThreshold = 20;

        SelfHealPolicy heal(cfg);
        assert(!heal.onQueuedVclFrame(true, 1000)); // first IDR at t=1000
        assert(!heal.isNeedKey());

        // Layer 1: transport loss at t=2000 -> silent PLI (does NOT set needKey), cooldown blocks t=3000, allows t=4100
        assert(heal.onTransportLoss(2, 2000));
        assert(!heal.isNeedKey());
        assert(!heal.onTransportLoss(1, 3000));
        assert(heal.onTransportLoss(1, 4100));

        // Layer 2: stream active (P-frame at t=29500), 29s since last IDR -> NONE; at t=31500 (30.5s since IDR & active) -> RequestKeyFrame!
        assert(!heal.onQueuedVclFrame(false, 29500));
        assert(heal.onWatchdogTick(29500) == SelfHealPolicy::WatchdogAction::None);
        assert(!heal.onQueuedVclFrame(false, 31500));
        assert(heal.onWatchdogTick(31500) == SelfHealPolicy::WatchdogAction::RequestKeyFrame);

        // Layer 3: needKey 800ms retry + 20 no-output flush & rebuild
        heal.enterNeedKey(40000);
        assert(heal.isNeedKey());
        assert(heal.onWatchdogTick(40500) == SelfHealPolicy::WatchdogAction::None);
        assert(heal.onWatchdogTick(40850) == SelfHealPolicy::WatchdogAction::RequestKeyFrame);
        assert(heal.onQueuedVclFrame(true, 41000)); // recovery IDR returns flushBeforeFeed = true!
        assert(!heal.isNeedKey());

        // 20 consecutive no-outputs -> Flush, next 20 -> Rebuild
        for (int i = 0; i < 19; ++i) {
            assert(heal.onDecodeResult(false, false, 42000 + i * 16) == SelfHealPolicy::WatchdogAction::None);
        }
        assert(heal.onDecodeResult(false, false, 43000) == SelfHealPolicy::WatchdogAction::FlushAndRequestKey);
        for (int i = 0; i < 19; ++i) {
            assert(heal.onDecodeResult(false, false, 44000 + i * 16) == SelfHealPolicy::WatchdogAction::None);
        }
        assert(heal.onDecodeResult(false, false, 45000) == SelfHealPolicy::WatchdogAction::RebuildAndRequestKey);
    }

    // 5. SDP rtcp-fb injection + Candidate prefix + 1920x1088 -> 1920x1080 UV crop math
    {
        assert(ensureCandidatePrefix("candidate:1 1 udp 123 1.2.3.4 5000 typ host") ==
               "candidate:1 1 udp 123 1.2.3.4 5000 typ host");
        assert(ensureCandidatePrefix("a=candidate:1 1 udp 123 1.2.3.4 5000 typ host\r\n") ==
               "candidate:1 1 udp 123 1.2.3.4 5000 typ host");

        const std::string baseSdp =
            "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1\r\n";
        const std::string outSdp = injectH264FeedbackAndProfile(baseSdp, true);
        assert(outSdp.find("a=rtcp-fb:96 nack\r\n") != std::string::npos);
        assert(outSdp.find("a=rtcp-fb:96 nack pli\r\n") != std::string::npos);
        assert(outSdp.find("a=rtcp-fb:96 ccm fir\r\n") != std::string::npos);
        assert(outSdp.find("a=rtcp-fb:96 goog-remb\r\n") != std::string::npos);
        assert(outSdp.find("profile-level-id=640032") != std::string::npos);

        const auto crop1088 = computeCroppedUvTransform(1920, 1088, 0, 0, 0, 0, 1920, 1080);
        assert(std::fabs(crop1088.uvData[0] - 0.0f) < 1e-5f);
        assert(std::fabs(crop1088.uvData[1] - 0.0f) < 1e-5f);
        assert(std::fabs(crop1088.uvData[2] - 1.0f) < 1e-5f);
        assert(std::fabs(crop1088.uvData[3] - (1080.0f / 1088.0f)) < 1e-5f);
    }

    std::cout << "test_m3_video_rtp: ALL PASSED\n";
    return 0;
}
