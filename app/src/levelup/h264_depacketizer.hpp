#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace levelup::video {

struct ParsedRtpPacket {
    uint8_t payloadType = 0;
    bool marker = false;
    uint16_t seq = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
};

/// Parses full RFC 3550 header (V=2, P padding, X extension header, CC CSRC list).
bool parseRtpPacket(const uint8_t* data, size_t len, ParsedRtpPacket& out);

/// Fast Annex-B NALU scanner helpers
void forEachAnnexBNalu(const uint8_t* data,
                       size_t len,
                       const std::function<bool(uint8_t naluType, const uint8_t* nalu, size_t naluLen)>& cb);
bool hasVclNalu(const uint8_t* data, size_t len);
bool containsIdrNalu(const uint8_t* data, size_t len);
bool containsSpsNalu(const uint8_t* data, size_t len);
bool findFirstSpsNalu(const uint8_t* data, size_t len, std::vector<uint8_t>& outSps);

/// Minimal H.264 baseline/main/high SPS pic dimension parser (width, height in luma pixels).
bool parseH264SpsDimensions(const uint8_t* sps, size_t len, int& widthOut, int& heightOut);

struct DepackedFrame {
    std::vector<uint8_t> annexb;
    uint32_t pts = 0;
    bool keyframe = false;
    bool dropped = false;
};

/// RFC 6184 H.264 RTP depacketizer (Single/STAP-A/FU-A) with YTCX marker=1 protection & SPS/PPS cache.
class H264Depacketizer {
  public:
    using FrameCallback = std::function<void(DepackedFrame&& frame)>;

    void reset();
    void markLoss();

    void push(const uint8_t* payload,
              size_t len,
              bool marker,
              uint32_t ts,
              uint16_t seq,
              const FrameCallback& onFrame);

    std::vector<uint8_t> sps() const;
    std::vector<uint8_t> pps() const;

    int naluCount() const;
    int frameCount() const;
    int keyFrameCount() const;
    int droppedCount() const;

  private:
    void startNaluLocked(uint8_t header);
    void appendNaluLocked(const uint8_t* nalu, size_t len);
    void resetFrameLocked();
    void flushLocked(const FrameCallback& onFrame);
    std::vector<uint8_t> prependParamsLocked(const std::vector<uint8_t>& frame) const;

    mutable std::mutex m_mu;
    uint32_t m_curTs = 0;
    std::vector<uint8_t> m_frame;
    bool m_started = false;
    bool m_fuOpen = false;
    bool m_frameDropped = false;
    bool m_lostPending = false;
    uint16_t m_frameSeq = 0;
    bool m_frameSeqOk = false;

    bool m_frameKey = false;
    bool m_frameVcl = false;
    bool m_hasParam = false;

    std::vector<uint8_t> m_sps;
    std::vector<uint8_t> m_pps;

    int m_naluCount = 0;
    int m_frameCount = 0;
    int m_keyFrames = 0;
    int m_dropped = 0;
};

struct ReorderCallbacks {
    std::function<void(const uint8_t* payload, size_t len, bool marker, uint32_t ts, uint16_t seq, uint8_t pt)> deliver;
    std::function<void(const std::string& reason, uint16_t gapCount)> onGap;
    std::function<void(const std::vector<uint16_t>& missingSeqs)> onNackNeeded;
};

/// 256-slot / 80ms RTP reorder buffer with signed wraparound drop & immediate NACK list calculation.
class RtpReorderBuffer {
  public:
    static constexpr size_t kDefaultLimit = 256;
    static constexpr int64_t kDefaultWaitMs = 80;

    explicit RtpReorderBuffer(size_t limit = kDefaultLimit, int64_t waitMs = kDefaultWaitMs);

    void reset();
    void push(uint16_t seq,
              uint8_t pt,
              const uint8_t* payload,
              size_t len,
              bool marker,
              uint32_t ts,
              int64_t nowMs,
              const ReorderCallbacks& cb);

    size_t pendingSize() const { return m_pending.size(); }
    uint64_t packetsReceived() const { return m_packetsReceived; }
    uint64_t outOfOrderCount() const { return m_outOfOrder; }
    uint64_t gapCount() const { return m_gaps; }

  private:
    struct Item {
        std::vector<uint8_t> payload;
        bool marker = false;
        uint32_t ts = 0;
        uint16_t seq = 0;
        uint8_t pt = 0;
        int64_t arrivedMs = 0;
    };

    size_t m_limit;
    int64_t m_waitMs;
    bool m_started = false;
    uint16_t m_expect = 0;
    uint16_t m_lastSeq = 0;
    int64_t m_lastNackMs = 0;
    std::map<uint16_t, Item> m_pending;

    uint64_t m_packetsReceived = 0;
    uint64_t m_outOfOrder = 0;
    uint64_t m_gaps = 0;
};

/// Pure testable 3-layer automatic self-healing state machine matching PROTOCOL_SPEC 5.2 rule #4.
class SelfHealPolicy {
  public:
    struct Config {
        int64_t seamlessCooldownMs = 2000;
        int64_t needKeyRetryMs = 800;
        int64_t idrMaxAgeMs = 30000;
        int64_t idrRefreshRetryMs = 5000;
        int64_t streamInactiveMs = 3000;
        int noOutputThreshold = 20;
    };

    enum class WatchdogAction {
        None,
        RequestKeyFrame,
        FlushAndRequestKey,
        RebuildAndRequestKey,
    };

    SelfHealPolicy() : m_cfg{} {}
    explicit SelfHealPolicy(const Config& cfg) : m_cfg(cfg) {}

    void reset();

    /// Called when reference chain breaks (seq gap, broken FU-A, decode error).
    bool enterNeedKey(int64_t nowMs);

    /// Called when depacked recovery IDR is queued (leaves needKey). Returns true if reset flag needed.
    bool onQueuedVclFrame(bool isIdr, int64_t nowMs);

    bool isNeedKey() const { return m_needKey; }

    /// Layer 1: transport loss delta from RTCP/stats (silent PLI+FIR, 2s cooldown, does NOT enter needKey).
    bool onTransportLoss(int64_t lossDelta, int64_t nowMs);

    /// Periodic 1s tick handling Layer 2 (30s infinite-GOP silent IDR refresh) & Layer 3 (800ms needKey retry).
    WatchdogAction onWatchdogTick(int64_t nowMs, std::string* reasonOut = nullptr);

    /// Layer 3 stuck decoder detector (feed -> decoded or noOutput or error).
    WatchdogAction onDecodeResult(bool gotOutput, bool decodeError, int64_t nowMs);

  private:
    bool tryRequestKeyWithInterval(int64_t nowMs, int64_t minIntervalMs);

    Config m_cfg;
    bool m_needKey = false;
    int64_t m_lastKeyReqMs = 0;
    int64_t m_firstVclMs = 0;
    int64_t m_lastVclMs = 0;
    int64_t m_lastIdrMs = 0;
    int m_consecNoOut = 0;
    int m_consecErrors = 0;
    int m_resetsSinceDecode = 0;
};

} // namespace levelup::video
