# L4D2 DXVK General Framework — Frame Generation

这是实验性开发包，不是正式 Release。当前目标是在 LDB v1.3.4 x64 Host 上通过 Vulkan Layer 使用 FidelityFX FSR3 Frame Interpolation。

## 安装

1. 完全退出 L4D2、`L4D2Bridge64.exe` 和 `L4D2Bridge32.exe`；Steam 可以保持运行。
2. 备份游戏原有的 `bin` 文件夹。
3. 将本压缩包直接解压到 L4D2 游戏根目录，允许 `bin` 文件夹合并并覆盖同名文件。
4. 从 Steam 正常启动游戏，无需运行额外脚本。

Client、Host32 和 Host64 来自同一 LDB v1.3.4 构建，必须成套替换。若曾设置 `client.testX86Server=True`，请改回 `False`。

## 默认设置

配置文件：`bin/.l4d2bridge/fg-config.ini`

- `enabled=1`：启用帧生成。
- `hud=1`：启用 HUD 保护。
- `viewmodel=1`：启用第一人称手部和武器保护。
- `dynamic_models=1`：启用第三人称及动态人物保护。
- `monitor=0`：默认不启动监测窗口。
- `auto_capture=0`、`content_probe=0`：默认关闭自动截图和内容探针。

运行时按 `F8` 可以暂停或恢复 FG。`F8` 不是硬关闭；如需接近纯净 LDB v1.3.4 的直通路径，请在启动游戏前设置 `enabled=0`。

## 日志

正常日志位于：

```text
L4D2BridgeLogs/<会话 GUID>/frame-generation/
```

若需诊断，可在 INI 中单独打开 `monitor`、`auto_capture` 或 `content_probe`。Monitor 脚本已放在 `bin/.l4d2bridge`，无需另外下载。

## 已知限制

- Steam Overlay 不属于游戏 HUD 捕获层，其 FPS 文字可能在生成帧中闪烁。
- 当前使用相机运动、深度和官方 Optical Flow；尚无完整引擎骨骼运动矢量。
- 官方 DXVK v2.6.1 是当前验证基线。可以实验替换其他官方 DXVK，但不保证兼容。
- 本包不会修改或修复第三方游戏模组、物理引擎 DLL 或 ReShade。

项目、源码、许可证与第三方引用：
https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork
