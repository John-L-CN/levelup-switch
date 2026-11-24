#include "levelup/crypto_util.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace levelup {
namespace {

inline uint32_t rotl32(uint32_t x, uint32_t n) {
    return (x << n) | (x >> (32U - n));
}

inline uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8U) |
           (static_cast<uint32_t>(p[2]) << 16U) |
           (static_cast<uint32_t>(p[3]) << 24U);
}

inline void writeU32LE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 16U) & 0xFFU);
    p[3] = static_cast<uint8_t>((v >> 24U) & 0xFFU);
}

inline uint32_t readU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24U) |
           (static_cast<uint32_t>(p[1]) << 16U) |
           (static_cast<uint32_t>(p[2]) << 8U) |
           static_cast<uint32_t>(p[3]);
}

inline void writeU32BE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24U) & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 16U) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 8U) & 0xFFU);
    p[3] = static_cast<uint8_t>(v & 0xFFU);
}

inline bool isUnreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '-' || c == '~';
}

} // namespace

std::array<uint8_t, 16> md5Raw(const uint8_t* data, size_t len) {
    static constexpr uint32_t kS[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    static constexpr uint32_t kK[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
        0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
        0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };

    std::vector<uint8_t> msg;
    msg.reserve(((len + 8) / 64 + 1) * 64);
    if (len > 0 && data != nullptr) {
        msg.insert(msg.end(), data, data + len);
    }
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) {
        msg.push_back(0x00);
    }
    const uint64_t bitLen = static_cast<uint64_t>(len) * 8ULL;
    for (int i = 0; i < 8; ++i) {
        msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFFULL));
    }

    uint32_t a0 = 0x67452301U;
    uint32_t b0 = 0xefcdab89U;
    uint32_t c0 = 0x98badcfeU;
    uint32_t d0 = 0x10325476U;

    for (size_t offset = 0; offset < msg.size(); offset += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; ++i) {
            M[i] = readU32LE(msg.data() + offset + i * 4);
        }

        uint32_t A = a0;
        uint32_t B = b0;
        uint32_t C = c0;
        uint32_t D = d0;

        for (int i = 0; i < 64; ++i) {
            uint32_t F = 0;
            int g = 0;
            if (i < 16) {
                F = (B & C) | ((~B) & D);
                g = i;
            } else if (i < 32) {
                F = (D & B) | ((~D) & C);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                F = B ^ C ^ D;
                g = (3 * i + 5) % 16;
            } else {
                F = C ^ (B | (~D));
                g = (7 * i) % 16;
            }
            const uint32_t temp = D;
            D = C;
            C = B;
            B = B + rotl32(A + F + kK[i] + M[g], kS[i]);
            A = temp;
        }

        a0 += A;
        b0 += B;
        c0 += C;
        d0 += D;
    }

    std::array<uint8_t, 16> out{};
    writeU32LE(out.data() + 0, a0);
    writeU32LE(out.data() + 4, b0);
    writeU32LE(out.data() + 8, c0);
    writeU32LE(out.data() + 12, d0);
    return out;
}

std::array<uint8_t, 16> md5Raw(std::string_view input) {
    return md5Raw(reinterpret_cast<const uint8_t*>(input.data()), input.size());
}

std::string md5HexLower(std::string_view input) {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto raw = md5Raw(input);
    std::string hex;
    hex.resize(32);
    for (size_t i = 0; i < 16; ++i) {
        hex[i * 2] = kHex[(raw[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = kHex[raw[i] & 0x0F];
    }
    return hex;
}

std::array<uint8_t, 20> sha1Raw(const uint8_t* data, size_t len) {
    std::vector<uint8_t> msg;
    msg.reserve(((len + 8) / 64 + 1) * 64);
    if (len > 0 && data != nullptr) {
        msg.insert(msg.end(), data, data + len);
    }
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) {
        msg.push_back(0x00);
    }
    const uint64_t bitLen = static_cast<uint64_t>(len) * 8ULL;
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFFULL));
    }

    uint32_t h0 = 0x67452301U;
    uint32_t h1 = 0xefcdab89U;
    uint32_t h2 = 0x98badcfeU;
    uint32_t h3 = 0x10325476U;
    uint32_t h4 = 0xc3d2e1f0U;

    for (size_t offset = 0; offset < msg.size(); offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = readU32BE(msg.data() + offset + i * 4);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;

        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            const uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl32(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<uint8_t, 20> out{};
    writeU32BE(out.data() + 0, h0);
    writeU32BE(out.data() + 4, h1);
    writeU32BE(out.data() + 8, h2);
    writeU32BE(out.data() + 12, h3);
    writeU32BE(out.data() + 16, h4);
    return out;
}

std::array<uint8_t, 20> sha1Raw(std::string_view input) {
    return sha1Raw(reinterpret_cast<const uint8_t*>(input.data()), input.size());
}

std::array<uint8_t, 20> hmacSha1Raw(std::string_view key, std::string_view message) {
    constexpr size_t kBlockSize = 64;
    std::array<uint8_t, kBlockSize> keyBlock{};
    if (key.size() > kBlockSize) {
        const auto hashed = sha1Raw(key);
        std::memcpy(keyBlock.data(), hashed.data(), hashed.size());
    } else if (!key.empty()) {
        std::memcpy(keyBlock.data(), key.data(), key.size());
    }

    std::vector<uint8_t> inner;
    inner.reserve(kBlockSize + message.size());
    for (size_t i = 0; i < kBlockSize; ++i) {
        inner.push_back(keyBlock[i] ^ 0x36U);
    }
    inner.insert(inner.end(),
                 reinterpret_cast<const uint8_t*>(message.data()),
                 reinterpret_cast<const uint8_t*>(message.data()) + message.size());
    const auto innerHash = sha1Raw(inner.data(), inner.size());

    std::vector<uint8_t> outer;
    outer.reserve(kBlockSize + innerHash.size());
    for (size_t i = 0; i < kBlockSize; ++i) {
        outer.push_back(keyBlock[i] ^ 0x5CU);
    }
    outer.insert(outer.end(), innerHash.begin(), innerHash.end());
    return sha1Raw(outer.data(), outer.size());
}

std::string base64Encode(const uint8_t* data, size_t len) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t b0 = data[i];
        const bool has1 = (i + 1) < len;
        const bool has2 = (i + 2) < len;
        const uint32_t b1 = has1 ? data[i + 1] : 0;
        const uint32_t b2 = has2 ? data[i + 2] : 0;
        const uint32_t triple = (b0 << 16U) | (b1 << 8U) | b2;

        out.push_back(kTable[(triple >> 18U) & 0x3FU]);
        out.push_back(kTable[(triple >> 12U) & 0x3FU]);
        out.push_back(has1 ? kTable[(triple >> 6U) & 0x3FU] : '=');
        out.push_back(has2 ? kTable[triple & 0x3FU] : '=');
    }
    return out;
}

std::string base64Encode(std::string_view data) {
    return base64Encode(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::vector<uint8_t> base64Decode(std::string_view b64) {
    if (const auto comma = b64.find(','); comma != std::string_view::npos) {
        const auto head = b64.substr(0, comma);
        if (head.find("base64") != std::string_view::npos) {
            b64 = b64.substr(comma + 1);
        }
    }

    auto decodeChar = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };

    std::vector<uint8_t> out;
    out.reserve((b64.size() * 3) / 4);
    uint32_t buf = 0;
    int bits = 0;
    for (unsigned char c : b64) {
        if (c == '=' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        const int val = decodeChar(c);
        if (val < 0) {
            return {};
        }
        buf = (buf << 6U) | static_cast<uint32_t>(val);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFFU));
        }
    }
    return out;
}

std::string urlEncodeStrict(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (isUnreserved(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4U) & 0x0FU]);
            out.push_back(kHex[c & 0x0FU]);
        }
    }
    return out;
}

std::string urlEncodeSlashSafe(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (isUnreserved(c) || c == '/') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4U) & 0x0FU]);
            out.push_back(kHex[c & 0x0FU]);
        }
    }
    return out;
}

std::string randomHex32() {
    static constexpr char kHex[] = "0123456789abcdef";
    std::array<uint8_t, 16> bytes{};
    bool filled = false;

#ifdef __SWITCH__
    if (R_SUCCEEDED(csrngInitialize())) {
        if (R_SUCCEEDED(csrngGetRandomBytes(bytes.data(), bytes.size()))) {
            filled = true;
        }
        csrngExit();
    }
#endif

    if (!filled) {
        std::random_device rd;
        std::seed_seq seed{
            rd(), rd(),
            static_cast<uint32_t>(unixMillisNow() & 0xFFFFFFFFULL),
            static_cast<uint32_t>((unixMillisNow() >> 32) & 0xFFFFFFFFULL),
        };
        std::mt19937_64 gen(seed);
        for (size_t i = 0; i < 2; ++i) {
            const uint64_t r = gen();
            std::memcpy(bytes.data() + i * 8, &r, 8);
        }
    }

    std::string hex;
    hex.resize(32);
    for (size_t i = 0; i < bytes.size(); ++i) {
        hex[i * 2] = kHex[(bytes[i] >> 4U) & 0x0FU];
        hex[i * 2 + 1] = kHex[bytes[i] & 0x0FU];
    }
    return hex;
}

int64_t unixMillisNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t unixSecondsNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace levelup
