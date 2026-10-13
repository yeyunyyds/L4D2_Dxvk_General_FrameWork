# Windows / Arc B580 实机验证

现已提供可运行的[独立 Host 采集原型](host-capture.md)，可先验证 DXVK x64 后端的颜色/深度快照和 Reset，再继续本页 FSR 官方样例验证。

当前云环境完成 Linux 编译与 shader 校验；以下 Windows 命令与 GPU 测试尚未执行。probe 不启动游戏、不安装 DLL、不创建逻辑设备或交换链，仅查询物理设备信息，不能据此宣称 FG 可运行。

## 1. 生成 GPU 探测结果

在本仓库的 **x64 Visual Studio Native Tools Command Prompt** 中，确保 Python 3、Git 与 MSVC 可用：

```bat
python scripts\prepare_research.py
python scripts\build_probe.py
.research\bin\vulkan_probe.exe --match B580 > .research\b580-probe.json
echo %ERRORLEVEL%
```

build_probe 使用固定 Vulkan-Headers，自行动态加载 vulkan-1.dll，不需要拷贝 Vulkan DLL 或链接 Vulkan SDK import library。输出物理设备 API、驱动信息、queue family/count、storage image feature、subgroup、图像格式能力与扩展列表。`--match B580` 按设备名匹配，所有设备仍保留在 JSON 中，selected 表示匹配项。

退出码：0=成功观察到匹配设备；1=命令参数错误；2=loader/driver/API 查询失败；3=未找到匹配设备。查询成功不等于 FG 已支持。`logical_device_enabled_features=null`、`surface_present_support=null`、`gpu_fg_execution=unrun` 是刻意保留的未验证状态。

需要继续确认：至少三个互不相同的队列可供 FFX proxy 使用；目标 present queue 对真实窗口 surface 支持呈现；FSR 实際需要的 feature 在目标逻辑设备已启用。仅凭 queueCount 不能判断这三项。

## 2. 运行固定版本官方 FSR Vulkan 样例

研究脚本的 sparse checkout 只用于源码/GLSL 检查，**不具备完整样例构建依赖**。请在独立短路径建立完整 SDK checkout，避免修改研究缓存或游戏安装：

```bat
git clone https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git C:\fg-lab\FidelityFX-SDK
git -C C:\fg-lab\FidelityFX-SDK checkout --detach c6efa6bf7f2027b3ec94f28578bb5965eabb9e55
```

如果目标目录已有内容，不覆盖。先核对实际 HEAD 与干净状态。上述命令不部署任何游戏文件。

按 SDK [官方构建说明](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/docs/getting-started/building-samples.md) 安装 VS/Windows SDK、CMake（该版文档范围 3.17–3.30）和 Vulkan SDK（文档最低 1.3.239）。样例使用预构建签名 FidelityFX DLL；保留原版来源和签名，SDK 媒体下载按其官方工具处理。

从 SDK 根目录运行 `BuildSamplesSolutionVK.bat`。仅选择 FSR（此版本菜单编号 8）；runtime shader recompile 先选 n，其他选择按官方说明。该批处理交互生成解决方案；不声称它已在本轮验证。打开生成的解决方案，构建 `FFX_API_FSR` 的 `ReleaseVK`，并另跑 `DebugVK` 作为故障对照。目标输出对应 `FFX_API_FSR_VK` / `FFX_API_FSR_VKD`，以构建实际输出为准。

## 3. 样例验收矩阵

固定分辨率、显示器刷新率、驱动和样例输入。先使用 SDR、窗口模式，关闭 async compute，保留与 FG 状态相配的配置记录：

| 操作 | 观察与通过条件 |
|---|---|
| FG 关闭 → 开启 → 关闭 | 基线正常；开启后确实产生新中间帧；关闭后无资源/线程挂起 |
| 平移、快速转动、动态对象 | 插帧输出区别于复制/简单混合；遮挡边缘和动画无严重缺陷 |
| 各 UI composition 模式 | 文本/透明 UI 正常，记录闪烁/消失问题发生在哪个模式 |
| Alt-Tab、失去/恢复焦点 | 无依赖切焦点才能恢复的挂起，日志可定位异常 |
| 改分辨率、重建交换链、退出 | 排空/重建正常；没有无限等待或退出残留线程 |
| ReleaseVK 与 DebugVK 对照 | 区分错误与诊断开销；Debug 运行成功不代替 Release 稳定性 |

所选 SDK 已知部分 Arc 存在 Vulkan FSR 样例 UI/挂起问题。若重现，保存同次运行日志、具体 UI 模式/async 配置、驱动版本和操作步骤，先判断原始 SDK 行为，再处理游戏接入。不要通过关闭校验或恢复焦点后继续计为通过。

## 4. 记录与后续游戏验证

保留：Windows/Intel 驱动版本、B580-probe JSON、样例构建版本与配置、分辨率/刷新率/VRR、FG 开关、真实/生成/显示帧统计、GPU 耗时、完整错误日志。记录可重现步骤比单张 FPS 截图更有用。

样例稳定后再测 L4D2：保持相同地图/回放/视角、预热着色器，分别测原生 DXVK、现有 LDB、定制后端 FG 关闭、FG 开启。真实渲染率与显示率分开，不能用显示 FPS 翻倍解释游戏 tick 或输入延迟。

游戏下一步应先捕获/可视化世界深度、相机帧对应关系与 HUD-less 边界，再启用 FG。反复换图、设备 Reset 与窗口变化需要单独验收；这份硬件步骤不意味着游戏捕获已经完成。
