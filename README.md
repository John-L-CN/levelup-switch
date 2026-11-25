# LevelUp-NX

Nintendo Switch 1（Horizon OS / Atmosphere + libnx）上的 **LevelUp 云游戏原生客户端**，产物是单个 `.nro` 文件：扫码登录 → 云端游戏大厅 → 一键起流，视频走 Tegra X1 的 NVDEC 硬解 + deko3d 上屏，手柄/震动/触屏直通云端。

> Native LevelUp Cloud Gaming client for Nintendo Switch 1 (`.nro`), forked from [`XITRIX/Moonlight-Switch`](https://github.com/XITRIX/Moonlight-Switch) (GPL-3.0), with the Moonlight host-discovery/pairing stack replaced by the LevelUp + YTCX + `libdatachannel` WebRTC stack.

---

## 功能

- **视频**：H.264（High Profile）→ `libdatachannel` RTP 重组 → `switch-ffmpeg` **NVDEC 硬解** → deko3d 上屏；底座自动申请 `1920x1080@60`、掌机 `1280x720@60`（按 `1088` 对齐解码后裁掉底部 8px）。
- **音频**：WebRTC Opus 48 kHz 立体声 → `libopus` 解码 → `audren` 低延迟播放。
- **输入**：Joy-Con / Pro 手柄原生 Nintendo 语义直通（**无 ABXY 反转**）、单指触屏绝对坐标映射、HD 震动（比例振幅，60 Hz 合批 + 3 s 归零）。
- **大厅**：顶栏账号 / VIP 到期 / HP·MP 时长，分类切换、中文搜索、最近游戏、游戏详情、设置面板；封面异步下载并缓存到 SD 卡。
- **健壮性**：BE 心跳双保活、首帧上报、丢包后自动补 PLI/FIR、解码异常自动重建、退出自动释放云端实例。

---

## 实现方式

代码分五层，除界面外大部分逻辑与 UI 解耦（可在主机上编译单测）。

| 层 | 关键实现 |
| --- | --- |
| 界面 | `Borealis`（Switch 原生 GLES/deko3d UI 框架）。大厅 = 顶栏 + 分类胶囊条 + 封面网格（`CulledGridBox` 行裁剪 + 分页，避免一次性建几百张卡片把命令缓冲顶爆）+ 模态框（详情 / 账号 / 设置 / 二维码）。封面用 `stb_image` 子线程下采样，封面与二维码纹理走同一套 RGBA 上传路径。 |
| 业务 HTTP | `client-api` 上的 JSON 接口：`getQRCode` / `getQRCodeState`（轮询扫码状态）/ `getUserInfo` / `game/list` / `game/detail` / `game/startGame` / `game/ytcx/stopGame`。每次请求注入公共参数（`t`、`appVer`、`streamingVer`、`clientInfo`（**键顺序敏感**）、`apiKey`、`sessionKey`、`ciKey`），按 `key=value` 字典序拼接 + 固定密钥做 MD5 得 `sig`。 |
| 串流调度 | 云厂商 YTCX：`/v3/union/apply`（HMAC-SHA1 签名）拿 `token` 与边缘节点 → 边缘 `POST /launch`（自签证书，跳过校验）拿编码参数与 WebRTC 信令端口 → BE **Protobuf over WebSocket**（`cmd=7` 申请、`cmd=5/6` 心跳、`cmd=17` 资源下发、`cmd=33/34` vPad 分配、`cmd=37` 首帧上报、`cmd=9` 退出）。 |
| 媒体 | `libdatachannel`（`libjuice` ICE / `libsrtp` / `usrsctp` / `mbedTLS`）作为 answerer：`onOffer` → Answer（H.264 `profile-level-id=640032`）→ **controlled** 角色、三个 m-line 各 trickle 一遍候选。收流侧自行解析 RTP 头（含 CSRC/扩展头）→ STAP-A / FU-A 重组为 Annex-B → NVDEC。 |
| 输入 | 服务端创建 `control` DataChannel，打开后依次发 `cSessionId` / `773` / `775` / `VQ_FIRST(1025)`，每秒 `1537` 心跳；手柄状态压成 **46 字节** 包（`type=518`）周期发送，`vpadID` 来自 `cmd=34`；下行 `type=518 / sub=31`（14 字节 LE）解析为 HD 震动，直接驱动 `hidSendVibrationValues`（绕开 Borealis 自带震动管线的满幅硬编码）。 |

目录速览：

```
app/src/levelup/          LevelUp 协议栈
  api_client.*            client-api：公共参数 + MD5 签名 + 各接口
  session_orchestrator.*  起流状态机（stopGame → startGame → union apply → BE → /launch → WebRTC）
  ytcx_client.*           union apply / BE Protobuf WebSocket / 边缘 /launch
  rtc_session.*           libdatachannel PeerConnection + 信令 + ICE
  video_pipeline.*        RTP → 重组 → NVDEC → deko3d
  h264_depacketizer.*     RTP 解包 + FU-A/STAP-A 重组
  input_packet.* / switch_input_bridge.* / rumble_switch.*   46 字节手柄包、按键映射、震动
  store.*                 config.json / 最近游戏 / 封面缓存 / 内置游戏库
  ui/                     大厅、串流视图
app/src/streaming/        Moonlight-Switch 遗留的探测/发现模块（桌面端仍用到，Switch 端不走）
scripts/                  Docker 交叉编译、依赖打补丁
resources/                romfs：内置游戏库、XML 布局、着色器、字体
tests/                    主机侧纯逻辑单测（协议/重组/输入/存储）
```

---

## 构建

需要 Docker（镜像基于 `devkitpro/devkita64:latest`）。

```bash
git clone https://github.com/John-L-CN/levelup-switch.git
cd levelup-switch
git submodule update --init --recursive extern/borealis extern/moonlight-common-c extern/CImg extern/mdns

# 首次编译 SDK 依赖（mbedTLS + libdatachannel，含 Horizon OS 补丁）；已有 build/deps/switch 可跳过
bash scripts/build_switch1_deps.sh

# 编译 .nro
bash scripts/build_switch1_docker.sh
```

产物：

- `build/switch/LevelUp-NX.nro` —— 拷进 Switch 即可运行
- `build/switch/Moonlight.elf` —— 带符号表，配合 `nm` / `addr2line` 排查崩溃

主机侧单测（Linux，无需 devkitPro）：

```bash
g++ -std=c++20 -DLEVELUP_NO_CURL -DLEVELUP_NO_RTC -Iapp/src -Ibuild/deps/switch/include \
    tests/test_m2_protocol.cpp app/src/levelup/api_client.cpp app/src/levelup/crypto_util.cpp \
    app/src/levelup/proto_lite.cpp app/src/levelup/ytcx_client.cpp -o t_m2 -lpthread && ./t_m2
```

---

## 使用说明

### 1. 环境要求

- Switch 1（Erista / Mariko 均可），系统已装 **Atmosphere**，能从 `hbmenu` 启动自制程序。
- 一张放 `.nro` 的 SD 卡；外网可访问 LevelUp 服务（建议下行 ≥ 50 Mbps、延迟稳定，**优先 5 GHz Wi-Fi 或 USB 有线网卡**）。
- 一个已注册的 LevelUp 账号（微信 / LevelUp App 扫码登录）。

### 2. 安装

1. 把 `LevelUp-NX.nro` 拷到 SD 卡：`sdmc:/switch/LevelUp-NX/LevelUp-NX.nro`
   （目录名随意，保持一致即可；程序会在 `sdmc:/switch/levelup-nx/` 建自己的配置目录。）
2. 桌面图标（可选）：启动客户端后 `+` 进设置 → **安装到 Switch 主界面（桌面图标）**，返回桌面即可看到入口（需要大气层签名补丁 / `sys-patch`）。
3. **必须以 Title Override（全内存）模式运行**：按住 `R` 键点开任意已安装游戏进入 `hbmenu`，再启动本程序。
   直接用相册（Album Applet）启动内存不够，大厅顶部会弹橙色提示，串流会失败。

### 3. 首次登录

1. 启动后按 `X`（或设置里的“微信 / App 扫码登录”）→ 屏幕显示登录二维码。
2. 手机微信 / LevelUp App 扫码 → 手机上确认登录。
3. 客户端每 2 秒轮询一次状态，成功后自动拉取账号信息（昵称、UID、VIP 到期时间、HP/MP 剩余时长），并把登录态写入 SD 卡，下次启动免登录。
4. 换号 / 登出：`+` → 设置 → **退出登录 (登出)**（只清本机凭据，不动云端账号）。

### 4. 大厅操作

| 按键 | 功能 |
| --- | --- |
| `A` / 触屏点卡片 | 打开游戏详情（详情页 `A` = 启动云游戏） |
| `B` | 关闭当前弹窗 / 返回 |
| `Y` | 打开系统软键盘搜索游戏（中 / 英 / 拼音关键字均可） |
| `X` | 未登录 → 扫码登录；已登录 → 账号详情（UID / 手机号 / VIP / 算力明细） |
| `L` / `R` | 上一个 / 下一个分类；触屏点顶部分类胶囊同效 |
| `ZL` / `ZR` | 上一页 / 下一页 |
| `+` | 打开设置 |
| `-` | 有搜索词时=清空搜索；否则=联网刷新账号与游戏库 |

顶栏从左到右：账号昵称（或 UID）、VIP 到期时间、`HP` / `MP` 剩余可玩时长（服务端返回秒数，界面按分钟显示）。

### 5. 设置项

`+` → 「LevelUp-NX 设置」：

| 分组 | 项 | 说明 |
| --- | --- | --- |
| 账号与登录 | 当前账号 / 扫码登录 / 退出登录 | 账号状态与凭据管理 |
| 串流与控制 | HD 手柄震动 | 震动总开关 |
| | HD 震动强度 | 0 / 25 / 50 / 75 / 100 % |
| | 云端视频画质档位 | 0 流畅 / 1 标清 / 2 高清 / 3 超清（推荐）/ 4 原画 |
| | 画质优先模式 (VQ_FIRST) | 开启后云端按画质优先分配码率，静态画面不会被压糊（默认开） |
| | 默认显示性能 HUD | 起流后默认叠加帧率/码率/丢包等信息 |
| | 反转摇杆 Y 轴 | 个别游戏上下反了再开 |
| | Xbox 按键布局 | 把 `A↔B`、`X↔Y` 互换（默认关，Switch 语义与云端一致） |
| | 串流音频音量 | 0 / 25 / 50 / 75 / 100 % |
| 操作 | 刷新在线游戏库 | 重新拉取账号信息 + 在线游戏目录（不动本地缓存文件） |
| | 清除本地封面缓存 | 封面显示异常时用，之后重进大厅会重新下载 |
| | 安装到 Switch 主界面 | 生成桌面 forwarder（见 §2.2） |

设置改完立即写盘（`sdmc:/switch/levelup-nx/config.json`），下次启动沿用。

### 6. 串流中操作

- 手柄按键原生直通云端，无需任何映射设置；触屏可当鼠标/触控板用。
- **呼出串流菜单**：长按 `-` + `+` 400 ms，或同时按 `L` + `R` + `-` + `+`（立即触发）。
  菜单项：继续游戏 / 切换性能 HUD / 请求关键帧（画面花屏、卡顿时手动刷新参考帧）/ 开关震动 / 安全退出。
- **安全退出**会按序发 BE `cmd=43`（回收 vPad）、`cmd=9`（退出游戏）、`stopGame`（释放云端实例），确保不占用云算力额度；直接关程序偶发会残留实例，下次启动客户端也会先尝试清理。
- 画面卡住/花屏时：先等 1–2 秒（客户端的丢包自愈会自动补关键帧），仍不行再从菜单点“请求关键帧”。

### 7. 画质与带宽参考

| 分辨率 | 档位 | 大致码率 | 建议带宽 |
| --- | --- | --- | --- |
| 1080p60（底座） | 3 超清 | 15–25 Mbps | ≥ 50 Mbps，延迟 < 40 ms |
| 720p60（掌机） | 3 超清 | 8–12 Mbps | ≥ 30 Mbps |

带宽紧张时把档位降到 `1`/`2`；追求画质可试 `4 原画`（需要更高带宽，弱网容易掉帧）。

### 8. 数据文件

全部在 `sdmc:/switch/levelup-nx/`：

| 文件 | 作用 |
| --- | --- |
| `config.json` | `deviceId`（设备指纹，**不要随便清空**）、登录态（`sessionKey` / `ciKey`）、账号信息与全部设置项 |
| `recent_<uid>.json` | 该账号的最近游戏（最多 12 条，按账号隔离） |
| `games_library.json` | 在线目录落盘缓存（离线时展示用；程序内置一份 romfs 兜底库） |
| `covers/` | 游戏封面缓存 |
| `levelup.log` | 运行日志（起流各阶段、ICE/DTLS、首帧、丢帧统计），排障时看这个 |

### 9. 常见问题

- **大厅顶部橙条提示“相册模式”**：用相册启动的，内存不足。按住 `R` 开任意游戏进 `hbmenu` 再启动。
- **提示 `1030001 登录过期`**：登录态失效，按 `X` 重新扫码。
- **起流卡在“等待云端首帧”**：多见于网络受限（UDP 被挡、NAT 严格）。看 `levelup.log` 里 ICE 是否 `connected`；换 5 GHz / 有线网络重试。
- **提示 `another game running`**：云端还有上次的实例。等 1–2 分钟，或退回大厅重新点启动（客户端会先自动 `stopGame` 清理）。
- **画面模糊、码率上不去**：确认“画质优先模式”已开；网络抖动大时云端会主动降码率。
- **花屏 / 拖影**：菜单里点“请求关键帧”；持续出现则降档位或换网络。
- **手柄无响应**：确认手柄已连接且被系统识别为主手柄（Settings → Controllers and Sensors）；HUD 里能看到 vPad 已分配即链路正常。
- **震动没有**：设置里 HD 震动已开、强度 > 0；部分游戏云端本身不下发震动。
- **封面空白**：设置 → 清除本地封面缓存，返回大厅重新进。
- **程序崩溃**：`sdmc:/switch/levelup-nx/levelup.log` 末尾一般是现场；构建产物里的 `Moonlight.elf` 可用来 `addr2line` 定位。

---

## 许可与致谢

**本项目是双许可结构，请按文件来源区分：**

| 范围 | 许可 |
| --- | --- |
| 本项目原创代码（`app/src/levelup/**`、`scripts/**`、`resources/` 中的本项目资源、README 等） | **PolyForm Noncommercial License 1.0.0** —— 见 [LICENSE](LICENSE)：允许个人学习、研究、非商业自用与修改分发，**禁止任何商业用途**（含售卖、随硬件捆绑销售、代运营收费、作为商业服务的一部分提供等） |
| 派生自 [`XITRIX/Moonlight-Switch`](https://github.com/XITRIX/Moonlight-Switch) 的部分（`app/src/streaming/**`、`app/src/libgamestream/**`、`app/src/` 其余上游文件、`resources/xml/**` 上游布局等） | **GPL-3.0** —— 全文见 [LICENSE-GPL-3.0.txt](LICENSE-GPL-3.0.txt) |

- **商业使用需要单独授权**：如需将本客户端用于商业用途（含商业分发、预装、SaaS、收费服务集成），请先联系仓库作者获取书面商业许可；GPL-3.0 覆盖的上游派生部分仍按其条款单独适用。
- 上游原始说明归档在 [docs/UPSTREAM_MOONLIGHT_README.md](docs/UPSTREAM_MOONLIGHT_README.md)。
- 依赖：`borealis`、`libdatachannel`（`libjuice` / `libsrtp` / `usrsctp`）、`mbedTLS`、`switch-ffmpeg`、`libopus`、`stb`、`nlohmann/json`，各自遵循其原始许可。
- 应用图标为 LevelUp 官方美术资源，仅随本客户端一同分发，不单独授权用于其它项目。
- 仓库不含任何个人凭据：登录态、设备指纹与缓存一律在运行时写入 SD 卡。
