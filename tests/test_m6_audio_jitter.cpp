// Host-side unit test for the audio jitter buffer used by the Switch player.
// Pure logic, no Switch/Opus dependency: compiles anywhere with a C++20 compiler.
#include "levelup/audio_jitter.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using levelup::audio::JitterBuffer;

namespace {

std::vector<uint8_t> payload(uint8_t tag) {
    return {tag, static_cast<uint8_t>(tag + 1)};
}

void pushTag(JitterBuffer& jb, uint16_t seq, uint8_t tag, int64_t atMs) {
    const auto p = payload(tag);
    jb.push(seq, p.data(), p.size(), atMs);
}

} // namespace

int main() {
    // 1. In-order playback
    {
        JitterBuffer jb;
        for (uint16_t i = 0; i < 5; ++i) {
            pushTag(jb, static_cast<uint16_t>(100 + i), static_cast<uint8_t>(i), 0);
        }
        assert(jb.size() == 5);
        std::vector<uint8_t> out;
        for (uint16_t i = 0; i < 5; ++i) {
            assert(jb.take(0, out) == JitterBuffer::Take::Packet);
            assert(out.size() == 2 && out[0] == static_cast<uint8_t>(i));
        }
        assert(jb.take(0, out) == JitterBuffer::Take::None);
        assert(jb.dropped() == 0 && jb.concealed() == 0 && jb.trimmed() == 0);
    }

    // 2. Reordering: 100, 102, 101 must play as 100, 101, 102
    {
        JitterBuffer jb;
        jb.setGapWaitMs(18);
        pushTag(jb, 100, 1, 0);
        pushTag(jb, 102, 3, 0);
        std::vector<uint8_t> out;
        assert(jb.take(0, out) == JitterBuffer::Take::Packet && out[0] == 1);
        // 101 has not arrived yet -> wait, do not conceal
        assert(jb.take(5, out) == JitterBuffer::Take::None);
        pushTag(jb, 101, 2, 6);
        assert(jb.take(6, out) == JitterBuffer::Take::Packet && out[0] == 2);
        assert(jb.take(6, out) == JitterBuffer::Take::Packet && out[0] == 3);
    }

    // 3. Real loss -> PLC after the reorder window, then continue
    {
        JitterBuffer jb;
        jb.setGapWaitMs(18);
        pushTag(jb, 200, 1, 0);
        pushTag(jb, 202, 3, 0);
        std::vector<uint8_t> out;
        assert(jb.take(0, out) == JitterBuffer::Take::Packet && out[0] == 1);
        assert(jb.take(10, out) == JitterBuffer::Take::None); // still inside the window
        assert(jb.take(19, out) == JitterBuffer::Take::Conceal);
        assert(jb.concealed() == 1);
        assert(jb.take(19, out) == JitterBuffer::Take::Packet && out[0] == 3);
    }

    // 4. Duplicate packets are rejected
    {
        JitterBuffer jb;
        pushTag(jb, 300, 1, 0);
        assert(jb.push(300, payload(1).data(), 2, 1) == false);
        assert(jb.dropped() == 1);
        assert(jb.size() == 1);
    }

    // 5. Late packets: inside the window they are dropped on take(); far behind rejected early
    {
        JitterBuffer jb;
        pushTag(jb, 300, 1, 0);
        std::vector<uint8_t> out;
        assert(jb.take(0, out) == JitterBuffer::Take::Packet);
        pushTag(jb, 299, 9, 1); // late by 2 (nextSeq=301) -> accepted, later discarded
        assert(jb.take(1, out) == JitterBuffer::Take::None);
        assert(jb.dropped() == 1);
        // way behind -> rejected at push time
        const auto p = payload(9);
        assert(jb.push(200, p.data(), p.size(), 2) == false);
        assert(jb.dropped() == 2);
    }

    // 6. 16-bit sequence wrap-around keeps strict ordering
    {
        JitterBuffer jb;
        const uint16_t seqs[4] = {65534, 65535, 0, 1};
        for (int i = 0; i < 4; ++i) {
            pushTag(jb, seqs[i], static_cast<uint8_t>(i + 1), 0);
        }
        std::vector<uint8_t> out;
        for (int i = 0; i < 4; ++i) {
            assert(jb.take(0, out) == JitterBuffer::Take::Packet);
            assert(out[0] == static_cast<uint8_t>(i + 1));
        }
    }

    // 7. Huge gap (encoder restart) resyncs instead of synthesising seconds of PLC
    {
        JitterBuffer jb;
        pushTag(jb, 500, 1, 0);
        pushTag(jb, 600, 2, 0);
        std::vector<uint8_t> out;
        assert(jb.take(0, out) == JitterBuffer::Take::Packet && out[0] == 1);
        assert(jb.take(0, out) == JitterBuffer::Take::None); // resync, no PLC storm
        assert(jb.concealed() == 0);
        assert(jb.take(0, out) == JitterBuffer::Take::Packet && out[0] == 2);
    }

    // 8. Backlog trimming keeps latency bounded
    {
        JitterBuffer jb;
        jb.setWatermarks(14, 8);
        for (uint16_t i = 0; i < 20; ++i) {
            pushTag(jb, static_cast<uint16_t>(10 + i), static_cast<uint8_t>(i), 0);
        }
        // 15th push crosses the high watermark -> trimmed down to 8, then 5 more pushes
        assert(jb.size() == 13);
        assert(jb.trimmed() == 7);
        // the trimmed head is still playable in order (oldest 7 skipped, not the newest)
        std::vector<uint8_t> out;
        assert(jb.take(0, out) == JitterBuffer::Take::Packet && out[0] == 7);
    }

    std::cout << "test_m6_audio_jitter: ALL PASSED\n";
    return 0;
}
