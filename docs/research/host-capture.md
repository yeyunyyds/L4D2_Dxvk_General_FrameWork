# 独立 Host 颜色 / 深度采集原型

当前原型在独立 Windows x64 进程中加载指定 DXVK，创建测试窗口与 D3D9 device，绘制移动三角形并采集真实 Vulkan 图像。它不注入 L4D2，不修改游戏或 LDB 安装，不执行插帧。

## 已实现与验证范围

- 拒绝非 AMD64 DLL；根据 adapter 名选择设备，并核对真实 Vulkan physical device，避免仅凭可修改的 D3D9 设备描述认定 GPU。
- 使用完整上游 `ID3D9VkInteropDevice/Texture` ABI 查询颜色、深度、格式、usage、默认 layout 和提交队列。
- 在同一 VkDevice 内创建独立颜色/深度快照，实际执行 GPU 图像复制与 staging buffer 回读。验证三角形区域的绿色和深度 0.25、背景颜色和深度 1.0；背景随帧变化，可发现错误帧内容。
- 使用 DXVK 的 layout transition / flush / resource wait，提交时锁住 DXVK 队列，在独立 fence 完成后回读，再恢复源 layout。没有跨进程传递 Vulkan handle。
- 中途改变 backbuffer 分辨率并调用 Reset；先释放旧快照和 COM surface 引用，再为新 generation 创建快照。
- 每次启动生成唯一 run ID，保存输入/后端 SHA256、JSONL、stderr、DXVK 日志与最终结果。缺帧、旧日志、错误尺寸、失败的复制/恢复/Present 或超时均不能通过。

本轮使用官方 DXVK 2.6.1 x64 DLL，通过 **Wine 10 + Mesa 25.0.7 llvmpipe + Xvfb** 执行真实 Vulkan 调用。60 帧 D32 与 8 帧 D24 流程均完成一次 Reset；底层 Vulkan loader 确认加载 Khronos validation layer，日志没有 Validation Error / VUID。错误位数 DLL 测试返回预期错误。

这是软件 Vulkan 的功能证据。B580、原生 Windows、MSVC 构建、游戏世界深度识别、相机/HUD/运动矢量和 FG 执行仍未验证。测试使用诊断性 GPU 等待和整图回读，不能把其 CPU 时间或 FPS 当作生产性能。

证据见 [host-capture-validation-2026-10-12.json](../../research/host-capture-validation-2026-10-12.json)。Linux 系统默认 ICD 仍不可用；软件驱动只通过每次测试的局部环境配置启用，并没有替换系统驱动。

## 在 B580 上运行

已有交叉编译的程序可直接在 Windows x64 使用，无需本机 MSVC。将测试工具包解压，使用 Python 3 执行：

```bat
python scripts\run_host_capture.py --exe .\dxvk_host_capture.exe --dxvk "D:\SteamLibrary\steamapps\common\Left 4 Dead 2\bin\.l4d2bridge\d3d9vk_x64.dll"
```

把路径改为自己的 **DXVK x64 后端**，不要选择游戏侧 x86 Bridge Client。建议先用官方 2.6.1 后端；若使用自定义后端，结果中的 SHA256 用于区分构建。程序会显示独立测试窗口，默认采集 60 帧，并在第 30 帧缩小 backbuffer 后 Reset。

另测常见 D24 深度：

```bat
python scripts\run_host_capture.py --exe .\dxvk_host_capture.exe --dxvk "D:\path\d3d9vk_x64.dll" --depth-format d24
```

默认 adapter 匹配 `B580`，不自动回退其他 GPU。默认尺寸 640×360，不使用 MSAA。每次结果位于 `.research/captures/<唯一编号>/`，launcher 成功返回 0、失败返回非零。请保留整个运行目录，其中 `inputs.json` 记录实际命令与文件哈希，`result.json` 给出该次结论。

也可以不用 Python直接运行 EXE（需自行使用新的 run ID、保存退出码和日志）：

```bat
dxvk_host_capture.exe --dxvk "D:\path\d3d9vk_x64.dll" --adapter B580 --run-id b580-new-test-001 --frames 60
```

这个 EXE 不需要启动 Steam 或 L4D2，也不会安装/覆盖 DLL。失败时先看错误记录与同次 DXVK 日志，不把窗口出现或 COM device 创建成功当作复制校验成功。

## 从源码构建

在 x64 Visual Studio Native Tools 命令提示符中：

```bat
python scripts\prepare_research.py
python scripts\build_host_capture.py
```

该脚本的 MSVC 分支尚未执行。已经验证的 Linux 交叉编译路径：

```bash
bash scripts/setup_windows_cross_tools.sh
python3 scripts/build_host_capture.py
```

generated interop header 保留上游完整 declarations/vtable 和原许可，只替换两个内部 include。生成文件和 build record 位于 `.research/bin/`。EXE 的导入库仅包括 KERNEL32、msvcrt、USER32；Vulkan 和选定后端在运行时加载。

## 复现软件功能检查

Debian trixie 云环境可选执行：

```bash
bash scripts/setup_software_runtime.sh
python3 scripts/software_capture_smoke.py --validation
python3 scripts/software_capture_smoke.py --frames 8 --depth-format d24 --validation
python3 scripts/software_capture_smoke.py --negative-case wrong-arch
```

软件运行时从签名 Debian 仓库获取，在 workspace 解包；官方 DXVK release 经 TLS 下载，并固定首次获取的 archive SHA256。校验失败时不使用或替换预期摘要。Debian Wine wrapper 的绝对路径需要 workspace relocation，原 wrapper 保留；没有修改 Wine/DXVK 二进制或关闭 Vulkan 校验。

Wine 不暴露外部 layer 枚举，因此仅看到 DXVK 的 “Validation layers not found” 不能确定底层是否激活。本工具要求 native loader 日志中实际存在 `Insert instance layer "VK_LAYER_KHRONOS_validation"`，并检查错误输出。每个测试创建独立 Wine prefix / Xvfb，在结束时仅停止自己启动的进程。

## 下一步接入 LDB 的边界

独立程序证明 interop 与 GPU 快照机制在这一测试组合下成立，**没有取得 L4D2 主视图数据**。后续应将采集逻辑整理为默认关闭的 Host 诊断组件，在真实 D3D9 对象上记录 bind/clear/draw/viewport 与快照边界。

游戏 depth 可能为 attachment-only、被武器投影覆盖、包含 MSAA 或被 clear/复用；不能简单把 Present 时绑定的深度认定为世界深度。应先可视化并验证主世界 / 第一人称 / HUD 分界。当前快照只用于 transfer/readback，供 FG shader 采样时还要重新设计 sampled usage、image view、layout 和异步生命周期。

在 GPU 采集可用后，再加入与真实帧命令有序关联的相机元数据与静态世界运动重建。完整 FSR 设备 feature、独立队列与 Presenter 适配仍是单独工作；这个程序没有改变普通 DXVK 的设备 feature。
