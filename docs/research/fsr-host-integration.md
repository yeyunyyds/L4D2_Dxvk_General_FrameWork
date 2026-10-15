# 官方 FSR Host 接入测试包

本包把官方 FSR Optical Flow 与 Frame Interpolation 接到 x64 Host 的实时 Vulkan 资源和实际 Present 路径。配套 DXVK 2.6.1 在 `LDB_FG_BACKEND=fsr` 时启用所需设备能力；不修改游戏渲染线程设置，不引入整套 RTX 兼容层。

前版已在 B580 游戏内完成 174 次生成帧 Present；本轮区域保护改动仍待实机验收。尚未证明完整动态场景画质、显示节奏或帧率收益。

## 安装与一次测试

1. 完全退出游戏、两个 Host 和 Steam。备份 `bin/dxvk_d3d9.dll` 及整个 `bin/.l4d2bridge`。
2. 将包内 `bin` 合并到游戏安装目录，覆盖同名文件。五个文件包括 Client、Host64、Host32、`LDBFsrBackend64.dll` 和 `d3d9vk_x64.dll`。后两项是本轮新加入的必需文件。
3. 运行 `start_fg_test.cmd`，进入地图正常游玩约一分钟，覆盖转头、平移、开枪、换弹及之前出现圆环特效的位置。默认显示生成帧预览，F9 切换到原帧＋生成帧双呈现；F8 切换插帧开关，不会暂停游戏。
4. 正常退出，压缩完整 `fg-results` 上传。启动脚本会在新的会话目录同时收集 Bridge、DXVK 与 FG 日志。无需 `capture.trigger`，也无需先测帧率收益。

必须使用 `L4D2Bridge64.exe`。若曾设置 `client.testX86Server=True`，改回 `False`；本包保留现有 `bridge.conf`。32 位 Host 随包保持 Client/Host 版本匹配，未接入 FSR。

回滚时完全退出游戏与 Host，恢复上述备份。改用旧启动方式时清除 `LDB_FG_BACKEND` 或设为 `custom`，以免把旧 DXVK 当成具备新能力的后端。

## 接入内容与保护

- 直接使用 Host GPU 历史，不通过 IPC 传输整幅图像，不为插帧上传/回读整幅游戏画面。有限诊断截图是例外。
- GPU 转换颜色、深度和运动输入；运动来自相机重建及有限范围图像匹配，官方光流继续处理图像运动。没有取得完整引擎物体/骨骼运动矢量。
- 通过已定位的 HUD 前快照向 SDK 提供 HUD-less 颜色。当前和前帧武器覆盖区保留当前原帧，完整透明层分解尚未实现。
- 保留深度／覆盖质量预检：过多无效场景、异常前景覆盖仍恢复完整原帧。细特效风险在 FSR 路径改为区域保护：可疑细亮区域及前帧旧位置的邻域保留当前原帧，避免单个风险点导致整帧拒绝和历史反复预热。该识别依据亮线形态，不等同于引擎材质标签，可能保守地保留普通白色细节；透明层仍未完整分解。自研后端继续整帧拒绝细特效风险。SDK 合成后，已验证的前帧遮挡填补区域沿用预检重投影结果；缺乏可靠运动的纯色突变区域也有原帧保护。因此当前交付是官方 FSR 与保守保护的组合。
- 首帧、帧编号断续、HUD 输入变化或 Reset 时重置历史。预热帧不会增加一次生成帧 Present。
- Host 检查真实 DXVK 逻辑设备已启用的能力，并要求支持区域保护的后端接口 v2；旧版 DLL 会被拒绝，不能只更新 Host。缺少配套模块/能力或 SDK 出错时记录 `fg_error`，停止该会话插帧并保持原始 Present 路径。

配套 DXVK 增加三个 storage-image feature，以及在启用 Vulkan memory model 时使用 Device scope 所需的能力。仅物理 GPU 宣称支持不够，所以使用导出查询检查真实逻辑设备的 enabled features。

## 日志解释

`session.backend=fsr_official` 表示选择官方后端，`fsr_history_primed` 表示历史预热，`fsr_dispatch.status=0` 表示本项目接受该次官方 SDK 输出，`frame_rejected.reason=fsr_history_warmup` 表示断帧后的原帧回退。驱动或 SDK 内部的画面重复不能仅靠这些计数排除。

自动截图总计最多 32 组，其中获准生成的候选最多 24 组、拒绝候选最多 8 组。`capture.quality_accepted` 只表示通过质量与历史检查，应结合相同帧的 `generated_present` 及 HRESULT 判断是否成功调用 Present，不能直接当作显示器送帧证明。

`gpu_dispatch_ms` 与 `motion_interpolated_pixels` 来自保留的预检，不是官方 SDK 内核耗时或完整动态物体真值；`fence_wait_ms` 包含 SDK 提交等待。原帧与生成帧调用数不等于显示器实际送帧数。诊断候选 PPM 也可能对应已拒绝、未显示的帧。

## 构建与验证来源

Client、Host64、Host32 由 Windows MSVC Release 编译；独立 FSR DLL 与配套 DXVK x64 由固定源码通过 MinGW 交叉编译，经同一 Windows ABI 在 Wine/Vulkan 集成测试。资产从 ZIP 分支的不可变提交获取，并检查源码、归档及每个文件的 SHA-256。

测试使用真实 D3D9 绘制和后备缓冲区读回，覆盖中间位置、HUD/武器保护、遮挡、Reset 和失败回退。SDK 会重采样颜色，因此动态纹理使用独立的空间相关性检查，并以实际原帧作为错误相位反向对照；自研后端继续保留既有精确颜色检查。软件 Vulkan 通过不代表 B580 游戏验收。
