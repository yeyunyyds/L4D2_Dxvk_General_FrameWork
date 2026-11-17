# 官方 FSR Host 接入测试包

本包把官方 FSR Optical Flow 与 Frame Interpolation 接到 x64 Host 的实时 Vulkan 资源和实际 Present 路径。包内 `d3d9vk_x64.dll` 是 SHA-256 固定的上游原版 DXVK v2.6.1；Host 只使用上游 `ID3D9VkInteropDevice`，不调用 GF 私有 DXVK 导出。用户可替换其他官方 DXVK 版本做兼容实验，但不承诺任意版本都具备相同 interop ABI。

当前 Client、Host32 与 Host64 已同步到正式 LDB v1.3.4（提交 `78f8c17aa8eb6c752a6d790e166c9ba55ed4a91e`）。FG hooks 在 v1.3.4 的幂等 Device 生命周期、Steam/ReShade Presenter 处理、Session 日志和异常路径之上手工重放；三个 Bridge 二进制必须继续同批替换。

前版已在 B580 游戏内完成 174 次生成帧 Present；本轮区域保护改动仍待实机验收。尚未证明完整动态场景画质、显示节奏或帧率收益。

## 安装与一次测试

1. 完全退出 L4D2 和两个 Bridge Host；Steam 可以保持运行。先自行备份原文件，再把包内 `bin` 合并到 L4D2 游戏根目录的 `bin`。本版的 Layer、INI 和 Monitor 都位于 `bin/.l4d2bridge`，不再依赖每次运行安装或启动脚本。
2. 按需编辑 `bin/.l4d2bridge/fg-config.ini`。默认启用 HUD 分层、viewmodel 保护与 Monitor，关闭逐帧内容探针。随后直接从 Steam 正常启动游戏；x64 Host 在 Vulkan 初始化前加入配套 Layer 搜索路径，并在 LDB v1.3.4 建立 GUID 会话后把 FG 输出绑定到同一日志树。
3. 进入地图后保持相似的转头或平移，先让 FG 开启约 20 秒，按一次 F8 暂停约 20 秒，再按 F8 恢复约 20 秒。无需 F9。正常退出后上传 `L4D2BridgeLogs/<本次 GUID>`；FG、Layer、DXVK 和 monitor 数据位于其中的 `frame-generation` 子目录。无需 `capture.trigger`。
4. `install_fg_test.cmd`、`start_fg_test.cmd` 和恢复脚本只保留给旧式隔离诊断。手动覆盖模式的恢复由用户自己的备份负责。

必须使用 `L4D2Bridge64.exe`。若曾设置 `client.testX86Server=True`，改回 `False`；本包保留现有 `bridge.conf`。32 位 Host 随包保持 Client/Host 版本匹配，未接入 FSR。

已经手工合并旧测试包的用户，应先使用当时的手工备份恢复，再运行本版 `install_fg_test.cmd`；否则自动备份保存的会是旧测试 DLL。改用旧启动方式时清除 `LDB_FG_BACKEND` 或设为 `custom`，以免把旧 DXVK 当成具备新能力的后端。

## 接入内容与保护

- 直接使用 Host GPU 历史，不通过 IPC 传输整幅图像，不为插帧上传/回读整幅游戏画面。有限诊断截图是例外。
- `enabled=0` 是进程启动时的硬关闭：不注入 Vulkan Layer，不建立 FG 目录，不启动 monitor，也不设置 DXVK 日志覆盖；编译进 Host 的 hooks 仅作常量级条件检查后直通原 LDB Present。F8 只是运行期间暂停生成，不等同于此硬关闭。
- GPU 转换颜色、深度和运动输入；运动来自相机重建及有限范围图像匹配，官方光流继续处理图像运动。没有取得完整引擎物体/骨骼运动矢量。
- 默认测试设置 `LDB_FG_FAST_INPUTS=1`。`hud=0` 保持 HUD 关闭；`hud=1` 在第一次屏幕 alpha 绘制前用 D3D9 `StretchRect` 把快照排进已有游戏命令流。`viewmodel=1` 另在近景 viewport 绘制前后各排入一张 HDR 快照，以差分获得手部／持枪遮罩。draw 阶段均不 Flush/Wait；Present 时最终画面、HUD 与两张近景快照共用一次 interop transition/flush 和一笔 Host GPU submission。FSR 完成后，Layer 在同一个 interpolation command buffer 内按当前与上一帧 viewmodel 遮罩的并集覆盖当前真实像素，既保持当前武器清晰，也擦除上一位置的插值残影。第一版的 viewmodel 仍按原生输入率更新，并不伪造手部运动矢量；它优先解决生成帧上的模糊和重影。`viewmodel_overlay_available` 与分析结果中的 `availability_ratio` 用于确认近景遮罩实际取得。Steam Overlay 位于游戏捕获边界之外，本轮不处理其 FPS 文字花屏。
- Vulkan Layer 会优先把 FFX async workload 放到独立 compute queue；没有独立队列时自动保持同步模式。`asteria-layer.json` 的 `fast_scene_inputs_active`、`async_workloads_enabled`、四个 queue family 和 `cpu_timing` 用于确认实际选择，并分开统计 `ffxWaitForPresents`、配置、回调与 `vkQueuePresentFFX` 的 CPU 时间。
- HUD 覆盖与 FSR 位于同一 interpolation command buffer，因此三个 Host 输入槽只按 interpolation timeline 延迟退休；完成信号已经包含 HUD 读取，无需再等待 native Present 序列。只有两个旧租约将耗尽三槽时才阻塞等待最旧 timeline。`input_retirement` 记录队列峰值、无等待释放和被迫等待耗时。
- 内容差异探针默认关闭，避免它干扰性能基线和 Intel 驱动稳定性；需要验证非重复内容时在 INI 中设置 `content_probe=1`。启用后 Layer 在插值 command buffer 内抽样比较 16 个固定场景像素，timeline 完成后才读回极小的环形缓冲。`output_pacing` 始终从实际 `vkQueuePresentKHR` 记录输出节奏。
- 设备丢失隔离包提供三个只改变一个变量的入口：`start_fg_isolate_a_probe_off.cmd` 关闭 GPU 像素探针，`start_fg_isolate_b_hud_off.cmd` 在 A 的基础上关闭 HUD 分层，`start_fg_isolate_c_merge_off.cmd` 再关闭 submission 合并。按 A、B、C 顺序测试即可定位首个不稳定路径；三个入口共用同一套二进制。
- Arc 实测的 present family 2 只有 transfer/present 能力，不能执行 HUD render pass。新路径不在 present family 录制 render pass：HUD compute 跟随 FSR 所在的 async-compute family（不可用时才跟随 graphics family），SDK UI 注册与内部双缓冲均关闭。`ui_composition_queue_family`、`ui_internal_double_buffering=false` 和 `hud_compute_composite_dispatches` 用于验收实际路径。
- 保留深度／覆盖质量预检：过多无效场景、异常前景覆盖仍恢复完整原帧。细特效风险在 FSR 路径改为区域保护：可疑细亮区域及前帧旧位置的邻域保留当前原帧，避免单个风险点导致整帧拒绝和历史反复预热。该识别依据亮线形态，不等同于引擎材质标签，可能保守地保留普通白色细节；透明层仍未完整分解。自研后端继续整帧拒绝细特效风险。SDK 合成后，已验证的前帧遮挡填补区域沿用预检重投影结果；缺乏可靠运动的纯色突变区域也有原帧保护。因此当前交付是官方 FSR 与保守保护的组合。
- 首帧、帧编号断续、HUD 输入变化或 Reset 时重置历史。预热帧不会增加一次生成帧 Present。
- 加载阶段的 estimated Provider 只负责建立 replacement swapchain，默认不执行插帧；原生 V2 Provider 接管并重置历史后才开始生成，避免加载画面污染游戏内颜色历史。
- Host 通过标准 D3D9/Vulkan interop 取得 instance、physical/logical device 与 DXVK 提交队列；Layer 在 `vkCreateDevice` 时启用 FSR 所需能力并管理额外队列。缺少 interop、Layer 能力或 SDK 出错时记录 `fg_error`，停止该会话插帧并保持原始 Present 路径。

## Danyria presenter 迁移基线

当前包使用独立的 **x64 Vulkan Layer 完整路径**：`L4D2Bridge64 -> 原版 DXVK -> Vulkan loader -> GF/Asteria layer -> Intel Vulkan driver`。它直接拦截 `vkCreateDevice`、swapchain、acquire 与 present，在 Host 进程内创建官方 FSR3 replacement swapchain；不要求客制化 DXVK 暴露私有插帧接口。`asteria-layer.json` 与 Host FG 日志统一写入 LDB GUID 会话的 `frame-generation` 子目录，无需让 Steam 继承启动脚本环境。

Asteria 原版只构建 x86 DLL，并要求同一个 graphics family 至少有四条 queue。本移植从固定 MIT 源码提交重新构建 AMD64 DLL；创建设备时申请所有 graphics/compute/transfer 候选 queue，拿到真实 Win32 surface 后再检查 present support，并为 game、async compute、present、image acquire 分配四条互不相同的物理 queue。各角色保留自己的 family index，交给官方 FFX 的跨 family ownership-transfer 路径。Arc B580 已观测到的 `1 + 4 + 2` 物理拓扑满足数量要求，最终能否启用由该次 surface 的 present-support 实测决定。

当前闭环已把 Asteria/Danyria 的 D3D9 语义采集方式移到 LDB x64 Host。Host 在实际命令序列中识别 HDR 主视图、深度表面、相机 shader 常量、HUD 前边界和 Present 时间。正式路径先向 Layer 查询与本次 acquire 对应的 ticket，再通过 `AsteriaSubmitFrameInputsV2` 提交三槽 GPU 资源；旧 `AsteriaSubmitHostFrameV2` 只保留兼容诊断。图像不经过 IPC 或 CPU 复制。

本版由 Host 在同一个 Vulkan 设备上把主场景深度转换成 `R32F`，并用当前/上一主视图矩阵生成 `RG32F` 相机运动向量。每个槽包含与 swapchain 同格式的 HUD-less 颜色和 `RGBA8` exact-pixel 覆盖（RGB 为当前最终像素，alpha 为 HUD 或 viewmodel mask），Layer 逐项核对设备、ticket、acquire 序号、图像、格式和布局。FSR3 插值结束后 compute pass 按 mask 覆盖当前 HUD／viewmodel；槽只有在 Layer interpolation timeline 完成并调用 release lease 后才能复用。

`LDB_FG_MODE=provider` 表示 Host 只生产数据且只调用一次原始 Present，不再运行旧预览或第二次 Present。`asteria-layer.json` 中同时满足以下条件，才能证明 Host 控制的算法与提交链已工作：

- `native_v2_accepted > 0`，且退出时 `native_v2_leases_released` 已追上接受数；
- swapchain 的 `native_frame_inputs_bound=true`、`host_depth_resource_connected=true`，且 `motion_input_source=1`（Engine）；
- `native_interpolation_dispatches > 0`，并且没有 native Present 失败；
- `native_output_presents_per_input` 接近 2；全会话 `output_presents_per_input` 包含未插帧的加载阶段，只作诊断。

Host 日志还会记录 `gpu_motion_pixels`；转头或平移时它应大于零，用来排除全零运动输入。上述条件证明 Host 真深度／运动输入、官方算法调度和约 2:1 输出提交链。外置监测窗口会自动发现 Host 创建的 `fg-*` 子目录，并同时读取 Host 事件和 Layer 状态，显示滚动原生输入率、原生阶段输出提交率、Host/fence 耗时、lease 进度和 native dispatch 总数；每秒样本写入 `fg-live-comparison.csv`。Layer 输出是 replacement swapchain 的提交率，不代表显示器实际扫描；直接排除“生成内容等于相邻原帧”仍需要后续 GPU 内容差异探针。

设备所有权不再依赖 `LdbFgQueryDeviceInfoV1` 或 `LdbFgQueryQueueTopologyV1`。Host 的 `session` 事件以 `interop_device_api`、`interop_submission_family` 和 `interop_queue_families` 记录标准 interop 结果；Layer 自己记录 replacement-swapchain 的 game、async、present 与 acquire 队列角色。

Host 同时按 `LdbFgFrameInputsV1` 形成逐帧输入包。包将 generation/frame ticket、最终颜色、场景深度、可选 HUD-less 颜色、当前/上一帧 view-projection、恢复后的相机参数和真实帧间隔绑定在一起；当前运动输入沿用已验证的 depth + camera reconstruction，动态物体由 FFX Optical Flow 补足。日志出现 `presenter_input.valid=true` 表示这些字段来自同一帧且通过合同校验。

旧 `LdbFgFrameInputsV1` 的 `gpu_ready_token=false` 和 `present_ticket=false` 仍只用于旧资源接入诊断。实际 Layer 消费使用 Asteria 原生 ticket/lease ABI V2：ticket 绑定 swapchain generation、image index 和 acquire sequence，三套资源允许前后帧同时处于 GPU 生命周期内；没有 ticket 或没有空闲槽时该帧保持原始 Present，不会覆盖仍在消费的图像。

Host 的标准 interop 拓扑只描述物理 queue family 和 DXVK 的提交 family，不声称知道 DXVK 内部创建了多少额外 queue。具体 Win32 surface 的 present support 与 FFX 队列角色由 Layer 在真实 swapchain 上验证，日志不写 queue handle。

正式 Provider 现已使用三套 GPU 输入槽。Host 只选择引用数为零的槽，Layer 在 FFX Present 完成后释放包含 depth、motion、HUD-less 和 UI 的整组 lease；日志分别记录 ticket、提交、接受、释放和满槽次数。`fg_input_ring.h` 仍保留为 presenter 无关的合同测试模型，实际跨 DLL 生命周期由 Asteria ticket/lease 实现。

GPU 导出提交与 FFX 使用同一条已验证的 game queue。V2 包携带 producer queue，Layer 只有在它与 replacement swapchain 的 game queue 完全相同时才接受 `GPU_ORDERED` 输入；因此 Host 不再在提交后立即阻塞等待 fence，而是在下一次复用单缓冲命令池前等待。该改动允许导出计算与中间的游戏 CPU 工作重叠，又不会让 FFX 读取未排在自己之前的资源。若 queue 不匹配，该帧直接拒绝原生输入。

## 日志解释

`session.backend=fsr_official` 表示选择官方后端，`fsr_history_primed` 表示历史预热，`fsr_dispatch.status=0` 表示本项目接受该次官方 SDK 输出，`frame_rejected.reason=fsr_history_warmup` 表示断帧后的原帧回退。驱动或 SDK 内部的画面重复不能仅靠这些计数排除。

自动截图总计最多 32 组，其中获准生成的候选最多 24 组、拒绝候选最多 8 组。`capture.quality_accepted` 只表示通过质量与历史检查，应结合相同帧的 `generated_present` 及 HRESULT 判断是否成功调用 Present，不能直接当作显示器送帧证明。

`gpu_dispatch_ms` 与 `motion_interpolated_pixels` 来自保留的预检，不是官方 SDK 内核耗时或完整动态物体真值；`fence_wait_ms` 包含 SDK 提交等待。原帧与生成帧调用数不等于显示器实际送帧数。诊断候选 PPM 也可能对应已拒绝、未显示的帧。

## 构建与验证来源

Client、Host64、Host32 由 Windows MSVC Release 编译；独立 FSR DLL 由固定源码构建。DXVK x64 直接取自上游 v2.6.1 release，并同时校验 release 归档与 `x64/d3d9.dll` 的 SHA-256；测试包不再交付 GF 魔改 DXVK。

测试使用真实 D3D9 绘制和后备缓冲区读回，覆盖中间位置、HUD/武器保护、遮挡、Reset 和失败回退。SDK 会重采样颜色，因此动态纹理使用独立的空间相关性检查，并以实际原帧作为错误相位反向对照；自研后端继续保留既有精确颜色检查。软件 Vulkan 通过不代表 B580 游戏验收。
