# RT Performance v36 000156 结果与 v37 生命周期修正

用户上传 `rt-performance-20261024-000156-1b3319f8.zip`，大小 7,874,039 字节，SHA256 `5ad7ba0611c4ff79ab75d321d4730aa3e131147dd01f209483880f7d3234f1f8`。550 个 ZIP 成员名称唯一且 CRC 全部通过。安装回执与日志构建身份匹配源码 `b1d7bf8ebbdc2e9c5e6b9d547fc871dde3dd30e8`，确认为 v36 结果。

v36 的配置修正确实生效：Remix 读取 `rtx.graphicsPreset = 4`，日志出现 `Integrate Indirect Mode: Importance Sampled - activated`，没有 ReSTIR GI 激活记录。因此本次设备丢失不能再归因于 Auto/Low 预设覆盖配置。

第一段固定 32 GPU 蒙皮模型的 30 帧窗口完整，墙钟 2161.895 ms，即 13.877 FPS。模型作用域 251.431 ms、CPU 蒙皮 44.052 ms、Present 3.276 ms；Host 设备线程 CPU 仅 78.125 ms，未解释的非 CPU 时间为 2083.770 ms。对齐采样中游戏进程约占单核 32.21%，Host 约占单核 16.11%，GPU 3D 两个采样平均 35.99%。这排除了“Host 设备线程计算吃满”作为该窗口主要瓶颈，剩余墙钟主要位于等待、同步或尚未计时的驱动/IPC 路径。

测试没有完成。Importance Sampled 激活约 4.15 秒后，Remix 报 `DxvkSubmissionQueue: Failed to sync fence: VK_ERROR_DEVICE_LOST`、`waitForIdle` 和命令提交失败；运行时随后记录 `api_mesh_fence_failed`，HRESULT 为 `0x8876086c`（`D3DERR_DEVICELOST`）。预览在第 60 个 RT Present、全局第 91 个 Present 终止，仍有 32 个 Mesh、1 个 Fence 和 6 个纹理引用，最终退出状态为 `unverified`。本轮是明确失败，不是完成。

v37 改为完整生命周期验收：使用 v35 已跑完 128 帧的显式 ReSTIR GI 模式；按 30 帧静态、60 帧上限 8、120 帧上限 16 分级运行，不再进入 32 档；随后自动关闭 RT，等待 Mesh、Fence 和引用归零，再要求 60 个普通光栅 Present。分析器把证据采集与测试验收分开，只有采集完整且游戏/Host 干净退出才返回成功。

本修正优先建立不冻结、不设备丢失、不靠异常退出结束的 GPU 基线。若 v37 通过，0/8/16 三段 CPU、GPU 与墙钟差值可直接用于下一轮 Bridge IPC 和 GPU 供给优化；若静态 0 档也丢失设备，问题将收敛到模型动态 BLAS 以外的 Remix/驱动路径。
