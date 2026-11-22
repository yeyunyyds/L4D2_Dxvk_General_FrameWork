# L4D2 DXVK General Framework

## 介绍

## v1.0.1 预备版（待验证）

当前 `main` 为 **v1.0.1 发布候选源代码**，尚未经过最终实机验证，**尚未正式发布 v1.0.1 Release**。验证通过后再决定正式发布。

本次基于已归档的 FPB 保护调优基线，保留原有 HUD、Viewmodel、动态像素与 FPB 保护（`dynamic_residual_threshold=0.2`、`first_person_fallback_start=1`、`dynamic_mask_dilation=1`，独立 FPB 投影默认关闭）；额外保留可热更新的 `generated_only` 实验开关，默认 `0`。开启后仅在 FG 正常生成时显示生成帧；FG 暂停或不可用时仍显示真实帧，显示帧率通常回到原生帧率。

**失败实验已撤回：** Viewmodel Sweep 的中间遮罩搜索未达到预期，未合并进 `main`。保留其原实验分支，仅供追溯；不宣称已解决武器残影或闪烁。

CI 在 `main` 上构建测试 ZIP 作为验证产物，**不会创建版本标签或 GitHub Release**。


L4D2 DXVK General Framework 为《Left 4 Dead 2》提供基于 FidelityFX FSR 3 的帧生成功能。

项目使用 L4D2Bridge v1.3.4 x64 Host 获取场景颜色、深度和相机数据，并通过 x64 Vulkan Layer 完成帧生成与显示。当前版本使用官方 DXVK v2.6.1，同时支持 HUD、第一人称手部与武器、第三人称人物及动态模型保护。

项目基于或参考以下开源项目：

- [L4D2_Dxvk_32to64_Bridge](https://github.com/yeyunyyds/L4D2_Dxvk_32to64_Bridge)
- [DXVK](https://github.com/doitsujin/dxvk)
- [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
- [L4D2 Asteria Graphics Enhancement](https://github.com/Anokianx/L4D2-Asteria-Graphics-Enhancement)
- [L4D2 Danyria Integration Tool](https://github.com/Anokianx/L4D2-Danyria-Integration-Tool)

完整引用、固定版本与许可证信息见 [THIRD_PARTY.md](THIRD_PARTY.md)。

## 安装

1. 从 [Releases](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/releases/latest) 下载最新版本。
2. 退出游戏以及 `L4D2Bridge64.exe`、`L4D2Bridge32.exe`；Steam 可以保持运行。
3. 备份游戏现有的 `bin` 文件夹。
4. 将压缩包解压到 Left 4 Dead 2 游戏根目录，合并并覆盖 `bin` 中的同名文件。
5. 从 Steam 正常启动游戏。

压缩包中的 Client、Host32 与 Host64 为配套版本，应当一并安装。若曾设置 `client.testX86Server=True`，请将其改回 `False`。

## 使用

帧生成在游戏启动时自动启用。运行过程中按 `F8` 可以暂停或恢复帧生成。

配置文件位于：

```text
bin/.l4d2bridge/fg-config.ini
```

| 选项 | 默认值 | 功能 |
| --- | ---: | --- |
| `enabled` | `1` | 启用帧生成；设为 `0` 可在下次启动时关闭 |
| `hud` | `1` | 保护游戏 HUD |
| `viewmodel` | `1` | 保护第一人称手部与武器 |
| `generated_only` | `0` | 实验性：仅显示生成帧（热更新） |
| `dynamic_models` | `1` | 保护第三人称人物及动态模型 |
| `monitor` | `0` | 启用实时监测窗口 |

运行日志保存在：

```text
L4D2BridgeLogs/<会话 GUID>/frame-generation/
```

---

# English

## Introduction

## v1.0.1 release candidate — validation pending

The `main` branch contains **v1.0.1 candidate source**. In-game validation is pending; **v1.0.1 has not been released**. A final release may follow successful validation.

This candidate preserves the archived tuned FPB protection baseline, including HUD, viewmodel, dynamic-pixel and FPB protection (`dynamic_residual_threshold=0.2`, `first_person_fallback_start=1`, `dynamic_mask_dilation=1`; independent FPB projection disabled by default). It adds only the opt-in, hot-reloadable `generated_only` experiment (default `0`). When enabled, only interpolated frames are presented while FG is working; original frames are still displayed when FG pauses or fails, and displayed FPS generally returns to source FPS.

**Withdrawn experiment:** Viewmodel Sweep did not meet expectations and is not merged into `main`. Its experiment branch remains available for traceability. No successful fix for viewmodel ghosting or flashing is claimed.

CI produces a test ZIP for validation but **does not create a version tag or GitHub Release**.


L4D2 DXVK General Framework adds FidelityFX FSR 3 Frame Generation to *Left 4 Dead 2*.

The project uses the L4D2Bridge v1.3.4 x64 Host to obtain scene color, depth, and camera data, then performs frame generation and presentation through an x64 Vulkan Layer. The current version uses the official DXVK v2.6.1 release and protects the HUD, first-person hands and weapons, third-person characters, and dynamic models.

The project is built upon or informed by the following open-source projects:

- [L4D2_Dxvk_32to64_Bridge](https://github.com/yeyunyyds/L4D2_Dxvk_32to64_Bridge)
- [DXVK](https://github.com/doitsujin/dxvk)
- [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
- [L4D2 Asteria Graphics Enhancement](https://github.com/Anokianx/L4D2-Asteria-Graphics-Enhancement)
- [L4D2 Danyria Integration Tool](https://github.com/Anokianx/L4D2-Danyria-Integration-Tool)

See [THIRD_PARTY.md](THIRD_PARTY.md) for complete credits, pinned versions, and license information.

## Installation

1. Download the latest version from [Releases](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/releases/latest).
2. Close the game, `L4D2Bridge64.exe`, and `L4D2Bridge32.exe`. Steam may remain open.
3. Back up the game's existing `bin` directory.
4. Extract the archive into the *Left 4 Dead 2* game directory, then merge and overwrite matching files in `bin`.
5. Launch the game normally from Steam.

The Client, Host32, and Host64 included in the archive are a matched set and must be installed together. If `client.testX86Server=True` was previously enabled, change it back to `False`.

## Usage

Frame Generation is enabled automatically at startup. Press `F8` during gameplay to pause or resume it.

Configuration file:

```text
bin/.l4d2bridge/fg-config.ini
```

| Option | Default | Description |
| --- | ---: | --- |
| `enabled` | `1` | Enables Frame Generation; set to `0` to disable it on the next launch |
| `hud` | `1` | Protects the game HUD |
| `viewmodel` | `1` | Protects first-person hands and weapons |
| `generated_only` | `0` | Experimental: generated frames only (hot-reload) |
| `dynamic_models` | `1` | Protects third-person characters and dynamic models |
| `monitor` | `0` | Enables the live monitoring window |

Runtime logs are stored in:

```text
L4D2BridgeLogs/<session GUID>/frame-generation/
```
