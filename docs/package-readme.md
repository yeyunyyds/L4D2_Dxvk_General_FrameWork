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
| `dynamic_models` | `1` | 保护第三人称人物及动态模型 |
| `monitor` | `0` | 启用实时监测窗口 |

运行日志保存在：

```text
L4D2BridgeLogs/<会话 GUID>/frame-generation/
```
