#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace levelup::input {

constexpr uint16_t kTypeControl = 518;
constexpr uint16_t kTypeCursor = 1025;
constexpr uint16_t kTypeBeat = 1537;

constexpr uint8_t kMouseModeRelative = 6;
constexpr uint8_t kMouseModeAbsolute = 9;
constexpr uint8_t kMouseButtonDown = 8;
constexpr uint8_t kMouseButtonUp = 9;
constexpr uint8_t kKeyDown = 3;
constexpr uint8_t kKeyUp = 4;
constexpr uint8_t kMouseLeft = 1;
constexpr uint8_t kMouseMiddle = 2;
constexpr uint8_t kMouseRight = 3;

// Native Nintendo / Switch XInput-compatible button bitmask (matches cloud switch-handheld semantics; DO NOT swap ABXY)
constexpr uint16_t kPadDPadUp = 0x0001;
constexpr uint16_t kPadDPadDown = 0x0002;
constexpr uint16_t kPadDPadLeft = 0x0004;
constexpr uint16_t kPadDPadRight = 0x0008;
constexpr uint16_t kPadStart = 0x0010;
constexpr uint16_t kPadBack = 0x0020;
constexpr uint16_t kPadL3 = 0x0040;
constexpr uint16_t kPadR3 = 0x0080;
constexpr uint16_t kPadLB = 0x0100;
constexpr uint16_t kPadRB = 0x0200;
constexpr uint16_t kPadGuide = 0x0400;
constexpr uint16_t kPadA = 0x1000;
constexpr uint16_t kPadB = 0x2000;
constexpr uint16_t kPadX = 0x4000;
constexpr uint16_t kPadY = 0x8000;

struct GamepadState {
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0;
    uint8_t rightTrigger = 0;
    int16_t lx = 0;
    int16_t ly = 0;
    int16_t rx = 0;
    int16_t ry = 0;

    bool operator==(const GamepadState&) const = default;
};

struct RumbleEvent {
    uint16_t controller = 0;
    uint16_t lowFreq = 0;
    uint16_t highFreq = 0;

    bool operator==(const RumbleEvent&) const = default;
};

std::vector<uint8_t> wrapPacket(const std::vector<uint8_t>& inner, uint16_t ptype);

std::vector<uint8_t> buildHandshakeSession(std::string_view cSessionId);
std::vector<uint8_t> buildHandshake773();
std::vector<uint8_t> buildHandshake775();
std::vector<uint8_t> buildBitrateModePacket(bool vqFirst);
std::vector<uint8_t> buildBitrateLevelPacket(uint32_t level);
std::vector<uint8_t> buildCursorPacket(bool show);
std::vector<uint8_t> buildHeartbeatPacket(uint64_t tsMs);

std::vector<uint8_t> buildGamepadPacket(uint32_t seq,
                                        uint64_t tsMs,
                                        uint16_t controller,
                                        const GamepadState& st);

std::vector<uint8_t> buildMouseMoveAbsolute(uint32_t seq, uint64_t tsMs, uint16_t x, uint16_t y);
std::vector<uint8_t> buildMouseMoveRelative(uint32_t seq, uint64_t tsMs, int16_t dx, int16_t dy);
std::vector<uint8_t> buildMouseButton(uint32_t seq, uint64_t tsMs, uint8_t button, bool down);
std::vector<uint8_t> buildMouseScroll(uint32_t seq, uint64_t tsMs, int dir);
std::vector<uint8_t> buildMouseClickAt(uint32_t seq,
                                       uint64_t tsMs,
                                       uint16_t x,
                                       uint16_t y,
                                       uint8_t button,
                                       bool down);
std::vector<uint8_t> buildKeyboardPacket(uint32_t seq,
                                         uint64_t tsMs,
                                         uint16_t vk,
                                         bool down,
                                         uint8_t mod = 0);
std::vector<uint8_t> buildPasteTextPacket(std::string_view utf8Text);

bool isHeartbeatEcho1537(const uint8_t* data, size_t len, uint64_t* sentTsMsOut = nullptr);
bool parseRumble(const uint8_t* data, size_t len, RumbleEvent& out);

uint16_t scaleMotorTo16(uint16_t raw);
float scaleMotorNorm(uint16_t raw, float userStrength = 1.0f);

} // namespace levelup::input
