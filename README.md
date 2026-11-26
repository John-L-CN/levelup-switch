# LevelUp-NX

Nintendo Switch 1（Horizon OS / Atmosphere + libnx）上的 **LevelUp 云游戏原生客户端**，产物是单个 `.nro` 文件：扫码登录 → 云端游戏大厅 → 一键起流，视频走 Tegra X1 的 NVDEC 硬解 + deko3d 上屏，手柄/震动/触屏直通云端。

> Native LevelUp Cloud Gaming client for Nintendo Switch 1 (`.nro`), forked from [`XITRIX/Moonlight-Switch`](https://github.com/XITRIX/Moonlight-Switch) and released under the same **GPL-3.0**, with the Moonlight host-discovery/pairing stack replaced by the LevelUp + YTCX + `libdatachannel` WebRTC stack.

---

## 功能

- **视频**：H.264（High Profile）→ `libdatachannel` RTP 重组 → `switch-ffmpeg` **NVDEC 硬解** → deko3d 上屏；底座固定申请 `1920x1080@60`，掌机默认 `1280x720@60` 且可在大厅设置切为 `1920x1080@60`（720p 屏超采样；按 `1088` 对齐解码后裁掉底部 8px）。画质**固定「高清」，不再提供档位选项**。
- **音频**：WebRTC Opus 48 kHz 立体声 → 独立抖动缓冲（按 RTP 序号重排 + 缺包 PLC 补偿）→ `libopus` 解码 → `audren` 按队列深度定速喂料，网络抖动不再表现为断音/爆音。
- **输入**：Joy-Con / Pro 手柄原生 Nintendo 语义直通（**无 ABXY 反转**）、单指触屏绝对坐标映射、HD 震动（比例振幅，60 Hz 合批 + 3 s 归零）。
- **大厅**：顶栏账号 / VIP 到期 / HP·MP 时长，分类切换、中文搜索、最近游戏、游戏详情、设置面板；封面异步下载并缓存到 SD 卡，**按卡片在屏上的实际像素解码**（掌机 720p / 底座 1080p 自动切换预算，插拔底座会重解码），不再被固定小尺寸拉伸糊掉。最上方固定展示 **B站 UP主「牛巨眼」** 署名条。
- **健壮性**：BE 心跳双保活、首帧上报、丢包后自动补 PLI/FIR、解码异常自动重建；**信令 WebSocket 断开不再中断串流**（媒体走 SRTP/UDP 独立通道，后台自动重连信令）；退出/掉线均先释放云端实例再拆传输层，**退出游戏不再闪退**；上次残留实例靠落盘标记在下次启动时强制回收。
- **边缘节点选路（v0.2.0 起，v0.3.0 实验版加强）**：开流稳定后后台对「除当前节点外」的各区域机房做延迟探测（`/v1/speed/dialpoints` 拉表 → 逐点建连测 RTT → p95），结果落盘 `dial_rtt.json`（最多 10 条），下一次申请实例时随 `dialtest_history_data` 上报给调度；v0.3.0 起还会在**大厅空闲时预热测速**（无实例、无 flow，不消耗时长），并把上一局真实的 WebRTC RTT / 丢包 / 首帧耗时回流进缓存给区域打分，apply 时按打分带上 `area_type`。同时解析 `spare_address`（主接入地址连不上时按序尝试）与 `area_stun_address`（作为首个 ICE STUN），退出时补上 `/v3/union/release`。

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
# M2 协议 / 签名 / 边缘 /launch
g++ -std=c++20 -DLEVELUP_NO_CURL -DLEVELUP_NO_RTC -Iapp/src -Ibuild/deps/switch/include \
    tests/test_m2_protocol.cpp app/src/levelup/api_client.cpp app/src/levelup/crypto_util.cpp \
    app/src/levelup/proto_lite.cpp app/src/levelup/ytcx_client.cpp -o t_m2 -lpthread && ./t_m2

# M7 多区域测速 / 选址字段（p95、缓存、apply 注入、响应解析）
g++ -std=c++20 -DLEVELUP_NO_CURL -DLEVELUP_NO_RTC -Iapp/src -Ibuild/deps/switch/include \
    tests/test_m7_dial_rtt.cpp app/src/levelup/crypto_util.cpp \
    app/src/levelup/proto_lite.cpp app/src/levelup/ytcx_client.cpp -o t_m7 -lpthread && ./t_m7
```

### 发布（GitHub Releases）

**Release 页只保留最新一版，且只有一个资产，文件名带版本号：`LevelUp-NX-vX.Y.Z.nro`。**

本仓库是 Switch 1 单平台客户端，上游 `XITRIX/Moonlight-Switch` 的 "All Builds" 多平台产物（Android APK/IPA、Linux AppImage·deb·rpm·tar.gz、macOS、PSVita、SteamOS、Switch2、Windows 以及 100MB 的 Debug ELF）**与本项目无关，已从全部历史 Release 中删除，也不要再加回来**（每份约 860 MB，只会让下载页变乱）。v0.1.2～v0.1.7 的旧 Release 与对应 tag 已一并清理，只留当前版本。

```bash
# 打包（只产出 LevelUp-NX-v0.2.0.nro）
python scripts/ci/collect_release.py v0.2.0 build/switch dist

# 发布：创建/复用 tag 对应 Release → 删除所有其它资源 → 上传 LevelUp-NX-v0.2.0.nro → 校验只剩这一个文件
python scripts/ci/publish_release.py --tag v0.2.0 --apply
```

`publish_release.py` 的凭据取自 git 凭据管理器（`git credential fill`），不在任何文件里存 PAT；不带 `--apply` 时只做 dry-run；`--nro` 省略则自动取 `build/switch/` 下最新的 `LevelUp-NX*.nro`。打包逻辑有单测覆盖：`python -m unittest discover -s scripts/ci`。

### 变更记录

#### v0.3.3（2026-11-27，**正式版 / stable**）

> 从这一版起 `v0.3.x` 系列退出「实验性」：优选节点选路、悬浮触控栏、鼠标锁定与退出链路都已在实际对局中验证稳定，作为正式版发布。以下改动自上版 `v0.3.2` 起累积。

- **优选边缘节点逻辑重做（选路真正生效）**：
  - 测速探针从 libcurl 7.69.1 `CONNECT_ONLY` 改为**非阻塞 BSD socket + `poll()`** 直接计三次握手耗时，优先直连 `dialpoints` 给出的 `rtt_ping_address` IPv4（省掉额外 DNS 查询与解析劫持），单点超时 200 ms 放宽到 900 ms（对齐 `go-client`），并支持 `spare_address` 的主机名/IPv6 兜底解析。修掉 Switch 上「所有点都记 999 ms → `pickArea` 恒为 0」导致选路形同虚设的问题。
  - **主/备双区域表态**：新增 `pickPreferredAreas()`，`union/apply` 同时上报首选 `area_type` 与次选 `try_area_type`；调度没给首选时按次选兜底，日志同时打印 `askedArea` / `askedTry`。
  - **反馈记账修正**：`mergeDialCache()` 覆盖测速条目时不再误清 `live_rtt` / 丢包 / `rejects`；服务端回包 `area=0`（未指派）不再误记一次「被拒」；`area=0` 时额外 dump apply 回包字段结构（`describeApplySchema`）便于排查。后台复测改为每点 1 次采样，缩短与首帧抢 Wi-Fi 的时间窗。
- **HUD 精简为单行**：
  - 原来两行的堆叠信息（`LevelUp | WxH fps | RTT | Loss | PLI | NACK` + 第二行 `vPad / 震动 / 音频解码·补包·延迟 / 视频解码·跳帧 / 链路状态`）合并成**一行半透明小胶囊**：`1280x720 | 59.9fps | 8.4Mbps | 网络 23ms | 解码 2.1ms | 丢包 0`，按文字实际宽度自适应，不再横向铺满整屏。
  - 新增实时指标：码率（RTP 收字节差值的指数平滑，Kbps/Mbps 自适应单位）、解码单帧耗时（NVDEC 解码往返，EMA 平滑）、**5 秒滑动窗实时丢包率**；串流暂停菜单的副标题也改用同一行紧凑格式。设置标题栏补上版本号（`LevelUp-NX 设置 v0.3.3`），一眼能确认当前跑的版本。
- **主界面快捷方式一次安装、升级 NRO 免重建**：
  - 内置轻量级链式引导器 `sdmc:/switch/levelup-nx/launcher.nro`（与 `LevelUp-NX.nro` 同仓构建并打包在 `romfs`）。桌面快捷方式统一指向该固定路径与固定 Title ID，由引导器扫描 `nro_dir.txt`、`sdmc:/switch/LevelUp-NX/`、`sdmc:/switch/levelup-nx/` 与 `sdmc:/switch/`，读取各 NRO 的 `NACP display_version` 与文件名语义版本，自动通过 `envSetNextLoad` 在同进程满血权限下链式加载最新 `LevelUp-NX-v*.nro`。
  - 后续删掉旧版 `.nro` 换成新版 `.nro` 后，旧的桌面快捷方式可直接启动新版；重装快捷方式时也会复用同一 Title ID 覆盖更新，不再多出重复图标。
- **修复主界面快捷方式显示 `ver.1.6.0`**：
  - 将 `APP_VERSION` 与 `MOONLIGHT_NACP_VERSION` 统一到单一版本变量，并在 `patchNacp()` 中显式清零并覆盖 `display_version`（同时清零 `lang.name/author`），避免沿用上游 Moonlight 默认的 `1.6.0` 或宿主游戏版本号。
- **修复优选边缘节点测速在 Switch 上全记 `999ms` 导致 `pickArea=0`**：
  - 把测速探测从旧版 `libcurl 7.69.1 CONNECT_ONLY` 改为直接非阻塞 BSD socket + `poll()` TCP 三次握手，优先直连 `dialpoints` 返回的 `rtt_ping_address` IPv4（跳过额外 DNS 查询与解析劫持），超时放宽到 900 ms（对齐 `go-client`），并在 `union/apply` 中同时上报次选区域 `try_area_type`。
  - 修复 `mergeDialCache()` 覆盖测速条目时误清 `live_rtt` / 丢包 / 拒绝惩罚的缺陷，以及 `unionApply` 回包 `area=0` 时误记一次拒绝的缺陷。
- **串流鼠标新增「鼠标锁定」模式与不遮挡底部轻提示**：
  - 触屏鼠标模式扩展为三档轮换：**点击模式** → **触控板模式** → **鼠标锁定**（锁定后触屏操作不传递到云端游戏，仅响应左上「虚拟键盘」与右上「鼠标模式切换」两个悬浮按钮），并同步接入大厅设置与串流暂停菜单。
  - 切换模式后的提示改为串流画面**底部居中非交互轻提示**（1.9s 自动淡出），不再使用右上角系统通知条遮挡切换按钮，支持连续点按切换。
- **彻底修复 Switch 退出期 `[FATAL] std::terminate` / 「由于发生错误，软件已关闭」**：
  - 查明 `libnx` 下 `pthread_detach()` 恒返回 `ENOSYS`，导致 `std::thread::detach()` 一定会抛出异常并在析构时触发 `std::terminate()`。`exit_guard` 改用 `libnx` 原生 `Thread` + `svcExitProcess()` 兜底、正常退出调用 `finishExitGuard()` 回收线程，并将 `rtc_session` 的 `graveyard` 改为进程常驻堆分配以避免 `__cxa_finalize` 析构 `libdatachannel` 全局态再次调用 `detach()`。

#### v0.3.2（2026-11-26，**实验性** / pre-release）

- **串流画面悬浮触控栏（点击屏幕显示，3 秒无操作自动隐藏）**：
  - **左上角「虚拟键盘」**：点击直接呼出 Switch 系统自带软键盘（支持中英文输入），确认后通过控制通道（`type=518, sub=50`）将文本直发云端游戏；键盘打开期间自动暂停背景手柄/触屏上报，关闭后恢复。
  - **右上角「鼠标模式切换」**：支持在 **点击模式**（触屏绝对坐标直接点击，默认）与 **触控板模式**（单指拖动相对移动光标、单指轻点左键、双指轻点右键、双指上下滑动滚轮）之间一键切换，并持久化到 `config.json`（`touchMouseMode`）。
  - 仅在游戏串流界面生效，不占用大厅界面；点击两个悬浮按钮时会自动屏蔽该次触屏穿透到云端游戏。
- **修复底座切手持退出崩溃与配置损坏无法启动**：
  - 大厅预热测速线程改为可 `join` 并在析构时汇合，避免退出时与 `curl_global_cleanup()` 竞争；`dialProbeOnceMs` 接入退出中止回调，切手持重绘与封面线程在退出期立即早退，退出前关闭 `levelup.log` 句柄。
  - `dial_rtt.json` 改为 `.tmp` + `rename` 原子写盘；`parseLibraryJson` 与 `dialCacheFromJson` 全部字段提取纳入 `try/catch`，损坏时安全回落，不再在启动期抛未捕获异常导致无法进入软件。

#### v0.3.1（2026-11-26，**实验性** / pre-release）

> 基于 `v0.3.0` 增加「掌机模式可选 1080p 超采样」。由于云端对 1080p 流的码率与编码参数上限更高，720p 掌机屏下申请 1080p 经 GPU 下采样呈现后锐度与细节明显优于直接申请 720p。

- **掌机分辨率设置**：大厅设置新增「掌机分辨率 (底座固定1080p)」选项（`720p (默认)` / `1080p (超采样更清晰)`），改完立即写入 `config.json`（字段 `handheldResolution`，缺省或旧配置默认 `720`）。
- **底座行为不变**：底座/主机模式始终固定向云端申请 `1920x1080@60`，不受该选项影响。
- **界面与日志同步**：顶栏输出徽标（`掌机 720p60` / `掌机 1080p60` / `底座 1080p60`）和游戏详情页摘要实时联动，插拔底座也会自动刷新顶栏；起流日志新增 `[StreamView] target resolution WxH (mode=..., handheldSetting=...p)` 便于核对。

#### v0.3.0（2026-11-26，**实验性** / pre-release）

> 这一版是「优选边缘节点」的实验分支：核心逻辑全在本地，起流链路零等待，但 `area_type` 是否被服务端采纳仍需实测。日志里 `askedArea=` 与 `area=` 的对照就是判据。

- **大厅预热测速**：学到 LP `uuid` 与游戏 `pkgName` 后（`config.json` 持久化），一进大厅就在后台拉 `dialpoints` 并对各区域探测——此时**既没有云实例也没有 flow，完全不消耗云游戏时长**。于是「点启动」之前手里就是新鲜的全区延迟表，不再像 v0.2.0 那样要等玩过一局才有数据。
- **真实会话质量回流**：退出时把这一局在命中 `area/idc` 上的 WebRTC RTT、丢包千分比、首帧耗时写回 `dial_rtt.json`。打分权重 **live 0.6 / probe 0.4**；丢包 ≥1% 加 200、≥5% 加 800，首帧 >8s 加 100，会话直接失败加 3000。
- **申请时按打分表态**：`area_type` 取综合得分最优区域。数据不足、得分超过信任阈值（900）、被服务端拒过两次、或该区域在排队 → **不指定**，回落到官方行为（`area_type=0` 交给服务端）。服务端没给到请求的区域 → `rejects++`；响应 `queue_info.rank>0` → 排队惩罚。两者都写回缓存影响下次排序。
- **后台复测节奏**：改成「首帧之后再等 10s」才启动，采样 5 次 → **3 次**、并发 2 路，最坏耗时从 ~13s 压到 ~5s，且不与首帧抢 Wi-Fi 队列。
- **配置/缓存兼容**：`config.json` 新增 `lpUuid`、`lastPkgName`；`dial_rtt.json` 新增 `live_rtt / loss_permille / first_frame_ms / live_ok / live_ts / rejects / queue_rank`，旧文件缺字段按默认值读取，不会丢已有数据。
- **日志**：`[Lobby] dial prewarm start|skipped`、`[Dial] prewarm points=/done entries=/bestArea=`、`[Session] dial history= pickArea= score=`、`unionApply OK … area= idc= queue= askedArea=`、`scheduler ignored asked area=`、`[Dial] live area= rtt= loss= first= ok=`。
- 单测 `test_m7_dial_rtt.cpp` 扩到：打分权重/惩罚、区域选择与回退、拒绝与排队记账、真实会话回流、旧缓存兼容。

#### v0.2.0（2026-11-26）

- **边缘节点首次按官方逻辑参与选址**：此前移植版每次起流都只是被动接受服务端下发的实例（`/v3/union/apply` 的 `access_info`），而原客户端（lp-sdk `muti_area_rtt`）会「开流稳定后后台测各区域机房 → 缓存 → 下一次申请时上报」。现在补齐整条链路：
  - `GET http://union-access.yuntiancloud.com:18088/v1/speed/dialpoints` 拉全量测速点（明文 http、无签名，按用户返回 `area_type/idc/isp/rtt_ws_url/rtt_ping_address`），跳过当前所在节点；
  - 每个区域用 libcurl `CONNECT_ONLY` 做 TCP 建连测 RTT（官方是 WS 每 100ms 发毫秒时间戳回显、>200ms 记 `999` 的等价实现），丢超时样本后取 **p95**；
  - 以官方 key 形状 `<公网IP>-<area_type>-<idc>` 落盘 `sdmc:/switch/levelup-nx/dial_rtt.json`，只保留最近 **10** 条；
  - 下一次 apply 时带 `dialtest_history_data`（`ts/ip/isp/country/province/city/area_type/idc/rtt_95`）交给调度；**首发没有历史时仍与官方一致**：`area_type=0`，由服务端按出口 IP 判断；
  - apply 响应新增解析并使用 `area_type`、`idc`、`spare_address[]`（BE 主接入地址连不上时按序尝试备用地址）、`area_stun_address/port`（作为首个 ICE STUN）、`uuid`、`dialpoint.rtt_ws_url`；
  - 退出流程补上官方 `/v3/union/release`（`stopGame` 之后 best-effort，失败只记日志）。
- **日志**：新增 `[Dial] area=… idc=… p95=…ms`、`[Dial] best area=…`、`[Dial] cache saved entries=…`，以及 `unionApply OK … area=… idc=… spare=…`，可直接对照判断这次被分到哪个机房。
- **单测**：新增 `tests/test_m7_dial_rtt.cpp`（p95 与超时剔除、缓存 key/JSON 往返/10 条上限/同 key 覆盖、apply 注入 `dialtest_history_data`+`area_type`、apply 响应解析 spare/STUN/uuid）。

#### v0.1.9（2026-11-25）

- **无线 Joy-Con 一进程序就小幅持续震动**：`borealis` 的 `SwitchInputManager::clearVibration()` 把频率（160/320）当成电机强度传给 `sendRumbleInternal()`，实际写出 `amp = 320`（`HidVibrationValue` 的幅值区间是 0~1，1.0 即满幅）配上 160/204 ≈ 0.78 Hz、320/204 ≈ 1.57 Hz 的次共振频率 —— 线性谐振马达这样驱动只会**微弱而持续地抖**，不是满幅狂震。它在控制器 style set 变化时被调用，正好命中「手柄已取下（无线连接）+ 启动程序」的时机；按 HOME 回桌面再进来 style 不再变、不再调用它，所以"出去再进来就好了"。现在：配置期打补丁把它改成真正的幅值 0；空闲震动看门狗重做为 45 个句柄全枚举（`Handheld` + `No1..No8` × `FullKey/JoyDual/Handheld/JoyLeft/JoyRight`）、不再因 `hidInitializeVibrationDevices()` 激活失败而放弃发送、启动后 ~60 Hz 突发补零并在 style 变化时重来、每秒回读实际幅值写进日志。
- **底座模式串流画面不全屏**：`LevelUpStreamView::draw()` 拿到的是 Borealis 内容单位而不是像素，底座下 `windowScale = 1.5` 时视频只画进 1920×1080 framebuffer 左上角的 1280×720 区域。现在按 `Application::windowScale` 换算出真实像素尺寸。
- **按 HOME 退出后系统提示「软件无法关闭」**：新增退出守卫（`OnExitRequest` → 撤回媒体播放声明、停震动看门狗并归零、3 秒硬退出兜底），退出时取消在途 curl 请求（`stopGame` 走关键请求通道，保证云端实例被释放），窗口失焦时降到 5 fps，避免主循环卡在 present。
- **串流中 HD 震动可能整段哑掉**：不再因震动设备激活失败而放弃发送。

#### v0.1.8（2026-11-24）

- 首页前向器（Home forwarder）换成官方图标；封面按控件实际像素解码；信号丢失时保留媒体、退出前先释放云端实例。

---

## 使用说明

### 1. 环境要求

- Switch 1（Erista / Mariko 均可），系统已装 **Atmosphere**，能从 `hbmenu` 启动自制程序。
- 一张放 `.nro` 的 SD 卡；外网可访问 LevelUp 服务（建议下行 ≥ 50 Mbps、延迟稳定，**优先 5 GHz Wi-Fi 或 USB 有线网卡**）。
- 一个已注册的 LevelUp 账号（微信 / LevelUp App 扫码登录）。

### 2. 安装

1. 从 [Releases](https://github.com/John-L-CN/levelup-switch/releases) 下载 `LevelUp-NX-vX.Y.Z.nro`，拷到 SD 卡：`sdmc:/switch/LevelUp-NX/`
   （文件名带版本号，方便保留旧版回滚；放进去的文件名随意，程序会在 `sdmc:/switch/levelup-nx/` 建自己的配置目录。）
2. 桌面图标（可选）：启动客户端后 `+` 进设置 → **安装到 Switch 主界面（桌面图标）**，返回桌面即可看到入口（需要大气层签名补丁 / `sys-patch`）。自 `v0.3.3` 起，快捷方式通过内置 `sdmc:/switch/levelup-nx/launcher.nro` 动态查找最新 `LevelUp-NX-v*.nro`，安装一次后**后续升级删除旧版 `.nro`、放入新版 `.nro` 无需重建快捷方式**（若之前装过 `v0.3.2` 或更早版本的旧桌面图标，删掉旧图标并在新版里重装一次即可）。
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
| | 掌机分辨率 (底座固定1080p) | 720p (默认) / 1080p (超采样更清晰)；仅掌机生效，底座始终 1080p |
| | 画质 | **固定「高清」**，不可调（云端档位 2）；弱网下云端会自动降码率 |
| | 画质优先模式 (VQ_FIRST) | 开启后云端按画质优先分配码率，静态画面不会被压糊（默认开） |
| | 默认显示性能 HUD | 起流后默认叠加单行性能条（分辨率 / 帧率 / 码率 / 网络 RTT / 解码耗时 / 实时丢包率） |
| | 触屏鼠标模式 | 点击模式（触屏绝对坐标直点）/ 触控板模式（滑动移动 + 轻点左键 + 双指右键与滚轮）/ 鼠标锁定（触屏不传给云端，只响应悬浮按钮） |
| | 反转摇杆 Y 轴 | 个别游戏上下反了再开 |
| | Xbox 按键布局 | 把 `A↔B`、`X↔Y` 互换（默认关，Switch 语义与云端一致） |
| | 串流音频音量 | 0 / 25 / 50 / 75 / 100 % |
| 操作 | 刷新在线游戏库 | 重新拉取账号信息 + 在线游戏目录（不动本地缓存文件） |
| | 清除本地封面缓存 | 封面显示异常时用，之后重进大厅会重新下载 |
| | 安装到 Switch 主界面 | 生成桌面 forwarder（见 §2.2） |

设置改完立即写盘（`sdmc:/switch/levelup-nx/config.json`），下次启动沿用。

### 6. 串流中操作

- 手柄按键原生直通云端，无需任何映射设置；触屏可当鼠标/触控板用。点击屏幕左上「虚拟键盘」呼出系统软键盘，右上按钮在 **点击模式 → 触控板模式 → 鼠标锁定** 之间轮换（每次切换在画面底部弹一条 1.9 s 的轻提示，不遮挡按钮）。
- **呼出串流菜单**：长按 `-` + `+` 400 ms，或同时按 `L` + `R` + `-` + `+`（立即触发）。
  菜单项：继续游戏 / 切换性能 HUD / 请求关键帧（画面花屏、卡顿时手动刷新参考帧）/ 开关震动 / 安全退出。
- **安全退出**会按序停媒体 → BE `cmd=43`（回收 vPad）→ `cmd=9`（退出游戏）→ `stopGame`（释放云端实例），最后才拆 WebRTC 传输层；顺序这么排是为了任何一步异常都不会跳过云端释放。直接关程序偶发会残留实例，客户端会在 `pending-release.txt` 里留下标记，下次启动先强制回收。
- 画面卡住/花屏时：先等 1–2 秒（客户端的丢包自愈会自动补关键帧），仍不行再从菜单点“请求关键帧”。
- **信令断开（HUD 显示“信令重连中(媒体继续)”）**：画面/声音继续，后台每 4 秒重试一次边缘信令；只有真正 8 秒收不到视频 RTP 才会判定“连接中断”。

### 7. 画质与带宽参考

画质固定为 **高清**（云端 `videoLevel=2`），客户端不再暴露档位选项：

| 分辨率 | 画质 | 大致码率 | 建议带宽 |
| --- | --- | --- | --- |
| 1080p60（底座 / 掌机超采样） | 高清 | 8–16 Mbps | ≥ 30 Mbps，延迟 < 50 ms |
| 720p60（掌机默认） | 高清 | 5–10 Mbps | ≥ 20 Mbps |

弱网下云端会自动降码率保帧率；如果长期糊，优先换 5 GHz / 有线网络，而不是降画质。

### 8. 数据文件

全部在 `sdmc:/switch/levelup-nx/`：

| 文件 | 作用 |
| --- | --- |
| `config.json` | `deviceId`（设备指纹，**不要随便清空**）、登录态（`sessionKey` / `ciKey`）、账号信息与全部设置项（含 `handheldResolution` 掌机分辨率、`touchMouseMode` 触屏鼠标模式）；v0.3.0 起还存 `lpUuid` / `lastPkgName`（大厅预热测速用，学到后自动写入） |
| `recent_<uid>.json` | 该账号的最近游戏（最多 12 条，按账号隔离） |
| `games_library.json` | 在线目录落盘缓存（离线时展示用；程序内置一份 romfs 兜底库） |
| `covers/` | 游戏封面缓存 |
| `pending-release.txt` | 起流成功后写入的「待释放」标记（`gameId\|pkgName`），云端实例释放成功后自动删除；下次启动会据此强制回收 |
| `dial_rtt.json` | 多区域测速缓存（`key = "<公网IP>-<area_type>-<idc>"`，最多 10 条）。v0.3.0 起每条还带真实会话回流（`live_rtt` / `loss_permille` / `first_frame_ms` / `live_ok`）与调度反馈（`rejects` / `queue_rank`），用于区域打分 |
| `levelup.log` | 运行日志（起流各阶段、ICE/DTLS、首帧、音频延迟/补帧、丢帧统计），排障时看这个 |

### 9. 常见问题

- **大厅顶部橙条提示“相册模式”**：用相册启动的，内存不足。按住 `R` 开任意游戏进 `hbmenu` 再启动。
- **提示 `1030001 登录过期`**：登录态失效，按 `X` 重新扫码。
- **起流卡在“等待云端首帧”**：多见于网络受限（UDP 被挡、NAT 严格）。看 `levelup.log` 里 ICE 是否 `connected`；换 5 GHz / 有线网络重试。
- **提示 `another game running`**：云端还有上次的实例。客户端会先自动 `stopGame` 强制释放再重试 `startGame`（最多重试一次）；若仍失败，等 1–2 分钟后退回大厅重新点启动。
- **声音断续 / 爆音**：客户端已内置 80–140 ms 抖动缓冲 + 缺包 PLC，收包线程与解码/喂料线程分离。若仍存在，看 HUD 的 `延迟:` 与日志 `aud(... plc=... trim=...)`：`trim` 一直涨说明网络下行抖动极大（换 5 GHz / 有线），`plc` 一直涨说明真的在丢音频包。
- **画面模糊、码率上不去**：画质已固定高清，确认“画质优先模式”已开；网络抖动大时云端会主动降码率。
- **花屏 / 拖影**：菜单里点“请求关键帧”；持续出现则换网络。
- **手柄无响应**：确认手柄已连接且被系统识别为主手柄（Settings → Controllers and Sensors）；HUD 里能看到 vPad 已分配即链路正常。
- **震动没有**：设置里 HD 震动已开、强度 > 0；部分游戏云端本身不下发震动。
- **封面空白**：设置 → 清除本地封面缓存，返回大厅重新进。
- **封面发糊**：封面纹理已按控件实际像素（含底座 1.5× UI 缩放）解码；SD 卡里缓存的是**原图**，换版本后进大厅会自动按新尺寸重新解码，不需要清缓存。若某张图本身源分辨率就低（云端 300px 缩略图），只能等云端换图。
- **程序崩溃**：`sdmc:/switch/levelup-nx/levelup.log` 末尾一般是现场；构建产物里的 `Moonlight.elf` 可用来 `addr2line` 定位。已知的“[FATAL] std::terminate”退出闪退已在 teardown 层修复（不再析构 libdatachannel 传输对象）。

---

## 许可与致谢

**本客户端由 B站 UP主「牛巨眼」制作与分享（bilibili 搜索：牛巨眼），欢迎关注支持。**

**本项目整体以 GNU General Public License v3.0（GPL-3.0）发布，与上游协议保持一致：**

- **许可**：全文见 [LICENSE](LICENSE)（GPL-3.0 标准文本，可在 <https://www.gnu.org/licenses/gpl-3.0.html> 获取）。本项目是 [`XITRIX/Moonlight-Switch`](https://github.com/XITRIX/Moonlight-Switch) 的派生作品（`app/src/streaming/**`、`app/src/libgamestream/**`、`app/src/` 其余上游文件、`resources/xml/**` 上游布局等均源自上游），因此**整个仓库（含本仓库新增的 `app/src/levelup/**`、`scripts/**`、`resources/` 中的本项目资源、README 等原创部分）统一按 GPL-3.0 授权**——不再区分双许可，也不对任何一部分附加额外限制。
- **分发义务**：分发编译产物（`.nro`）时请一并提供本许可全文与完整对应源代码（或 GPL-3.0 第 6 条允许的书面要约），并保留上游与 B站 UP主「牛巨眼」的版权声明与署名。
- **商业使用**：GPL-3.0 本身不禁商，但任何商业分发/预装/以服务形式提供同样受 GPL-3.0 约束（须开源对应源码、保留许可与署名，且不得对接收者追加限制）。
- 上游原始说明归档在 [docs/UPSTREAM_MOONLIGHT_README.md](docs/UPSTREAM_MOONLIGHT_README.md)。
- 依赖：`borealis`、`libdatachannel`（`libjuice` / `libsrtp` / `usrsctp`）、`mbedTLS`、`switch-ffmpeg`、`libopus`、`stb`、`nlohmann/json`，各自遵循其原始许可。
- 应用图标为 LevelUp 官方美术资源，仅随本客户端一同分发，不单独授权用于其它项目。
- 仓库不含任何个人凭据：登录态、设备指纹与缓存一律在运行时写入 SD 卡。
