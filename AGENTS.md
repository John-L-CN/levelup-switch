# Switch 原生客户端工作区指南（`Switch/`）

本工作区用于开发 **Nintendo Switch 1（Horizon OS / Atmosphere + libnx）原生 `.nro` 版 LevelUp 云游戏客户端**（基于 `XITRIX/Moonlight-Switch` 套壳改造）。

## 核心文档导航
1. [PROMPT.md](PROMPT.md) —— 新会话启动主提示词、功能目标、避坑铁律与 M0~M5 里程碑。
2. [PROTOCOL_SPEC.md](PROTOCOL_SPEC.md) —— LevelUp HTTP / YTCX union / BE Protobuf WebSocket / 边缘 `/launch` / WebRTC / 46 字节手柄控制包全链路字节级协议规格。
3. [ARCHITECTURE_MOONLIGHT_SHELL.md](ARCHITECTURE_MOONLIGHT_SHELL.md) —— `Moonlight-Switch` 模块保留/剥离清单、`libdatachannel` 交叉编译要点与 Switch 端运行约束。

## 同级参考实现
- 协议已实测跑通的纯 Go 桌面/Linux 客户端位于 `../go-client/`（源码、抓包协议分析文档与 `data/games_library.json` 均可直接对照查阅）。

## 工程纪律
- **禁止硬编码个人凭据**：任何手机号、密码、`sessionKey`、`ciKey`、用户 ID 与个人 Token 不得写入源码或提交进仓库；真机配置一律落盘至 `sdmc:/switch/levelup-nx/`。
- **优先验证工具链再移植**：严格按 `M0 → M1 → M2 → M3 → M4 → M5` 推进，每阶段产出可独立验证的可编译构建物或日志证据。
