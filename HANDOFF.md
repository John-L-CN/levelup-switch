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

## 3. 当前唯一阻塞点与已确认 100% 根因

### 现象
ICE 已成功进入 `connected`，但界面仍停留在「等待云端首帧 H.264 解码...」：
- `PC state` 停在 `connecting`，DTLS 未完成、`DataChannel: control` 未打开、未收到音视频 RTP 包（约 1.6s 后 BE 网关下发 `cmd=17 gameStatus=17`，约 52s 后退出）。

### 100% 确认的根因（日志 + `libdatachannel` 源码互证）
对照最新真机日志与 `~/levelup-switch/build/deps/src/libdatachannel/src/impl/peerconnection.cpp:1010-1034`：

1. `libdatachannel` 的 `PeerConnection::mTracks` 只保存 **`std::weak_ptr<rtc::Track>`**；如果用户回调 `onTrack` 没有保留 `std::shared_ptr<rtc::Track>`，回调返回后 Track 对象会被立刻析构。
2. 查看 [app/src/levelup/rtc_session.cpp](app/src/levelup/rtc_session.cpp)：
   - `kind == "video"` 分支把 `track` 保存到了 `m_impl->videoTrack`；
   - **`kind == "audio"` 分支完全没有保存 `track`（也没有保存 `audioRtcp`）**！
3. 因此 `mid=0` 的 `audio` Track 在 `pc->onTrack` 返回时即刻销毁；紧接着 `pc->setLocalDescription(Answer)` 调用 `populateLocalDescription()`，查到 `mTracks["0"].lock() == nullptr`，于是自动将 `mid=0` 标记为 `removed`！
4. 最新日志实锤打印出了错误 SDP Answer（第 85–87 行）：
   - **`a=group:BUNDLE 1 2`**（把 `mid=0` 踢出了 BUNDLE！）
   - **`m=audio 0 UDP/TLS/RTP/SAVPF ...`**（端口置 0 拒绝了音频，同时把 `m=video 9` 变成了唯一残留媒体）
5. 云端（Pion/SRS 类服务端）的 Offer 以 `mid=0`（audio）为 BUNDLE 主 tag，且所有候选全部走 `mid=0`。收到 `m=audio 0` + `BUNDLE 1 2` 后服务端无法匹配传输，**不会发回 DTLS ServerHello / 媒体与 DataChannel**，导致客户端 MbedTLS DTLS（`a=setup:active`）一直卡在 `MBEDTLS_ERR_SSL_WANT_READ`。

（附注：日志里 `remote candidate #1/#2 resolved=0` 是 `rtc_session.cpp` 里 `rtc::Candidate(cs, mid)` 构造时的初始 `resolved` 日志——此时还未调用 `pc->addRemoteCandidate(rcand)` 触发解析；ICE 已连通说明远端候选在 `libjuice` 中实际解析工作正常，无需再怀疑 `getnameinfo`。）

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

### Step 3：真机复测（下一步唯一的动作）
把 `build/switch/LevelUp-NX.nro` 拷到 `sdmc:/switch/LevelUp-NX.nro`，在 Title Override / `appletType=0` 全内存模式下进一次游戏，取回 `sdmc:/switch/levelup-nx/levelup.log`，按第 5 节 checklist 对照。

---

## 5. 复测预期与后续排查 Checklist

- **预期正常日志链路**：
  1. `[RTC] Offer m=audio ...` / `[RTC] Offer m=video ...` / `[RTC] Offer a=group:BUNDLE 0 1 2` / `[RTC] remote offer applied: m-lines=3 candidates=N`。
  2. SDP Answer 输出 **`a=group:BUNDLE 0 1 2`** 且 **`m=audio 9 UDP/TLS/RTP/SAVPF ...`**（不再出现 `m=audio 0`，且 `auditAnswerBundle()` 不打印任何 `Answer rejected` / `Answer BUNDLE omits` 错误行）。
  3. `[RTC] ICE state: connected` 之后出现 `[libRTC] ... Starting DTLS transport` → `[libRTC] ... DTLS handshake finished` → `[RTC] video track open mid=1` / `[RTC] audio track open mid=0` → `[RTC] PC state: connected`。
  4. 触发 `[RTC] DataChannel: control` → `[RTC] control DataChannel open` → `[RTC] control handshake sent (VQ_FIRST=1 ok=1)` → `[RTC] DC 1537 first echo RTT=...ms`。
  5. 触发 `[RTC] first video RTP ...` 与 `[RTC] first audio RTP ...` → 首个 IDR 解码出图 → `[Session] sent BE cmd=37 CGCNtfStreamStatus(playing=1)` → 2s 后 `[Session] vPad allocated`。
- **若 `BUNDLE 0 1 2` 修正后 DTLS 仍未握手成功**：
  1. 查看新增的 `[libRTC]` `Debug` 日志，确认 `Starting DTLS transport` 是否触发以及是否有 TLS alert / certificate 错误。
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
