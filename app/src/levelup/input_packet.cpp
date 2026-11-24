#include "levelup/input_packet.hpp"

#include <algorithm>
#include <cstring>

namespace levelup::input {
namespace {

inline void putU16LE(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFFU);
    dst[1] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
}

inline void putU32LE(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFFU);
    dst[1] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
    dst[2] = static_cast<uint8_t>((v >> 16U) & 0xFFU);
    dst[3] = static_cast<uint8_t>((v >> 24U) & 0xFFU);
}

inline void putU32BE(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>((v >> 24U) & 0xFFU);
    dst[1] = static_cast<uint8_t>((v >> 16U) & 0xFFU);
    dst[2] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
    dst[3] = static_cast<uint8_t>(v & 0xFFU);
}

inline void putU64LE(uint8_t* dst, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFFULL);
    }
}

inline uint16_t readU16LE(const uint8_t* src) {
    return static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8U);
}

inline uint32_t readU32LE(const uint8_t* src) {
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8U) |
           (static_cast<uint32_t>(src[2]) << 16U) |
           (static_cast<uint32_t>(src[3]) << 24U);
}

inline uint64_t readU64LE(const uint8_t* src) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(src[i]) << (i * 8);
    }
    return v;
}

void appendTail(std::vector<uint8_t>& inner, uint32_t seq, uint64_t tsMs) {
    const size_t old = inner.size();
    inner.resize(old + 12);
    putU32LE(inner.data() + old, seq);
    putU64LE(inner.data() + old + 4, tsMs);
}

} // namespace

std::vector<uint8_t> wrapPacket(const std::vector<uint8_t>& inner, uint16_t ptype) {
    std::vector<uint8_t> out(8 + inner.size());
    putU16LE(out.data() + 0, ptype);
    putU16LE(out.data() + 2, static_cast<uint16_t>(inner.size() + 4));
    putU32LE(out.data() + 4, static_cast<uint32_t>(inner.size()));
    if (!inner.empty()) {
        std::memcpy(out.data() + 8, inner.data(), inner.size());
    }
    return out;
}

std::vector<uint8_t> buildHandshakeSession(std::string_view cSessionId) {
    std::vector<uint8_t> out(4 + cSessionId.size());
    putU16LE(out.data() + 0, 1792);
    putU16LE(out.data() + 2, static_cast<uint16_t>(cSessionId.size()));
    if (!cSessionId.empty()) {
        std::memcpy(out.data() + 4, cSessionId.data(), cSessionId.size());
    }
    return out;
}

std::vector<uint8_t> buildHandshake773() {
    return {0x05, 0x03, 0x02, 0x00, 0x00, 0x00};
}

std::vector<uint8_t> buildHandshake775() {
    return {0x07, 0x03, 0x01, 0x00, 0x00};
}

std::vector<uint8_t> buildBitrateModePacket(bool vqFirst) {
    const uint8_t mode = vqFirst ? 1 : 0;
    return wrapPacket({0x00, 0x00, 0x00, 0x49, mode}, kTypeCursor);
}

std::vector<uint8_t> buildBitrateLevelPacket(uint32_t level) {
    std::vector<uint8_t> inner(8);
    putU32BE(inner.data() + 0, 64);
    putU32BE(inner.data() + 4, level);
    return wrapPacket(inner, kTypeCursor);
}

std::vector<uint8_t> buildCursorPacket(bool show) {
    const uint8_t state = show ? 1 : 2;
    return wrapPacket({0x00, 0x00, 0x00, 0x44, state}, kTypeCursor);
}

std::vector<uint8_t> buildHeartbeatPacket(uint64_t tsMs) {
    std::vector<uint8_t> inner(8);
    putU64LE(inner.data(), tsMs);
    return wrapPacket(inner, kTypeBeat);
}

std::vector<uint8_t> buildGamepadPacket(uint32_t seq,
                                        uint64_t tsMs,
                                        uint16_t controller,
                                        const GamepadState& st) {
    // inner = 34 bytes before 12-byte tail -> 46 bytes total inside wrapPacket -> 54 bytes total with 8-byte header
    // Note Go's Gamepad produces inner=34+12=46 bytes, wrapped to 54 bytes!
    std::vector<uint8_t> inner;
    inner.reserve(46);

    inner.push_back(0x00);
    inner.push_back(0x00);
    inner.push_back(0x00);
    inner.push_back(0x1E);

    inner.push_back(12);
    inner.push_back(0);
    inner.push_back(0);
    inner.push_back(0);

    inner.push_back(26);
    inner.push_back(0);

    inner.push_back(static_cast<uint8_t>(controller & 0xFFU));
    inner.push_back(static_cast<uint8_t>((controller >> 8U) & 0xFFU));

    inner.push_back(1);
    inner.push_back(0); // activeGamepadMask

    inner.push_back(20);
    inner.push_back(0);

    inner.push_back(static_cast<uint8_t>(st.buttons & 0xFFU));
    inner.push_back(static_cast<uint8_t>((st.buttons >> 8U) & 0xFFU));
    inner.push_back(st.leftTrigger);
    inner.push_back(st.rightTrigger);

    const int16_t axes[4] = {st.lx, st.ly, st.rx, st.ry};
    for (int16_t ax : axes) {
        const uint16_t u = static_cast<uint16_t>(ax);
        inner.push_back(static_cast<uint8_t>(u & 0xFFU));
        inner.push_back(static_cast<uint8_t>((u >> 8U) & 0xFFU));
    }

    inner.push_back(156);
    inner.push_back(0);
    inner.push_back(0);
    inner.push_back(0);

    inner.push_back(85);
    inner.push_back(0);

    appendTail(inner, seq, tsMs);
    return wrapPacket(inner, kTypeControl);
}

std::vector<uint8_t> buildMouseMoveAbsolute(uint32_t seq, uint64_t tsMs, uint16_t x, uint16_t y) {
    std::vector<uint8_t> inner = {
        0x00, 0x00, 0x00, 0x08, kMouseModeAbsolute, 0, 0, 0,
        static_cast<uint8_t>(x >> 8U), static_cast<uint8_t>(x & 0xFFU),
        static_cast<uint8_t>(y >> 8U), static_cast<uint8_t>(y & 0xFFU),
    };
    appendTail(inner, seq, tsMs);
    return wrapPacket(inner, kTypeControl);
}

std::vector<uint8_t> buildMouseClickAt(uint32_t seq,
                                       uint64_t tsMs,
                                       uint16_t x,
                                       uint16_t y,
                                       uint8_t button,
                                       bool down) {
    const uint8_t stage = down ? 8 : 9;
    std::vector<uint8_t> inner = {
        0x00, 0x00, 0x00, 0x23, 0x01, button, stage, 0x09,
        static_cast<uint8_t>(x >> 8U), static_cast<uint8_t>(x & 0xFFU),
        static_cast<uint8_t>(y >> 8U), static_cast<uint8_t>(y & 0xFFU),
    };
    appendTail(inner, seq, tsMs);
    return wrapPacket(inner, kTypeControl);
}

bool isHeartbeatEcho1537(const uint8_t* data, size_t len, uint64_t* sentTsMsOut) {
    if (!data || len != 16 || data[0] != 0x01 || data[1] != 0x06) {
        return false;
    }
    if (sentTsMsOut) {
        *sentTsMsOut = readU64LE(data + 8);
    }
    return true;
}

bool parseRumble(const uint8_t* data, size_t len, RumbleEvent& out) {
    if (!data || len < 14) {
        return false;
    }
    if (readU16LE(data + 0) != kTypeControl) {
        return false;
    }
    if (readU32LE(data + 4) != 31U) {
        return false;
    }
    out.controller = readU16LE(data + 8);
    out.lowFreq = readU16LE(data + 10);
    out.highFreq = readU16LE(data + 12);
    return true;
}

uint16_t scaleMotorTo16(uint16_t raw) {
    if (raw <= 255) {
        return static_cast<uint16_t>(raw * 257U);
    }
    return raw;
}

float scaleMotorNorm(uint16_t raw, float userStrength) {
    const float clampedStrength = std::clamp(userStrength, 0.0f, 1.0f);
    const float base = static_cast<float>(scaleMotorTo16(raw)) / 65535.0f;
    return std::clamp(base * clampedStrength, 0.0f, 1.0f);
}

} // namespace levelup::input
