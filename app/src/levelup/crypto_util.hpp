#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace levelup {

/// Computes raw 16-byte MD5 digest (RFC 1321).
std::array<uint8_t, 16> md5Raw(std::string_view input);
std::array<uint8_t, 16> md5Raw(const uint8_t* data, size_t len);

/// Computes 32-char lowercase hex MD5 digest.
std::string md5HexLower(std::string_view input);

/// Computes raw 20-byte SHA-1 digest (RFC 3174).
std::array<uint8_t, 20> sha1Raw(std::string_view input);
std::array<uint8_t, 20> sha1Raw(const uint8_t* data, size_t len);

/// Computes raw 20-byte HMAC-SHA1 (RFC 2104).
std::array<uint8_t, 20> hmacSha1Raw(std::string_view key, std::string_view message);

/// Standard Base64 encoder / decoder (tolerates whitespace & optional data URI prefix).
std::string base64Encode(const uint8_t* data, size_t len);
std::string base64Encode(std::string_view data);
std::vector<uint8_t> base64Decode(std::string_view b64);

/// RFC 3986 percent-encoding keeping only unreserved A-Z a-z 0-9 _ . - ~ (Python quote safe="").
std::string urlEncodeStrict(std::string_view s);

/// Same as urlEncodeStrict, also preserving '/' unescaped (Python quote safe="/").
std::string urlEncodeSlashSafe(std::string_view s);

/// Generates a 32-char lowercase hex device ID (csrng on Switch, urandom fallback on host).
std::string randomHex32();

/// Monotonic/wall-clock timestamp helpers matching Go time.Now().UnixMilli()/Unix().
int64_t unixMillisNow();
int64_t unixSecondsNow();

} // namespace levelup
