# 新会话启动提示词（直接复制以下内容开启 Switch 原生版开发）

> 使用方法：新建会话后将工作目录指向 `e:\AICode\lianzidashi\levelup\Switch`，直接把下方分割线之间的内容粘贴发给 Agent。

---

我要基于开源 **`Moonlight-Switch`**（推荐 `https://github.com/XITRIX/Moonlight-Switch`）套壳改造，开发运行在 **Nintendo Switch 1 原生 Horizon OS（大气层 Atmosphere / hbmenu，Title Override Full RAM 模式）** 上的 **LevelUp 云游戏原生 `.nro` 客户端**。

请按以下步骤启动工作：

## 1. 先阅读本目录规范与参考代码
1. 阅读当前目录下的：
   - [ARCHITECTURE_MOONLIGHT_SHELL.md](ARCHITECTURE_MOONLIGHT_SHELL.md)（Moonlight-Switch 套壳改造架构、模块替换清单与 Docker 交叉编译指南）
   - [PROTOCOL_SPEC.md](PROTOCOL_SPEC.md)（已实测跑通的 LevelUp HTTP / YTCX union / BE Protobuf WebSocket / WebRTC / 46 字节手柄控制包全链路字节级规格）
2. 遇到任何细节不明确时，直接对照同级已验证稳定运行的纯 Go 参考实现 `../go-client`：
   - `../go-client/internal/api/client.go`（签名算法、登录、游戏库、启停游戏）
   - `../go-client/internal/ytcx/union.go`、`proto.go`、`be.go`、`launch.go`（调度、BE 心跳、cmd=37 起流确认、vPad 申请、边缘 `/launch`）
   - `../go-client/internal/rtc/rtc.go` & `../go-client/internal/video/h264.go`（WebRTC 信令、ICE controlled、3×m-line trickle、RTP 扩展头与 FU-A 重组）
   - `../go-client/internal/input/input.go`（DataChannel 握手、1537 保活、VQ_FIRST、46 字节手柄控制包）
   - `../go-client/internal/ui/lobby.go` & `../go-client/data/games_library.json`（大厅分类、最近游戏隔离、精准名称搜索算法、预置游戏库）

## 2. 目标功能（MVP 到完整版）
1. **视频**：Tegra X1 **NVDEC 硬件解码 + VIC 色彩转换 + deko3d/GLES 上屏**（Docked 模式默认申请 `1920x1080` @ 60fps，Handheld 模式 `1280x720` @ 60fps；CPU 软解禁止作为主路径）。
2. **音频**：WebRTC `audio/opus` 48kHz 立体声 → `libopus` 解码 → `audren` / `SDL2_audio` 低延迟播放。
3. **输入与震动**：
   - `libnx padUpdate` 读取 Joy-Con / Pro Controller 按键、摇杆、ZL/ZR 扳机与触屏事件 → 映射为 46 字节 YTCX 手柄包通过 `control` DataChannel 发送（**注意 Switch 物理 ABXY 天生已是 Nintendo 语义布局，无需 Xbox→Nintendo 反转**）。
   - 解析云端 `control` 通道下行 `type=518, sub=31`（14 字节 LE）震动包，通过独立异步合批线程（~60Hz）直调 `libnx hidSendVibrationValues` 实现比例振幅 HD Rumble（支持掌机 `HidNpadIdType_Handheld` 与底座/无线主手柄 `HidNpadIdType_No1`，含 3s 超时归零与退出自动停震，并在 M5 提供震动开关/强度设置）。
4. **大厅 UI**：基于 Moonlight-Switch 自带的 `Borealis` 原生界面框架，实现：
   - 扫码登录界面（将服务端返回的 Base64 JPEG 渲染为原生纹理展示，轮询登录态并持久化到 `sdmc:/switch/levelup-nx/config.json`）
   - 顶栏账号昵称、VIP 到期、HP/MP 算力（分钟）显示
   - 「全部 / 最近游戏（`recent_<userId>.json`）/ 服务端分类」切换 + 调用 Switch 原生软键盘 `swkbd` 做游戏名精准搜索
   - 退出串流自动执行 BE `cmd=9` (`CGReqExitGame`) + `stopGame` 释放云端实例并退回大厅

## 3. 必须死守的协议铁律与避坑清单（严禁回退）
1. **`clientInfo` 键顺序不能改**：MD5 签名只取前 50 字符，`clientInfo` JSON 键顺序一旦变化会报 `1030001 登录过期`。
2. **BE 长连接双保活（不满足必断连）**：
   - 收到边缘资源后立即按周期发 `cmd=5 CGReqHeartbeat`（含 `CommonHeader`，根据 `cmd=6` 调整周期），否则 ~80 秒必断；
   - **NVDEC 首帧解码出图后必须立即发 `cmd=37 CGCNtfStreamStatus{status=1}`**，否则 60 秒必报 `Start failure` 断连！
3. **WebRTC 信令与 ICE**：
   - `answer` 必须嵌套 `{"answer":{"type":"answer","sdp":...}}`；
   - ICE 角色保持 **controlled**（answerer 默认）；trickle candidate 必须对 `sdpMLineIndex=0/1/2` 各发一遍；
   - H.264 声明 High Profile（`profile-level-id=640032`）。
4. **防 2 秒周期模糊与 RTP 解包**：
   - 首帧出图前（前 8 秒）发 `PLI+FIR` 加速起流；**首帧出图后立刻停止周期性 PLI/FIR**，仅在真正丢包/花屏时按需节流请求；
   - 服务端 RTP 带扩展头，禁止硬编码跳过 12 字节；FU-A 首片前必须补 `00 00 00 01`；不要跨帧检测 `seq` 空洞（云端跳帧是正常降帧率）。
5. **DataChannel 握手与 `vpadID`**：
   - 打开 `control` 通道时必须判断 `isOpen()` 避免回调竞态；
   - Open 后依次发送 `1792(cSessionId)` → `773` → `775` → `VQ_FIRST (1025)`；每秒发 `1537` 心跳（**收到云端回显 1537 时忽略，切勿回发形成死循环**）；
   - 开局容器就绪约 8 秒后向 BE 发 `cmd=33 AllocVPad(padIndex=65535, usage=2)`，拿到 `vpadID` 后填入 46 字节手柄包的 `controller` 字段。
6. **`libdatachannel` 替代 `libwebrtc`**：切勿尝试在 devkitA64 上编译 Google 庞大的 `libwebrtc`；使用轻量纯 C++ 的 `libdatachannel`（`libjuice` + `mbedtls`/`openssl` + `libsrtp` + `usrsctp`）。
7. **内存与运行模式**：Switch 端需在 **Title Override（按住 R 进 hbmenu，Full RAM）** 下运行；确保任何个人凭据、配置绝不硬编码进仓库源码。

## 4. 请按以下里程碑立即推进
- **M0（工具链基线）**：检查本机 Docker（或 WSL2 / 局域网 Ubuntu）是否有 `devkitpro/devkita64` 环境；拉取 `XITRIX/Moonlight-Switch` 到本目录源码区并原封不动编出一个基线 `.nro` 验证交叉编译通畅。
- **M1（WebRTC 库交叉编译）**：将 `libdatachannel`（含 `libjuice`, `libsrtp`, `usrsctp`）交叉编译为 `aarch64-none-elf` 静态库并集成进 CMake，编出可链接通过的最小测试 `.nro`。
- **M2（信令与调度层打通）**：用 C++（`libcurl` + `nlohmann/json` + `mbedtls` + `rtc::WebSocket`）实现 LevelUp HTTP 与 YTCX 调度、BE Protobuf 与 `/launch`，可先在宿主机测试拿到边缘 `onOffer` SDP。
- **M3（Switch NVDEC 硬解出图出声）**：接通 `libdatachannel` `PeerConnection` 视频 H.264 帧喂入 Moonlight 的 `FFmpeg/NVDEC` 硬解渲染管线 + `Opus` 音频输出 + `cmd=37` 保活。
- **M4（原生手柄、震动与触屏控制）**：接入 `libnx padUpdate` + `AllocVPad` + 46 字节包 + `control` 通道三连握手 + `type=518/sub=31` 震动解包与 `libnx hidSendVibrationValues` 比例 HD Rumble（60Hz 合批 + 3s 看门狗 + 退出归零）。
- **M5（Borealis 大厅与全功能封板）**：扫码登录 UI、算力展示、分类、最近游戏、`swkbd` 搜索、Dock/Handheld 自适应，真机 Title Override 10 分钟稳定压测。

现在请先检查本目录文档以及本机 Docker / Git / CMake 构建环境现状，给出 M0 实施方案并立刻开始执行 M0。
