# L4D2 DXVK General Framework

## 介绍

L4D2 DXVK General Framework 为《Left 4 Dead 2》提供基于 FidelityFX FSR 3 的帧生成功能。

本安装包包含配套的 L4D2Bridge v1.3.4 Client、Host32 与 Host64，并通过 x64 Vulkan Layer 完成帧生成与显示。当前版本使用官方 DXVK v2.6.1，同时支持 HUD、第一人称手部与武器、第三人称人物及动态模型保护。

项目基于或参考以下开源项目：

- [L4D2_Dxvk_32to64_Bridge](https://github.com/yeyunyyds/L4D2_Dxvk_32to64_Bridge)
- [DXVK](https://github.com/doitsujin/dxvk)
- [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
- [L4D2 Asteria Graphics Enhancement](https://github.com/Anokianx/L4D2-Asteria-Graphics-Enhancement)
- [L4D2 Danyria Integration Tool](https://github.com/Anokianx/L4D2-Danyria-Integration-Tool)

完整引用、固定版本与许可证信息见[项目仓库](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/main/THIRD_PARTY.md)。

## 安装

1. 退出游戏以及 `L4D2Bridge64.exe`、`L4D2Bridge32.exe`；Steam 可以保持运行。
2. 备份游戏现有的 `bin` 文件夹。
3. 将本压缩包解压到 Left 4 Dead 2 游戏根目录，合并并覆盖 `bin` 中的同名文件。
4. 从 Steam 正常启动游戏。

压缩包中的 Client、Host32 与 Host64 应当一并安装。若曾设置 `client.testX86Server=True`，请将其改回 `False`。

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
| `viewmodel_sweep` | `1` | **实验性**：在前后 Viewmodel 掩码之间补充中间帧保护（热更新） |
| `viewmodel_sweep_radius` | `16` | 扫掠半距离，4–24 像素、步长 4；越高 GPU 开销可能越大 |
| `generated_only` | `0` | **实验性**：改为只显示生成帧（热更新）；插帧不可用时允许原帧回退 |
| `dynamic_models` | `1` | 保护第三人称人物及动态模型 |
| `monitor` | `0` | 启用实时监测窗口 |

运行日志保存在：

```text
L4D2BridgeLogs/<会话 GUID>/frame-generation/
```

---

# English

## Introduction

L4D2 DXVK General Framework adds FidelityFX FSR 3 Frame Generation to *Left 4 Dead 2*.

This package contains a matched L4D2Bridge v1.3.4 Client, Host32, and Host64. Frame generation and presentation are provided through an x64 Vulkan Layer. The current version uses the official DXVK v2.6.1 release and protects the HUD, first-person hands and weapons, third-person characters, and dynamic models.

The project is built upon or informed by the following open-source projects:

- [L4D2_Dxvk_32to64_Bridge](https://github.com/yeyunyyds/L4D2_Dxvk_32to64_Bridge)
- [DXVK](https://github.com/doitsujin/dxvk)
- [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
- [L4D2 Asteria Graphics Enhancement](https://github.com/Anokianx/L4D2-Asteria-Graphics-Enhancement)
- [L4D2 Danyria Integration Tool](https://github.com/Anokianx/L4D2-Danyria-Integration-Tool)

See the [project repository](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/main/THIRD_PARTY.md) for complete credits, pinned versions, and license information.

## Installation

1. Close the game, `L4D2Bridge64.exe`, and `L4D2Bridge32.exe`. Steam may remain open.
2. Back up the game's existing `bin` directory.
3. Extract this archive into the *Left 4 Dead 2* game directory, then merge and overwrite matching files in `bin`.
4. Launch the game normally from Steam.

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
| `viewmodel_sweep` | `1` | **Experimental**: bracket A/B masks to cover generated-frame midpoints; hot reload |
| `viewmodel_sweep_radius` | `16` | Half-distance in pixels, 4–24 in steps of 4; costs extra GPU work |
| `generated_only` | `0` | **Experimental**: present generated frames only; hot reload; real-frame fallback when FG unavailable |
| `dynamic_models` | `1` | Protects third-person characters and dynamic models |
| `monitor` | `0` | Enables the live monitoring window |

Runtime logs are stored in:

```text
L4D2BridgeLogs/<session GUID>/frame-generation/
```
