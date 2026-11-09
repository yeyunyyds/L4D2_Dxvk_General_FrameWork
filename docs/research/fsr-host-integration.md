# 官方 FSR Host 接入测试包

本包把官方 FSR Optical Flow 与 Frame Interpolation 接到 x64 Host 的实时 Vulkan 资源和实际 Present 路径。配套 DXVK 2.6.1 在 `LDB_FG_BACKEND=fsr` 时启用所需设备能力；不修改游戏渲染线程设置，不引入整套 RTX 兼容层。

前版已在 B580 游戏内完成 174 次生成帧 Present；本轮区域保护改动仍待实机验收。尚未证明完整动态场景画质、显示节奏或帧率收益。

## 安装与一次测试

1. 解压到独立文件夹，完全退出游戏和两个 Host，运行 `install_fg_test.cmd`。脚本自动定位 L4D2、校验包内 Host、后端与 Vulkan Layer 文件，并将八个目标文件的原内容和哈希保存到游戏目录的 `.ldb-fg-test-backup`；找不到非默认 Steam 库时，把游戏根目录拖到 CMD 上或作为第一个参数传入。
2. 完全退出 Steam，再运行 `start_fg_test.cmd`，进入地图正常游玩约一分钟，覆盖转头、平移、开枪、换弹及之前出现圆环特效的位置。本轮由 x64 Host 采集主视图相机、真实帧间隔、深度资源身份和 HUD 边界，通过同进程 ABI 控制 Vulkan Layer；只有 Layer 接管 replacement swapchain，Host 不额外 Present；无需按 F8 或 F9。
3. 正常退出，压缩完整 `fg-results` 上传。启动脚本会在新的会话目录同时收集 Bridge、DXVK 与 FG 日志。无需 `capture.trigger`，也无需先测帧率收益。
4. 测试结束且游戏和 Host 已退出时运行 `remove_fg_test.cmd`。脚本先确认八个测试文件仍与本包完全相同，再恢复原文件并删除 `.ldb-fg-test-backup`。若文件在测试后被其他工具修改，脚本会停止而不会覆盖后来的文件。

必须使用 `L4D2Bridge64.exe`。若曾设置 `client.testX86Server=True`，改回 `False`；本包保留现有 `bridge.conf`。32 位 Host 随包保持 Client/Host 版本匹配，未接入 FSR。

已经手工合并旧测试包的用户，应先使用当时的手工备份恢复，再运行本版 `install_fg_test.cmd`；否则自动备份保存的会是旧测试 DLL。改用旧启动方式时清除 `LDB_FG_BACKEND` 或设为 `custom`，以免把旧 DXVK 当成具备新能力的后端。

## 接入内容与保护

- 直接使用 Host GPU 历史，不通过 IPC 传输整幅图像，不为插帧上传/回读整幅游戏画面。有限诊断截图是例外。
- GPU 转换颜色、深度和运动输入；运动来自相机重建及有限范围图像匹配，官方光流继续处理图像运动。没有取得完整引擎物体/骨骼运动矢量。
- 通过已定位的 HUD 前快照向 SDK 提供 HUD-less 颜色。当前和前帧武器覆盖区保留当前原帧，完整透明层分解尚未实现。
- 保留深度／覆盖质量预检：过多无效场景、异常前景覆盖仍恢复完整原帧。细特效风险在 FSR 路径改为区域保护：可疑细亮区域及前帧旧位置的邻域保留当前原帧，避免单个风险点导致整帧拒绝和历史反复预热。该识别依据亮线形态，不等同于引擎材质标签，可能保守地保留普通白色细节；透明层仍未完整分解。自研后端继续整帧拒绝细特效风险。SDK 合成后，已验证的前帧遮挡填补区域沿用预检重投影结果；缺乏可靠运动的纯色突变区域也有原帧保护。因此当前交付是官方 FSR 与保守保护的组合。
- 首帧、帧编号断续、HUD 输入变化或 Reset 时重置历史。预热帧不会增加一次生成帧 Present。
- Host 检查真实 DXVK 逻辑设备已启用的能力，并要求支持区域保护的后端接口 v2；旧版 DLL 会被拒绝，不能只更新 Host。缺少配套模块/能力或 SDK 出错时记录 `fg_error`，停止该会话插帧并保持原始 Present 路径。

配套 DXVK 增加三个 storage-image feature，以及在启用 Vulkan memory model 时使用 Device scope 所需的能力。仅物理 GPU 宣称支持不够，所以使用导出查询检查真实逻辑设备的 enabled features。

## Danyria presenter 迁移基线

当前包新增独立的 **x64 Vulkan Layer 完整路径**：`L4D2Bridge64 -> Vulkan loader -> GF/Asteria layer -> Intel Vulkan driver`。它直接拦截 `vkCreateDevice`、swapchain、acquire 与 present，在 Host 进程内创建官方 FSR3 replacement swapchain；不要求客制化 DXVK 暴露私有插帧接口。`start_fg_test.cmd` 会在保留用户现有 Vulkan Layer 搜索路径的同时加入本包、启用 Layer，并将 `asteria-layer.json` 写入本次 `fg-results`。

Asteria 原版只构建 x86 DLL，并要求同一个 graphics family 至少有四条 queue。本移植从固定 MIT 源码提交重新构建 AMD64 DLL；创建设备时申请所有 graphics/compute/transfer 候选 queue，拿到真实 Win32 surface 后再检查 present support，并为 game、async compute、present、image acquire 分配四条互不相同的物理 queue。各角色保留自己的 family index，交给官方 FFX 的跨 family ownership-transfer 路径。Arc B580 已观测到的 `1 + 4 + 2` 物理拓扑满足数量要求，最终能否启用由该次 surface 的 present-support 实测决定。

当前闭环已把 Asteria/Danyria 的 D3D9 语义采集方式移到 LDB x64 Host。Host 在实际命令序列中识别 HDR 主视图、深度表面、相机 shader 常量、HUD 前边界和 Present 时间，再调用 `AsteriaSubmitHostFrameV2`。Layer 会采用 Host 提交的相机、near/far/FOV、单位、真实 delta 和历史重置信号；图像不经过 IPC 或 CPU 复制。

本版由 Host 在同一个 Vulkan 设备上把主场景深度转换成 `R32F`，并用当前/上一主视图矩阵生成 `RG32F` 相机运动向量。ABI v2 同时携带设备、图像、格式和布局；Layer 逐项核对后直接把资源交给 FFX，拒绝跨设备、空句柄、错误格式或非只读布局。Layer 的 Vulkan 猜测采集仅保留为回退诊断，不参与通过 Host v2 验证的帧。

`LDB_FG_MODE=provider` 表示 Host 只生产数据且只调用一次原始 Present，不再运行旧预览或第二次 Present。`asteria-layer.json` 中同时满足以下条件，才能证明 Host 控制的算法与提交链已工作：

- `host_frame_accepted > 0` 且 `host_frame_consumed > 0`；
- swapchain 的 `host_camera_control_active=true`、`host_depth_resource_connected=true`，且 `motion_input_source=1`（Engine）；
- `interpolation_dispatches > 0`，并且没有 native Present 失败；
- `output_presents_per_input` 接近 2。

Host 日志还会记录 `gpu_motion_pixels`；转头或平移时它应大于零，用来排除全零运动输入。上述条件证明 Host 真深度／运动输入、官方算法调度和约 2:1 输出提交链。它们仍不证明显示器实际扫描了全部帧，也不能单独排除生成图等于相邻原帧。`generated_content_distinctness=unverified_no_gpu_content_probe` 会保持未验证，直到 Layer 的 GPU 内容差异探针完成。分析器当前最高结论为 `host_depth_motion_algorithm_and_extra_present_confirmed`，不会把提交计数写成物理显示验收。

本轮开始迁移 Danyria/Asteria 的 replacement-swapchain 路径。设置 `LDB_FG_PRESENTER=1` 后，配套 x64 DXVK 会从实际 graphics family 按能力请求最多四个 queue，并通过 `LdbFgQueryDeviceInfoV1` 向 x64 Host 返回 Vulkan instance、physical/logical device、game/present/acquire/async queue 及逻辑设备 feature。Host 会把队列数和三项 handle 的所有者一致性写入 `session` 事件。

该基线只完成 64 Host 设备所有权和队列数据采集，尚未用 replacement swapchain 显示生成帧。`presenter_device_flags & 2` 表示至少有 game、present、acquire 三个互不相同的 queue；`& 4` 表示另有独立 async compute queue。Arc B580 若只提供一个或两个 queue，日志会如实记录，后续 presenter 必须选择兼容拓扑，不能访问未在 `vkCreateDevice` 中申请的 queue。

Host 同时按 `LdbFgFrameInputsV1` 形成逐帧输入包。包将 generation/frame ticket、最终颜色、场景深度、可选 HUD-less 颜色、当前/上一帧 view-projection、恢复后的相机参数和真实帧间隔绑定在一起；当前运动输入沿用已验证的 depth + camera reconstruction，动态物体由 FFX Optical Flow 补足。日志出现 `presenter_input.valid=true` 表示这些字段来自同一帧且通过合同校验。

旧 `LdbFgFrameInputsV1` 的 `gpu_ready_token=false` 和 `present_ticket=false` 仍只用于旧资源接入诊断。实际 Layer 消费走同进程 Host ABI v2：Host 在提交前等待导出计算完成，Layer 在同一次原始 Present 内消费，下一帧 Host 写入前又会等待共用队列，因此不依赖旧 ticket 字段。

B580 首次采集显示当前 DXVK graphics family 只申请到一个 queue，`presenter_reserved_queues=0`。这只能说明当前逻辑设备的该队列族拓扑，不能推断物理设备没有其他 queue family。官方 FFX replacement swapchain 会拒绝 game、async、present、acquire 中任意两个非空 handle 相同；其中 async 可以为空，但 game、present、acquire 仍需三个不同 handle。因此不能把同一个 graphics queue 填进四个角色，也不能直接照搬 Danyria 固定四队列初始化。

拓扑 API 会在同一次游戏会话中记录每个物理 queue family 的 Vulkan flags、物理 queue 数量和当前 DXVK 逻辑设备实际创建的 queue 数量。日志不写 queue handle。该结果仍不包含具体 Win32 surface 的 present-support；只有确定候选 family 后，才在真实 swapchain surface 上验证 present 支持。

`fg_input_ring.h` 已加入 presenter 无关的三槽所有权层：单生产者发布完整输入，单消费者只取最新 Ready 帧；generation reset 和落后 Ready 帧由消费者回收，InFlight 槽必须等 GPU timeline 达到 `retireValue` 才能复用。原生测试覆盖乱序拒绝、满环重试、reset、旧帧丢弃、提前完成拒绝和 20000 帧并发压力。该层尚未连接实际 FFX dispatch，不构成性能或异步显示验收。下一阶段需要先枚举所有 queue family，选择满足 FFX 不别名约束的 present/acquire handle；若物理拓扑不满足，则改造 DXVK 的队列创建与呈现所有权，而不是在同步 Host 路径继续叠加 fence wait。

## 日志解释

`session.backend=fsr_official` 表示选择官方后端，`fsr_history_primed` 表示历史预热，`fsr_dispatch.status=0` 表示本项目接受该次官方 SDK 输出，`frame_rejected.reason=fsr_history_warmup` 表示断帧后的原帧回退。驱动或 SDK 内部的画面重复不能仅靠这些计数排除。

自动截图总计最多 32 组，其中获准生成的候选最多 24 组、拒绝候选最多 8 组。`capture.quality_accepted` 只表示通过质量与历史检查，应结合相同帧的 `generated_present` 及 HRESULT 判断是否成功调用 Present，不能直接当作显示器送帧证明。

`gpu_dispatch_ms` 与 `motion_interpolated_pixels` 来自保留的预检，不是官方 SDK 内核耗时或完整动态物体真值；`fence_wait_ms` 包含 SDK 提交等待。原帧与生成帧调用数不等于显示器实际送帧数。诊断候选 PPM 也可能对应已拒绝、未显示的帧。

## 构建与验证来源

Client、Host64、Host32 由 Windows MSVC Release 编译；独立 FSR DLL 与配套 DXVK x64 由固定源码通过 MinGW 交叉编译，经同一 Windows ABI 在 Wine/Vulkan 集成测试。资产从 ZIP 分支的不可变提交获取，并检查源码、归档及每个文件的 SHA-256。

测试使用真实 D3D9 绘制和后备缓冲区读回，覆盖中间位置、HUD/武器保护、遮挡、Reset 和失败回退。SDK 会重采样颜色，因此动态纹理使用独立的空间相关性检查，并以实际原帧作为错误相位反向对照；自研后端继续保留既有精确颜色检查。软件 Vulkan 通过不代表 B580 游戏验收。
