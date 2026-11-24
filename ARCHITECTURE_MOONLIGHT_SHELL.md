# Moonlight-Switch 套壳改造工程架构蓝图

## 1. 为什么采用 Moonlight-Switch 套壳方案

从零在 Horizon OS（Nintendo Switch 1）上编写完整云游戏客户端工作量巨大（`deko3d/GLES` 渲染上下文、Tegra X1 `NVDEC + VIC` 零拷贝硬解管线、手柄热插拔、`Borealis` UI、Docked/Handheld 切换、`swkbd` 软键盘、休眠唤醒恢复）。

以开源项目 **`XITRIX/Moonlight-Switch`** 为外壳底座，可以**直接白嫖约 70% 的底层重资产**，把开发精力 100% 聚焦在 **LevelUp 协议接入 + `libdatachannel` WebRTC 串流层 + 大厅 UI 替换** 上。

---

## 2. 模块保留、剥离与新增清单

| 模块层次 | Moonlight-Switch 原实现 | 改造策略 | 替换 / 新增说明 |
| --- | --- | --- | --- |
| **应用框架 / UI** | `Borealis` (brls) + `GLFW` / `deko3d` / `GLES` | **保留并改造页面** | 保留 `Borealis` 主题、网格布局、手柄焦点导航、弹窗、`swkbd` 软键盘调用；替换为主机无关的 LevelUp 游戏库、扫码登录页、最近游戏页与顶栏搜索 |
| **视频硬件解码** | `FFmpegDecoder` (`switch-ffmpeg` `h264_nvtegra` / `NVDEC` + `VIC`) | **完全保留** | 把 WebRTC `H264Depacketizer` 还原出的 Annex-B 帧（`AVPacket` 形态）直接喂给 Moonlight 现有 `decodeInputData()` / `FFmpegVideoDecoder` |
| **视频上屏渲染** | `GLVideoRenderer` / `deko3d` 渲染器 | **完全保留** | 直接复用现有零拷贝/硬解纹理渲染管线；首帧成功解码回调通知 BE 发送 `cmd=37` |
| **音频解码与输出** | `AudrenAudioRenderer` / `SDL2` + `Opus` | **保留音频后端** | 将 WebRTC 音轨收到的 48kHz Opus 包（每个 RTP 负载为一个完整 Opus 包）喂给 `libopus` → 送入 Moonlight 的 Switch 音频输出后端 |
| **流媒体协议层** | `limelight-common-c` (GameStream / RTSP / ENet) + mDNS 发现 + PIN 配对 | **彻底剥离移除** | 移除 GameStream/Sunshine 相关依赖与发现逻辑，释放编译体积与状态耦合 |
| **LevelUp HTTP & 调度** | 无 | **新增 `src/levelup/`** | `api_client.{hpp,cpp}` + `ytcx_be.{hpp,cpp}` + `edge_launch.{hpp,cpp}` |
| **WebRTC & 数据通道** | 无 | **新增 `src/levelup/`** | `rtc_session.{hpp,cpp}`（基于 **`libdatachannel`**）+ `h264_depacketizer.{hpp,cpp}` + `input_packet.{hpp,cpp}` |
| **本地状态持久化** | Moonlight JSON 配置 | **改造复用** | `sdmc:/switch/levelup-nx/config.json` 与 `recent_<userId>.json`，封面缩略图缓存至 `sdmc:/switch/levelup-nx/covers/` |

---

## 3. 建议新增的目录与类职责设计

```text
Moonlight-Switch/
└─ app/src/levelup/
   ├─ crypto_util.{hpp,cpp}       # MD5 / HMAC-SHA1 / Base64（复用 devkitPro switch-mbedtls）
   ├─ proto_lite.{hpp,cpp}        # 极简 Protobuf varint/bytes/fixed32 + BE CGMessage 封包拆包
   ├─ api_client.{hpp,cpp}        # LevelUp HTTPS 接口（libcurl + nlohmann/json + 固定键序 clientInfo）
   ├─ ytcx_session.{hpp,cpp}      # 编排：stopGame → startGame → union/apply → BE WS → /launch → AllocVPad
   ├─ h264_depacketizer.{hpp,cpp} # RFC 6184 + 扩展头解析 + 单帧 seq 检查（严格对齐 ../go-client/internal/video/h264.go）
   ├─ rtc_session.{hpp,cpp}       # libdatachannel PeerConnection + rtc::WebSocket 信令 + Track/DC 回调
   ├─ input_packet.{hpp,cpp}      # 1792/773/775 握手 + VQ_FIRST + 1537 心跳 + 46 字节手柄控制包 + type=518/sub=31 震动解包
   ├─ rumble_switch.{hpp,cpp}     # 异步 ~60Hz 震动合批工作线程 + 3s 看门狗 + libnx 直驱 HD Rumble（掌机/无线手柄）
   └─ store.{hpp,cpp}             # config.json / recent_<uid>.json / 封面并发预取缓存
```

### 关键设计技巧（减少依赖与踩坑）
1. **一套 `libdatachannel` 吃掉 WebSocket + WebRTC**：`libdatachannel` 自带完整的 `rtc::WebSocket`（支持 `ws://` 与 `wss://`，且支持 `disableTlsVerification`），可以直接用来连接 **BE 网关 (`ws://`)** 和 **边缘信令 (`wss://`)**，无需再额外引入 Beast / IXWebSocket / libwebsockets！
2. **首帧出图回调联动**：在 Moonlight 原视频解码回调里首次成功产出 `AVFrame`（准备渲染第 1 帧）时，立即设置 `rtcSession->notifyFirstVideoFrame()` 停止开局 PLI/FIR 轰炸，并在工作线程触发 BE 发送 `cmd=37`（`CGCNtfStreamStatus{status=1}`）。
3. **搜索功能接入 Switch 原生软键盘**：使用 `brls::Application::getPlatform()->getImeManager()->openForText(...)`（底层封装 `libnx swkbdCreate`），用户输入完中文/英文关键词后直接调用与 `go-client` 完全同构的 `MatchGames()` 过滤游戏名称。
4. **手柄震动绕过 Borealis `amp=320.0f` 缺陷**：`extern/borealis/library/lib/platforms/switch/switch_input.cpp` 中的 `SwitchInputManager::sendRumbleInternal()` 将 `amp_low`/`amp_high` 写成固定 `320.0f`（在 Horizon 上等同于满幅开关震动，失去细腻强弱层次）。`rumble_switch.{hpp,cpp}` 直接调用 `libnx` `hidInitializeVibrationDevices` 与 `hidSendVibrationValues`，以 `0.0f..1.0f` 比例振幅分别映射低频 `160Hz` 与高频 `320Hz` 马达，配合 `16ms`（~60Hz）异步合批工作线程、收到 `(0, 0)` 立即停震、3 秒无新指令自动归零看门狗以及退出/Home/断开自动停震。

---

## 4. 交叉编译环境与依赖清单

### 4.1 推荐构建环境
- **首选**：Docker `devkitpro/devkita64:latest`（Windows 宿主机无需配置复杂的 MSYS2/pacman，一行容器挂载源码目录编译 `.nro`；或者直接在本机局域网 Ubuntu / WSL2 安装 devkitPro）。
- **真机热部署调试**：Switch 打开 hbmenu 按 `Y` 启用 NetLoader，宿主机执行 `nxlink -s levelup-nx.nro`，支持无线秒推并通过 TCP 回显 `printf`/`log` 到宿主机终端。

### 4.2 devkitPro 现成 `dkp-pacman` 包
- `switch-ffmpeg`（核心：自带 Tegra X1 `NVDEC` + `VIC` 硬件加速）
- `switch-libopus`
- `switch-curl`
- `switch-mbedtls`（或 `switch-openssl`，推荐与 `libdatachannel` 保持同一套 TLS 库避免链接符号冲突）
- `switch-sdl2` / `switch-glfw` / `switch-deko3d` / `switch-mesa`
- `switch-zlib`、`nlohmann-json`（如 portlibs 未带可直接 FetchContent / single header）

### 4.3 `libdatachannel` 交叉编译注意事项（devkitA64 / Horizon OS）
1. **CMake 配置建议**：
   - 使用 `$DEVKITPRO/cmake/Switch.cmake` 工具链文件；
   - 关闭不必要目标：`NO_EXAMPLES=ON`、`NO_TESTS=ON`、`BUILD_SHARED_LIBS=OFF`、`BUILD_SHARED_DEPS_LIBS=OFF`；
   - 选择 TLS 后端：优先试 `USE_MBEDTLS=ON`（轻量）或 `USE_GNUTLS=OFF` + `switch-openssl`；确保 `libjuice`、`libsrtp`、`usrsctp` 全部静态编入。
2. **Horizon `nn::socket` POSIX 适配点（M1 阶段重点排查）**：
   - 核对 `libjuice` 网卡枚举（`getifaddrs` 在新版 libnx 已支持，若报错需检查头文件或针对 Switch 返回 `gethostname`/默认网卡地址）；
   - 核对 `SO_RCVBUF`/`SO_SNDBUF`/`IP_PKTINFO` 等 socket option 在 Horizon 上的支持情况（不支持的可在宏开关里优雅降级而不失败返回）；
   - libnx 线程栈默认偏小，给 WebRTC / 网络接收线程和解码线程显式保证足够的栈大小与合适优先级，避免抢占 NVDEC 或 UI 主线程。
3. **NACK 与重传**：确认 `libdatachannel` 的 RtcpNackResponder / H264 RTX 或至少本端 RTP seq/loss 检测到缺片时能正确发出 `PLI`/`FIR`（`rtc::Track::requestKeyframe()` / RTCP PLI）。

---

## 5. Switch 1 真机运行规则

1. **必须 Title Override 模式启动（Full RAM）**：Album Applet 模式可用内存过小，开启 NVDEC 硬解 + 1080p 帧缓冲 + WebRTC 栈大概率 OOM 崩溃；务必**按住 `R` 键打开任意已安装游戏进入 hbmenu** 启动本程序。
2. **底座 / 掌机分辨率自适应**：
   - 通过 `appletGetOperationMode()` 判断 `AppletOperationMode_Console`（底座接电视 → 申请 `1920x1080`）还是 `AppletOperationMode_Handheld`（掌机屏幕 → 申请 `1280x720` 省流且原生点对点清晰）。
3. **防息屏 & 退回桌面/休眠清理**：
   - 串流期间调用 `appletSetMediaPlaybackState(true)` 防止无按键看剧情时 Switch 自动息屏；
   - 捕捉退出/Home 键挂起信号时及时发送 `cmd=9` (`CGReqExitGame`) + HTTP `stopGame` 防止白扣算力时长。
