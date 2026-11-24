#include "levelup/h264_depacketizer.hpp"

#include <algorithm>
#include <cstring>

namespace levelup::video {
namespace {

inline uint16_t readU16BE(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8U) | static_cast<uint16_t>(p[1]);
}

inline uint32_t readU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24U) |
           (static_cast<uint32_t>(p[1]) << 16U) |
           (static_cast<uint32_t>(p[2]) << 8U) |
           static_cast<uint32_t>(p[3]);
}

struct BitReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t bitPos = 0;

    bool readBit(uint32_t& out) {
        const size_t byteIdx = bitPos >> 3U;
        if (byteIdx >= size) return false;
        const size_t bitIdx = 7U - (bitPos & 7U);
        out = (data[byteIdx] >> bitIdx) & 1U;
        ++bitPos;
        return true;
    }

    bool readBits(size_t n, uint32_t& out) {
        out = 0;
        for (size_t i = 0; i < n; ++i) {
            uint32_t b = 0;
            if (!readBit(b)) return false;
            out = (out << 1U) | b;
        }
        return true;
    }

    bool skipBits(size_t n) {
        if (bitPos + n > size * 8U) return false;
        bitPos += n;
        return true;
    }

    bool readUE(uint32_t& out) {
        size_t zeros = 0;
        uint32_t b = 0;
        while (true) {
            if (!readBit(b)) return false;
            if (b != 0) break;
            ++zeros;
            if (zeros > 31) return false;
        }
        uint32_t suffix = 0;
        if (zeros > 0 && !readBits(zeros, suffix)) return false;
        out = ((1U << zeros) - 1U) + suffix;
        return true;
    }

    bool readSE(int32_t& out) {
        uint32_t ue = 0;
        if (!readUE(ue)) return false;
        if (ue & 1U) {
            out = static_cast<int32_t>((ue + 1U) >> 1U);
        } else {
            out = -static_cast<int32_t>(ue >> 1U);
        }
        return true;
    }
};

std::vector<uint8_t> removeEmulationPrevention(const uint8_t* sps, size_t len) {
    std::vector<uint8_t> rbsp;
    rbsp.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        if (i + 2 < len && sps[i] == 0x00 && sps[i + 1] == 0x00 && sps[i + 2] == 0x03) {
            rbsp.push_back(0x00);
            rbsp.push_back(0x00);
            i += 2;
        } else {
            rbsp.push_back(sps[i]);
        }
    }
    return rbsp;
}

} // namespace

bool parseRtpPacket(const uint8_t* data, size_t len, ParsedRtpPacket& out) {
    if (!data || len < 12) {
        return false;
    }
    const uint8_t v = (data[0] >> 6U) & 0x03U;
    if (v != 2U) {
        return false;
    }
    const bool hasPadding = (data[0] & 0x20U) != 0;
    const bool hasExt = (data[0] & 0x10U) != 0;
    const uint8_t csrcCount = data[0] & 0x0FU;

    out.marker = (data[1] & 0x80U) != 0;
    out.payloadType = data[1] & 0x7FU;
    out.seq = readU16BE(data + 2);
    out.timestamp = readU32BE(data + 4);
    out.ssrc = readU32BE(data + 8);

    size_t offset = 12 + static_cast<size_t>(csrcCount) * 4U;
    if (offset > len) {
        return false;
    }

    if (hasExt) {
        if (offset + 4 > len) {
            return false;
        }
        const uint16_t extWords = readU16BE(data + offset + 2);
        offset += 4U + static_cast<size_t>(extWords) * 4U;
        if (offset > len) {
            return false;
        }
    }

    size_t end = len;
    if (hasPadding) {
        if (end <= offset) {
            return false;
        }
        const uint8_t padLen = data[end - 1];
        if (padLen == 0 || static_cast<size_t>(padLen) > end - offset) {
            return false;
        }
        end -= static_cast<size_t>(padLen);
    }

    out.payload = data + offset;
    out.payloadLen = end - offset;
    return true;
}

void forEachAnnexBNalu(const uint8_t* data,
                       size_t len,
                       const std::function<bool(uint8_t, const uint8_t*, size_t)>& cb) {
    if (!data || len < 4 || !cb) return;
    size_t i = 0;
    while (i + 4 <= len) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            const size_t start = i + 4;
            if (start >= len) return;
            size_t end = len;
            for (size_t j = start + 1; j + 3 < len; ++j) {
                if (data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 0 && data[j + 3] == 1) {
                    end = j;
                    break;
                }
            }
            if (!cb(data[start] & 0x1FU, data + start, end - start)) {
                return;
            }
            i = end;
            continue;
        }
        ++i;
    }
}

bool hasVclNalu(const uint8_t* data, size_t len) {
    bool found = false;
    forEachAnnexBNalu(data, len, [&](uint8_t t, const uint8_t*, size_t) {
        if (t >= 1 && t <= 5) {
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

bool containsIdrNalu(const uint8_t* data, size_t len) {
    bool found = false;
    forEachAnnexBNalu(data, len, [&](uint8_t t, const uint8_t*, size_t) {
        if (t == 5) {
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

bool containsSpsNalu(const uint8_t* data, size_t len) {
    bool found = false;
    forEachAnnexBNalu(data, len, [&](uint8_t t, const uint8_t*, size_t) {
        if (t == 7) {
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

bool findFirstSpsNalu(const uint8_t* data, size_t len, std::vector<uint8_t>& outSps) {
    bool found = false;
    forEachAnnexBNalu(data, len, [&](uint8_t t, const uint8_t* nalu, size_t naluLen) {
        if (t == 7 && naluLen > 0) {
            outSps.assign(nalu, nalu + naluLen);
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

bool parseH264SpsDimensions(const uint8_t* sps, size_t len, int& widthOut, int& heightOut) {
    if (!sps || len < 4) return false;
    const auto rbsp = removeEmulationPrevention(sps + 1, len - 1);
    BitReader br{rbsp.data(), rbsp.size(), 0};

    uint32_t profileIdc = 0;
    uint32_t constraintFlags = 0;
    uint32_t levelIdc = 0;
    uint32_t spsId = 0;
    if (!br.readBits(8, profileIdc) || !br.readBits(8, constraintFlags) ||
        !br.readBits(8, levelIdc) || !br.readUE(spsId)) {
        return false;
    }

    uint32_t chromaFormatIdc = 1;
    if (profileIdc == 100 || profileIdc == 110 || profileIdc == 122 ||
        profileIdc == 244 || profileIdc == 44 || profileIdc == 83 ||
        profileIdc == 86 || profileIdc == 118 || profileIdc == 128 ||
        profileIdc == 138 || profileIdc == 139 || profileIdc == 134) {
        if (!br.readUE(chromaFormatIdc)) return false;
        if (chromaFormatIdc == 3) {
            if (!br.skipBits(1)) return false;
        }
        uint32_t dummy = 0;
        if (!br.readUE(dummy) || !br.readUE(dummy) || !br.skipBits(1)) return false;
        uint32_t seqScalingMatrixPresent = 0;
        if (!br.readBit(seqScalingMatrixPresent)) return false;
        if (seqScalingMatrixPresent) {
            const size_t count = (chromaFormatIdc != 3) ? 8 : 12;
            for (size_t i = 0; i < count; ++i) {
                uint32_t present = 0;
                if (!br.readBit(present)) return false;
                if (present) {
                    const size_t listSize = (i < 6) ? 16 : 64;
                    int32_t lastScale = 8;
                    int32_t nextScale = 8;
                    for (size_t j = 0; j < listSize; ++j) {
                        if (nextScale != 0) {
                            int32_t delta = 0;
                            if (!br.readSE(delta)) return false;
                            nextScale = (lastScale + delta + 256) % 256;
                        }
                        lastScale = (nextScale == 0) ? lastScale : nextScale;
                    }
                }
            }
        }
    }

    uint32_t log2MaxFrameNum = 0;
    uint32_t picOrderCntType = 0;
    if (!br.readUE(log2MaxFrameNum) || !br.readUE(picOrderCntType)) return false;
    if (picOrderCntType == 0) {
        uint32_t dummy = 0;
        if (!br.readUE(dummy)) return false;
    } else if (picOrderCntType == 1) {
        uint32_t numRefFramesInCycle = 0;
        if (!br.skipBits(1)) return false;
        int32_t s = 0;
        if (!br.readSE(s) || !br.readSE(s) || !br.readUE(numRefFramesInCycle)) return false;
        for (uint32_t i = 0; i < numRefFramesInCycle; ++i) {
            if (!br.readSE(s)) return false;
        }
    }

    uint32_t maxNumRefFrames = 0;
    uint32_t picWidthInMbsMinus1 = 0;
    uint32_t picHeightInMapUnitsMinus1 = 0;
    uint32_t frameMbsOnlyFlag = 0;
    if (!br.readUE(maxNumRefFrames) || !br.skipBits(1) ||
        !br.readUE(picWidthInMbsMinus1) || !br.readUE(picHeightInMapUnitsMinus1) ||
        !br.readBit(frameMbsOnlyFlag)) {
        return false;
    }
    if (!frameMbsOnlyFlag && !br.skipBits(1)) return false;
    if (!br.skipBits(1)) return false;

    uint32_t frameCroppingFlag = 0;
    if (!br.readBit(frameCroppingFlag)) return false;
    uint32_t cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    if (frameCroppingFlag) {
        if (!br.readUE(cropLeft) || !br.readUE(cropRight) ||
            !br.readUE(cropTop) || !br.readUE(cropBottom)) {
            return false;
        }
    }

    int w = static_cast<int>((picWidthInMbsMinus1 + 1U) * 16U);
    int h = static_cast<int>((2U - frameMbsOnlyFlag) * (picHeightInMapUnitsMinus1 + 1U) * 16U);
    if (frameCroppingFlag) {
        const uint32_t cropUnitX = (chromaFormatIdc == 0) ? 1U : ((chromaFormatIdc == 3) ? 1U : 2U);
        const uint32_t cropUnitY = (chromaFormatIdc == 0 || chromaFormatIdc == 3)
                                       ? (2U - frameMbsOnlyFlag)
                                       : 2U * (2U - frameMbsOnlyFlag);
        w -= static_cast<int>((cropLeft + cropRight) * cropUnitX);
        h -= static_cast<int>((cropTop + cropBottom) * cropUnitY);
    }
    if (w <= 0 || h <= 0) return false;
    widthOut = w;
    heightOut = h;
    return true;
}

// ---------------- H264Depacketizer ----------------

void H264Depacketizer::reset() {
    std::lock_guard lock(m_mu);
    m_frame.clear();
    m_started = false;
    m_fuOpen = false;
    m_frameDropped = false;
    m_lostPending = false;
    m_frameSeqOk = false;
    m_frameKey = false;
    m_frameVcl = false;
    m_hasParam = false;
}

void H264Depacketizer::markLoss() {
    std::lock_guard lock(m_mu);
    m_frameDropped = true;
    m_lostPending = true;
}

void H264Depacketizer::push(const uint8_t* payload,
                            size_t len,
                            bool marker,
                            uint32_t ts,
                            uint16_t seq,
                            const FrameCallback& onFrame) {
    std::lock_guard lock(m_mu);

    if (!m_started) {
        m_started = true;
        m_curTs = ts;
    }

    if (ts != m_curTs) {
        if (m_fuOpen) {
            m_fuOpen = false;
            m_dropped++;
            m_lostPending = true;
            const uint32_t prevTs = m_curTs;
            resetFrameLocked();
            if (onFrame) {
                DepackedFrame df;
                df.pts = prevTs;
                df.keyframe = false;
                df.dropped = true;
                onFrame(std::move(df));
            }
        } else {
            flushLocked(onFrame);
        }
        m_curTs = ts;
        m_frameSeqOk = false;
    }

    if (m_frameSeqOk && seq != static_cast<uint16_t>(m_frameSeq + 1U)) {
        m_frameDropped = true;
        m_lostPending = true;
    }
    m_frameSeq = seq;
    m_frameSeqOk = true;

    if (!payload || len == 0) {
        return;
    }

    const uint8_t naluType = payload[0] & 0x1FU;
    if (naluType == 24) { // STAP-A
        size_t off = 1;
        while (off + 2 <= len) {
            const uint16_t sz = readU16BE(payload + off);
            off += 2;
            if (sz == 0 || off + sz > len) {
                break;
            }
            appendNaluLocked(payload + off, sz);
            off += sz;
        }
    } else if (naluType == 28) { // FU-A
        if (len < 2) {
            return;
        }
        const uint8_t head = payload[1];
        const bool startBit = (head & 0x80U) != 0;
        const bool endBit = (head & 0x40U) != 0;
        const uint8_t origType = head & 0x1FU;

        if (startBit) {
            if (m_fuOpen) {
                m_frameDropped = true;
                m_lostPending = true;
            }
            m_fuOpen = true;
            const uint8_t hdr = (payload[0] & 0xE0U) | origType;
            startNaluLocked(hdr);
            if (len > 2) {
                m_frame.insert(m_frame.end(), payload + 2, payload + len);
            }
            if (origType == 5) {
                m_frameKey = true;
                m_keyFrames++;
            }
            if (origType >= 1 && origType <= 5) {
                m_frameVcl = true;
            }
        } else if (m_fuOpen) {
            if (len > 2) {
                m_frame.insert(m_frame.end(), payload + 2, payload + len);
            }
        } else {
            m_frameDropped = true;
            m_lostPending = true;
        }

        if (endBit) {
            m_fuOpen = false;
            m_naluCount++;
        }
    } else {
        appendNaluLocked(payload, len);
    }

    // YTCX sets marker=1 on every RTP packet: only treat marker as AU end when !m_fuOpen
    if (marker) {
        if (m_fuOpen) {
            m_fuOpen = false;
            m_dropped++;
            m_lostPending = true;
            resetFrameLocked();
            if (onFrame) {
                DepackedFrame df;
                df.pts = m_curTs;
                df.keyframe = false;
                df.dropped = true;
                onFrame(std::move(df));
            }
        } else {
            flushLocked(onFrame);
        }
    }
}

void H264Depacketizer::startNaluLocked(uint8_t header) {
    m_frame.push_back(0x00);
    m_frame.push_back(0x00);
    m_frame.push_back(0x00);
    m_frame.push_back(0x01);
    m_frame.push_back(header);
}

void H264Depacketizer::appendNaluLocked(const uint8_t* nalu, size_t len) {
    if (!nalu || len == 0) return;
    const uint8_t t = nalu[0] & 0x1FU;
    if (t == 7) {
        m_sps.assign(nalu, nalu + len);
        m_hasParam = true;
    } else if (t == 8) {
        m_pps.assign(nalu, nalu + len);
        m_hasParam = true;
    } else if (t == 5) {
        m_keyFrames++;
        m_frameKey = true;
    }
    if (t >= 1 && t <= 5) {
        m_frameVcl = true;
    }
    m_naluCount++;
    startNaluLocked(nalu[0]);
    if (len > 1) {
        m_frame.insert(m_frame.end(), nalu + 1, nalu + len);
    }
}

void H264Depacketizer::resetFrameLocked() {
    m_frame.clear();
    m_frameKey = false;
    m_frameVcl = false;
    m_hasParam = false;
}

std::vector<uint8_t> H264Depacketizer::prependParamsLocked(const std::vector<uint8_t>& frame) const {
    std::vector<uint8_t> out;
    out.reserve(m_sps.size() + m_pps.size() + frame.size() + 8);
    if (!m_sps.empty()) {
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);
        out.push_back(1);
        out.insert(out.end(), m_sps.begin(), m_sps.end());
    }
    if (!m_pps.empty()) {
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);
        out.push_back(1);
        out.insert(out.end(), m_pps.begin(), m_pps.end());
    }
    out.insert(out.end(), frame.begin(), frame.end());
    return out;
}

void H264Depacketizer::flushLocked(const FrameCallback& onFrame) {
    if (!m_frame.empty() && onFrame) {
        std::vector<uint8_t> outFrame = std::move(m_frame);
        const bool isParamOnly = !m_frameVcl && m_hasParam;
        const bool dropped = m_frameDropped || (m_lostPending && !m_frameKey && !isParamOnly);
        if (m_frameKey && !m_frameDropped) {
            m_lostPending = false;
            if (!containsSpsNalu(outFrame.data(), outFrame.size()) &&
                (!m_sps.empty() || !m_pps.empty())) {
                outFrame = prependParamsLocked(outFrame);
            }
        } else if (dropped) {
            m_lostPending = true;
        }
        m_frameCount++;
        DepackedFrame df;
        df.annexb = std::move(outFrame);
        df.pts = m_curTs;
        df.keyframe = m_frameKey;
        df.dropped = dropped;
        onFrame(std::move(df));
    }
    m_frame.clear();
    m_frameDropped = false;
    m_frameKey = false;
    m_frameVcl = false;
    m_hasParam = false;
}

std::vector<uint8_t> H264Depacketizer::sps() const {
    std::lock_guard lock(m_mu);
    return m_sps;
}

std::vector<uint8_t> H264Depacketizer::pps() const {
    std::lock_guard lock(m_mu);
    return m_pps;
}

int H264Depacketizer::naluCount() const {
    std::lock_guard lock(m_mu);
    return m_naluCount;
}

int H264Depacketizer::frameCount() const {
    std::lock_guard lock(m_mu);
    return m_frameCount;
}

int H264Depacketizer::keyFrameCount() const {
    std::lock_guard lock(m_mu);
    return m_keyFrames;
}

int H264Depacketizer::droppedCount() const {
    std::lock_guard lock(m_mu);
    return m_dropped;
}

// ---------------- RtpReorderBuffer ----------------

RtpReorderBuffer::RtpReorderBuffer(size_t limit, int64_t waitMs)
    : m_limit(limit == 0 ? kDefaultLimit : limit),
      m_waitMs(waitMs <= 0 ? kDefaultWaitMs : waitMs) {}

void RtpReorderBuffer::reset() {
    m_started = false;
    m_expect = 0;
    m_lastSeq = 0;
    m_lastNackMs = 0;
    m_pending.clear();
    m_packetsReceived = 0;
    m_outOfOrder = 0;
    m_gaps = 0;
}

void RtpReorderBuffer::push(uint16_t seq,
                            uint8_t pt,
                            const uint8_t* payload,
                            size_t len,
                            bool marker,
                            uint32_t ts,
                            int64_t nowMs,
                            const ReorderCallbacks& cb) {
    m_packetsReceived++;
    if (!m_started) {
        m_started = true;
        m_expect = seq;
        m_lastSeq = static_cast<uint16_t>(seq - 1U);
    }

    const int16_t seqDeltaFromLast = static_cast<int16_t>(seq - m_lastSeq);
    if (seqDeltaFromLast <= 0) {
        m_outOfOrder++;
    }
    m_lastSeq = seq;

    const int16_t diff = static_cast<int16_t>(seq - m_expect);
    if (diff < 0) {
        // Stale / already-skipped packet: drop immediately to prevent 65535 wraparound pollution.
        m_outOfOrder++;
        return;
    }

    if (diff > 2000) {
        // Huge jump: reset sequence baseline and request recovery IDR.
        m_pending.clear();
        m_expect = seq;
        m_lastSeq = static_cast<uint16_t>(seq - 1U);
        m_gaps++;
        if (cb.onGap) {
            cb.onGap("RTP seq jump (" + std::to_string(diff) + ")", static_cast<uint16_t>(diff));
        }
    }

    if (m_pending.find(seq) != m_pending.end()) {
        return;
    }

    Item item;
    if (payload && len > 0) {
        item.payload.assign(payload, payload + len);
    }
    item.marker = marker;
    item.ts = ts;
    item.seq = seq;
    item.pt = pt;
    item.arrivedMs = nowMs;
    m_pending.emplace(seq, std::move(item));

    // Deliver all contiguous ready packets from m_expect.
    while (true) {
        auto it = m_pending.find(m_expect);
        if (it == m_pending.end()) break;
        Item ready = std::move(it->second);
        m_pending.erase(it);
        m_expect = static_cast<uint16_t>(m_expect + 1U);
        if (cb.deliver) {
            cb.deliver(ready.payload.data(), ready.payload.size(), ready.marker, ready.ts, ready.seq, ready.pt);
        }
    }

    if (m_pending.empty()) {
        return;
    }

    // Trigger RTCP NACK for currently missing sequence numbers between m_expect and highest pending seq
    if (cb.onNackNeeded && (m_lastNackMs == 0 || nowMs - m_lastNackMs >= 5)) {
        std::vector<uint16_t> missing;
        uint16_t maxDist = 0;
        for (const auto& [pseq, _] : m_pending) {
            const uint16_t d = static_cast<uint16_t>(pseq - m_expect);
            if (d < 32768U && d > maxDist) {
                maxDist = d;
            }
        }
        const uint16_t scanMax = std::min<uint16_t>(maxDist, 64);
        for (uint16_t d = 0; d < scanMax; ++d) {
            const uint16_t candidate = static_cast<uint16_t>(m_expect + d);
            if (m_pending.find(candidate) == m_pending.end()) {
                missing.push_back(candidate);
            }
        }
        if (!missing.empty()) {
            m_lastNackMs = nowMs;
            cb.onNackNeeded(missing);
        }
    }

    // Check overflow (> m_limit) or earliest hole wait > m_waitMs.
    bool flushGap = m_pending.size() > m_limit;
    if (!flushGap) {
        int64_t oldestMs = -1;
        for (const auto& [_, it] : m_pending) {
            if (oldestMs < 0 || it.arrivedMs < oldestMs) {
                oldestMs = it.arrivedMs;
            }
        }
        if (oldestMs >= 0 && (nowMs - oldestMs) >= m_waitMs) {
            flushGap = true;
        }
    }

    if (!flushGap) {
        return;
    }

    uint16_t minDiff = 0;
    bool found = false;
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        const uint16_t d = static_cast<uint16_t>(it->first - m_expect);
        if (d >= 32768U) {
            it = m_pending.erase(it);
            continue;
        }
        if (d > 0 && (!found || d < minDiff)) {
            minDiff = d;
            found = true;
        }
        ++it;
    }

    if (!found) {
        return;
    }

    m_gaps++;
    m_expect = static_cast<uint16_t>(m_expect + minDiff);
    if (cb.onGap) {
        cb.onGap("RTP seq gap (" + std::to_string(minDiff) + " pkts)", minDiff);
    }

    while (true) {
        auto it = m_pending.find(m_expect);
        if (it == m_pending.end()) break;
        Item ready = std::move(it->second);
        m_pending.erase(it);
        m_expect = static_cast<uint16_t>(m_expect + 1U);
        if (cb.deliver) {
            cb.deliver(ready.payload.data(), ready.payload.size(), ready.marker, ready.ts, ready.seq, ready.pt);
        }
    }
}

// ---------------- SelfHealPolicy ----------------

void SelfHealPolicy::reset() {
    m_needKey = false;
    m_lastKeyReqMs = 0;
    m_firstVclMs = 0;
    m_lastVclMs = 0;
    m_lastIdrMs = 0;
    m_consecNoOut = 0;
    m_consecErrors = 0;
    m_resetsSinceDecode = 0;
}

bool SelfHealPolicy::enterNeedKey(int64_t nowMs) {
    const bool entered = !m_needKey;
    m_needKey = true;
    m_lastKeyReqMs = nowMs;
    return entered;
}

bool SelfHealPolicy::onQueuedVclFrame(bool isIdr, int64_t nowMs) {
    m_lastVclMs = nowMs;
    if (m_firstVclMs == 0) {
        m_firstVclMs = nowMs;
    }
    bool needFlushBeforeFeed = false;
    if (isIdr) {
        m_lastIdrMs = nowMs;
        if (m_needKey) {
            m_needKey = false;
            needFlushBeforeFeed = true;
        }
    }
    return needFlushBeforeFeed;
}

bool SelfHealPolicy::tryRequestKeyWithInterval(int64_t nowMs, int64_t minIntervalMs) {
    if (m_lastKeyReqMs > 0 && (nowMs - m_lastKeyReqMs) < minIntervalMs) {
        return false;
    }
    m_lastKeyReqMs = nowMs;
    return true;
}

bool SelfHealPolicy::onTransportLoss(int64_t lossDelta, int64_t nowMs) {
    if (lossDelta <= 0 || m_needKey) {
        return false;
    }
    return tryRequestKeyWithInterval(nowMs, m_cfg.seamlessCooldownMs);
}

SelfHealPolicy::WatchdogAction SelfHealPolicy::onWatchdogTick(int64_t nowMs, std::string* reasonOut) {
    if (m_needKey) {
        if (tryRequestKeyWithInterval(nowMs, m_cfg.needKeyRetryMs)) {
            if (reasonOut) *reasonOut = "needKey retry";
            return WatchdogAction::RequestKeyFrame;
        }
        return WatchdogAction::None;
    }

    if (m_cfg.idrMaxAgeMs <= 0 || m_lastVclMs == 0 ||
        (nowMs - m_lastVclMs) > m_cfg.streamInactiveMs) {
        return WatchdogAction::None;
    }

    const int64_t refMs = (m_lastIdrMs > 0) ? m_lastIdrMs : m_firstVclMs;
    if (refMs == 0 || (nowMs - refMs) < m_cfg.idrMaxAgeMs) {
        return WatchdogAction::None;
    }

    if (tryRequestKeyWithInterval(nowMs, m_cfg.idrRefreshRetryMs)) {
        if (reasonOut) {
            *reasonOut = "infinite-GOP silent IDR refresh (" +
                         std::to_string((nowMs - refMs) / 1000) + "s)";
        }
        return WatchdogAction::RequestKeyFrame;
    }
    return WatchdogAction::None;
}

SelfHealPolicy::WatchdogAction SelfHealPolicy::onDecodeResult(bool gotOutput,
                                                              bool decodeError,
                                                              int64_t nowMs) {
    if (gotOutput) {
        m_consecNoOut = 0;
        m_consecErrors = 0;
        m_resetsSinceDecode = 0;
        return WatchdogAction::None;
    }

    if (decodeError) {
        m_consecNoOut = 0;
        m_consecErrors++;
        m_resetsSinceDecode++;
        enterNeedKey(nowMs);
        if (m_resetsSinceDecode >= 3) {
            m_resetsSinceDecode = 0;
            return WatchdogAction::RebuildAndRequestKey;
        }
        return WatchdogAction::FlushAndRequestKey;
    }

    // Decoded packet cleanly but produced no output frame yet
    if (m_cfg.noOutputThreshold > 0) {
        m_consecNoOut++;
        if (m_consecNoOut >= m_cfg.noOutputThreshold) {
            m_consecNoOut = 0;
            m_resetsSinceDecode++;
            enterNeedKey(nowMs);
            if (m_resetsSinceDecode >= 2) {
                m_resetsSinceDecode = 0;
                return WatchdogAction::RebuildAndRequestKey;
            }
            return WatchdogAction::FlushAndRequestKey;
        }
    }
    return WatchdogAction::None;
}

} // namespace levelup::video
