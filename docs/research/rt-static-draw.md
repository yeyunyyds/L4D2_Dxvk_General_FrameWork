# 静态世界的 Host Draw 转换

这是实际 Host 绘制调用旁的转换组件，尚未安装游戏 Hook，也尚未验证 B580 输出。它复用固定 `l4d2-rtx` 提交中 `LightmappedGeneric` 世界路径的做法：清空顶点 shader，设置 `XYZ | NORMAL | TEX2`，提供该 Draw 的世界／视图／投影矩阵。没有把参考 mod 的全局临时状态或单线程 CVar 设置搬过来。

参考位置：`model_render.cpp` 的格式 `0x480003` 分支及其前面的三组矩阵设置，提交 `1fc8ded63a8491e9c1972eb2940d6cebbc20ad9e`。这是静态转换的第一小部分；材质、光照、可见性、动态对象等适配仍不完整。

## 执行位置与数据

`rt_host.h::drawCommand` 从现有有序流取出该实际 Draw 的自有 `DrawSnapshot`，交给 `rt_static_draw.h`，随后调用真实 D3D9 Draw，再恢复状态。Bridge 的四个 Draw 分支均经过这一入口；UP 路径只观察并保留原绘制，不执行本转换。

这些修改发生在 Host 的图形命令执行线程，不在 Source 准备下一帧的线程。不增加全局渲染锁，不修改 `mat_queue_mode`、粒子、detail 或绳索 CVar。它也不能证明当前游戏已经保持并行；实际 Source Hook／队列插入点仍待验证。

默认关闭。`LDB_RT_STATIC_DIAGNOSTIC=1` 在观察器创建时读取，只用于开发诊断。此开关不能验证匹配 runtime、开启硬件 RT 或代替综合包 A 的版本检查。没有对应元数据时不转换。

首版要求同时满足：

- 元数据 `DrawInfo.categories == kStaticOpaqueWorld (1)`，`modifiers == 0`，视图为 Main。分类必须由后续游戏适配器确认，不能只凭布局猜测材质或对象类型。
- 非 UP 的 triangle list／strip；stream 0 单流、非 instancing、40 字节步长。
- 精确声明：position float3 @0、normal float3 @12、UV0 float2 @24、UV1 float2 @32，结束项正确。
- 不启用 alpha blend／alpha test，开启深度和深度写入，viewport 尺寸匹配采集相机。
- 相机矩阵可逆、near／far 有效、世界变换数值有限。

不满足时保留原 Draw，并记录未覆盖原因；不把 decal／sprite 的 `0x80005` 路径混入静态世界。像素 shader、纹理、render target、viewport 和混合／深度状态不由本组件修改。其最终 RT 材质表现仍须单独验证。

## 矩阵约定与恢复

`DrawInfo.transform` 为 Source 行主序 3×4 仿射矩阵，作用于列向量；转换时转为 D3D9 行向量 4×4，平移放在最后一行。`Camera.view/projection` 已是 D3D9 行向量矩阵，按行展开；游戏侧使用 Source `VMatrix` 时必须显式转换，不能重复转置，也不能拿帧号相近的 shader 常量替代。

修改前保留顶点 shader、原 declaration、三组矩阵和必要 COM 引用。FVF 由 declaration 派生；恢复原声明，而非仅设置等价 FVF，避免 Windows 选择另一声明对象。成功、物理 Draw 失败和异常退栈均恢复。某个 setter 失败时仍尝试恢复所有已保存字段；恢复成功则执行原 raster Draw，同时保留转换失败。恢复失败会返回错误，不能继续用不确定的状态绘制或宣称成功。

本组件依赖既有 D3D9 资源与 backend 的生命周期，不新建完整 mesh／材质上传协议，也不声称解决动态资源的 GPU completion 释放问题。

## 诊断与验证范围

二进制 trace 新增两类记录，既有记录继续兼容：

| 事件 | 内容 |
| --- | --- |
| 12 | schema 1、静态诊断已请求，在元数据前写入 |
| 11 | schema、转换／跳过原因、capture／apply／restore HRESULT、物理 Draw HRESULT、是否转换／执行、frame／view／metadata sequence；记录头包含实际 Draw UID |

分析器核对 UID、帧、视图、序号、重复／缺失诊断以及最终 Draw 返回值。被跳过的已分类 Draw 令静态诊断为 `incomplete`；转换或恢复失败不能计作通过。即使状态诊断通过，`b580_rt`、`image_quality`、实际游戏队列和性能验收仍为 `unrun`。

开发检查包含：

- x86／x64 的转换、非单位旋转／缩放／平移、原 FVF 与任意 declaration 恢复，以及 COM 保留／释放。
- 布局、混合、深度、非主视图和无标注路径；12 个 getter 失败点、5 个 apply 失败点和5个 restore 失败点；异常退栈与物理 Draw 失败。
- 现有 512 帧 SDK 队列重叠用例接上 Host 转换；确认只在消费线程使用该帧的相机，并在下一个 Draw 前恢复。
- Windows 系统 D3D9 的可选 NULLREF state／Draw 往返：设备创建前的平台缺失记为 unavailable；设备创建后的接口／恢复失败不能跳过。NULLREF 不做 rasterization，也不执行 Remix RT。

原生提交 `9188a862a4128e425abe8119bca2ddab0fae2cf6` 的结果：

| 检查 | 已执行结果 |
| --- | --- |
| 完整 Bridge Release | x86 client／x64 Host 通过，PE、build ID、源输入 SHA 和 O2／NDEBUG 参数已复核 |
| 作用域故障与恢复 | x86／x64 各三组检查通过 |
| 真实系统 D3D9 NULLREF | x86／x64 均通过；恢复声明的 canonical IUnknown 身份，FVF 均从 530 恢复到 530，三组矩阵一致 |
| SDK 队列关联 | 512 帧通过，其中 511 次 Draw 发生时下一帧已发布；512 次转换与帧／视图／序号及物理 Draw 一致，且恢复完成 |
| 离线与本地 | 四份原生 trace 全部通过；71 项 Python 检查中 70 项通过、1 项留给 Windows 原生执行；严格 MinGW x64 交叉编译通过 |

原始 ZIP、二进制及 trace SHA、原因边界见 [静态转换验证记录](../../research/rt-static-draw-validation.json)。这些检查使用测试队列和同进程 COM／Host 适配器；NULLREF 检查单独调用系统 D3D9，二者不能合并推导实际跨进程 Remix 渲染已成功。

这是使用逐 Draw getter 的正确性诊断版本，不能用于目标 60 FPS 的性能验收。后续生产路径需要减少状态查询和重复设置，并在独立性能段验证。

综合包仍需当前游戏版本的可靠 Hook／context 绑定、静态世界分类、实际跨进程与 Remix/B580 执行、启动／恢复及采集协调器。当前不要求用户为此单独补采。
