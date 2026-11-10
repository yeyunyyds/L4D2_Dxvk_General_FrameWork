# 官方 FSR Host 接入测试包

本包把官方 FSR Optical Flow 与 Frame Interpolation 接到 x64 Host 的实时 Vulkan 资源和实际 Present 路径。配套 DXVK 2.6.1 在 `LDB_FG_BACKEND=fsr` 时启用所需设备能力；不修改游戏渲染线程设置，不引入整套 RTX 兼容层。

前版已在 B580 游戏内完成 174 次生成帧 Present；本轮区域保护改动仍待实机验收。尚未证明完整动态场景画质、显示节奏或帧率收益。

## 安装与一次测试

1. 解压到独立文件夹，完全退出游戏和两个 Host，运行 `install_fg_test.cmd`。脚本自动定位 L4D2、校验包内 Host、后端与 Vulkan Layer 文件，并将八个目标文件的原内容和哈希保存到游戏目录的 `.ldb-fg-test-backup`；找不到非默认 Steam 库时，把游戏根目录拖到 CMD 上或作为第一个参数传入。
2. 完全退出 Steam，再运行 `start_fg_test.cmd`，进入地图正常游玩约一分钟，覆盖转头、平移、开枪、换弹及之前出现圆环特效的位置。本轮由 x64 Host 采集主视图相机、真实帧间隔、深度和运动，通过同进程 ticket/lease ABI 交给 Vulkan Layer；颜色默认使用 replacement swapchain 自己的 `presentColor`，只有 Layer 接管 replacement swapchain，Host 不额外 Present；无需按 F8 或 F9。启动时会同时打开外置 `GF Frame Generation Monitor`，它不进入游戏画面，也不会被插帧。
3. 正常退出，压缩完整 `fg-results` 上传。启动脚本会在新的会话目录同时收集 Bridge、DXVK 与 FG 日志。无需 `capture.trigger`，也无需先测帧率收益。
4. 测试结束且游戏和 Host 已退出时运行 `remove_fg_test.cmd`。脚本先确认八个测试文件仍与本包完全相同，再恢复原文件并删除 `.ldb-fg-test-backup`。若文件在测试后被其他工具修改，脚本会停止而不会覆盖后来的文件。

必须使用 `L4D2Bridge64.exe`。若曾设置 `client.testX86Server=True`，改回 `False`；本包保留现有 `bridge.conf`。32 位 Host 随包保持 Client/Host 版本匹配，未接入 FSR。

已经手工合并旧测试包的用户，应先使用当时的手工备份恢复，再运行本版 `install_fg_test.cmd`；否则自动备份保存的会是旧测试 DLL。改用旧启动方式时清除 `LDB_FG_BACKEND` 或设为 `custom`，以免把旧 DXVK 当成具备新能力的后端。

## 接入内容与保护

- 直接使用 Host GPU 历史，不通过 IPC 传输整幅图像，不为插帧上传/回读整幅游戏画面。有限诊断截图是例外。
- GPU 转换颜色、深度和运动输入；运动来自相机重建及有限范围图像匹配，官方光流继续处理图像运动。没有取得完整引擎物体/骨骼运动矢量。
- Host 仍采集 HUD 前快照供后续分层实验，但默认不再把启发式 HUD-less/UI 图层替换为 SDK 的颜色输入；设置 `ASTERIA_FG_SEPARATE_UI=1` 才启用该实验路径。
- 保留深度／覆盖质量预检：过多无效场景、异常前景覆盖仍恢复完整原帧。细特效风险在 FSR 路径改为区域保护：可疑细亮区域及前帧旧位置的邻域保留当前原帧，避免单个风险点导致整帧拒绝和历史反复预热。该识别依据亮线形态，不等同于引擎材质标签，可能保守地保留普通白色细节；透明层仍未完整分解。自研后端继续整帧拒绝细特效风险。SDK 合成后，已验证的前帧遮挡填补区域沿用预检重投影结果；缺乏可靠运动的纯色突变区域也有原帧保护。因此当前交付是官方 FSR 与保守保护的组合。
- 首帧、帧编号断续、HUD 输入变化或 Reset 时重置历史。预热帧不会增加一次生成帧 Present。
- 加载阶段的 estimated Provider 只负责建立 replacement swapchain，默认不执行插帧；原生 V2 Provider 接管并重置历史后才开始生成，避免加载画面污染游戏内颜色历史。
- Host 检查真实 DXVK 逻辑设备已启用的能力，并要求支持区域保护的后端接口 v2；旧版 DLL 会被拒绝，不能只更新 Host。缺少配套模块/能力或 SDK 出错时记录 `fg_error`，停止该会话插帧并保持原始 Present 路径。

配套 DXVK 增加三个 storage-image feature，以及在启用 Vulkan memory model 时使用 Device scope 所需的能力。仅物理 GPU 宣称支持不够，所以使用导出查询检查真实逻辑设备的 enabled features。

## Danyria presenter 迁移基线

当前包新增独立的 **x64 Vulkan Layer 完整路径**：`L4D2Bridge64 -> Vulkan loader -> GF/Asteria layer -> Intel Vulkan driver`。它直接拦截 `vkCreateDevice`、swapchain、acquire 与 present，在 Host 进程内创建官方 FSR3 replacement swapchain；不要求客制化 DXVK 暴露私有插帧接口。`start_fg_test.cmd` 会在保留用户现有 Vulkan Layer 搜索路径的同时加入本包、启用 Layer，并将 `asteria-layer.json` 写入本次 `fg-results`。

Asteria 原版只构建 x86 DLL，并要求同一个 graphics family 至少有四条 queue。本移植从固定 MIT 源码提交重新构建 AMD64 DLL；创建设备时申请所有 graphics/compute/transfer 候选 queue，拿到真实 Win32 surface 后再检查 present support，并为 game、async compute、present、image acquire 分配四条互不相同的物理 queue。各角色保留自己的 family index，交给官方 FFX 的跨 family ownership-transfer 路径。Arc B580 已观测到的 `1 + 4 + 2` 物理拓扑满足数量要求，最终能否启用由该次 surface 的 present-support 实测决定。

当前闭环已把 Asteria/Danyria 的 D3D9 语义采集方式移到 LDB x64 Host。Host 在实际命令序列中识别 HDR 主视图、深度表面、相机 shader 常量、HUD 前边界和 Present 时间。正式路径先向 Layer 查询与本次 acquire 对应的 ticket，再通过 `AsteriaSubmitFrameInputsV2` 提交三槽 GPU 资源；旧 `AsteriaSubmitHostFrameV2` 只保留兼容诊断。图像不经过 IPC 或 CPU 复制。

本版由 Host 在同一个 Vulkan 设备上把主场景深度转换成 `R32F`，并用当前/上一主视图矩阵生成 `RG32F` 相机运动向量。每个槽仍包含与 swapchain 同格式的 HUD-less 颜色和 `RGBA8` UI 覆盖，Layer 逐项核对设备、ticket、acquire 序号、图像、格式和布局。为排除实机发现的固定加载帧交替，默认 FSR3 颜色和 optical flow 改用 replacement swapchain 的当前 `presentColor`；HUD-less/UI 合成保留为显式实验开关。槽只有在 Layer 等待 GPU 完成并调用 release lease 后才能复用。

`LDB_FG_MODE=provider` 表示 Host 只生产数据且只调用一次原始 Present，不再运行旧预览或第二次 Present。`asteria-layer.json` 中同时满足以下条件，才能证明 Host 控制的算法与提交链已工作：

- `native_v2_accepted > 0`，且退出时 `native_v2_leases_released` 已追上接受数；
- swapchain 的 `native_frame_inputs_bound=true`、`host_depth_resource_connected=true`，且 `motion_input_source=1`（Engine）；
- `native_interpolation_dispatches > 0`，并且没有 native Present 失败；
- `output_presents_per_input` 接近 2。

Host 日志还会记录 `gpu_motion_pixels`；转头或平移时它应大于零，用来排除全零运动输入。上述条件证明 Host 真深度／运动/HUD 分层输入、官方算法调度和约 2:1 输出提交链。外置监测窗口会自动发现 Host 创建的 `fg-*` 子目录，并同时读取 Host 事件和 Layer 状态，显示原生输入率、native 接受率、Layer 输入／输出提交率、Host/fence 耗时、lease 进度和 native dispatch 总数；每秒样本写入 `fg-live-comparison.csv`。Layer 输出是 replacement swapchain 的提交率，不代表显示器实际扫描；直接排除“生成内容等于相邻原帧”仍需要后续 GPU 内容差异探针。

本轮开始迁移 Danyria/Asteria 的 replacement-swapchain 路径。设置 `LDB_FG_PRESENTER=1` 后，配套 x64 DXVK 会从实际 graphics family 按能力请求最多四个 queue，并通过 `LdbFgQueryDeviceInfoV1` 向 x64 Host 返回 Vulkan instance、physical/logical device、game/present/acquire/async queue 及逻辑设备 feature。Host 会把队列数和三项 handle 的所有者一致性写入 `session` 事件。

该基线只完成 64 Host 设备所有权和队列数据采集，尚未用 replacement swapchain 显示生成帧。`presenter_device_flags & 2` 表示至少有 game、present、acquire 三个互不相同的 queue；`& 4` 表示另有独立 async compute queue。Arc B580 若只提供一个或两个 queue，日志会如实记录，后续 presenter 必须选择兼容拓扑，不能访问未在 `vkCreateDevice` 中申请的 queue。

Host 同时按 `LdbFgFrameInputsV1` 形成逐帧输入包。包将 generation/frame ticket、最终颜色、场景深度、可选 HUD-less 颜色、当前/上一帧 view-projection、恢复后的相机参数和真实帧间隔绑定在一起；当前运动输入沿用已验证的 depth + camera reconstruction，动态物体由 FFX Optical Flow 补足。日志出现 `presenter_input.valid=true` 表示这些字段来自同一帧且通过合同校验。

旧 `LdbFgFrameInputsV1` 的 `gpu_ready_token=false` 和 `present_ticket=false` 仍只用于旧资源接入诊断。实际 Layer 消费使用 Asteria 原生 ticket/lease ABI V2：ticket 绑定 swapchain generation、image index 和 acquire sequence，三套资源允许前后帧同时处于 GPU 生命周期内；没有 ticket 或没有空闲槽时该帧保持原始 Present，不会覆盖仍在消费的图像。

B580 首次采集显示当前 DXVK graphics family 只申请到一个 queue，`presenter_reserved_queues=0`。这只能说明当前逻辑设备的该队列族拓扑，不能推断物理设备没有其他 queue family。官方 FFX replacement swapchain 会拒绝 game、async、present、acquire 中任意两个非空 handle 相同；其中 async 可以为空，但 game、present、acquire 仍需三个不同 handle。因此不能把同一个 graphics queue 填进四个角色，也不能直接照搬 Danyria 固定四队列初始化。

拓扑 API 会在同一次游戏会话中记录每个物理 queue family 的 Vulkan flags、物理 queue 数量和当前 DXVK 逻辑设备实际创建的 queue 数量。日志不写 queue handle。该结果仍不包含具体 Win32 surface 的 present-support；只有确定候选 family 后，才在真实 swapchain surface 上验证 present 支持。

正式 Provider 现已使用三套 GPU 输入槽。Host 只选择引用数为零的槽，Layer 在 FFX Present 完成后释放包含 depth、motion、HUD-less 和 UI 的整组 lease；日志分别记录 ticket、提交、接受、释放和满槽次数。`fg_input_ring.h` 仍保留为 presenter 无关的合同测试模型，实际跨 DLL 生命周期由 Asteria ticket/lease 实现。

GPU 导出提交与 FFX 使用同一条已验证的 game queue。V2 包携带 producer queue，Layer 只有在它与 replacement swapchain 的 game queue 完全相同时才接受 `GPU_ORDERED` 输入；因此 Host 不再在提交后立即阻塞等待 fence，而是在下一次复用单缓冲命令池前等待。该改动允许导出计算与中间的游戏 CPU 工作重叠，又不会让 FFX 读取未排在自己之前的资源。若 queue 不匹配，该帧直接拒绝原生输入。

## 日志解释

`session.backend=fsr_official` 表示选择官方后端，`fsr_history_primed` 表示历史预热，`fsr_dispatch.status=0` 表示本项目接受该次官方 SDK 输出，`frame_rejected.reason=fsr_history_warmup` 表示断帧后的原帧回退。驱动或 SDK 内部的画面重复不能仅靠这些计数排除。

自动截图总计最多 32 组，其中获准生成的候选最多 24 组、拒绝候选最多 8 组。`capture.quality_accepted` 只表示通过质量与历史检查，应结合相同帧的 `generated_present` 及 HRESULT 判断是否成功调用 Present，不能直接当作显示器送帧证明。

`gpu_dispatch_ms` 与 `motion_interpolated_pixels` 来自保留的预检，不是官方 SDK 内核耗时或完整动态物体真值；`fence_wait_ms` 包含 SDK 提交等待。原帧与生成帧调用数不等于显示器实际送帧数。诊断候选 PPM 也可能对应已拒绝、未显示的帧。

## 构建与验证来源

Client、Host64、Host32 由 Windows MSVC Release 编译；独立 FSR DLL 与配套 DXVK x64 由固定源码通过 MinGW 交叉编译，经同一 Windows ABI 在 Wine/Vulkan 集成测试。资产从 ZIP 分支的不可变提交获取，并检查源码、归档及每个文件的 SHA-256。

测试使用真实 D3D9 绘制和后备缓冲区读回，覆盖中间位置、HUD/武器保护、遮挡、Reset 和失败回退。SDK 会重采样颜色，因此动态纹理使用独立的空间相关性检查，并以实际原帧作为错误相位反向对照；自研后端继续保留既有精确颜色检查。软件 Vulkan 通过不代表 B580 游戏验收。
