#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace levelup::audio {

/// RTP Opus 包的抖动缓冲（纯逻辑，主机端可单测）。
///
/// 职责：按 16 位 RTP 序号重排 / 去重 / 丢迟到包、缺包超时后判定丢失（交由
/// Opus PLC 补帧），以及积压超过高水位时回落到低水位以保证端到端延迟。
///
/// 序号按首次收到的包展开成 32 位（`m_nextSeq + (int16_t)(seq - nextSeq)`），
/// 因此 65536 回绕不影响排序。
class JitterBuffer {
  public:
    enum class Take {
        None,    ///< 暂时没有可播的帧（等包 / 等乱序追平）
        Packet,  ///< payload 已填充，可直接解码
        Conceal, ///< 该序号判定丢失，应输出一帧 PLC
    };

    JitterBuffer() = default;

    void reset() {
        m_queue.clear();
        m_seqInit = false;
        m_nextSeq = 0;
        m_dropped = 0;
        m_concealed = 0;
        m_trimmed = 0;
    }

    /// 收到一个包。返回 false 表示被判为迟到/重复而丢弃。
    bool push(uint16_t seq, const uint8_t* data, size_t len, int64_t arrivalMs) {
        if (!m_seqInit) {
            m_seqInit = true;
            m_nextSeq = seq;
        }

        const int16_t delta =
            static_cast<int16_t>(static_cast<uint16_t>(seq - static_cast<uint16_t>(m_nextSeq)));
        if (delta < -16) {
            // 已经播过的迟到包，丢掉（不打断连续播放）
            m_dropped++;
            return false;
        }
        const uint32_t extSeq = m_nextSeq + static_cast<uint32_t>(static_cast<int32_t>(delta));

        for (const auto& p : m_queue) {
            if (p.extSeq == extSeq) {
                m_dropped++; // 重复包
                return false;
            }
        }

        Packet pkt;
        pkt.extSeq = extSeq;
        pkt.arrivalMs = arrivalMs;
        pkt.data.assign(data, data + len);

        auto it = m_queue.begin();
        while (it != m_queue.end() && static_cast<int32_t>(it->extSeq - extSeq) < 0) {
            ++it;
        }
        m_queue.insert(it, std::move(pkt));

        while (m_queue.size() > m_hardCap) {
            // 丢掉最旧的一帧，并把下一个期望序号推进到它之后
            const uint32_t dropped = m_queue.front().extSeq;
            m_queue.pop_front();
            m_nextSeq = dropped + 1;
            m_trimmed++;
        }
        if (m_queue.size() > m_highWater) {
            while (m_queue.size() > m_lowWater) {
                const uint32_t dropped = m_queue.front().extSeq;
                m_queue.pop_front();
                m_nextSeq = dropped + 1;
                m_trimmed++;
            }
        }
        return true;
    }

    /// 取下一帧。
    Take take(int64_t nowMs, std::vector<uint8_t>& payloadOut) {
        payloadOut.clear();
        if (!m_seqInit || m_queue.empty()) {
            return Take::None;
        }

        // 丢掉过期/重复序号
        while (!m_queue.empty() && static_cast<int32_t>(m_queue.front().extSeq - m_nextSeq) < 0) {
            m_queue.pop_front();
            m_dropped++;
        }
        if (m_queue.empty()) {
            return Take::None;
        }

        const int32_t lead = static_cast<int32_t>(m_queue.front().extSeq - m_nextSeq);
        if (lead == 0) {
            payloadOut = std::move(m_queue.front().data);
            m_queue.pop_front();
            m_nextSeq++;
            return Take::Packet;
        }

        if (lead > m_maxGap) {
            // 缺口太大（对端重启 / 长抖动）：直接对齐，不合成几百毫秒假音
            m_nextSeq = m_queue.front().extSeq;
            return Take::None;
        }
        if ((nowMs - m_queue.front().arrivalMs) >= m_gapWaitMs) {
            m_nextSeq++;
            m_concealed++;
            return Take::Conceal;
        }
        return Take::None;
    }

    size_t size() const { return m_queue.size(); }
    bool initialized() const { return m_seqInit; }
    uint32_t nextSeq() const { return m_nextSeq; }
    uint64_t dropped() const { return m_dropped; }
    uint64_t concealed() const { return m_concealed; }
    uint64_t trimmed() const { return m_trimmed; }

    void setGapWaitMs(int64_t ms) { m_gapWaitMs = ms; }
    void setWatermarks(size_t high, size_t low) {
        m_highWater = high;
        m_lowWater = low;
    }
    void setHardCap(size_t cap) { m_hardCap = cap; }
    void setMaxGapPackets(int32_t n) { m_maxGap = n; }

  private:
    struct Packet {
        uint32_t extSeq = 0;
        int64_t arrivalMs = 0;
        std::vector<uint8_t> data;
    };

    std::deque<Packet> m_queue;
    bool m_seqInit = false;
    uint32_t m_nextSeq = 0;

    int64_t m_gapWaitMs = 18;
    size_t m_highWater = 14;
    size_t m_lowWater = 8;
    size_t m_hardCap = 24;
    int32_t m_maxGap = 8;

    uint64_t m_dropped = 0;
    uint64_t m_concealed = 0;
    uint64_t m_trimmed = 0;
};

} // namespace levelup::audio
