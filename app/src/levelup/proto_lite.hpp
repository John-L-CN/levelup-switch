#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace levelup::ytcx {

// BE gateway command IDs matching Cloudgame.java / Go ytcx/be.go
constexpr uint32_t kCmdLogin = 2;
constexpr uint32_t kCmdReqHeartbeat = 5;
constexpr uint32_t kCmdRspHeartbeat = 6;
constexpr uint32_t kCmdReqLaunchGame = 7;
constexpr uint32_t kCmdRspLaunchGame = 8;
constexpr uint32_t kCmdReqExitGame = 9;
constexpr uint32_t kCmdNtfGameStatus = 17;
constexpr uint32_t kCmdReqAllocVPad = 33;
constexpr uint32_t kCmdRspAllocVPad = 34;
constexpr uint32_t kCmdNtfStreamStatus = 37;
constexpr uint32_t kCmdCfmStreamStatus = 38;
constexpr uint32_t kCmdReqRecycleVPad = 43;

constexpr uint32_t kCGMagicVersion = 39;

struct ProtoField {
    int num = 0;
    int wire = 0;
    uint64_t u64 = 0;
    std::vector<uint8_t> data;
};

struct CGMessage {
    uint32_t cmd = 0;
    uint32_t version = 0;
    std::vector<uint8_t> payload;
};

struct GameResource {
    std::string ipv4;
    std::string ipv6;
    int basePort = 0;
    std::string aSessionId;
    std::string vSessionId;
    std::string cSessionId;
    std::map<std::string, uint64_t> ports;
};

struct HeartbeatRsp {
    int errcode = -1;
    int periodMs = 0;
};

struct AllocVPadRsp {
    bool ok = false;
    int vpadId = 0;
    int errcode = 0;
};

void appendVarint(std::vector<uint8_t>& dst, uint64_t v);
bool decodeVarint(const uint8_t* data, size_t len, size_t& off, uint64_t& out);

void appendVarintField(std::vector<uint8_t>& dst, int num, uint64_t v);
void appendBytesField(std::vector<uint8_t>& dst, int num, const uint8_t* data, size_t len);
void appendBytesField(std::vector<uint8_t>& dst, int num, const std::vector<uint8_t>& bytes);
void appendStringField(std::vector<uint8_t>& dst, int num, std::string_view s);
void appendFixed32Field(std::vector<uint8_t>& dst, int num, uint32_t v);

bool parseFields(const uint8_t* data, size_t len, std::vector<ProtoField>& out, std::string* err = nullptr);
const std::vector<uint8_t>* getFieldBytes(const std::vector<ProtoField>& fields, int num);
std::string getFieldString(const std::vector<ProtoField>& fields, int num);
uint64_t getFieldU64(const std::vector<ProtoField>& fields, int num, uint64_t fallback = 0);

std::vector<uint8_t> encodeCGMessage(uint32_t cmd, const std::vector<uint8_t>& payload);
bool decodeCGMessage(const uint8_t* raw, size_t len, CGMessage& out, std::string* err = nullptr);

std::vector<uint8_t> buildCommonHeader(std::string_view requestId,
                                       std::string_view token,
                                       int64_t timestampMs = 0);

std::vector<uint8_t> buildLaunchGameProto(std::string_view proData,
                                          int64_t pkgName,
                                          std::string_view deviceInfoJson,
                                          std::string_view streamInfoJson,
                                          int64_t timestampMs = 0,
                                          std::string_view requestId = {});

bool parseGameResource(const uint8_t* data, size_t len, GameResource& out, std::string* err = nullptr);

std::vector<uint8_t> buildHeartbeatReq(int64_t seq, std::string_view token, int64_t timestampMs = 0);
HeartbeatRsp parseHeartbeatRsp(const uint8_t* data, size_t len);

std::vector<uint8_t> buildStreamStatusNtf(bool playing, std::string_view token, int64_t timestampMs = 0);

std::vector<uint8_t> buildAllocVPadReq(std::string_view token,
                                       std::string_view vpadCert,
                                       int padIndex,
                                       int usage,
                                       int64_t timestampMs = 0,
                                       std::string_view requestId = {});
AllocVPadRsp parseAllocVPadRsp(const uint8_t* data, size_t len);

std::vector<uint8_t> buildRecycleVPadReq(std::string_view token, int vpadId, int64_t timestampMs = 0);
std::vector<uint8_t> buildExitGameReq(std::string_view token, int64_t pkgName, int64_t timestampMs = 0);

} // namespace levelup::ytcx
