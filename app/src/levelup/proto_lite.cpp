#include "levelup/proto_lite.hpp"

#include "levelup/crypto_util.hpp"

#include <cstring>

namespace levelup::ytcx {
namespace {

inline void appendU32LE(std::vector<uint8_t>& dst, uint32_t v) {
    dst.push_back(static_cast<uint8_t>(v & 0xFFU));
    dst.push_back(static_cast<uint8_t>((v >> 8U) & 0xFFU));
    dst.push_back(static_cast<uint8_t>((v >> 16U) & 0xFFU));
    dst.push_back(static_cast<uint8_t>((v >> 24U) & 0xFFU));
}

inline uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8U) |
           (static_cast<uint32_t>(p[2]) << 16U) |
           (static_cast<uint32_t>(p[3]) << 24U);
}

inline uint64_t readU64LE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return v;
}

} // namespace

void appendVarint(std::vector<uint8_t>& dst, uint64_t v) {
    while (v >= 0x80ULL) {
        dst.push_back(static_cast<uint8_t>((v & 0x7FULL) | 0x80ULL));
        v >>= 7U;
    }
    dst.push_back(static_cast<uint8_t>(v));
}

bool decodeVarint(const uint8_t* data, size_t len, size_t& off, uint64_t& out) {
    uint64_t res = 0;
    unsigned shift = 0;
    while (true) {
        if (off >= len) {
            return false;
        }
        const uint8_t b = data[off++];
        res |= static_cast<uint64_t>(b & 0x7FU) << shift;
        if ((b & 0x80U) == 0) {
            out = res;
            return true;
        }
        shift += 7U;
        if (shift > 63U) {
            return false;
        }
    }
}

void appendVarintField(std::vector<uint8_t>& dst, int num, uint64_t v) {
    appendVarint(dst, (static_cast<uint64_t>(num) << 3U) | 0ULL);
    appendVarint(dst, v);
}

void appendBytesField(std::vector<uint8_t>& dst, int num, const uint8_t* data, size_t len) {
    appendVarint(dst, (static_cast<uint64_t>(num) << 3U) | 2ULL);
    appendVarint(dst, len);
    if (len > 0 && data != nullptr) {
        dst.insert(dst.end(), data, data + len);
    }
}

void appendBytesField(std::vector<uint8_t>& dst, int num, const std::vector<uint8_t>& bytes) {
    appendBytesField(dst, num, bytes.data(), bytes.size());
}

void appendStringField(std::vector<uint8_t>& dst, int num, std::string_view s) {
    appendBytesField(dst, num, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

void appendFixed32Field(std::vector<uint8_t>& dst, int num, uint32_t v) {
    appendVarint(dst, (static_cast<uint64_t>(num) << 3U) | 5ULL);
    appendU32LE(dst, v);
}

bool parseFields(const uint8_t* data, size_t len, std::vector<ProtoField>& out, std::string* err) {
    out.clear();
    size_t off = 0;
    while (off < len) {
        uint64_t tag = 0;
        if (!decodeVarint(data, len, off, tag)) {
            if (err) *err = "varint tag out of bounds";
            return false;
        }
        ProtoField f;
        f.num = static_cast<int>(tag >> 3U);
        f.wire = static_cast<int>(tag & 0x07ULL);
        switch (f.wire) {
        case 0: {
            if (!decodeVarint(data, len, off, f.u64)) {
                if (err) *err = "varint value out of bounds";
                return false;
            }
            break;
        }
        case 1: {
            if (off + 8 > len) {
                if (err) *err = "fixed64 out of bounds";
                return false;
            }
            f.u64 = readU64LE(data + off);
            off += 8;
            break;
        }
        case 2: {
            uint64_t flen = 0;
            if (!decodeVarint(data, len, off, flen)) {
                if (err) *err = "bytes length out of bounds";
                return false;
            }
            if (flen > len - off) {
                if (err) *err = "bytes payload out of bounds";
                return false;
            }
            if (flen > 0) {
                f.data.assign(data + off, data + off + flen);
            }
            off += static_cast<size_t>(flen);
            break;
        }
        case 5: {
            if (off + 4 > len) {
                if (err) *err = "fixed32 out of bounds";
                return false;
            }
            f.u64 = readU32LE(data + off);
            off += 4;
            break;
        }
        default:
            if (err) *err = "unsupported wire type " + std::to_string(f.wire);
            return false;
        }
        out.push_back(std::move(f));
    }
    return true;
}

const std::vector<uint8_t>* getFieldBytes(const std::vector<ProtoField>& fields, int num) {
    for (const auto& f : fields) {
        if (f.num == num && f.wire == 2) {
            return &f.data;
        }
    }
    return nullptr;
}

std::string getFieldString(const std::vector<ProtoField>& fields, int num) {
    const auto* b = getFieldBytes(fields, num);
    if (!b || b->empty()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(b->data()), b->size());
}

uint64_t getFieldU64(const std::vector<ProtoField>& fields, int num, uint64_t fallback) {
    for (const auto& f : fields) {
        if (f.num == num) {
            return f.u64;
        }
    }
    return fallback;
}

std::vector<uint8_t> encodeCGMessage(uint32_t cmd, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> buf;
    buf.reserve(16 + payload.size());
    buf.push_back(0x0D);
    appendU32LE(buf, cmd);
    buf.push_back(0x15);
    appendU32LE(buf, kCGMagicVersion);
    if (!payload.empty()) {
        appendBytesField(buf, 3, payload);
    }
    return buf;
}

bool decodeCGMessage(const uint8_t* raw, size_t len, CGMessage& out, std::string* err) {
    if (!raw || len < 5) {
        if (err) *err = "CGMessage too short";
        return false;
    }
    if (raw[0] != 0x0D) {
        if (err) *err = "CGMessage invalid header byte";
        return false;
    }
    out = CGMessage{};
    out.cmd = readU32LE(raw + 1);
    const uint8_t* rest = raw + 5;
    size_t restLen = len - 5;
    if (restLen >= 5 && rest[0] == 0x15) {
        out.version = readU32LE(rest + 1);
        rest += 5;
        restLen -= 5;
    }

    std::vector<ProtoField> fields;
    if (!parseFields(rest, restLen, fields, err)) {
        out.payload.assign(rest, rest + restLen);
        return false;
    }
    if (const auto* f3 = getFieldBytes(fields, 3)) {
        out.payload = *f3;
    } else {
        out.payload.assign(rest, rest + restLen);
    }
    return true;
}

std::vector<uint8_t> buildCommonHeader(std::string_view requestId,
                                       std::string_view token,
                                       int64_t timestampMs) {
    const int64_t nowMs = timestampMs > 0 ? timestampMs : unixMillisNow();
    std::vector<uint8_t> hdr;
    appendVarintField(hdr, 1, static_cast<uint64_t>(nowMs));
    if (!token.empty()) {
        appendStringField(hdr, 2, token);
    }
    if (!requestId.empty()) {
        appendStringField(hdr, 3, requestId);
    }
    return hdr;
}

std::vector<uint8_t> buildLaunchGameProto(std::string_view proData,
                                          int64_t pkgName,
                                          std::string_view deviceInfoJson,
                                          std::string_view streamInfoJson,
                                          int64_t timestampMs,
                                          std::string_view requestId) {
    const int64_t nowMs = timestampMs > 0 ? timestampMs : unixMillisNow();
    const std::string reqIdStr =
        requestId.empty() ? ("Req_" + std::to_string(nowMs / 1000)) : std::string(requestId);
    const auto hdr = buildCommonHeader(reqIdStr, "", nowMs);

    std::vector<uint8_t> body;
    appendBytesField(body, 1, hdr);
    appendVarintField(body, 2, static_cast<uint64_t>(pkgName));
    appendStringField(body, 3, proData);
    appendStringField(body, 7, deviceInfoJson);
    appendStringField(body, 8, streamInfoJson);
    appendVarintField(body, 9, 1ULL);
    return body;
}

bool parseGameResource(const uint8_t* data, size_t len, GameResource& out, std::string* err) {
    std::vector<ProtoField> fields;
    if (!parseFields(data, len, fields, err)) {
        return false;
    }
    out = GameResource{};
    out.ipv4 = getFieldString(fields, 1);
    out.ipv6 = getFieldString(fields, 7);
    out.basePort = static_cast<int>(getFieldU64(fields, 2)) * 100;
    out.aSessionId = getFieldString(fields, 10);
    out.vSessionId = getFieldString(fields, 11);
    out.cSessionId = getFieldString(fields, 12);
    for (const auto& f : fields) {
        if (f.num == 20 && f.wire == 2) {
            std::vector<ProtoField> entry;
            if (parseFields(f.data.data(), f.data.size(), entry)) {
                const std::string k = getFieldString(entry, 1);
                if (!k.empty()) {
                    out.ports[k] = getFieldU64(entry, 2);
                }
            }
        }
    }
    return true;
}

std::vector<uint8_t> buildHeartbeatReq(int64_t seq, std::string_view token, int64_t timestampMs) {
    const int64_t nowMs = timestampMs > 0 ? timestampMs : unixMillisNow();
    const std::string reqId = "HB_" + std::to_string(seq) + "_" + std::to_string(nowMs);
    const auto hdr = buildCommonHeader(reqId, token, nowMs);
    std::vector<uint8_t> body;
    appendBytesField(body, 1, hdr);
    return body;
}

HeartbeatRsp parseHeartbeatRsp(const uint8_t* data, size_t len) {
    HeartbeatRsp rsp;
    std::vector<ProtoField> fields;
    if (!parseFields(data, len, fields)) {
        return rsp;
    }
    rsp.errcode = 0;
    if (const auto* res = getFieldBytes(fields, 2)) {
        std::vector<ProtoField> rf;
        if (parseFields(res->data(), res->size(), rf)) {
            rsp.errcode = static_cast<int>(getFieldU64(rf, 1));
        }
    }
    rsp.periodMs = static_cast<int>(getFieldU64(fields, 3));
    return rsp;
}

std::vector<uint8_t> buildStreamStatusNtf(bool playing, std::string_view token, int64_t timestampMs) {
    const int64_t nowMs = timestampMs > 0 ? timestampMs : unixMillisNow();
    const std::string reqId = "StreamStatus_" + std::to_string(nowMs);
    const auto hdr = buildCommonHeader(reqId, token, nowMs);
    std::vector<uint8_t> body;
    appendBytesField(body, 1, hdr);
    if (playing) {
        appendVarintField(body, 2, 1ULL);
    }
    return body;
}

std::vector<uint8_t> buildAllocVPadReq(std::string_view token,
                                       std::string_view vpadCert,
                                       int padIndex,
                                       int usage,
                                       int64_t timestampMs,
                                       std::string_view requestId) {
    const int64_t nowMs = timestampMs > 0 ? timestampMs : unixMillisNow();
    const std::string reqId =
        requestId.empty() ? ("AllocVPad_" + std::to_string(nowMs)) : std::string(requestId);
    const auto hdr = buildCommonHeader(reqId, token, nowMs);

    std::vector<uint8_t> body;
    appendBytesField(body, 1, hdr);
    if (padIndex == 0) {
        padIndex = 65535;
    }
    appendVarintField(body, 2, static_cast<uint64_t>(padIndex));
    if (!vpadCert.empty()) {
        appendStringField(body, 3, vpadCert);
    }
    if (usage != 0) {
        appendVarintField(body, 5, static_cast<uint64_t>(usage));
    }
    return body;
}

AllocVPadRsp parseAllocVPadRsp(const uint8_t* data, size_t len) {
    AllocVPadRsp rsp;
    std::vector<ProtoField> fields;
    if (!parseFields(data, len, fields)) {
        return rsp;
    }
    if (const auto* res = getFieldBytes(fields, 2)) {
        std::vector<ProtoField> rf;
        if (parseFields(res->data(), res->size(), rf)) {
            rsp.errcode = static_cast<int>(getFieldU64(rf, 1));
        }
    }
    rsp.vpadId = static_cast<int>(getFieldU64(fields, 3));
    rsp.ok = !(rsp.vpadId == 0 && rsp.errcode == 0);
    return rsp;
}

std::vector<uint8_t> buildRecycleVPadReq(std::string_view token, int vpadId, int64_t timestampMs) {
    const int64_t nowMs = timestampMs > 0 ? timestampMs : unixMillisNow();
    const std::string reqId = "RecycleVPad_" + std::to_string(nowMs);
    const auto hdr = buildCommonHeader(reqId, token, nowMs);
    std::vector<uint8_t> body;
    appendBytesField(body, 1, hdr);
    if (vpadId != 0) {
        appendVarintField(body, 2, static_cast<uint64_t>(vpadId));
    }
    return body;
}

std::vector<uint8_t> buildExitGameReq(std::string_view token, int64_t pkgName, int64_t timestampMs) {
    const int64_t nowMs = timestampMs > 0 ? timestampMs : unixMillisNow();
    const std::string reqId = "Exit_" + std::to_string(nowMs);
    const auto hdr = buildCommonHeader(reqId, token, nowMs);
    std::vector<uint8_t> body;
    appendBytesField(body, 1, hdr);
    if (pkgName != 0) {
        appendVarintField(body, 2, static_cast<uint64_t>(pkgName));
    }
    return body;
}

} // namespace levelup::ytcx
