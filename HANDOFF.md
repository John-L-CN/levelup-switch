# LevelUp-NX Switch 1 客户端交接文档（Handoff）

> 更新时间：2026-11-24 21:0x | 基线 Git 提交：`fa57eff`（`origin/main` 同步），其上已叠加「音频 Track 保活」修复（待真机复测后提交）  
> 目标设备：Nintendo Switch 1（Horizon OS 21.1.0 / Atmosphere 1.10.2 E）

---

## 1. 项目与开发环境概览

- **本地工作区**：`e:\AICode\lianzidashi\levelup\Switch`
- **GitHub 仓库**：`https://github.com/John-L-CN/levelup-switch`
- **同级参考实现**：`../go-client/`（PC 端全链路已实测跑通的纯 Go 客户端，核心 WebRTC 参考见 `../go-client/internal/rtc/rtc.go` 与 `../go-client/internal/session/session.go`）
- **核心设计文档**：
  - `PROMPT.md`（里程碑与避坑铁律）
  - `PROTOCOL_SPEC.md`（HTTP / YTCX union / BE Protobuf / `/launch` / WebRTC / 46 字节手柄与震动规格）
  - `ARCHITECTURE_MOONLIGHT_SHELL.md`（Moonlight 保留/剥离架构与 `libdatachannel` Switch 1 约束）
- **远程交叉编译主机**：`ssh ub`（目录 `~/levelup-switch`）
  - Docker 编译镜像：`levelup-switch-devkita64:latest`（含 `libnx 4.12.0-1`）
  - 预编译依赖前缀：`~/levelup-switch/build/deps/switch/`
  - 依赖源码：`~/levelup-switch/build/deps/src/{mbedtls,libdatachannel}`
- **自动化脚本**：
  - 同步 + 宿主单测 + Switch 编译 + ELF 检查 + 拉回 `.nro`：
    `C:\Users\Administrator\.gemini\antigravity-cli\brain\97afa178-3b1c-406b-aa7d-986da643589c\scratch\ub_sync_and_run.py`（支持 `sync`、`verify-m5` 等子命令）
  - 隐私脱敏审计：
    `C:\Users\Administrator\.gemini\antigravity-cli\brain\97afa178-3b1c-406b-aa7d-986da643589c\scratch\privacy_audit.py`
- **构建产物与真机日志路径**：
  - 本地 `.nro` 产物：`build/switch/LevelUp-NX.nro`
  - 真机配置目录：`sdmc:/switch/levelup-nx/`
  - 真机运行日志：`sdmc:/switch/levelup-nx/levelup.log`（需在 Title Override / `appletType=0` 全内存模式运行）

---

## 2. 已完成并实机验证的进度

1. **修复启动崩溃（Commit `f64ce7f`）**
   - 解决了 Switch 1 HOS 21.1.0 首帧 deko3d 动态命令缓冲（`0x20000`）渲染 238 个游戏卡片溢出崩溃问题，改为 `CulledGridBox` 行裁剪 + 24/页分页 + 子线程 `stb_image` 缩图，启用线程安全日志与 `curl_global_init(CURL_GLOBAL_ALL)`，并添加 `[BOOT] 1..7` 启动轨迹与异常兜底。
   - **真机已验证**：~0.5–0.77s 进入主界面、扫码登录成功、游戏列表/分类/详情/选服流程全部正常。

2. **打通 WebRTC 信令、STUN 与 ICE 连通（Commit `fa57eff`）**
   - 修复了 Switch Horizon OS（`libnx`）`getnameinfo()` 忽略 `NI_NUMERICHOST` 以及 `AI_ADDRCONFIG` / IPv6 双栈导致 ICE 候选解析失败的问题：
     - `scripts/switch1/patch_webrtc_deps.py` + `scripts/build_switch1_deps.sh`：为 `libdatachannel`（`src/candidate.cpp`, `src/impl/tcptransport.cpp`）和 `libjuice`（`src/udp.c`, `src/addr.c`, `src/ice.c`, `src/conn_mux.c`）全量替换为直接 `inet_pton`/`inet_ntop` 与纯 IPv4 绑定，并加入了 `nm -u` 编译门禁确保 `libdatachannel.a` / `libjuice.a` 对 `getnameinfo` 零未定义引用。
     - `app/src/levelup/rtc_session.cpp`：接入 `rtc::InitLogger`，增加 SDP Answer/候选/首包 RTP 诊断与本地候选重发。
     - `app/src/levelup/session_orchestrator.cpp`：修复 `m_vpadAllocThread` 捕获局部 lambda 引用隐患，并增加首帧出图后的 `CGReqAllocVPad` 重试。
     - `app/src/levelup/api_client.cpp`：缓存 `g_preferInsecureTls`，减少重复 TLS 握手重试耗时。
   - **真机最新实测结果（基于 `fa57eff` 日志）**：
     `startGame` → `unionApply` → BE WebSocket + 心跳 + `cmd=17` → `/launch` → Audren 48kHz 初始化 → 边缘 `wss://` TLS 握手 → 收到 `onOffer` → 采集到 host 与 STUN `srflx` 候选（`resolved=1`）→ **约 0.25 秒完成 `[RTC] ICE state: connected` 并触发 `PeerConnection::initDtlsTransport`**！

---

## 3. 阻塞点与根因（两个都已定位，第一个已真机验证修好）

### 3.1 音频 Track 弱引用 → Answer 把 mid=0 踢出 BUNDLE（已修，真机验证通过）
**现象（`fa57eff`）**：ICE `connected` 后界面卡「等待云端首帧 H.264 解码...」，`PC state` 停在 `connecting`，DTLS 不完成、DataChannel 不开、无 RTP。

**根因（日志 + `libdatachannel/src/impl/peerconnection.cpp:1010-1034` 互证）**：

1. `PeerConnection::mTracks` 只存 **`std::weak_ptr<rtc::Track>`**，用户 `onTrack` 回调不保留 `shared_ptr` 时 Track 立刻析构。
2. [app/src/levelup/rtc_session.cpp](app/src/levelup/rtc_session.cpp) 里 `video` 分支保存了 `m_impl->videoTrack`，**`audio` 分支什么都没保存**。
3. 于是 `mid=0` 的 audio Track 在回调返回时销毁；`setLocalDescription(Answer)` 的 `populateLocalDescription()` 拿到 `mTracks["0"].lock() == nullptr` → 把 mid=0 标记 `removed`。
4. 错误 Answer：`a=group:BUNDLE 1 2` + `m=audio 0 ...`。云端以 mid=0 为 BUNDLE 主 tag、所有候选都挂 mid=0 → 匹配不到传输，永不回 DTLS ServerHello。

**修复（`8c4387b`）**：Impl 持有 `audioTrack` / `audioRtcpHandler`，`close()` 一并释放；另加 `auditAnswerBundle()` 自检 + Track `onOpen/onError` 日志 + `[libRTC]` Debug 级日志 + Offer 摘要日志。

**真机验证（21:02 日志）**：
```
[libRTC] populateLocalDescription@1018: Adding media to local description, mid="0", removed=false
[libRTC] populateLocalDescription@1018: Adding media to local description, mid="1", removed=false
[RTC] SDP a=group:BUNDLE 0 1 2
[RTC] SDP m=audio 9 UDP/TLS/RTP/SAVPF 111 106 ...
[libRTC] DtlsTransport::doRecv@565: DTLS handshake finished      <- 30.158
```
即 **DTLS 在 ICE connected 后 ~0.14 秒握手完成**，第一阶段根因确认修复。

### 3.2 `struct sockaddr_conn` 布局与 libnx BSD sockaddr 不匹配 → `usrsctp_bind` 失败 → `std::terminate`（本轮修复）
**现象（同一份 21:02 日志，DTLS 之后 0.1 秒）**：
```
[libRTC] SctpTransport::connect@358: SCTP connecting (local port=5000, remote port=5000)
[libRTC] SctpTransport::~SctpTransport@318: Destroying SCTP transport
[libRTC] DtlsTransport::stop@469: Stopping DTLS transport
[ERROR] [libRTC] PeerConnection::initSctpTransport@360: Could not bind usrsctp socket, errno=106
[libRTC] PeerConnection::changeState@1381: Changed state to failed
[FATAL] std::terminate invoked      <- 进程被杀 = 用户看到的「软件已关闭」
```
`initSctpTransport` 的 catch 分支在 `changeState(Failed)` 之后 **rethrow**，异常从 libdatachannel 内部线程抛出 → 未捕获 → `std::terminate`。

**100% 确认的根因（三处源码 + 头文件互证）**：

1. `errno=106` 在 **newlib（devkitA64）** 不是 EISCONN，而是 **`EAFNOSUPPORT`**（`sys/errno.h:106`；newlib 的 EISCONN 是 127、EADDRINUSE 是 112、ECONNREFUSED 是 111 —— 全都不等 106）。
2. `EAFNOSUPPORT` 只能来自 `usrsctp` 的 `sobind()` 的 `default:` 分支：它按 `nam->sa_family` 分派 `AF_INET / AF_INET6 / AF_CONN`。libnx 的 `struct sockaddr` 是 **BSD 布局**（`sa_len` 在偏移 0、`sa_family` 在偏移 1，`/opt/devkitpro/libnx/include/sys/socket.h:289`），usrsctp 的 CMake 也因此探测出 `HAVE_SA_LEN` 并编译进去。
3. 但 `usrsctp.h` 里 `struct sockaddr_conn` 只有 **Apple/BSD 平台宏** 才用 `{sconn_len, sconn_family, ...}` 布局，`__SWITCH__` 不在名单里 → Switch 用的是 `{uint16_t sconn_family, ...}`。libdatachannel 写 `family=AF_CONN(123)` 落在偏移 0，usrsctplib 从偏移 1 读 `sa_family` → 读到 **0** → `default:` → `EAFNOSUPPORT`。

**修复**：`scripts/switch1/patch_webrtc_deps.py` 的 `patch_usrsctp()` 增加一条幂等补丁，把 `__SWITCH__` 加入 `usrsctp.h` 的 BSD 布局分支（`sconn_len` 由 usrsctp 自己的 `getsockaddr()` 写成 `namelen`，无需 libdatachannel 定义 `HAVE_SCONN_LEN`）。随后必须 **重跑 `scripts/build_switch1_deps.sh`** 重建 `libusrsctp.a` + `libdatachannel.a`（头文件布局变了，两个 TU 都要重编）。

（附注：日志里 `remote candidate #N ... resolved=0` 已改为 `applied`，那个 0 只是 `rtc::Candidate(cs, mid)` 构造后的初始值，不代表解析失败；ICE `completed` 已证伪该怀疑。）


---

## 4. 下一步立即执行计划

### Step 1：修复 [app/src/levelup/rtc_session.cpp](app/src/levelup/rtc_session.cpp) 并提升 [libRTC] 日志级别
状态：**已完成（2026-11-24 21:00，本地工作区改动，已编译通过）**

1. 在 `RtcSession::Impl`（`trackMu` 保护下）新增成员：
   - `std::shared_ptr<rtc::Track> audioTrack;`
   - `std::shared_ptr<rtc::RtcpReceivingSession> audioRtcpHandler;`
   - 另加只读访问器 `getAudioTrack()`（便于后续排查/未来音频统计）
2. 在 `pc->onTrack` 的 `kind == "audio"` 分支中，将 `track` 与 `audioRtcp` 存入 `m_impl->audioTrack` 和 `m_impl->audioRtcpHandler`；在 `RtcSession::close()` 的 `trackMu` 锁内一并 `.reset()`。
3. `initRtcLoggingOnce()` 日志级别 `rtc::LogLevel::Info` → `rtc::LogLevel::Debug`（`Verbose` 才是按包日志，`Debug` 可完整看到 `Starting DTLS transport` / `DTLS handshake finished` / `SctpTransport` / `Track::open`，不刷屏）。
4. `handleOffer()` 追加远端 Offer 摘要（`m=` / `a=group:` / `a=mid:` / `a=setup:` / `a=fingerprint:` / `a=ice-ufrag:`），并打印 `remote offer applied: m-lines=N candidates=N`；远端候选日志改为先 `addRemoteCandidate` 再打印 `applied`，去掉误导性的 `resolved=0`（该值在 add 之前恒为 0）。
5. 额外加固（本次新增）：
   - `auditAnswerBundle()`：本地 Answer 生成后立刻自检——若出现 `m=audio 0` / `m=video 0`，或 `a=group:BUNDLE` 漏掉任一 `a=mid`，直接 `logError` 报警（同类回归不会再静默）。
   - video / audio Track 增加 `onOpen` / `onError` 日志（`[RTC] video|audio track open mid=N`），用于确认 DTLS 真正建立。

### Step 2：增量编译与交付复测
状态：**已完成（2026-11-24 21:00）**，只改应用层 C++，未重跑 `build_switch1_deps.sh`：

```powershell
python "C:\Users\Administrator\.gemini\antigravity-cli\brain\97afa178-3b1c-406b-aa7d-986da643589c\scratch\ub_sync_and_run.py" verify-m5
```

- 结果：宿主单测（M2/M3/M4/M5）全绿 → Docker 交叉编译 `[100%] Built target LevelUp-NX.nro`（20,106,203 字节，`NRO0` 校验通过）→ ELF 符号门禁 `ELF symbol check PASSED`（含 `dtls`/`audren`/`hid*`/`Curl_ssl_libnx` 等，且无 MbedTLS 2.x 遗留符号）→ 已拉回 `build/switch/LevelUp-NX.nro`。
- `privacy_audit.py`：`Privacy audit PASSED (0 hits)`。

### Step 3：重建设备端依赖（Stage 2 修复必需）
状态：**已完成（2026-11-24 21:16）**

```powershell
ssh ub "cd ~/levelup-switch && bash scripts/build_switch1_deps.sh"
```

- 补丁落地证据：`~/levelup-switch/build/deps/src/libdatachannel/deps/usrsctp/usrsctplib/usrsctp.h` 的 `struct sockaddr_conn` 分支已含 `defined(__SWITCH__)`。
- 依赖产物全部重编：`libusrsctp.a`（867K）、`libdatachannel.a`（7.9M）、`libjuice.a`、`libsrtp2.a` 均为 21:13 时间戳。
- 注意：改的虽是 deps，仍需同步 `scripts/` 到 ub（`ub_sync_and_run.py sync`），否则 ub 上的补丁脚本还是旧版，会漏掉 `usrsctp.h` 这条。

### Step 4：真机复测（下一步唯一的动作）
把 `build/switch/LevelUp-NX.nro`（2026-11-24 21:16:23 构建，SHA256 `9FAB25A472B8EDF71CE3611AD8ACDA23FB8499664488E4BBB898D1918C3DB835`）拷到 `sdmc:/switch/LevelUp-NX.nro`，在 Title Override / `appletType=0` 全内存模式下进一次游戏，取回 `sdmc:/switch/levelup-nx/levelup.log`，按第 5 节 checklist 对照。

---

## 5. 复测预期与后续排查 Checklist

- **预期正常日志链路**：
  1. `[RTC] Offer m=audio ...` / `[RTC] Offer m=video ...` / `[RTC] Offer a=group:BUNDLE 0 1 2` / `[RTC] remote offer applied: m-lines=3 candidates=N`。
  2. SDP Answer 输出 **`a=group:BUNDLE 0 1 2`** 且 **`m=audio 9 UDP/TLS/RTP/SAVPF ...`**（不再出现 `m=audio 0`，且 `auditAnswerBundle()` 不打印任何 `Answer rejected` / `Answer BUNDLE omits` 错误行）。
  3. `[RTC] ICE state: connected` 之后出现 `[libRTC] ... Starting DTLS transport` → `[libRTC] ... DTLS handshake finished` → `[RTC] video track open mid=1` / `[RTC] audio track open mid=0` → `[RTC] PC state: connected`。
  4. **（Stage 2 新增关键位）**`[libRTC] SctpTransport::connect@358: SCTP connecting (local port=5000, remote port=5000)` 之后**不能**出现 `Could not bind usrsctp socket` / `Could not create SCTP socket` / `SCTP transport initialization failed` / `[FATAL] std::terminate`；应出现 `SctpTransport` 关联建立 → `[RTC] DataChannel: control` → `[RTC] control DataChannel open` → `[RTC] control handshake sent (VQ_FIRST=1 ok=1)` → `[RTC] DC 1537 first echo RTT=...ms`。
  5. 触发 `[RTC] first video RTP ...` 与 `[RTC] first audio RTP ...` → 首个 IDR 解码出图 → `[Session] sent BE cmd=37 CGCNtfStreamStatus(playing=1)` → 2s 后 `[Session] vPad allocated`。
- **若 DTLS 通了但 SCTP 仍失败**：
  1. 先看 errno 数值：newlib 下 `106=EAFNOSUPPORT`、`112=EADDRINUSE`、`127=EISCONN`、`119=EINPROGRESS`、`105=ENOBUFS`（与 Linux 完全不同，别照 Linux 语义猜）。
  2. `EAFNOSUPPORT` = sockaddr 家族没对上（布局问题，见 3.2）；`EADDRINUSE` = `usrsctp_init(0, ...)` 被调了两次（多次 `RtcSession::start` 时检查 `SctpTransport::Start()` 的全局 init 守卫）。
  3. 二次确认头文件是否真的替换：`grep -A4 'defined(__SWITCH__)' ~/levelup-switch/build/deps/src/libdatachannel/deps/usrsctp/usrsctplib/usrsctp.h`。
- **若 DTLS 没握手成功**：
  1. 查看 `[libRTC]` `Debug` 日志，确认 `Starting DTLS transport` 是否触发以及是否有 TLS alert / certificate 错误。
  2. 检查 `~/levelup-switch/build/deps/src/libdatachannel/src/impl/dtlstransport.cpp`（MbedTLS `WriteCallback` / `ReadCallback` / `DoRecv` 定时器重传），必要时对比 `../go-client/internal/rtc/rtc.go` 检查 Offer/Answer 的 `a=setup:active`、SRTP profile 或证书类型（ECDSA P-256 vs RSA）。
- **画面与控制联调验证点**（出图后逐项检查）：
  - Tegra X1 NVDEC 硬解与 deko3d 渲染裁剪（`1920x1088` -> `1920x1080`，防止底部 8px 绿条：[app/src/levelup/video_pipeline.cpp](app/src/levelup/video_pipeline.cpp)、[app/src/levelup/ui/levelup_stream_view.cpp](app/src/levelup/ui/levelup_stream_view.cpp)）。
  - 音频 Audren 48kHz Opus 播放（[app/src/levelup/audio_player.cpp](app/src/levelup/audio_player.cpp)）。
  - Switch 原生按键语义 46 字节包透传与 `vPad` 分配、HD Rumble 震动（[app/src/levelup/switch_input_bridge.cpp](app/src/levelup/switch_input_bridge.cpp)、[app/src/levelup/rumble_switch.cpp](app/src/levelup/rumble_switch.cpp)）。
  - 次要优化（非阻塞）：`libjuice` 当前打印 `juice: Falling back on rand() for random bytes`，后续可在 `patch_webrtc_deps.py` 中为 `deps/libjuice/src/random.c` 接入 Horizon `csrngGetRandomBytes`。

---

## 6. 工程红线与历史真机日志存档

- **严禁提交个人凭据/隐私**：每次提交前必须跑 `privacy_audit.py` 确认 `0 hits`（禁止在源码、注释、文档或 commit 中写入账号/UID/Token/真实 IP）。
- **严禁修改 `extern/borealis` 子模块**；构建仅更新应用层及 `scripts/switch1/`。
- **PowerShell + SSH 注意事项**：避免直接用 PowerShell 管道传脚本给 `ssh ub bash -s`（会带 UTF-8 BOM），优先用 `ub_sync_and_run.py` 或单行 `ssh ub "..."`。
- **历史真机日志路径（仅供本地排障参考，勿写入 git）**：
  - 修复 ICE 前（卡 `ICE state: checking`）：`C:\Users\Administrator\.gemini\antigravity-cli\brain\97afa178-3b1c-406b-aa7d-986da643589c\.user_uploaded\media_1795522523475_9b85dc28.log`
  - `fa57eff` 实测（ICE 已 `connected`，暴露 `m=audio 0` / `BUNDLE 1 2`）：`C:\Users\Administrator\.gemini\antigravity-cli\brain\97afa178-3b1c-406b-aa7d-986da643589c\.user_uploaded\media_1795524219325_00274991.log`
