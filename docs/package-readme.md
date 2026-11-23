# L4D2 DXVK General Framework

**v1.0.2 — FidelityFX FSR 3 Frame Generation for Left 4 Dead 2**

Project / 项目主页：https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork

## 中文说明

**v1.0.2：**修复 NVIDIA 驱动 Vulkan Device Feature Chain 导致的 FG 初始化失败；保留既有渲染和画面保护逻辑。Alt+Tab 后可能冻结、额外延迟与 MSAA 捕获限制尚未在此补丁中修复。

GF 通过 L4D2Bridge v1.3.4 获取场景颜色、深度和相机信息，再通过 FidelityFX FSR 3 与 x64 Vulkan Layer 生成中间帧。此安装包包含配套的 Client、Host32、Host64、官方 DXVK v2.6.1 和 Vulkan Layer，提供 HUD、第一人称武器、动态模型及 FPB 画面保护。

### 安装

1. 退出游戏与 `L4D2Bridge32.exe`、`L4D2Bridge64.exe`（Steam 可保持运行）。
2. 备份现有游戏 `bin` 目录。
3. 将安装 ZIP 解压到 **Left 4 Dead 2 游戏根目录**，按需合并并覆盖 `bin` 中的文件。
4. 在 Steam 中正常启动游戏。

请成套安装所有组件。此 ZIP **不包含也不覆盖** `bin/.l4d2bridge/bridge.conf`。请检查该文件的 LDB Host 配置，确保：
```ini
forceX64Server = True
client.testX86Server = False
server.useVanillaDxvk = True
```
这些是 **Bridge 配置项**，不能写入 `fg-config.ini` 的 `[frame_generation]` 下。修改后完整退出游戏和 Bridge Host 再启动。确认 Host 日志显示加载 `d3d9vk_x64.dll`，而非 Windows 系统 `d3d9.dll`。

当前 FG 捕获路径要求单采样图像。如果日志出现 `Capture requires a single-sample, single-subresource transfer-source image`，请先将游戏内 MSAA/抗锯齿设为关闭并重启；本测试版尚未实现多采样图像捕获兼容。

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

**v1.0.2:** Fixes the Vulkan device feature-chain compatibility blocker observed on NVIDIA drivers. Existing rendering and protection paths are unchanged. Alt+Tab display freezes, additional latency and MSAA capture limitations are not fixed by this patch.

GF provides FidelityFX FSR 3 Frame Generation for *Left 4 Dead 2*, using L4D2Bridge v1.3.4 and an x64 Vulkan Layer. This package includes matched Client/Host32/Host64 components, upstream DXVK v2.6.1, and HUD, Viewmodel, dynamic-model, and FPB protection.

### Installation

1. Exit L4D2, `L4D2Bridge32.exe` and `L4D2Bridge64.exe`; Steam can stay open.
2. Back up your game `bin` directory.
3. Extract the ZIP into the *Left 4 Dead 2* game root, merging matching files in `bin`.
4. Launch normally through Steam.

Install the bundled components as a matched set. The ZIP **does not contain or overwrite** `bin/.l4d2bridge/bridge.conf`. Verify these LDB Host settings in that file:
```ini
forceX64Server = True
client.testX86Server = False
server.useVanillaDxvk = True
```
These belong in **`bridge.conf`**, not under `[frame_generation]` in `fg-config.ini`. Completely exit L4D2 and the Bridge Host before restarting. The Host log must show `d3d9vk_x64.dll`, not Windows `d3d9.dll`.

The current FG capture path requires single-sample images. If the log reports `Capture requires a single-sample, single-subresource transfer-source image`, disable in-game MSAA/anti-aliasing and restart. Multisampled capture is not yet supported by this test build.

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
