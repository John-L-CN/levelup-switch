# LevelUp / YTCX 全链路协议权威规格书（Switch C++ 移植基准）

> 本文档所有接口、常量、Protobuf 字段号与字节布局均提取并实测验证自 `../go-client`（截至 2026-11-24 已实现长时间稳定串流、1080p60 High Profile 解码、0 丢包与完整输入握手）。

---

## 1. LevelUp HTTP API（`../go-client/internal/api/client.go`）

### 1.1 基础常量（App 内置公开常量，非个人凭据）
- `BaseURL`: `https://client-api.yunshungame.com/api/`
- `signKey`: `84c1f93f61b0c0053952f444853afc4b`
- `apiKey`: `8d5e1cf4465e90091ef0029b4618e404`
- `channelCode`: `main`
- `appVer`: `2.2.6` (`appVerCode`: `35`)
- `streamingVer`: `1.1.8` (`streamingVerCode`: `18`)
- `User-Agent`: `okhttp/4.12.0`
- `Content-Type`: `application/json; charset=utf-8`

### 1.2 公共参数与 `clientInfo` 键顺序铁律
每次 POST 请求（JSON body，非空字段才写入）除业务字段外，必须注入以下公共参数：
- `t`: 当前毫秒时间戳字符串
- `appVer`, `appVerCode`, `streamingVer`, `streamingVerCode`
- `clientInfo`: **必须严格按下方键顺序拼接，不能用无序 map 重排**（服务端用前 50 字符做签名与风控，键序改变必报 `1030001 登录过期`）：
  ```json
  {"screen":"2560*1600","os":"14","sdkVersion":34,"deviceId":"<32位hex>","androidId":"<32位hex>","brand":"Lenovo","model":"TB320FC","manufacturer":"LENOVO","device":"TB320FC","product":"TB320FC_PRC","board":"kalama","hardware":"qcom","incremental":"V1","isAdbEnabled":true,"isDeviceRooted":false,"cpuAbi":"arm64-v8a","networkType":"WIFI","carrier":"","ipAddressIpv4":"","ipAddress":""}
  ```
- `apiKey`, `channelCode`
- `sessionKey`, `ciKey`（已登录时带上；登录前省略空值）

### 1.3 MD5 签名算法
1. 收集所有非空且 `k != "sig"` 的键值对；若 `len(v) > 50`（按字节/字符截断至前 50 字节：`v[:50]`），格式化为 `k=v`。
2. 按字典序（`sort.Strings`）升序排序后直接无分隔符拼接：`joined = strings.Join(items, "")`。
3. 计算 `md5_hex_lower(joined + signKey)`，写入参数 `sig`。

### 1.4 核心接口清单（响应格式 `{ "code": 0, "msg": "...", "data": ... }`）
| 接口路径 | 请求参数 | 核心返回字段 |
| --- | --- | --- |
| `v1/app/login/getQRCode` | `{}` | `codeId`, `buffer` (Base64 JPEG 二维码) |
| `v1/app/login/getQRCodeState` | `codeId` | `state`(0未扫/1待确认/2成功), `sessionKey`, `ciKey`, `secretKey` |
| `v1/app/user/getUserInfo` | `{}` | `userInfo.{userId, nickName, mobile}`, `powerInfo.{vipExpireTime, idleTime, hpPowerDuration, mpPowerDuration}`（**Duration 单位为秒，界面显示需 `÷ 60` 得分钟**） |
| `v1/app/game/list` | `channel="all"`, `categoryId=""`, `searchKey=""`, `page`, `size` | `list[]`, `count`（注意 `categories[].id` 可能为数字或字符串） |
| `v1/app/game/detail` | `gameId` | `gameInfo.{id, gid, gameName, gameFullName, coverUrl, description, categories}` |
| `v1/app/game/startGame` | `gameId` | `proData`, `platformInfo.{pkgName, userId}`, `gameName` |
| `v1/app/game/ytcx/stopGame` | `gameId`（可空，空=全释放） | 无（建议每次开新局前也预调一次防上次异常占用） |

- **本地持久化建议**（Switch SD 卡）：
  - 登录态与随机 `deviceId`：`sdmc:/switch/levelup-nx/config.json`
  - 最近游戏（按账号隔离，最多 12 条）：`sdmc:/switch/levelup-nx/recent_<userId|guest>.json`

---

## 2. YTCX 调度申请（`../go-client/internal/ytcx/union.go`）

### 2.1 常量与 URL
- `UnionApplyURL`: `https://union-access.yuntiancloud.com:18112/v3/union/apply`
- `UnionAccessKey`: `8f84a176b3edbe732f3126a9548ebf06`
- `UnionSecretKey`: `97c3cfb53bb18714566fd533dde8d415`
- `UnionAppID`: `mvp_114`
- `UnionBizID`: `biz-basic`
- `UnionSDKVersion`: `"7.40.47"` (`UnionSDKVersionNum`: `7040047`)

### 2.2 `DeviceInfo` / `StreamInfo` 与 `BuildLaunchGameProto`
- **`StreamInfo` JSON**：
  - `clientType`: `"switch-handheld"`
  - `connectionType`: `"webrtcVideo"`
  - `desiredWidth` / `desiredHeight`: 目标编码宽高（建议 Docked `1920x1080` / Handheld `1280x720`）
  - `decoderHeightLimited`: `2160`
  - `maxFps`: `60`
  - **`clientSupportHevc`: `0`**（强制云端编 H.264，对齐 Tegra NVDEC 硬解与已有测试）
  - `videoLevel`: `3`（UltraHigh；0=LOW, 1=MIDDLE, 2=HIGH, 3=UltraHigh, 4=Original）
  - `withHWCursor`: `0`, `sdkVersion`: `"7.40.47"`, `vibration`: `true`, `gid`: `pkgName`, `autoEncoding`: `1`
- **`CGReqLaunchGame` Protobuf（`BuildLaunchGameProto`）**：
  - `CommonHeader`：field 1 (`requestId`, bytes) + field 2 (`timestamp_ms`, varint) + field 3 (`""`, bytes)
  - 主体：field 1 (`header`, bytes)、field 2 (`pkgName`, varint)、field 3 (`proData`, bytes)、field 7 (`deviceInfo` JSON bytes)、field 8 (`streamInfo` JSON bytes)、field 9 (`1`, varint)

### 2.3 HMAC-SHA1 请求签名
- 请求体 `bodyBytes`：JSON 字段参见 `union.go` `applyBody`（含 `prelaunch_param = base64(pbLaunchGame)`、`clientDisplayWidth/Height`、`dev_type=2`、`os_type=1`、`account_auth_type=2`）。
- `bodyMD5 = base64(md5(bodyBytes))`
- Query 参数（9 个基础字段 + `Signature`）：`AccessKey`, `AppId`, `BizId`, `Format="JSON"`, `SignatureMethod="HMAC-SHA1"`, `SignatureNonce`, `SignatureVersion="1.1"`, `Timestamp(ms)`, `Version="2020-11-01"`
- `canonical`: 将 9 个参数按 key 字典序排列，每个做严格 RFC3986 percent-encoding（仅保留 `A-Za-z0-9_.-~`，`/` 也编码为 `%2F`），拼成 `k1=v1&k2=v2...`
- `stringToSign = "POST&" + quote("/v3/union/apply") + "&" + quote(bodyMD5) + "&" + quote(canonical)`
- `Signature = base64(HMAC_SHA1(key = UnionSecretKey + "&", msg = stringToSign))`
- 最终 URL Query 编码 `/` 不转义（保留安全字符 `_.-~/`），按 `AccessKey, AppId, BizId, Format, SignatureMethod, SignatureNonce, SignatureVersion, Timestamp, Version, Signature` 顺序拼接发出 POST（`User-Agent: okhttp/3.12.0`）。
- 解析返回 `body`：`flow_id`, `token`, `access_info.access_ipv4_address`, `access_info.access_ipv4_port`。
- 注意从 `token` 提取 `-` 分隔的第二段 `realUserID = split(token, '-')[1]`（若存在），供后续 `/launch` 与 WebRTC `uid` 使用。

---

## 3. BE WebSocket Protobuf 网关（`../go-client/internal/ytcx/proto.go` & `be.go`）

### 3.1 连接与外层帧封装 (`CGMessage`)
- URL：`ws://<AccessIP>:<AccessPort>/v1/endpoint?token=<token>&dev_type=2&dev_id=<deviceId>&conn_id=1_<ms>_99&conn_seq=1`
- **二进制帧结构（非标准全 protobuf 根消息，注意魔数前缀）**：
  - `[0]`：`0x0D`（field 1 fixed32 tag）
  - `[1..4]`：`u32le(cmdtype)`
  - `[5]`：`0x15`（field 2 fixed32 tag）
  - `[6..9]`：`u32le(39)`（version = 39）
  - `[10..]`：若 `payload` 非空，按 Protobuf field 3 (wire=2 bytes) 写入 `0x1A` + `varint(len(payload))` + `payload`
- `CommonHeader`：field 1 (`timestamp_ms`, varint) + field 2 (`token`, bytes) + field 3 (`requestId`, bytes)

### 3.2 关键命令与状态机时序
| Cmd | 名称 | 方向 | 触发时机与载荷要点 |
| --- | --- | --- | --- |
| `7` | `CGReqLaunchGame` | C → S | 建连后立刻发送 `BuildLaunchGameProto(...)` |
| `8` | `CGRspLaunchGame` | S → C | 检查是否包含 `"another game running"`，命中则发 `cmd=9` 并报错 |
| `17` | `CGNtfGameStatus` | S → C | 抽取 field 6 (`GameResource`)：field 1=`IPv4`, field 2×100=`BasePort`, field 7=`IPv6`, field 10/11/12=`aSessionId/vSessionId/cSessionId`；若 `gameStatus==7` 说明服务端已停流 |
| `5 / 6` | `CGReqHeartbeat / Rsp` | 双向 | 发 `cmd=7` 后立刻启动心跳协程（仅带 field 1=`CommonHeader`）；收到 `cmd=6` 读取 field 3 `period` (ms，限幅 2000..30000，默认 5000) 动态调整间隔。**不发会导致 ~80 秒断连** |
| **`37 / 38`** | **`CGCNtfStreamStatus / Cfm`** | **C → S → C** | **首帧视频成功解码出图后必须立刻发送 `cmd=37`（field 1=`CommonHeader`, field 2=`1` varint）！不发会在 ~60 秒收到 `close 4000: Start failure` 并被强踢** |
| `33 / 34` | `CGReqAllocVPad / Rsp` | C → S → C | 边缘 `/launch` 后延迟 ~8 秒发送：field 1=`CommonHeader`, field 2=`65535` (自动分配 `vpadId`), `usage(field 5)=2`；从 `cmd=34` field 3 取 `vpadID` 填入手柄控制包 `controller` |
| `43` | `CGReqRecycleVPad` | C → S | 退出前释放：field 1=`CommonHeader`, field 2=`vpadID` |
| `9` | `CGReqExitGame` | C → S | 退出前释放容器：field 1=`CommonHeader`, field 2=`pkgName` |

---

## 4. 边缘节点 `/launch`（`../go-client/internal/ytcx/launch.go`）

- 请求：`POST https://<EdgeIP>:<BasePort+4>/launch`（边缘节点使用自签 TLS 证书，须跳过证书校验 `CURLOPT_SSL_VERIFYPEER=0, CURLOPT_SSL_VERIFYHOST=0`）
- 关键入参字段：
  - `gid=pkgName`, `userId=realUserID`, `flowId`, `aSessionId`, `vSessionId`, `cSessionId`, `deviceId`
  - `desiredWidth`, `desiredHeight`, `decoderHeightLimited=2160`, `maxFps=60`
  - `videoLevel=3`, `bitrateGuidanceAdjustMode=1` (`VQ_FIRST`), `clientSupportHevc=0`, `clientType="switch-handheld"`
  - `supportVideoCodecProfiles=[4096, 8192, 1, 2, 4]`, `supportCodecs=[0, 1]`, `enableSEIInfo=1`
- 返回关键字段：`webrtcSignalingPort`, `encodedWidth`, `encodedHeight`, `remoteAddr`, `supportInputLabelList`。

---

## 5. WebRTC 信令、媒体与避坑铁律（`../go-client/internal/rtc/rtc.go` & `video/h264.go`）

### 5.1 信令 WebSocket（`wss://<EdgeIP>:<webrtcSignalingPort>/`，跳过自签 TLS 校验）
1. 建连后立刻发送 `join`：
   ```json
   {
     "uid": "<realUserID>_<aSessionId>_<vSessionId>_<cSessionId>",
     "join": "{}",
     "aSessionId": "...", "vSessionId": "...", "cSessionId": "...",
     "token": "", "disableEncryption": false, "clientSupportHevc": 0
   }
   ```
2. 收到 `"type":"onOffer"`（保存报文里的 `"peer"`）：
   - 对远端 SDP 调用 `SetRemoteDescription(offer)`；
   - 生成 Answer SDP（保留视频 H.264 能力 `level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=640032`，即 High Profile + CABAC）；
   - **必须发送嵌套结构**（扁平 `type/sdp` 服务端不认）：
     ```json
     {
       "uid": "<uid>",
       "peer": "<peer>",
       "token": "",
       "answer": { "type": "answer", "sdp": "<answer_sdp>" }
     }
     ```
3. **ICE 角色与 Trickle Candidate**：
   - 本端是 Answerer，**ICE 角色必须保持 `controlled`**（切勿抢发 `USE-CANDIDATE`）。
   - 使用与桌面版相同的 STUN 列表辅助打洞（如局域网/运营商环境需要）：
     `stun:111.206.174.2:3478`、`stun:stun.hitv.com:3478`、`stun:stun.chat.bilibili.com:3478`。
   - **每个本端 Candidate 必须对 3 个 m-line（`0`, `1`, `2`）各发一遍**（只发 `0` 时服务端 DTLS 不启动）：
     ```json
     {
       "uid": "<uid>", "peer": "<peer>", "token": "",
       "candidate": {
         "sdpMLineIndex": 0, // 分别发 0, 1, 2
         "sdpMid": "0",      // 分别发 "0", "1", "2"
         "candidate": "candidate:..."
       }
     }
     ```
4. **WS 保活**：每 20 秒发一次 WebSocket Ping 帧。

### 5.2 H.264 RTP 重组与关键帧请求铁律
- **不要硬跳过 12 字节**：服务端 RTP 包带扩展头，必须完整解析标准 RTP Header + CSRC + RFC 3550 Extension 头后取负载。
- **RTP 重组规则（Annex-B 输出）**：
  - 处理 Single NALU (`1..23`)、STAP-A (`24`)、FU-A (`28`)；**每个 NALU（包括 FU-A 首片和 SPS/PPS）前缀必须写入 `00 00 00 01`**。
  - **严格 seq 连续与过期包过滤**：实测云端长稳串流时即使静态跳帧也**只跳 timestamp、保持 `seq` 严格 +1 连续**。重排缓冲必须留足 1080p60 大帧余量（建议上限 `>= 256` 包、等待 `~80ms`），并用有符号环形差 `int16(seq - expect) < 0` 直接丢弃过期迟到包防止 65535 绕回；一旦确认 seq 缺口（含整帧丢失）或 `ts` 切换/`marker` 时 `fuOpen` 未闭合，必须立即标记参考链断裂 (`needKey`) 并显式上报丢帧、暂停向解码器喂 P 帧。
  - 纯参数集帧（仅 SPS/PPS/SEI，无 VCL 1..5）缓存在前缀，拼到下一个 VCL 帧之前一起送入硬解。
  - 缺片或解码错误后进入 `needKey` 丢弃后续 P 帧直到收到完整 IDR：**切回 IDR 喂帧前先 `Flush` 解码器清空 DPB 残留脏帧，并确保前端带完整 SPS+PPS+IDR**。
- **开启 NACK + 全自动零打扰自愈关键帧闭环**：
  - SDP 协商视频必须启用 `nack`、`nack pli`、`ccm fir`，优先靠底层 NACK 重传毫秒级补回偶发丢包。
  - 首帧解码出图前（最长前 8 秒）：每 500ms 发一次 `PLI + FIR` 加速出图。
  - **首帧一旦成功解码出图：彻底停止短周期 `PLI/FIR`（避免 2 秒定期降码率模糊）**；由程序后台全程全自动触发以下三层闭环自愈（无需任何人工按键刷新）：
    1. **WebRTC 底层丢包闭环**：每秒检查 `InboundRTPStreamStats.PacketsLost` 增量，一旦有丢包增量立刻静默补发一次 `PLI + FIR`（不冻结画面、不断 P 帧）。
    2. **无限 GOP 长时间防漂移静默保鲜**：持续收流期间若超过 **30 秒**未收到自然 IDR，静默请求一帧新 IDR 刷新参考链（`<0.3%` 码率开销，无停顿/模糊）。
    3. **硬解异常自动重置**：解码连续无输出超过阈值（`>= 20` 帧）或连续错误时，自动执行解码器 `Flush` / 重建并进入 `needKey` 重试（后台每秒巡检兜底，即使断流暂停也不卡死）。

> 说明：`go-client/third_party/srtp` 仅为绕过 Go 1.26 Windows `runtime.sellock` 崩溃所做的轮询补丁，并非自定义加密协议。Switch 端直接编译标准 `libsrtp` 即可 100% 互通。

---

## 6. 控制 DataChannel 与 Switch 手柄控制包（`../go-client/internal/input/input.go`）

### 6.1 `control` 通道握手与保活
- `control` 通道由**服务端创建**（OnDataChannel callback 时务必检查 `dc->isOpen()`，防止 open 竞态导致漏握手）。
- 通道一旦 open，立刻依次发送：
  1. `HandshakeSession(cSessionId)`：`u16le(1792)` + `u16le(len)` + `ASCII(cSessionId)`
  2. `Handshake773()`：固定 6 字节 `05 03 02 00 00 00`
  3. `Handshake775()`：固定 5 字节 `07 03 01 00 00`
  4. `BitrateMode(VQ_FIRST=true)`：外层包头 `type=1025` + 5 字节 inner `00 00 00 49 01`（保画质，防止低运动降码率模糊）
- **周期心跳 `type=1537`**：
  - 每 1 秒发一次：`type=1537` + 8 字节 inner `u64le(uptime_ms)`。
  - 服务端会原样把这 16 字节包回显过来（头两字节 `01 06`），**收到后仅可统计 RTT，绝不能再回发**。

### 6.2 外层统一封包头（8 字节）
```
offset 0..1 : u16le type        (518=控制输入, 1025=指令/光标/画质, 1537=DC心跳)
offset 2..3 : u16le innerLen+4
offset 4..7 : u32le innerLen
offset 8..  : inner[innerLen]
```

### 6.3 46 字节手柄控制包（`type = 518`, `innerLen = 46`）
> **Switch 开发者重大利好**：YTCX 会话本身就是 `clientType = "switch-handheld"`（Nintendo 布局：A 在右、B 在下、X 在上、Y 在左）。Switch 原生 Joy-Con / Pro 手柄物理按键布局与云端语义**天生一致**，直接按功能语义映射，**不要做 Xbox→Nintendo 互换**！

| Inner 偏移 | 长度 | 值 / 含义 | 说明 |
| --- | ---: | --- | --- |
| `0..3` | 4 | `00 00 00 1E` | 固定魔数/子包类型 30 (BE) |
| `4..7` | 4 | `0C 00 00 00` | `12` (LE u32) |
| `8..9` | 2 | `1A 00` | `26` (LE u16) |
| `10..11` | 2 | `u16le(controller)` | 填 BE `cmd=34` 返回的 **`vpadID`**（未拿到前默认 `1`） |
| `12..13` | 2 | `01 00` | `activeGamepadMask = 1` (LE u16) |
| `14..15` | 2 | `14 00` | `20` (LE u16) |
| **`16..17`** | 2 | **`u16le(buttons)`** | 手柄按键位掩码，见下表 |
| `18` | 1 | `lt` (`0..255`) | 左扳机（Switch ZL 按下=255，松开=0） |
| `19` | 1 | `rt` (`0..255`) | 右扳机（Switch ZR 按下=255，松开=0） |
| `20..21` | 2 | `i16le(lx)` | 左摇杆 X (`-32768..32767`) |
| `22..23` | 2 | `i16le(ly)` | 左摇杆 Y（注意核对 Y 轴方向：libnx 向上为正，若游戏中上下反则取 `-stick.y`） |
| `24..25` | 2 | `i16le(rx)` | 右摇杆 X (`-32768..32767`) |
| `26..27` | 2 | `i16le(ry)` | 右摇杆 Y |
| `28..31` | 4 | `9C 00 00 00` | `156` (LE u32) |
| `32..33` | 2 | `55 00` | `85` (LE u16) |
| `34..37` | 4 | `u32le(seq)` | 递增包序号 |
| `38..45` | 8 | `u64le(ts_ms)` | 发送时 Unix 毫秒时间戳 |

#### `buttons` 位掩码与 `libnx HidNpadButton` 映射表
| Bit 掩码 | YTCX 语义 | `libnx` Switch 按键 |
| --- | --- | --- |
| `0x0001` | D-Pad Up | `HidNpadButton_Up` |
| `0x0002` | D-Pad Down | `HidNpadButton_Down` |
| `0x0004` | D-Pad Left | `HidNpadButton_Left` |
| `0x0008` | D-Pad Right | `HidNpadButton_Right` |
| `0x0010` | Start / `+` | `HidNpadButton_Plus` |
| `0x0020` | Back / `-` | `HidNpadButton_Minus` |
| `0x0040` | L3 (左摇杆按压) | `HidNpadButton_StickL` |
| `0x0080` | R3 (右摇杆按压) | `HidNpadButton_StickR` |
| `0x0100` | LB | `HidNpadButton_L` |
| `0x0200` | RB | `HidNpadButton_R` |
| `0x1000` | **A（Switch 语义：右）** | `HidNpadButton_A` |
| `0x2000` | **B（Switch 语义：下）** | `HidNpadButton_B` |
| `0x4000` | **X（Switch 语义：上）** | `HidNpadButton_X` |
| `0x8000` | **Y（Switch 语义：左）** | `HidNpadButton_Y` |

### 6.4 下行手柄震动包（`type = 518`, `sub = 31`）
云端通过 `control` DataChannel 下发 14 字节 LE 震动指令（频率可达 ~200Hz）：

| 字节偏移 | 长度 | 值 / 含义 | 说明 |
| --- | ---: | --- | --- |
| `0..1` | 2 | `06 02` | `u16le(518)` (`TypeControl`) |
| `2..3` | 2 | `0A 00` | `u16le(10)`（inner+4，即后续 10 字节） |
| `4..7` | 4 | `1F 00 00 00` | `u32le(31)`（子命令：手柄震动反馈） |
| `8..9` | 2 | `u16le(controller)` | 云端 1-based 手柄编号（通常 `1`，映射至本机主控手柄槽位 `0`） |
| `10..11` | 2 | `u16le(lowFreq)` | 低频马达强度（云端实测 `0..255`，映射到标准区间时需按比例放大） |
| `12..13` | 2 | `u16le(highFreq)` | 高频马达强度（`0..255`） |

- **实现建议**：Switch `hidSendVibrationValues` 请做异步合批限频（~60Hz）后再投递、不要同步阻塞 DataChannel 收包线程；收到 `(0, 0)` 立即停震，并加 2~3 秒无新震动包自动归零兜底与退出停震。
