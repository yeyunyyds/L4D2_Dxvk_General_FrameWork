# L4D2 DXVK General Framework

面向《Left 4 Dead 2》的实验性 x86 → x64 图形扩展框架。目前的首要成果是在 **L4D2Bridge（LDB）x64 Host** 中采集真实场景输入，并通过独立 x64 Vulkan Layer 接入 FidelityFX FSR 3 Frame Interpolation；光线追踪路线保留为后续研究分支。

> 本仓库仍处于开发验证阶段，暂未发布 Release。`main` 保存当前已验收的源码基线，不代表面向普通玩家的稳定发行版。

## 当前状态

### 帧生成

- 已同步到 LDB v1.3.4，Client、Host32 和 Host64 必须成套使用。
- x64 Host 从实际 D3D9 命令序列获取 HDR 场景颜色、深度、相机矩阵、HUD 和 viewmodel 边界。
- 颜色、深度、运动和覆盖资源保持在同一 GPU 内，不通过跨进程 IPC 复制整幅画面。
- 独立 x64 Vulkan Layer 管理 FSR3 replacement swapchain、三槽输入生命周期、异步队列和约 2:1 的输出提交。
- HUD 已稳定分层；第一人称武器和手部已完成保护。当前基线进一步保护第三人称与其他动态人物。
- 已在 Intel Arc B580 上观察到 FG 开启后 GPU 利用率上升，并完成可见画面、节奏和约 2 倍输出链验证。
- Steam Overlay 不在游戏 HUD 捕获边界内，其 FPS 文本仍可能在生成帧中闪烁。
- 最近一次长时间验收被自定义 `vphysics.dll` 的 x86 物理回调崩溃中断；日志未发现 Host、Vulkan、Present 或 FG 设备错误。该问题不归入当前 FG 基线。

这仍不是“任意机器即装即用”的成品。完整引擎物体/骨骼运动矢量尚未接入，快速运动、透明物体和第三方 Overlay 仍需持续测试。

### 光线追踪

仓库保留静态世界、材质、模型、跨进程协议和 Remix 运行时实验。独立 B580 静态 RT 已通过，但实时游戏 RT 尚未达到可发布 Beta，当前开发优先级低于帧生成稳定化。历史过程和实机证据保存在 [`docs/research`](docs/research/) 与 [`research`](research/) 中。

## 架构

```text
L4D2 x86
  -> LDB Client
  -> LDB x64 Host + official DXVK D3D9
       |  GPU scene/depth/camera/HUD/viewmodel inputs
       v
     GF x64 Vulkan Layer
       -> FidelityFX FSR3 Frame Interpolation
       -> Vulkan driver / display
```

GF 不再要求修改 DXVK 本体。当前包使用官方 DXVK v2.6.1，并只依赖其标准 D3D9/Vulkan interop；理论上允许替换其他官方 DXVK 版本进行实验，但不保证 interop ABI、功能或稳定性兼容。

## 使用方式

当前没有 Release，也不建议把历史 `downloads` 分支中的开发 ZIP 当作正式版本。已构建的开发包采用覆盖式目录结构：退出 L4D2 和 Bridge Host 后，将包内 `bin` 合并到游戏根目录的 `bin`，然后从 Steam 正常启动。Steam 无需退出，旧式安装/启动脚本仅用于隔离诊断。

配置文件位于：

```text
bin/.l4d2bridge/fg-config.ini
```

主要选项：

| 选项 | 默认值 | 作用 |
| --- | ---: | --- |
| `enabled` | `1` | 进程启动时启用 FG；设为 `0` 时不加载 Layer、Monitor 或 FG 日志路径，尽量接近纯净 LDB v1.3.4 |
| `hud` | `1` | 保护 HUD |
| `viewmodel` | `1` | 保护第一人称手部与武器 |
| `dynamic_models` | `1` | 保护第三人称及其他动态人物 |
| `content_probe` | `0` | 启用小规模生成帧差异探针；诊断用，会增加开销 |
| `monitor` | `1` | 自动启动实时监测窗口并写入 CSV |
| `merge_submissions` | `1` | 合并 Host GPU submission |

运行时按 `F8` 暂停或恢复帧生成。`F8` 只是热切换；只有在启动前设置 `enabled=0` 才是硬关闭。

每次会话的 Host、Layer、DXVK 与 Monitor 数据统一写入：

```text
L4D2BridgeLogs/<会话 GUID>/frame-generation/
```

完整安装、验收字段和日志解释见[帧生成 Host 接入文档](docs/research/fsr-host-integration.md)。

## 从源码构建

Windows 原生包需要 Visual Studio 2022、MSVC x86/x64 工具链、Python 3.12、CMake、Meson 和 Ninja。准备脚本会按固定提交拉取依赖并拒绝静默使用错误版本：

```powershell
python -m pip install meson==1.3.2 ninja==1.11.1.1
python scripts/prepare_research.py --only dxvk vulkan_headers
python scripts/prepare_ldb_capture.py
powershell -NoProfile -File scripts/build_ldb_capture.ps1
python -m unittest discover -s tests -v
```

`build_ldb_capture.ps1` 会准备并构建 Asteria x64 Layer、编译 LDB 三件套、运行安装/恢复夹具并调用 `scripts/package_ldb_capture.py` 打包。构建输出和第三方源码位于被忽略的 `.research/`；当前 CI 工作流仍服务于开发分支和开发制品，不等同于 Release。

Linux 可运行不依赖 Windows/MSVC 的研究和回归检查：

```bash
bash scripts/setup_research_tools.sh
python3 scripts/prepare_research.py
python3 scripts/build_probe.py
python3 -m unittest discover -s tests -v
```

## 仓库结构

- `integration/ldb/`：LDB v1.3.4 Host 补丁、FG 配置和输入/生命周期实现。
- `integration/fg/`：Asteria x64 Vulkan Layer 补丁及 HUD 合成资源。
- `integration/rt/`：实验性实时光追协议、世界与模型路径。
- `shaders/`、`tools/`：GPU 输入处理、插值保护和诊断工具。
- `scripts/`：固定源码准备、Windows 构建、打包和结果分析。
- `tests/`：Python、x86/x64 原生合同和回归用例。
- `docs/research/`、`research/`：设计记录、实机分析和机器可读证据。

## 上游项目与许可

本项目实际使用的固定源码、参考实现和各自角色列于 [`THIRD_PARTY.md`](THIRD_PARTY.md)。核心来源包括 L4D2Bridge、DXVK、FidelityFX SDK、Asteria、DXVK Remix 和 l4d2-rtx。

第三方代码和构建产物继续受各自上游许可证约束；本仓库中的补丁不会改变或替代这些许可证。构建包会随对应组件附带上游许可证。L4D2、Source Engine、Steam 及相关商标归其权利人所有。
