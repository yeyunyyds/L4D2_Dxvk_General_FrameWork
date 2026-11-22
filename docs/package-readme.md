# L4D2 DXVK General Framework

**v1.0.1 — FidelityFX FSR 3 Frame Generation for Left 4 Dead 2**

Project / 项目主页：https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork

## 中文说明

GF 通过 L4D2Bridge v1.3.4 获取场景颜色、深度和相机信息，再通过 FidelityFX FSR 3 与 x64 Vulkan Layer 生成中间帧。此安装包包含配套的 Client、Host32、Host64、官方 DXVK v2.6.1 和 Vulkan Layer，提供 HUD、第一人称武器、动态模型及 FPB 画面保护。

### 安装

1. 退出游戏与 `L4D2Bridge32.exe`、`L4D2Bridge64.exe`（Steam 可保持运行）。
2. 备份现有游戏 `bin` 目录。
3. 将安装 ZIP 解压到 **Left 4 Dead 2 游戏根目录**，按需合并并覆盖 `bin` 中的文件。
4. 在 Steam 中正常启动游戏。

请成套安装所有组件。若此前设置过 `client.testX86Server=True`，请改回 `False`。

### 使用与配置

帧生成默认启用。在游戏内按 **F8** 暂停或恢复。配置文件位于 `bin/.l4d2bridge/fg-config.ini`。

| 选项 | 默认值 | 说明 |
| --- | ---: | --- |
| `enabled` | `1` | FG 开关，关闭后需重启 |
| `hud` | `1` | HUD 保护 |
| `viewmodel` | `1` | 第一人称武器与手部保护 |
| `dynamic_models` | `1` | 动态模型保护 |
| `fpb_protect` | `1` | FPB 动态像素保护 |
| `generated_only` | `0` | 实验性仅显示生成帧（热更新） |
| `fpb_projection_protect` | `0` | 实验性身体投影保护 |
| `monitor` | `0` | 监测窗口 |

其他 FPB 参数范围与说明见配置文件注释。`generated_only` 仅改变显示策略，不解决生成帧自身伪影；开启后正常情况下显示帧率通常等于原生帧率，FG 停止或失败时将回退真实帧。

日志：`L4D2BridgeLogs/<会话 GUID>/frame-generation/`

### 兼容性与许可

项目针对 Windows L4D2 与本安装包配套的 Bridge/DXVK 构建。自定义模型、显卡驱动及其他 DXVK 版本未必兼容；帧生成不会改变游戏逻辑更新率或原始输入延迟。

第三方再分发许可证全文附于本文末尾。项目的完整致谢和依赖版本见 [THIRD_PARTY.md](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/main/THIRD_PARTY.md)。

---

## English

GF provides FidelityFX FSR 3 Frame Generation for *Left 4 Dead 2*, using L4D2Bridge v1.3.4 and an x64 Vulkan Layer. This package includes matched Client/Host32/Host64 components, upstream DXVK v2.6.1, and HUD, Viewmodel, dynamic-model, and FPB protection.

### Installation

1. Exit L4D2, `L4D2Bridge32.exe` and `L4D2Bridge64.exe`; Steam can stay open.
2. Back up your game `bin` directory.
3. Extract the ZIP into the *Left 4 Dead 2* game root, merging matching files in `bin`.
4. Launch normally through Steam.

Use all bundled components as a matched set. Restore `client.testX86Server=False` if previously changed.

### Usage and configuration

Frame Generation is enabled by default. Press **F8** in-game to pause or resume. Edit `bin/.l4d2bridge/fg-config.ini`:

| Setting | Default | Description |
| --- | ---: | --- |
| `enabled` | `1` | FG switch; restart after disabling |
| `hud` | `1` | HUD protection |
| `viewmodel` | `1` | First-person weapon protection |
| `dynamic_models` | `1` | Dynamic model protection |
| `fpb_protect` | `1` | FPB dynamic-pixel protection |
| `generated_only` | `0` | Experimental generated-only output (hot-reload) |
| `fpb_projection_protect` | `0` | Experimental body projection |
| `monitor` | `0` | Live monitoring window |

Other FPB settings are described in the INI comments. Generated-only presentation does not fix interpolation artifacts. Display FPS typically matches source FPS in this mode, with real frames shown when FG is unavailable.

Logs: `L4D2BridgeLogs/<session GUID>/frame-generation/`

### Compatibility and licensing

Designed for Windows L4D2 with the bundled Bridge/DXVK combination. Alternative DXVK builds, GPU drivers and Mods are not guaranteed. Frame Generation does not remove native simulation or input latency.

Required third-party license notices follow this guide. See [THIRD_PARTY.md](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/main/THIRD_PARTY.md) for dependency versions and full credits.
