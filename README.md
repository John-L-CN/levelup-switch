# levelup-switch

Nintendo Switch 1 (Horizon OS / Atmosphere + libnx) 原生 **LevelUp 云游戏客户端（`.nro`）**，基于开源 [`XITRIX/Moonlight-Switch`](https://github.com/XITRIX/Moonlight-Switch)（GPL-3.0）构建底座改造，集成 Tegra X1 `NVDEC + VIC + deko3d` 硬解渲染、`Borealis` 原生界面与 `libdatachannel` WebRTC 串流栈。

> Native **LevelUp Cloud Gaming client (`.nro`)** for Nintendo Switch 1 (Horizon OS / Atmosphere, Title Override Full RAM), forked from `XITRIX/Moonlight-Switch` and adapted with `libdatachannel` WebRTC + LevelUp/YTCX signaling.

---

## 核心文档 / Documentation

- [ARCHITECTURE_MOONLIGHT_SHELL.md](ARCHITECTURE_MOONLIGHT_SHELL.md) —— `Moonlight-Switch` 套壳改造架构、模块保留/剥离清单、`libdatachannel` 交叉编译与 Switch 运行约束
- [PROTOCOL_SPEC.md](PROTOCOL_SPEC.md) —— LevelUp HTTP / YTCX union / BE Protobuf WebSocket / 边缘 `/launch` / WebRTC / 46 字节手柄控制包字节级协议规格
- [PROMPT.md](PROMPT.md) —— 开发启动主提示词、避坑铁律与 M0–M5 里程碑定义
- [docs/UPSTREAM_MOONLIGHT_README.md](docs/UPSTREAM_MOONLIGHT_README.md) —— 上游 `XITRIX/Moonlight-Switch` 原始说明归档

---

## 开发里程碑 / Milestones

- [x] **M0（工具链基线）**：基于 `devkitpro/devkita64:latest` Docker 工具链拉通 Switch 1 `Moonlight-Switch` 原生 `.nro` 交叉编译与 `NRO0` 产物校验
- [x] **M1（WebRTC 库交叉编译）**：交叉编译 `libdatachannel`（`libjuice` / `libsrtp` / `usrsctp` / `mbedtls 3.6.2`）为 `aarch64-none-elf` 静态库并集成进 CMake
- [x] **M2（信令与调度层打通）**：实现 LevelUp HTTPS API、YTCX `/v3/union/apply`、BE Protobuf WebSocket 与边缘 `/launch`
- [x] **M3（Switch NVDEC 硬解出图出声）**：接通 `libdatachannel` H.264 RTP 重组 → `switch-ffmpeg` NVDEC 硬解 + `deko3d` 渲染 + `Opus` 音频输出 + `cmd=37` 保活
- [x] **M4（原生手柄、震动与触屏控制）**：接入 `libnx padUpdate` + `AllocVPad` + 46 字节手柄控制包 + `control` DataChannel 握手/`1537` 心跳 + `type=518/sub=31` 比例 HD Rumble（60Hz 合批、3s 看门狗、退出归零）
- [x] **M5（Borealis 大厅与全功能封板）**：扫码登录 UI、VIP/HP/MP 算力展示、游戏分类、最近游戏、`swkbd` 搜索、HD 震动开关与强度/画质设置、Dock(1080p60)/Handheld(720p60) 自适应、串流悬浮菜单与 `LevelUp-NX.nro` 打包

---

## 编译构建 / Build (Switch 1 Docker)

### 1. 拉取仓库与 Switch 1 必需子模块

可跳过仅桌面/Apple 平台使用的 `extern/vcpkg`：

```bash
git clone https://github.com/John-L-CN/levelup-switch.git
cd levelup-switch
git submodule update --init --recursive extern/borealis extern/moonlight-common-c extern/CImg extern/mdns
```

### 2. 使用 Docker `devkitpro/devkita64:latest` 交叉编译依赖与 `.nro`

```bash
# （可选单独运行）拉取并应用 Horizon OS 补丁，交叉编译 MbedTLS 3.6.2 + libdatachannel 静态库
bash scripts/build_switch1_deps.sh

# 编译 Switch 1 .nro（首次缺少静态库时会自动调用 build_switch1_deps.sh）
bash scripts/build_switch1_docker.sh
```

脚本会自动构建派生镜像 `levelup-switch-devkita64:latest`（移除与 `moonlight-common-c` 内置 `enet` 冲突的 `switch-enet`），将 MbedTLS 3.6.2 与 `libdatachannel`（含 `libjuice` Horizon 默认路由探测与数值 candidate 修复、`libsrtp2`、`usrsctp`）静态库安装至 `build/deps/switch/`，执行两遍 CMake 配置与并行编译，并在成功后输出：

- `build/switch/LevelUp-NX.nro`（已校验 `NRO0` 魔数头与内置 `romfs:/levelup/games_library.json`）
- `build/switch/Moonlight.elf`（带完整符号表，用于 `nm`/`addr2line` 排错）

---

## 真机运行要求 / Runtime Requirements

1. **必须使用 Title Override（Full RAM）模式**：在 Atmosphere 下**按住 `R` 键打开任意已安装游戏**进入 `hbmenu` 后启动 `/switch/LevelUp-NX/LevelUp-NX.nro`（Album Applet 模式内存受限，无法稳定支撑 NVDEC + 1080p 帧缓冲 + WebRTC 栈；程序检测到处于相册模式时会在大厅顶部弹出醒目提示）。
2. **本地配置落盘路径**：所有个人登录态、设备 ID、最近游戏、封面缓存与运行日志均在运行时写入 Switch SD 卡 `sdmc:/switch/levelup-nx/`（`config.json`、`recent_<uid>.json`、`games_library.json`、`covers/`、`levelup.log`），源码仓库不包含任何个人凭据。
3. **底座 / 掌机自适应**：Docked 模式自动请求 `1920x1080 @ 60fps`，Handheld 模式自动请求 `1280x720 @ 60fps`（底层按 Tegra NVDEC `1920x1088` 对齐自动裁剪至 `1080`，消除底部 8px 绿边）。
4. **大厅按键操作**：
   - `A` / 触屏点击卡片：查看游戏详情与启动云游戏
   - `Y`：调用 Switch 系统软键盘（`swkbd`）搜索游戏（支持中文 / 英文 / 拼音关键字）
   - `X`：微信 / LevelUp App 扫码登录或查看账号 VIP / HP / MP 时长
   - `L` / `R`（或点击顶部分类胶囊）：切换游戏分类 /「全部」/「最近」
   - `+`：打开设置（HD 震动开关与强度、画质档位、`VQ_FIRST`、默认性能 HUD、摇杆 Y 轴反转、音频音量、清缓存）
   - `-`：若当前有搜索词则一键清除搜索，否则联网刷新账号与在线游戏目录
5. **串流中操作与自愈**：
   - 手柄按键原生 Nintendo 语义直通云端 `switch-handheld`（无 Xbox/Switch ABXY 错位），支持单指触屏绝对鼠标映射与比例 HD Rumble。
   - **长按 `-` + `+` 400ms**（或同时按下 `L + R + - + +`）：呼出半透明串流菜单（继续、切换性能 HUD、手动请求恢复关键帧 PLI+FIR、切换震动、安全退出并释放云端实例）。

---

## 致谢与许可 / License & Credits

本项目基于 [`XITRIX/Moonlight-Switch`](https://github.com/XITRIX/Moonlight-Switch) 进行定制改造，沿用上游 **GPL-3.0** 开源许可证（详见 [LICENSE](LICENSE)）。协议与桌面对照实现参考同级项目 [`John-L-CN/levelup-go`](https://github.com/John-L-CN/levelup-go)。
