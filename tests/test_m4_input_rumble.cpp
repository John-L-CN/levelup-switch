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

    // 6. Mouse relative/button/scroll, keyboard & UTF-8 paste packets
    {
        // MouseMoveRelative(-12, 34) -> type 518, inner 24: [00 00 00 08][06 00 00 00][FF F4][00 22][seq][ts]
        const auto rel = buildMouseMoveRelative(5, 1700000000001ULL, -12, 34);
        assert(rel.size() == 32);
        assert(readU16LE(rel.data()) == kTypeControl);
        assert(readU32LE(rel.data() + 4) == 24);
        assert(rel[11] == 0x08 && rel[12] == kMouseModeRelative);
        assert(rel[16] == 0xFF && rel[17] == 0xF4); // -12 BE
        assert(rel[18] == 0x00 && rel[19] == 0x22); // +34 BE
        assert(readU32LE(rel.data() + 20) == 5);

        // MouseButton(Right, down=true) -> type 518, inner 21: [00 00 00 05][08][00 00 00][03][seq][ts]
        const auto btn = buildMouseButton(6, 1700000000002ULL, kMouseRight, true);
        assert(btn.size() == 29);
        assert(readU32LE(btn.data() + 4) == 21);
        assert(btn[11] == 0x05 && btn[12] == kMouseButtonDown && btn[16] == kMouseRight);

        // MouseScroll(+1) -> type 518, inner 26: [00 00 00 0A][0A][00 00 00][00 78][00 00 00 00][seq][ts]
        const auto scr = buildMouseScroll(7, 1700000000003ULL, 1);
        assert(scr.size() == 34);
        assert(readU32LE(scr.data() + 4) == 26);
        assert(scr[11] == 0x0A && scr[12] == 0x0A);
        assert(scr[16] == 0x00 && scr[17] == 0x78); // +120 BE

        // Keyboard(0x41='A', down=true, mod=1) -> type 518, inner 26
        const auto kb = buildKeyboardPacket(8, 1700000000004ULL, 0x41, true, 1);
        assert(kb.size() == 34);
        assert(readU32LE(kb.data() + 4) == 26);
        assert(kb[11] == 0x0A && kb[12] == kKeyDown);
        assert(readU16LE(kb.data() + 17) == 0x41);
        assert(kb[19] == 1);

        // PasteText("你好") -> type 518, inner 4 + 6 = 10: [00 00 00 32]["你好" UTF-8]
        const std::string utf8 = "\xE4\xBD\xA0\xE5\xA5\xBD";
        const auto paste = buildPasteTextPacket(utf8);
        assert(paste.size() == 8 + 4 + utf8.size());
        assert(readU16LE(paste.data()) == kTypeControl);
        assert(readU32LE(paste.data() + 4) == 4 + utf8.size());
        assert(paste[8] == 0x00 && paste[9] == 0x00 && paste[10] == 0x00 && paste[11] == 0x32);
        assert(std::memcmp(paste.data() + 12, utf8.data(), utf8.size()) == 0);
    }

    // 7. TouchMouseProcessor: Click mode, Trackpad mode, and overlay button suppression
    {
        std::vector<std::vector<uint8_t>> sent;
        uint32_t seq = 0;
        auto sendFn = [&sent](const std::vector<uint8_t>& pkt) {
            sent.push_back(pkt);
            return true;
        };
        auto seqFn = [&seq]() { return ++seq; };

        TouchMouseProcessor proc;
        proc.setOverlayRects(true,
                             TouchHitRect{16.0f, 16.0f, 136.0f, 44.0f},
                             TouchHitRect{1108.0f, 16.0f, 156.0f, 44.0f});

        // Touch inside visible overlay button is suppressed
        TouchPoint btnTouch{30, 30};
        proc.process(&btnTouch, 1, 1000, sendFn, seqFn);
        proc.process(&btnTouch, 1, 1020, sendFn, seqFn);
        proc.process(nullptr, 0, 1050, sendFn, seqFn);
        assert(sent.empty());

        // Click mode: down -> moveAbs + clickAt(down), up -> clickAt(up)
        TouchPoint p1{640, 360};
        proc.process(&p1, 1, 1100, sendFn, seqFn);
        assert(sent.size() == 2);
        assert(sent[0][12] == kMouseModeAbsolute);
        assert(sent[1][11] == 0x23 && sent[1][14] == kMouseButtonDown);
        proc.process(nullptr, 0, 1150, sendFn, seqFn);
        assert(sent.size() == 3);
        assert(sent[2][11] == 0x23 && sent[2][14] == kMouseButtonUp);

        // Trackpad mode: single-finger drag emits relative move, no click
        sent.clear();
        proc.setMode(TouchMouseMode::Trackpad, 1200, sendFn, seqFn);
        TouchPoint tpStart{500, 400};
        TouchPoint tpMove{520, 410};
        proc.process(&tpStart, 1, 1210, sendFn, seqFn);
        assert(sent.empty());
        proc.process(&tpMove, 1, 1230, sendFn, seqFn);
        assert(sent.size() == 1);
        assert(sent[0][12] == kMouseModeRelative);
        proc.process(nullptr, 0, 1250, sendFn, seqFn);
        assert(sent.size() == 1); // dragged -> no left click on release

        // Trackpad mode: single-finger quick tap -> left click down + up
        sent.clear();
        TouchPoint tapPt{600, 400};
        proc.process(&tapPt, 1, 1300, sendFn, seqFn);
        proc.process(nullptr, 0, 1380, sendFn, seqFn);
        assert(sent.size() == 2);
        assert(sent[0][11] == 0x05 && sent[0][12] == kMouseButtonDown && sent[0][16] == kMouseLeft);
        assert(sent[1][11] == 0x05 && sent[1][12] == kMouseButtonUp && sent[1][16] == kMouseLeft);

        // Trackpad mode: two-finger tap -> right click down + up
        sent.clear();
        TouchPoint twoPts[2] = {{600, 400}, {650, 400}};
        proc.process(twoPts, 2, 1500, sendFn, seqFn);
        proc.process(nullptr, 0, 1580, sendFn, seqFn);
        assert(sent.size() == 2);
        assert(sent[0][11] == 0x05 && sent[0][12] == kMouseButtonDown && sent[0][16] == kMouseRight);
        assert(sent[1][11] == 0x05 && sent[1][12] == kMouseButtonUp && sent[1][16] == kMouseRight);

        // Locked mode: single tap, drag, two-finger tap/scroll emit nothing
        sent.clear();
        proc.setMode(TouchMouseMode::Locked, 1600, sendFn, seqFn);
        proc.process(&tapPt, 1, 1610, sendFn, seqFn);
        proc.process(&tpMove, 1, 1630, sendFn, seqFn);
        proc.process(nullptr, 0, 1660, sendFn, seqFn);
        TouchPoint twoScroll[2] = {{600, 460}, {650, 460}};
        proc.process(twoPts, 2, 1700, sendFn, seqFn);
        proc.process(twoScroll, 2, 1720, sendFn, seqFn);
        proc.process(nullptr, 0, 1760, sendFn, seqFn);
        assert(sent.empty());

        // Switching Locked -> Click mid-touch suppresses stray clicks until lift
        proc.process(&tapPt, 1, 1800, sendFn, seqFn);
        proc.setMode(TouchMouseMode::Click, 1810, sendFn, seqFn);
        proc.process(&tapPt, 1, 1820, sendFn, seqFn);
        assert(sent.empty());
        proc.process(nullptr, 0, 1840, sendFn, seqFn);
        assert(sent.empty());
        proc.process(&p1, 1, 1860, sendFn, seqFn);
        assert(sent.size() == 2);

        // Switching Click -> Locked mid-touch cleanly releases left mouse button
        sent.clear();
        proc.setMode(TouchMouseMode::Locked, 1900, sendFn, seqFn);
        assert(sent.size() == 1);
        assert(sent[0][11] == 0x23 && sent[0][14] == kMouseButtonUp);
        proc.process(&p1, 1, 1910, sendFn, seqFn);
        assert(sent.size() == 1);
        proc.process(nullptr, 0, 1930, sendFn, seqFn);
        assert(sent.size() == 1);
    }

    std::cout << "test_m4_input_rumble: ALL PASSED\n";
    return 0;
}
