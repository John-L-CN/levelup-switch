#include "levelup/video_pipeline.hpp"

#include "levelup/crypto_util.hpp"
#include "levelup/log.hpp"

#include <algorithm>
#include <cstring>

#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}
#ifdef BOREALIS_USE_DEKO3D
#include "AVFrameHolder.hpp"
#endif
#endif

namespace levelup::video {

CroppedUvResult computeCroppedUvTransform(int codedWidth,
                                          int codedHeight,
                                          int cropLeft,
                                          int cropRight,
                                          int cropTop,
                                          int cropBottom,
                                          int screenWidth,
                                          int screenHeight) {
    CroppedUvResult res;
    if (codedWidth <= 0 || codedHeight <= 0 || screenWidth <= 0 || screenHeight <= 0) {
        return res;
    }

    int cl = std::max(0, cropLeft);
    int cr = std::max(0, cropRight);
    int ct = std::max(0, cropTop);
    int cb = std::max(0, cropBottom);
    int visW = codedWidth - cl - cr;
    int visH = codedHeight - ct - cb;
    if (visW <= 0 || visH <= 0) {
        cl = 0;
        ct = 0;
        visW = codedWidth;
        visH = codedHeight;
    }

    // Tegra X1 NVDEC decodes 1080p H.264 to 16-byte aligned 1920x1088 when container omits crop
    if (codedWidth == 1920 && codedHeight == 1088 && ct == 0 && cb == 0) {
        visH = 1080;
    }

    res.srcOffsetX = static_cast<float>(cl);
    res.srcOffsetY = static_cast<float>(ct);
    res.srcWidth = static_cast<float>(visW);
    res.srcHeight = static_cast<float>(visH);

    const float visAspect = static_cast<float>(visH) / static_cast<float>(visW);
    const float screenAspect = static_cast<float>(screenHeight) / static_cast<float>(screenWidth);

    float dstX = 0.0f;
    float dstY = 0.0f;
    float dstW = 1.0f;
    float dstH = 1.0f;
    if (visAspect > screenAspect) {
        dstW = screenAspect / visAspect;
        dstX = 0.5f * (1.0f - dstW);
    } else if (visAspect < screenAspect) {
        dstH = visAspect / screenAspect;
        dstY = 0.5f * (1.0f - dstH);
    }

    // texture_fsh.glsl: uv = (vTextureCoord - uv_data.xy) * uv_data.zw
    res.uvData[2] = (static_cast<float>(visW) / static_cast<float>(codedWidth)) / dstW;
    res.uvData[3] = (static_cast<float>(visH) / static_cast<float>(codedHeight)) / dstH;
    res.uvData[0] = dstX - (static_cast<float>(cl) / static_cast<float>(codedWidth)) / res.uvData[2];
    res.uvData[1] = dstY - (static_cast<float>(ct) / static_cast<float>(codedHeight)) / res.uvData[3];
    return res;
}

namespace {

constexpr size_t kMaxEncodedQueue = 64;

} // namespace

VideoPipeline::VideoPipeline() = default;

VideoPipeline::~VideoPipeline() {
    stop();
}

void VideoPipeline::setKeyFrameRequester(KeyFrameRequestCallback cb) {
    std::lock_guard lock(m_cbMu);
    m_reqKeyCb = std::move(cb);
}

void VideoPipeline::setNackRequester(NackRequestCallback cb) {
    std::lock_guard lock(m_cbMu);
    m_reqNackCb = std::move(cb);
}

void VideoPipeline::setFirstFrameCallback(FirstFrameCallback cb) {
    std::lock_guard lock(m_cbMu);
    m_firstFrameCb = std::move(cb);
}

void VideoPipeline::setBeforeCodecCloseCallback(BeforeCodecCloseCallback cb) {
    std::lock_guard lock(m_cbMu);
    m_beforeCodecCloseCb = std::move(cb);
}

void VideoPipeline::start(int initialWidth, int initialHeight, int targetFps) {
    stop();

    m_width.store(initialWidth);
    m_height.store(initialHeight);
    m_firstFrameFired.store(false);
    m_depack.reset();
    {
        std::lock_guard lock(m_rtpMu);
        m_reorder.reset();
        m_paramPrefix.clear();
    }
    {
        std::lock_guard lock(m_healMu);
        m_heal.reset();
    }
    {
        std::lock_guard lock(m_queueMu);
        m_frameQueue.clear();
    }

    m_framesDepacked.store(0);
    m_lostFrames.store(0);
    m_skippedFrames.store(0);
    m_fedFrames.store(0);
    m_decodedFrames.store(0);
    m_noOutputFrames.store(0);
    m_decodeErrors.store(0);
    m_decoderResets.store(0);
    m_keyRequests.store(0);
    m_queueFullDrops.store(0);

#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST) && defined(BOREALIS_USE_DEKO3D)
    Settings::instance().set_frames_queue_size(1);
    AVFrameHolder::instance().prepare(targetFps > 0 ? targetFps : 60, true, 3);
#else
    (void)targetFps;
#endif

    m_running.store(true);
    m_decodeThread = std::thread([this]() { decodeLoop(); });
    m_watchdogThread = std::thread([this]() { watchdogLoop(); });
}

void VideoPipeline::stop() {
    m_running.store(false);
    m_queueCv.notify_all();
    m_wdCv.notify_all();
    if (m_decodeThread.joinable()) {
        m_decodeThread.join();
    }
    if (m_watchdogThread.joinable()) {
        m_watchdogThread.join();
    }
    {
        std::lock_guard lock(m_queueMu);
        m_frameQueue.clear();
    }
#if defined(__SWITCH__) && !defined(LEVELUP_HOST_TEST) && defined(BOREALIS_USE_DEKO3D)
    AVFrameHolder::instance().cleanup();
#endif
}

void VideoPipeline::triggerKeyFrameRequest() {
    m_keyRequests.fetch_add(1);
    KeyFrameRequestCallback cb;
    {
        std::lock_guard lock(m_cbMu);
        cb = m_reqKeyCb;
    }
    if (cb) {
        cb();
    }
}

void VideoPipeline::requestRecoveryKeyFrame(const std::string& reason) {
    bool entered = false;
    {
        std::lock_guard lock(m_healMu);
        entered = m_heal.enterNeedKey(unixMillisNow());
    }
    if (entered) {
        logWarn("[Video] reference broken (" + reason + "): stopping P-frames until next IDR");
    }
    triggerKeyFrameRequest();
}

void VideoPipeline::onTransportLoss(int64_t lossDelta) {
    bool shouldRequest = false;
    {
        std::lock_guard lock(m_healMu);
        shouldRequest = m_heal.onTransportLoss(lossDelta, unixMillisNow());
    }
    if (shouldRequest) {
        logInfo("[Video] silent Layer-1 keyframe request after transport loss (+" +
                std::to_string(lossDelta) + ")");
        triggerKeyFrameRequest();
    }
}

void VideoPipeline::pushRtp(uint16_t seq,
                            uint8_t pt,
                            const uint8_t* payload,
                            size_t len,
                            bool marker,
                            uint32_t ts) {
    if (!m_running.load()) return;

    ReorderCallbacks rcb;
    rcb.deliver = [this](const uint8_t* pl, size_t plen, bool m, uint32_t rts, uint16_t rseq, uint8_t) {
        m_depack.push(pl, plen, m, rts, rseq, [this](DepackedFrame&& df) {
            onDepacked(std::move(df));
        });
    };
    rcb.onGap = [this](const std::string& reason, uint16_t) {
        m_depack.markLoss();
        requestRecoveryKeyFrame(reason);
    };
    rcb.onNackNeeded = [this](const std::vector<uint16_t>& missingSeqs) {
        NackRequestCallback cb;
        {
            std::lock_guard lock(m_cbMu);
            cb = m_reqNackCb;
        }
        if (cb) {
            cb(missingSeqs);
        }
    };

    std::lock_guard lock(m_rtpMu);
    m_reorder.push(seq, pt, payload, len, marker, ts, unixMillisNow(), rcb);
}

void VideoPipeline::onDepacked(DepackedFrame&& df) {
    m_framesDepacked.fetch_add(1);
    if (df.dropped) {
        m_lostFrames.fetch_add(1);
        m_paramPrefix.clear();
        requestRecoveryKeyFrame("RTP/FU-A frame dropped");
        return;
    }

    if (!hasVclNalu(df.annexb.data(), df.annexb.size())) {
        // Standalone SPS/PPS/SEI access unit: prepend to the next VCL frame rather than feeding alone
        m_paramPrefix = std::move(df.annexb);
        return;
    }

    std::vector<uint8_t> frameData = std::move(df.annexb);
    if (!m_paramPrefix.empty()) {
        std::vector<uint8_t> merged;
        merged.reserve(m_paramPrefix.size() + frameData.size());
        merged.insert(merged.end(), m_paramPrefix.begin(), m_paramPrefix.end());
        merged.insert(merged.end(), frameData.begin(), frameData.end());
        m_paramPrefix.clear();
        frameData = std::move(merged);
    }

    const int64_t nowMs = unixMillisNow();
    bool needKeyNow = false;
    {
        std::lock_guard lock(m_healMu);
        needKeyNow = m_heal.isNeedKey();
    }

    if (needKeyNow && !df.keyframe) {
        m_skippedFrames.fetch_add(1);
        SelfHealPolicy::WatchdogAction act = SelfHealPolicy::WatchdogAction::None;
        {
            std::lock_guard lock(m_healMu);
            act = m_heal.onWatchdogTick(nowMs, nullptr);
        }
        if (act == SelfHealPolicy::WatchdogAction::RequestKeyFrame) {
            triggerKeyFrameRequest();
        }
        return;
    }

    bool flushBeforeFeed = false;
    {
        std::lock_guard lock(m_healMu);
        flushBeforeFeed = m_heal.onQueuedVclFrame(df.keyframe, nowMs);
    }

    bool droppedQueueFull = false;
    {
        std::lock_guard qLock(m_queueMu);
        if (flushBeforeFeed) {
            // Recovery IDR: discard stale pre-recovery queued frames so DPB is never poisoned after Flush
            m_frameQueue.clear();
        }
        if (m_frameQueue.size() >= kMaxEncodedQueue) {
            droppedQueueFull = true;
        } else {
            m_frameQueue.push_back(FrameItem{
                std::move(frameData),
                df.keyframe,
                df.pts,
                flushBeforeFeed,
            });
        }
    }

    if (droppedQueueFull) {
        m_queueFullDrops.fetch_add(1);
        requestRecoveryKeyFrame("decode queue full");
        return;
    }

    if (flushBeforeFeed) {
        logInfo("[Video] recovery IDR queued (" + std::to_string(m_framesDepacked.load()) + " frames)");
    }
    m_queueCv.notify_one();
}

void VideoPipeline::watchdogLoop() {
    while (m_running.load()) {
        std::string reason;
        SelfHealPolicy::WatchdogAction act = SelfHealPolicy::WatchdogAction::None;
        {
            std::lock_guard lock(m_healMu);
            act = m_heal.onWatchdogTick(unixMillisNow(), &reason);
        }
        if (act == SelfHealPolicy::WatchdogAction::RequestKeyFrame) {
            if (!reason.empty()) {
                logInfo("[Video] watchdog " + reason);
            }
            triggerKeyFrameRequest();
        }

        std::unique_lock lock(m_wdMu);
        m_wdCv.wait_for(lock, std::chrono::milliseconds(1000),
                        [this]() { return !m_running.load(); });
    }
}

PipelineStatsSnapshot VideoPipeline::stats() const {
    PipelineStatsSnapshot snap;
    {
        std::lock_guard lock(m_rtpMu);
        snap.rtpPackets = m_reorder.packetsReceived();
        snap.rtpOutOfOrder = m_reorder.outOfOrderCount();
        snap.rtpGaps = m_reorder.gapCount();
    }
    {
        std::lock_guard lock(m_healMu);
        snap.needKey = m_heal.isNeedKey();
    }
    snap.framesDepacked = m_framesDepacked.load();
    snap.lostFrames = m_lostFrames.load();
    snap.skippedFrames = m_skippedFrames.load();
    snap.fedFrames = m_fedFrames.load();
    snap.decodedFrames = m_decodedFrames.load();
    snap.noOutputFrames = m_noOutputFrames.load();
    snap.decodeErrors = m_decodeErrors.load();
    snap.decoderResets = m_decoderResets.load();
    snap.keyRequests = m_keyRequests.load();
    snap.queueFullDrops = m_queueFullDrops.load();
    snap.width = m_width.load();
    snap.height = m_height.load();
    return snap;
}

void VideoPipeline::decodeLoop() {
#if !defined(__SWITCH__) || defined(LEVELUP_HOST_TEST) || !defined(BOREALIS_USE_DEKO3D)
    // Host test path: drain queue without FFmpeg NVDEC
    while (m_running.load()) {
        FrameItem item;
        {
            std::unique_lock lock(m_queueMu);
            m_queueCv.wait(lock, [this]() { return !m_running.load() || !m_frameQueue.empty(); });
            if (!m_running.load()) break;
            item = std::move(m_frameQueue.front());
            m_frameQueue.pop_front();
        }
        m_fedFrames.fetch_add(1);
        m_decodedFrames.fetch_add(1);
        if (!m_firstFrameFired.exchange(true)) {
            FirstFrameCallback cb;
            {
                std::lock_guard lock(m_cbMu);
                cb = m_firstFrameCb;
            }
            if (cb) cb();
        }
    }
#else
    AVBufferRef* hwDeviceCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    AVPacket* pkt = av_packet_alloc();
    std::vector<uint8_t> paddedBuf;
    std::vector<uint8_t> curSps;
    int64_t ptsCounter = 0;

    auto closeCodec = [&]() {
        if (codecCtx) {
            if (m_running.load()) {
                BeforeCodecCloseCallback cb;
                {
                    std::lock_guard lock(m_cbMu);
                    cb = m_beforeCodecCloseCb;
                }
                if (cb) {
                    cb();
                }
            } else {
                // Stopping path: LevelUpStreamView already invalidated/destroyed DKVideoRenderer
                // and stopped draw(), so cleaning AVFrameHolder here before freeing NVDEC is safe.
                AVFrameHolder::instance().cleanup();
            }
            avcodec_free_context(&codecCtx);
            codecCtx = nullptr;
        }
    };
    auto closeHw = [&]() {
        closeCodec();
        if (hwDeviceCtx) {
            av_buffer_unref(&hwDeviceCtx);
            hwDeviceCtx = nullptr;
        }
    };

    auto openCodec = [&](int w, int h) -> bool {
        closeCodec();
        if (!hwDeviceCtx) {
            const int err = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_NVTEGRA, nullptr, nullptr, 0);
            if (err < 0) {
                char ebuf[128]{};
                av_strerror(err, ebuf, sizeof(ebuf));
                logError(std::string("[NVDEC] av_hwdevice_ctx_create failed: ") + ebuf);
                return false;
            }
        }
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            logError("[NVDEC] AV_CODEC_ID_H264 decoder not found");
            return false;
        }
        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            return false;
        }
        codecCtx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT | AV_CODEC_FLAG_LOW_DELAY;
        codecCtx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL | AV_CODEC_FLAG2_FAST;
        codecCtx->thread_type = FF_THREAD_FRAME;
        codecCtx->thread_count = 1;
        codecCtx->width = w;
        codecCtx->height = h;
        codecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
        codecCtx->pix_fmt = AV_PIX_FMT_NVTEGRA;

        const int openErr = avcodec_open2(codecCtx, codec, nullptr);
        if (openErr < 0) {
            char ebuf[128]{};
            av_strerror(openErr, ebuf, sizeof(ebuf));
            logError(std::string("[NVDEC] avcodec_open2 failed: ") + ebuf);
            closeCodec();
            return false;
        }
        logInfo("[NVDEC] H.264 hardware decoder opened " + std::to_string(w) + "x" + std::to_string(h));
        return true;
    };

    while (m_running.load()) {
        FrameItem item;
        {
            std::unique_lock lock(m_queueMu);
            m_queueCv.wait(lock, [this]() { return !m_running.load() || !m_frameQueue.empty(); });
            if (!m_running.load()) break;
            item = std::move(m_frameQueue.front());
            m_frameQueue.pop_front();
        }

        bool needKeyNow = false;
        {
            std::lock_guard lock(m_healMu);
            needKeyNow = m_heal.isNeedKey();
        }
        if (needKeyNow && !item.isKey) {
            m_skippedFrames.fetch_add(1);
            continue;
        }

        std::vector<uint8_t> sps;
        const bool hasSps = findFirstSpsNalu(item.annexb.data(), item.annexb.size(), sps);
        if (codecCtx && hasSps && !curSps.empty() && sps != curSps) {
            logInfo("[NVDEC] SPS changed; reopening decoder");
            closeCodec();
        }

        if (!codecCtx) {
            int initW = m_width.load() > 0 ? m_width.load() : 1280;
            int initH = m_height.load() > 0 ? m_height.load() : 720;
            if (hasSps) {
                int pw = 0, ph = 0;
                if (parseH264SpsDimensions(sps.data(), sps.size(), pw, ph)) {
                    initW = pw;
                    initH = ph;
                }
                curSps = sps;
            }
            if (!openCodec(initW, initH)) {
                requestRecoveryKeyFrame("NVDEC init failed");
                continue;
            }
            m_width.store(initW);
            m_height.store(initH);
        } else if (item.flushBeforeFeed) {
            logInfo("[NVDEC] flushing buffers before recovery IDR");
            avcodec_flush_buffers(codecCtx);
            logInfo("[NVDEC] flush buffers done");
            m_decoderResets.fetch_add(1);
        }

        const size_t dataLen = item.annexb.size();
        paddedBuf.resize(dataLen + AV_INPUT_BUFFER_PADDING_SIZE);
        std::memcpy(paddedBuf.data(), item.annexb.data(), dataLen);
        std::memset(paddedBuf.data() + dataLen, 0, AV_INPUT_BUFFER_PADDING_SIZE);

        pkt->data = paddedBuf.data();
        pkt->size = static_cast<int>(dataLen);
        pkt->pts = ++ptsCounter;

        m_fedFrames.fetch_add(1);

        auto drain = [&]() -> int {
            int outCount = 0;
            while (true) {
                AVFrame* frame = AVFrameHolder::instance().acquireWriteFrame();
                if (!frame) {
                    return outCount;
                }
                const int r = avcodec_receive_frame(codecCtx, frame);
                if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                    AVFrameHolder::instance().recycleWriteFrame(frame);
                    return outCount;
                }
                if (r < 0) {
                    AVFrameHolder::instance().recycleWriteFrame(frame);
                    return r;
                }
                if (frame->width > 0 && frame->height > 0) {
                    m_width.store(frame->width);
                    m_height.store(frame->height);
                }
                AVFrameHolder::instance().pushTransferred(frame);
                outCount++;
            }
        };

        int sendErr = avcodec_send_packet(codecCtx, pkt);
        int drained = 0;
        if (sendErr == AVERROR(EAGAIN)) {
            drained = drain();
            if (drained >= 0) {
                sendErr = avcodec_send_packet(codecCtx, pkt);
            }
        }
        int drained2 = 0;
        bool decodeErr = false;
        if (sendErr < 0 || drained < 0) {
            decodeErr = true;
        } else {
            drained2 = drain();
            if (drained2 < 0) {
                decodeErr = true;
            }
        }

        const int totalOut = std::max(0, drained) + std::max(0, drained2);
        if (decodeErr) {
            m_decodeErrors.fetch_add(1);
        } else if (totalOut == 0) {
            m_noOutputFrames.fetch_add(1);
        } else {
            m_decodedFrames.fetch_add(static_cast<uint64_t>(totalOut));
            if (!m_firstFrameFired.exchange(true)) {
                logInfo("[NVDEC] first decoded frame delivered!");
                FirstFrameCallback cb;
                {
                    std::lock_guard lock(m_cbMu);
                    cb = m_firstFrameCb;
                }
                if (cb) cb();
            }
        }

        SelfHealPolicy::WatchdogAction act = SelfHealPolicy::WatchdogAction::None;
        {
            std::lock_guard lock(m_healMu);
            act = m_heal.onDecodeResult(totalOut > 0, decodeErr, unixMillisNow());
        }
        if (act == SelfHealPolicy::WatchdogAction::FlushAndRequestKey) {
            logWarn("[NVDEC] flushing buffers & requesting IDR after no-output/error");
            if (codecCtx) avcodec_flush_buffers(codecCtx);
            m_decoderResets.fetch_add(1);
            {
                std::lock_guard rtpLock(m_rtpMu);
                m_depack.markLoss();
            }
            triggerKeyFrameRequest();
        } else if (act == SelfHealPolicy::WatchdogAction::RebuildAndRequestKey) {
            logError("[NVDEC] rebuilding hardware decoder & requesting IDR after repeated failure");
            closeCodec();
            m_decoderResets.fetch_add(1);
            {
                std::lock_guard rtpLock(m_rtpMu);
                m_depack.markLoss();
            }
            triggerKeyFrameRequest();
        }
    }

    av_packet_free(&pkt);
    closeHw();
#endif
}

} // namespace levelup::video
