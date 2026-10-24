# RT Performance v37 145832 结果与 v38 完整模型回退

用户上传 `rt-performance-20261024-145832-2244a65b.zip`，大小 4,181,305 字节，SHA256 `145ae33a996d46dc3be1c133ac324fd55e974cd7329e6551337fb0feba63cd96`。525 个成员、展开 21,102,776 字节，Python ZIP CRC 检查通过且没有归档遗漏。安装与运行日志匹配 v37 提交 `7617f9ddfd3cdd977493f151c6f8fac41b0f2e12`。

设备生命周期成功：ReSTIR GI 实际激活，没有 `VK_ERROR_DEVICE_LOST`、Fence/API 失败或异常块。30/60/120 个 RT Present 三段全部完成；关闭 RT 后显式 Mesh、Fence、纹理驻留与引用均归零，随后 60 个光栅 Present 完成。两端 Bridge 均记录正常 Terminate 和 `Shutdown cleanup successful`。Remix 仍报告项目既有的 `[40] common device objects were not disposed of`，因此退出单列为 cleanup warning，不能写成后端资源全部释放。

三段显式 GPU 蒙皮上限为 0、8、16，对应 8.55、14.57、15.78 FPS。第一段包含一次 1310.56 ms 的首次 Present 编译停顿；后两段 GPU 3D 平均约 39.19% 与 43.24%。16 档 120 帧提交 1920 次，创建 8 个新姿态槽并复用 1912 次。与 v36 的设备丢失相比，显式 ReSTIR GI 和 16 档生命周期已经稳定，但仍低于 24 FPS。

用户观察到模型只显示头部。日志给出确定原因：v37 为隔离显式 GPU 蒙皮，共把 122,753 次超限/不支持部件标成 `performance_gpu_skin_only_omitted`；代码随后仍给所有原 Draw 设置 Remix Ignore。与此同时 `rtx.useVertexCapture=False`，Remix 明确记录 `Skipping draw call with shader usage as vertex capture is not enabled.`。因此未显式提交的身体部件没有任何可见路径，头部只是绘制排序靠前而进入 8/16 配额的部件。这不是纹理丢失。

结果中的 `runtime-analysis.json` 还把门禁阶段 30 条 `stage=off` 的诊断 `api_mesh_frame` 混进性能阶段，错误报告 `invalid_gpu_skinning_performance_ramp`；同包独立 `performance-analysis.json` 已正确通过。v38 按 `stage=api_continuous_preview` 过滤，消除此误报。

v38 启用原生 vertex capture，并把 Ignore 范围改为仅覆盖成功提交了显式 API 替代实例的 Draw。不支持或超出 0/8/16 显式预算的部件保留原 Draw，避免重新引入 v31 每帧数百个 CPU 姿态 API Mesh 的创建风暴。下一轮同时验收完整身体、无拉伸、无设备丢失、资源归零和光栅恢复；原生 vertex capture 的实际画面仍需用户目视确认。
