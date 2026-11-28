#pragma once

#include "levelup/h264_depacketizer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace levelup::video {

struct CroppedUvResult {
    float uvData[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    float srcOffsetX = 0.0f;
    float srcOffsetY = 0.0f;
    float srcWidth = 0.0f;
    float srcHeight = 0.0f;
};

/// Computes visible cropped region & UV transform matching texture_fsh.glsl:
///   uv = (vTextureCoord - uv_data.xy) * uv_data.zw
CroppedUvResult computeCroppedUvTransform(int codedWidth,
                                          int codedHeight,
                                          int cropLeft,
                                          int cropRight,
                                          int cropTop,
                                          int cropBottom,
                                          int screenWidth,
                                          int screenHeight);

struct PipelineStatsSnapshot {
    uint64_t rtpPackets = 0;
    uint64_t rtpOutOfOrder = 0;
    uint64_t rtpGaps = 0;
    uint64_t framesDepacked = 0;
    uint64_t lostFrames = 0;
    uint64_t skippedFrames = 0;
    uint64_t fedFrames = 0;
    uint64_t decodedFrames = 0;
    uint64_t noOutputFrames = 0;
    uint64_t decodeErrors = 0;
    uint64_t decoderResets = 0;
    uint64_t keyRequests = 0;
    uint64_t queueFullDrops = 0;
    uint32_t decodeTimeUs = 0;
    int width = 0;
    int height = 0;
    bool needKey = false;
};

class VideoPipeline {
  public:
    using KeyFrameRequestCallback = std::function<void()>;
    using NackRequestCallback = std::function<void(const std::vector<uint16_t>& missingSeqs)>;
    using FirstFrameCallback = std::function<void()>;
    using BeforeCodecCloseCallback = std::function<void()>;

    VideoPipeline();
    ~VideoPipeline();

    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    void setKeyFrameRequester(KeyFrameRequestCallback cb);
    void setNackRequester(NackRequestCallback cb);
    void setFirstFrameCallback(FirstFrameCallback cb);
    void setBeforeCodecCloseCallback(BeforeCodecCloseCallback cb);

    void start(int initialWidth = 1280, int initialHeight = 720, int targetFps = 60);
    void stop();

    void pushRtp(uint16_t seq, uint8_t pt, const uint8_t* payload, size_t len, bool marker, uint32_t ts);
    void onTransportLoss(int64_t lossDelta);
    void requestRecoveryKeyFrame(const std::string& reason);
    void setPaused(bool paused);
    bool isPaused() const { return m_paused.load(std::memory_order_relaxed); }

    PipelineStatsSnapshot stats() const;

  private:
    struct FrameItem {
        std::vector<uint8_t> annexb;
        bool isKey = false;
        uint32_t pts = 0;
        bool flushBeforeFeed = false;
    };

    void onDepacked(DepackedFrame&& frame);
    void triggerKeyFrameRequest();
    void decodeLoop();
    void watchdogLoop();

    H264Depacketizer m_depack;
    RtpReorderBuffer m_reorder{RtpReorderBuffer::kDefaultLimit, RtpReorderBuffer::kDefaultWaitMs};
    mutable std::mutex m_rtpMu;

    SelfHealPolicy m_heal;
    mutable std::mutex m_healMu;

    std::vector<uint8_t> m_paramPrefix;

    std::mutex m_queueMu;
    std::condition_variable m_queueCv;
    std::deque<FrameItem> m_frameQueue;

    std::mutex m_cbMu;
    KeyFrameRequestCallback m_reqKeyCb;
    NackRequestCallback m_reqNackCb;
    FirstFrameCallback m_firstFrameCb;
    BeforeCodecCloseCallback m_beforeCodecCloseCb;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_firstFrameFired{false};
    std::thread m_decodeThread;
    std::thread m_watchdogThread;
    std::mutex m_wdMu;
    std::condition_variable m_wdCv;

    // Stats
    std::atomic<uint64_t> m_framesDepacked{0};
    std::atomic<uint64_t> m_lostFrames{0};
    std::atomic<uint64_t> m_skippedFrames{0};
    std::atomic<uint64_t> m_fedFrames{0};
    std::atomic<uint64_t> m_decodedFrames{0};
    std::atomic<uint64_t> m_noOutputFrames{0};
    std::atomic<uint64_t> m_decodeErrors{0};
    std::atomic<uint64_t> m_decoderResets{0};
    std::atomic<uint64_t> m_keyRequests{0};
    std::atomic<uint64_t> m_queueFullDrops{0};
    std::atomic<uint32_t> m_decodeTimeUs{0};
    std::atomic<int> m_width{0};
    std::atomic<int> m_height{0};
};

} // namespace levelup::video
