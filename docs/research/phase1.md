# 第一阶段：FSR Vulkan 接入研究

日期：2026-10-12。当前成果是源码研究、可复现 shader 验证与探测工具；Windows/B580 实测仍未完成。

## 决策

固定普通 DXVK 2.6.1 与 FidelityFX SDK v1.1.4，先研究保留 L4D2 原始光栅渲染的 2× FG。该 SDK 对应 FSR 3.1.4 接入文档，不能把 SDK 版本误认为算法版本。x64 Host 保持 GPU 资源在同一进程与设备内，x86 接入仅提供主视图时序元数据。依赖完整 SHA 见 [sources.lock.json](../../research/sources.lock.json)。

第一原型不启用 Remix 场景重建，不强制 CPU 蒙皮或关闭引擎多线程，不同时引入超分、HDR、分屏或多倍插帧。动态角色画质是后续独立验收项；静态场景原型不代表可玩版本。

## DXVK 已有 interop，减少诊断改动

DXVK 的真实 Device 可 QueryInterface 为 `ID3D9VkInteropDevice`，真实 Surface 可 QueryInterface 为 `ID3D9VkInteropTexture`。已有 `GetVulkanHandles`、`GetSubmissionQueue`、`GetVulkanImageInfo`、`TransitionTextureLayout`、`FlushRenderingCommands` 与队列锁。

证据：[interfaces](https://github.com/doitsujin/dxvk/blob/2b1a284f8453baa2bd193709b67e5183074c74ba/src/d3d9/d3d9_interfaces.h#L125)、[interop 实现](https://github.com/doitsujin/dxvk/blob/2b1a284f8453baa2bd193709b67e5183074c74ba/src/d3d9/d3d9_interop.cpp#L109)、[Surface QI](https://github.com/doitsujin/dxvk/blob/2b1a284f8453baa2bd193709b67e5183074c74ba/src/d3d9/d3d9_surface.cpp#L78)。

接口只能在 **Host 的真实 COM 对象**上使用。Bridge Client 的 Device QueryInterface 不转发此接口；不能把 x64 Vulkan handle 传到 x86 当作同进程资源使用。Bridge 固定上游 [Client Device](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/9aa74f8dfad2188efbd0f717c64d9f8fa909787e/bridge/src/client/d3d9_device.cpp#L114) 及当前累计补丁均未增加此 interop。

`GetVulkanImageInfo` 返回图像定义中的 layout，不等于任意时刻 GPU 实际临时 layout。它拒绝带 pNext 的 VkImageCreateInfo。不能借助这份信息推断资源已完成，或恢复全部 mutable-format 创建信息。

提交前应完成需要的 D3D9 调用、layout 命令和 flush，再锁提交队列执行 Vulkan 提交，立即解锁。持有提交锁时再次调用 D3D9 方法有死锁风险；flush 不是 GPU 空闲，图像读写仍需 GPU barrier/fence。SDK 的异步线程和 DXVK 提交线程需要另行设计锁与提交顺序。

## 颜色与深度的候选接入点

| 数据 | 具体源码位置 | 研究结果 |
|---|---|---|
| 最终颜色 | `D3D9SwapChainEx::PresentImage`，`src/d3d9/d3d9_swapchain.cpp:824` | 在 blit 前拥有游戏 backbuffer 的 DxvkImage/View；WSI 图像是另一资源 |
| 深度绑定 | `D3D9DeviceEx::SetDepthStencilSurface`，device.cpp:1731 | 可记录绑定/资源身份，不能因此断定当前就是主世界深度 |
| 深度被清除 | `D3D9DeviceEx::Clear`，device.cpp:1827 | 可识别 Z/stencil 清除并在覆盖前安排 GPU 快照 |
| 深度实际格式/usage | `D3D9CommonTexture::CreatePrimaryImage`，common_texture.cpp:330 | 默认有 TRANSFER_SRC/DST；attachment-only 深度可能没有 SAMPLED usage |
| HUD-less color | 游戏侧 HUD 开始前 | 尚未实机定位；Present/EndScene 都不是可靠的通用 HUD 分界 |
| 相机元数据 | 游戏侧主视图 Hook | 参考 l4d2-rtx 的 View/Model 接入，但不整体继承固定功能改造 |

证据：[PresentImage](https://github.com/doitsujin/dxvk/blob/2b1a284f8453baa2bd193709b67e5183074c74ba/src/d3d9/d3d9_swapchain.cpp#L824)、[Device](https://github.com/doitsujin/dxvk/blob/2b1a284f8453baa2bd193709b67e5183074c74ba/src/d3d9/d3d9_device.cpp#L1731)、[图像创建](https://github.com/doitsujin/dxvk/blob/2b1a284f8453baa2bd193709b67e5183074c74ba/src/d3d9/d3d9_common_texture.cpp#L330)。

先用 Host 侧 bind/clear/draw/viewport 统计识别候选深度，再以可视化验证。第一版关闭 MSAA；不能直接用 vkCmdCopyImage 把任意 D24/S8 深度转换成 R32_FLOAT。深度复制应保持兼容 depth 格式/aspect，再采样或转换到独立的工作纹理。颜色也需明确 sRGB/UNORM 与 FSR transfer function。

## 需要定制后端的三处确定差异

### 1. 逻辑设备 feature

本轮 102 个 shader 输出使用的 SPIR-V capability 包括 `StorageImageExtendedFormats`、`StorageImageReadWithoutFormat`、`StorageImageWriteWithoutFormat`、subgroup arithmetic/quad。

普通 DXVK 的 [D3D9DeviceEx::GetDeviceFeatures](https://github.com/doitsujin/dxvk/blob/2b1a284f8453baa2bd193709b67e5183074c74ba/src/d3d9/d3d9_device.cpp#L4518) 从零初始化 enabled feature，未启用上述三个 storage image feature；[Adapter createDevice](https://github.com/doitsujin/dxvk/blob/2b1a284f8453baa2bd193709b67e5183074c74ba/src/dxvk/dxvk_adapter.cpp#L275) 也没有补齐它们。

因此，即便物理 GPU 支持这些能力，也不能直接在原 Device 创建相应 FSR pipeline。首版 FG 后端必须在 device 创建前按实际 shader/资源需求检查并启用；不能事后启用。FP16 仅在对应 feature 已启用后选择，第一版暂以 FP32 为研究范围。

### 2. 队列数量与所有权

DXVK 每个选定队列族只请求一个队列，graphics/transfer/sparse 使用 index 0。FSR Vulkan proxy 的初始化则显式拒绝 game/present/image-acquire 等队列句柄相同。**关闭 async compute 仍至少需要 game、present、image-acquire 三个不同的非空队列句柄。**

证据：[DXVK queueCount=1](https://github.com/doitsujin/dxvk/blob/2b1a284f8453baa2bd193709b67e5183074c74ba/src/dxvk/dxvk_adapter.cpp#L485)、[FFX 队列限制](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/src/backends/vk/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK.cpp#L1496)、[队列接口说明](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/include/FidelityFX/host/backends/vk/ffx_vk.h#L51)。

不能借用正在使用的 DXVK transfer queue 当作 FSR 私有 present 队列；也不能对未在 vkCreateDevice 请求的 index 调用 vkGetDeviceQueue。优先研究同一支持 WSI 的 graphics family 预留两个额外队列；是否可用取决于 B580 物理队列数量、surface 支持和驱动。如果不满足，应调整队列拓扑或改写呈现方案，而不是强行复用同一 handle。

### 3. 交换链与 Present 扩展链

DXVK 的 [Presenter::presentImage](https://github.com/doitsujin/dxvk/blob/2b1a284f8453baa2bd193709b67e5183074c74ba/src/dxvk/dxvk_presenter.cpp#L169) 可提交 VkPresentIdKHR、VkSwapchainPresentFenceInfoEXT 和 VkSwapchainPresentModeInfoEXT。FFX 在 passthrough 分支 [断言 pNext 为空](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/src/backends/vk/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK.cpp#L2648)，其真实呈现构造也不转发这些对象。

首版需要在独立 FG Presenter 路径中选择兼容的 frame-latency/完成信号策略，或为 FFX 实现扩展链处理。不能仅清空 pNext：那会丢失 DXVK 等待的 fence/present ID，可能挂起或破坏资源复用。即使 FG 暂时关闭，proxy passthrough 也需要正确处理。

FFX proxy 的 VkSwapchainKHR 实际承载代理对象地址。create/get-images/acquire/present/destroy/HDR 应统一走 replacement functions；不能把代理 handle 交给真实驱动的 vkWaitForPresentKHR 等函数。DXVK 2.6.1 的 Presenter、frame-latency signal 与错误恢复要成套适配，不能原样移植旧 Remix `src/vulkan` 的实现。

## B580 风险：官方已知问题

[所选版本 README](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/readme.md#L63) 明确记录：FSR API Vulkan Release 样例在部分 Intel Arc 上可能 UI 闪烁/消失，以及偶发挂起。没有说明具体影响哪一代 Arc，因此既不能断定 B580 必然受影响，也不能假定已经修复。

先运行官方样例，将 FG 关闭/开启、UI composition、窗口焦点变化和分辨率重建分别验证；失败时区分原始样例问题与游戏集成问题。这是采用该 SDK 的硬件验证关卡。

## 本轮实际验证

| 检查 | 结果 | 证据范围 |
|---|---|---|
| 固定 SHA 拉取与准备脚本 | 新缓存成功；重复执行成功 | 三个依赖固定版本，不覆盖已有修改 |
| 官方 FG GLSL | 11 passes × 8 variants = 88，通过 | FP32，low-res MV / jitter / inverted depth 的组合 |
| 官方 optical-flow GLSL | 7 passes × SDR/HDR = 14，通过 | 同时运行 spirv-val；不是 GPU 执行 |
| Vulkan probe | Linux g++ 编译成功 | 动态加载系统 loader；Windows 分支尚未编译/运行 |
| 云 GPU 探测 | 未通过：vkCreateInstance 返回 -9 | VK_ERROR_INCOMPATIBLE_DRIVER；无可用 ICD，FG 执行为 unrun |
| probe 错误传播 | 错误参数返回 1，无效 ICD 返回 2 | 不会把无 GPU 状态标成 passed |
| 源码保护回归 | 4/4 通过 | 拒绝错误 commit、修改/未跟踪文件与父仓库误识别 |

shader 检查直接使用官方 GLSL，未修改 shader、关闭断言、禁用 TLS 或跳过包签名校验。使用签名 Debian 软件源下载并在 workspace 解包工具，不安装到系统目录。完整 logs/SPIR-V 留在 `.research/shaders/<run-id>/`；可携带的结果摘要见 [validation JSON](../../research/validation-2026-10-12.json)。未构建完整 SDK host library、DXVK DLL 或 Bridge Windows 二进制。

## 下一实现边界

1. B580 官方样例验证与 queue/feature 观测并行于源码开发，不把物理能力探测当作 FG 成功。
2. 增加默认关闭的 Host 数据诊断：真实 device interop、backbuffer/depth 描述、主视图 bind/clear 序列。先不替换交换链、不上传整图到 IPC。
3. 用 [帧输入契约](frame-input-contract.md) 定义版本化主视图元数据与快照边界，保持原有 Bridge 不开启 FG 时的行为。
4. 再开发 opt-in DXVK FG device/queue/Presenter 分支，从无 async、固定 SDR 尺寸、静态世界相机运动开始。

只有获取可信的颜色/深度/相机输入、验证 SDK 在 B580 运行，并证明新增呈现和恢复正确后，才进入游戏插帧可玩质量验收。当前不对具体工期、帧率增益或 B580 兼容性作承诺。
