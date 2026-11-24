#include "levelup/input_packet.hpp"
#include "levelup/rumble_switch.hpp"
#include "levelup/switch_input_bridge.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

using namespace levelup::input;

namespace {

uint16_t readU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8U);
}

uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8U) |
           (static_cast<uint32_t>(p[2]) << 16U) |
           (static_cast<uint32_t>(p[3]) << 24U);
}

uint64_t readU64LE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return v;
}

} // namespace

int main() {
    // 1. ParseRumble valid cases matching go-client/internal/input/input_test.go byte-for-byte
    {
        const uint8_t p1[] = {0x06, 0x02, 0x0A, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x01, 0x00, 0xCC, 0x00, 0xCC, 0x00};
        RumbleEvent ev;
        assert(parseRumble(p1, sizeof(p1), ev));
        assert(ev.controller == 1 && ev.lowFreq == 204 && ev.highFreq == 204);

        const uint8_t pStop[] = {0x06, 0x02, 0x0A, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
        assert(parseRumble(pStop, sizeof(pStop), ev));
        assert(ev.controller == 1 && ev.lowFreq == 0 && ev.highFreq == 0);

        const uint8_t pAsym[] = {0x06, 0x02, 0x0A, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x02, 0x00, 0xFF, 0x00, 0x80, 0x00};
        assert(parseRumble(pAsym, sizeof(pAsym), ev));
        assert(ev.controller == 2 && ev.lowFreq == 255 && ev.highFreq == 128);
    }

    // 2. ParseRumble reject cases matching Go tests
    {
        RumbleEvent ev;
        const uint8_t cShort[] = {0x06, 0x02, 0x0A, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x01};
        const uint8_t cEcho1537[] = {0x01, 0x06, 0x0C, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0, 0, 0, 0, 0, 0};
        const uint8_t cSub30[] = {0x06, 0x02, 0x0A, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x01, 0x00, 0xCC, 0x00, 0xCC, 0x00};
        const uint8_t cType7[] = {0x07, 0x02, 0x0A, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x01, 0x00, 0xCC, 0x00, 0xCC, 0x00};
        assert(!parseRumble(cShort, sizeof(cShort), ev));
        assert(!parseRumble(cEcho1537, sizeof(cEcho1537), ev));
        assert(!parseRumble(cSub30, sizeof(cSub30), ev));
        assert(!parseRumble(cType7, sizeof(cType7), ev));

        uint64_t echoTs = 0;
        assert(isHeartbeatEcho1537(cEcho1537, sizeof(cEcho1537), &echoTs));
    }

    // 3. ScaleMotorTo16 + ScaleMotorNorm proportional [0..1]
    {
        assert(scaleMotorTo16(0) == 0);
        assert(scaleMotorTo16(1) == 257);
        assert(scaleMotorTo16(128) == 32896);
        assert(scaleMotorTo16(204) == 52428);
        assert(scaleMotorTo16(255) == 65535);
        assert(scaleMotorTo16(40000) == 40000);

        assert(std::fabs(scaleMotorNorm(0, 1.0f) - 0.0f) < 1e-6f);
        assert(std::fabs(scaleMotorNorm(255, 1.0f) - 1.0f) < 1e-5f);
        assert(std::fabs(scaleMotorNorm(255, 0.5f) - 0.5f) < 1e-5f);
        assert(std::fabs(scaleMotorNorm(204, 1.0f) - 0.8f) < 1e-3f);
    }

    // 4. Control DC handshakes & exact 46-byte inner (54-byte wrapped) gamepad layout
    {
        const auto hs = buildHandshakeSession("csess-xyz");
        assert(readU16LE(hs.data()) == 1792);
        assert(readU16LE(hs.data() + 2) == 9);
        assert(std::memcmp(hs.data() + 4, "csess-xyz", 9) == 0);

        const auto vq = buildBitrateModePacket(true);
        assert(vq.size() == 13);
        assert(readU16LE(vq.data()) == 1025);
        assert(vq[11] == 0x49 && vq[12] == 0x01);

        const GamepadState st = mapSwitchHidToGamepad(
            nxhid::kA | nxhid::kR | nxhid::kZR | nxhid::kPlus,
            1234, 5678, -4321, -8765, false, false);
        // Nintendo ABXY direct passthrough (A stays 0x1000, NO Xbox swap)
        assert((st.buttons & kPadA) != 0);
        assert((st.buttons & kPadB) == 0);
        assert((st.buttons & kPadRB) != 0);
        assert((st.buttons & kPadStart) != 0);
        assert(st.leftTrigger == 0 && st.rightTrigger == 255);
        assert(st.lx == 1234 && st.ly == 5678 && st.rx == -4321 && st.ry == -8765);

        // Xbox layout swap (A<->B, X<->Y)
        const GamepadState stXbox = mapSwitchHidToGamepad(
            nxhid::kA | nxhid::kX, 0, 0, 0, 0, false, true);
        assert((stXbox.buttons & kPadB) != 0); // Switch A -> Xbox B
        assert((stXbox.buttons & kPadA) == 0);
        assert((stXbox.buttons & kPadY) != 0); // Switch X -> Xbox Y
        assert((stXbox.buttons & kPadX) == 0);

        const auto pkt = buildGamepadPacket(77, 1700000000123ULL, 2, st);
        assert(pkt.size() == 54); // 8-byte wrapper + 46-byte inner
        assert(readU16LE(pkt.data() + 0) == kTypeControl);
        assert(readU16LE(pkt.data() + 2) == 50); // innerLen + 4
        assert(readU32LE(pkt.data() + 4) == 46); // innerLen
        // Verify inner layout offsets from input.go
        assert(pkt[8] == 0x00 && pkt[11] == 0x1E);
        assert(readU16LE(pkt.data() + 18) == 2);          // controller = 2
        assert(readU16LE(pkt.data() + 20) == 1);          // activeGamepadMask = 1
        assert(readU16LE(pkt.data() + 24) == st.buttons); // buttons
        assert(pkt[26] == 0 && pkt[27] == 255);           // LT/RT
        assert(readU32LE(pkt.data() + 42) == 77);         // seq LE
        assert(readU64LE(pkt.data() + 46) == 1700000000123ULL);
    }

    // 5. RumbleCoalescer: start edge, 16ms coalesce, dedup, stop edge, 3s idle watchdog
    {
        RumbleCoalescer rc;
        float low = -1.0f, high = -1.0f;

        // Start edge -> immediate urgent=true and tick emits at t=100
        assert(rc.submit(204, 102, 100) == true);
        assert(rc.tick(100, low, high) == true);
        assert(std::fabs(low - (204.0f / 255.0f)) < 1e-3f);
        assert(std::fabs(high - (102.0f / 255.0f)) < 1e-3f);

        // Coalesce burst 5ms later -> NOT emitted yet until 16ms elapsed
        assert(rc.submit(255, 255, 105) == false);
        assert(rc.tick(108, low, high) == false);
        assert(rc.submit(128, 64, 112) == false);
        assert(rc.tick(116, low, high) == true); // 116 - 100 = 16ms -> emits coalesced latest!
        assert(std::fabs(low - (128.0f / 255.0f)) < 1e-3f);

        // Dedup: identical active state emits nothing
        assert(rc.tick(150, low, high) == false);

        // Stop edge (0/0) -> immediate wakeup and emits 0 even if <16ms since last
        assert(rc.submit(0, 0, 120) == true);
        assert(rc.tick(120, low, high) == true);
        assert(low == 0.0f && high == 0.0f);

        // Failsafe 3000ms idle watchdog: start at 1000, no stop packet arrives -> auto-zeros at 4000
        assert(rc.submit(200, 200, 1000) == true);
        assert(rc.tick(1000, low, high) == true && low > 0.5f);
        assert(rc.tick(3999, low, high) == false);
        assert(rc.tick(4000, low, high) == true && low == 0.0f && high == 0.0f);
    }

    std::cout << "test_m4_input_rumble: ALL PASSED\n";
    return 0;
}
