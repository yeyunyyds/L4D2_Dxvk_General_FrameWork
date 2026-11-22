# L4D2 DXVK General Framework

**FidelityFX FSR 3 Frame Generation for Left 4 Dead 2**

[Download latest release](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/releases/latest) · [Releases / Changelog](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/releases) · [Third-party licenses](THIRD_PARTY.md)

## 中文

### 简介

L4D2 DXVK General Framework（GF）为《Left 4 Dead 2》提供基于 **FidelityFX FSR 3** 的帧生成。项目通过 L4D2Bridge x64 Host 获取场景颜色、深度与相机信息，再由独立的 x64 Vulkan Layer 进行插帧和显示。

目前的安装包集成 **L4D2Bridge v1.3.4** 与官方 **DXVK v2.6.1**。GF 是画面增强组件；生成更多显示帧并不意味着游戏逻辑运行更快，也不会直接降低原始输入延迟。

### 功能

- **帧生成：** 利用游戏原始帧和渲染数据生成中间画面。
- **画面保护：** 对 HUD、第一人称手部与武器、人物及动态模型进行遮罩和合成处理，尽量减少插值伪影。
- **FPB 保护：** 提供可调的动态像素颜色保护；独立的相机投影保护为可选实验功能，默认关闭。
- **运行时控制：** 默认启用 FG；游戏中按 `F8` 暂停或恢复。
- **显示模式：** 可选 `generated_only` 模式仅显示插值帧，支持热更新；FG 暂停或失败时回退显示真实帧。

### 安装

1. 从 [Releases](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/releases/latest) 下载安装包。
2. 退出游戏与 `L4D2Bridge32.exe`、`L4D2Bridge64.exe`；Steam 可以保持运行。
3. 备份游戏的 `bin` 目录。
4. 将 ZIP 解压到 **Left 4 Dead 2 游戏根目录**，合并并覆盖 `bin` 中的同名文件。
5. 通过 Steam 正常启动游戏。

Client、Host32、Host64、DXVK 与 Vulkan Layer 必须使用安装包内的配套版本，不建议只替换其中一部分。此前若设置了 `client.testX86Server=True`，请恢复为 `False`。

### 配置

配置文件：`bin/.l4d2bridge/fg-config.ini`（`[frame_generation]`）。

| 参数 | 默认值 | 说明 |
| --- | ---: | --- |
| `enabled` | `1` | FG 总开关；设为 0 后需重新启动游戏 |
| `hud` | `1` | HUD 保护 |
| `viewmodel` | `1` | 第一人称手部与武器保护 |
| `dynamic_models` | `1` | 动态模型保护 |
| `fpb_protect` | `1` | FPB 动态像素保护 |
| `dynamic_residual_threshold` | `0.2` | 动态颜色差阈值；降低通常会扩大保护范围 |
| `first_person_fallback_start` | `1` | 下半屏回退保护起点，1 表示不启用回退 |
| `dynamic_mask_dilation` | `1` | 动态保护遮罩扩张 |
| `fpb_projection_protect` | `0` | 实验性身体投影保护 |
| `generated_only` | `0` | 实验性仅显示生成帧（支持热更新） |
| `monitor` | `0` | 实时监测窗口 |
| `mask_debug_white` | `0` | 在生成帧中以白色标识受到保护的像素 |

其他 FPB 位置、尺寸及参数范围详见 `fg-config.ini` 注释。`generated_only=1` 仅改变显示策略，**不能修复生成帧本身的残影**，正常显示帧率通常回到原生帧率。

### 日志与故障排查

会话日志：`L4D2BridgeLogs/<会话 GUID>/frame-generation/`。

遇到黑屏、冻结、画面异常或掉帧，请记录 GPU/驱动、游戏 Mod、配置文件和对应会话日志。可先使用默认配置，逐项停用实验选项排查。

### 兼容性与已知限制

- 面向 Windows 的 L4D2 和本项目提供的 Bridge/DXVK 配套构建；其他 DXVK、GPU、驱动或 Mod 组合不保证兼容。
- 快速移动、遮挡与自定义模型可能造成武器、人物或 HUD 残影和闪烁。
- 原生帧率过低时，即使显示帧数增加，操作响应仍受到原生帧率限制。
- `generated_only` 与 FPB 投影均为实验性可选功能。

### 致谢与许可证

本项目使用或参考 [L4D2Bridge](https://github.com/yeyunyyds/L4D2_Dxvk_32to64_Bridge)、[DXVK](https://github.com/doitsujin/dxvk)、[AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)、[Asteria Graphics Enhancement](https://github.com/Anokianx/L4D2-Asteria-Graphics-Enhancement) 和 [Danyria Integration Tool](https://github.com/Anokianx/L4D2-Danyria-Integration-Tool)。

第三方依赖、固定版本与许可证见 [THIRD_PARTY.md](THIRD_PARTY.md)。发布包 README 附有相应的第三方再分发声明。

---

## English

### Overview

**L4D2 DXVK General Framework (GF)** adds FidelityFX FSR 3 Frame Generation to *Left 4 Dead 2*. It captures scene color, depth, and camera information through an L4D2Bridge x64 Host and presents interpolated frames using a separate x64 Vulkan Layer.

The package integrates **L4D2Bridge v1.3.4** and upstream **DXVK v2.6.1**. Frame generation enhances presentation smoothness; it does not increase the game's simulation rate or eliminate native input latency.

### Features

- **Frame Generation:** Interpolated frames from source frames and game rendering inputs.
- **Visual protection:** HUD, first-person Viewmodel, characters, and dynamic-model masking and composition.
- **FPB protection:** Tunable dynamic-color protection and an optional camera-based body proxy (off by default).
- **Runtime control:** Press `F8` to pause or resume FG.
- **Optional display mode:** Hot-reloadable `generated_only` presents generated frames when FG is available, falling back to real frames otherwise.

### Installation

1. Download the latest package from [Releases](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/releases/latest).
2. Exit L4D2 and both `L4D2Bridge32.exe` and `L4D2Bridge64.exe`. Steam can stay open.
3. Back up the game's `bin` directory.
4. Extract the ZIP into the *Left 4 Dead 2* game root and merge matching files in `bin`.
5. Launch normally through Steam.

Install the included Client, Host32, Host64, DXVK, and Vulkan Layer together. If `client.testX86Server=True` was previously enabled, restore it to `False`.

### Configuration

Edit `bin/.l4d2bridge/fg-config.ini` under `[frame_generation]`.

| Setting | Default | Description |
| --- | ---: | --- |
| `enabled` | `1` | FG master switch; restarting is required after disabling |
| `hud` | `1` | HUD protection |
| `viewmodel` | `1` | First-person hands and weapon protection |
| `dynamic_models` | `1` | Dynamic model protection |
| `fpb_protect` | `1` | FPB dynamic-pixel protection |
| `dynamic_residual_threshold` | `0.2` | Dynamic color threshold; lower generally expands coverage |
| `first_person_fallback_start` | `1` | Lower-screen fallback start; 1 disables this fallback |
| `dynamic_mask_dilation` | `1` | Dynamic mask expansion |
| `fpb_projection_protect` | `0` | Experimental body proxy protection |
| `generated_only` | `0` | Experimental generated-only presentation; hot-reloadable |
| `monitor` | `0` | Monitoring window |
| `mask_debug_white` | `0` | Show protected generated-frame pixels in white |

See the INI comments for additional FPB projection settings and valid ranges. `generated_only=1` changes presentation, **not interpolation quality**; displayed FPS normally returns to the source rate.

### Logs and troubleshooting

Session logs: `L4D2BridgeLogs/<session GUID>/frame-generation/`.

For black screens, freezes, artifacts or unexpected performance, collect the GPU/driver details, installed Mods, configuration and session logs. Start from the default configuration and disable experimental options one by one if needed.

### Compatibility and limitations

- Designed for Windows L4D2 with the bundled Bridge/DXVK versions. Other drivers, GPUs, DXVK builds, and Mods may not work.
- Rapid motion, occlusion and custom models can still produce ghosting and flicker.
- More displayed frames do not remove native input or simulation latency.
- Generated-only presentation and projected FPB protection remain experimental.

### Credits and licenses

Built upon or informed by [L4D2Bridge](https://github.com/yeyunyyds/L4D2_Dxvk_32to64_Bridge), [DXVK](https://github.com/doitsujin/dxvk), [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), [Asteria Graphics Enhancement](https://github.com/Anokianx/L4D2-Asteria-Graphics-Enhancement), and [Danyria Integration Tool](https://github.com/Anokianx/L4D2-Danyria-Integration-Tool).

See [THIRD_PARTY.md](THIRD_PARTY.md) for pinned dependencies and licensing. Required redistribution notices are also included in the release package README.
