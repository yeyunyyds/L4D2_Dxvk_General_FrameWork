# 静态世界试验首次启动黑屏回包

输入为 `rt-world-20261016-093012-e025a525.zip`，原包 14,442 字节，CRC 通过；构建、配套安装身份与已发布 v1 一致。[结构化复核记录](../../research/rt-world-093012-review.json) 保留原包 SHA 与结论边界。

用户报告菜单开始全黑、有声音，盲操作进入地图并按 F9，之后按控制台提示主动退出。回包包含运行时配置、client/Host/Remix 日志，但没有 Draw trace、采集完成标记或颜色/深度/法线读回。采集器摘要为 incomplete，原因是检测到游戏退出前没有完成标记。因此不能认定 240 Present、世界转换或游戏光追已完成，也不能将“无 trace”解释成用户没有按 F9。

## 已确认的接入错误

固定运行时成功加载，API 0.6.4 配置请求成功，B580 游戏设备创建返回 `0x00000000`。启动配置却已经是 `rtx.enableRaytracing=True`、`rtx.useVertexCapture=False`。Remix 日志记录跳过 shader 绘制以及没有有效相机。

复核固定提交 `b73eb23053aa50d3787b0c6c8b4b8ae49962880e` 的源码：

- `src/d3d9/d3d9_rtx.cpp` 的 `makeDrawCallType` 将可编程 VS 且不启用 vertex capture 的 Draw 判成 Ignored。
- 同文件 `internalPrepareDraw` 在光追启用时把 Ignored 转成 `PrepareDrawFlag::Ignore`，并非保留原 raster Draw。
- `src/d3d9/d3d9_device.cpp` 只有包含 OriginalDrawCall 标志才发出原绘制。
- `src/dxvk/rtx_render/rtx_context.cpp` 在没有有效相机时也不会生成可替代的光追画面。

这条路径足以解释启动菜单被清空。v1 文档中“关闭 vertex capture 可保留未转换 shader 绘制的 raster 路径”的判断错误，已更正。这是接入策略错误，不应让用户继续盲操作或重新采同一包。

## 与黑屏分开记录的现象

client 在日志时间 09:31:10 记录 VertexBuffer Lock `command-aborted`，UID 4386；其原因尚未证明。Host 在 09:31:15 执行正常清理，随后发生访问违规，调用栈含 Intel 驱动、OpenGL 和系统安装的 ReShade64。用户已确认退出是主动操作；这个退出阶段调用栈不能证明之前发生了自动崩溃，也不能单凭 ReShade 出现在栈上将菜单黑屏归因于它。测试包不会修改或禁用用户的全局 ReShade 安装。

本回包采集器日志没有记录“采集完成后请退出”，而用户看到过这样的提示。提示与包内证据不一致的具体原因仍未确定；新版以明确的完成标记、实际 trace 分析、最终 COMPLETE/INCOMPLETE 和进程退出码区分状态，保留启动/F9 证据用于核对。

## v2 修正与合并测试

启动配置关闭 RT，以 Remix 的普通光栅路径进入菜单和地图。标准 Direct3DCreate9 完成后重新提交配置，避免在 option layer 尚未初始化时 API 接受请求但未应用。F9 后前 30 Present 仍为光栅；只有首个世界候选通过材质、矩阵、目标、管线、布局和状态应用才请求开启 RT。请求由运行时在帧边界应用，事件成功不能代替实际 GPU 证据。未通过条件或开启请求失败时保留/恢复原 Draw 状态。结束、失败和 Reset 请求关闭 RT，并停止继续转换。

一次入口、一份 ZIP 的操作不增加：F9 前自动保留有界模块清单、首次 Draw 到达 Host/调用计数以及 client 请求、F9 与前台 PID；F9 后合并原有 trace、模块、读回和最终日志。菜单若仍黑屏，直接正常退出收取部分包，无需盲操作。完整武器/HUD/动态模型合成、Source producer 并行、资源生命周期与性能继续未验收。
