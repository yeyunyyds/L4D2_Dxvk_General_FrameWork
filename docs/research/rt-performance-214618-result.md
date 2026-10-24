# 214618：v35 稳定性通过，但目标配置被 ReSTIR 覆盖

输入 `rt-performance-20261023-214618-6aeefb26.zip` 为 4,444,174 字节，SHA256 `b8df00e9070c2276b706d56f61279cc786577e5b24658c35771654da4b8fe676`。ZIP CRC、成员路径和安装哈希复核通过，构建提交为 `a58f3bde0d902e0f583e14dd99ddd48946889f33`。采集完整；退出状态单独为异常，原因是 Bridge 已完成清理后 `necola.dll` 执行访问异常，不是本轮 RT 运行期间的设备丢失。

128 帧性能段完整结束，原因是 `performance_stability_complete`。8 帧渐进预热后，四个固定 32 个 GPU 蒙皮 Mesh 的 30 帧窗口如下：

| 窗口 | 耗时 | FPS | 模型作用域 | CPU skin | GPU 3D 平均 |
|---|---:|---:|---:|---:|---:|
| 1 | 2,137.23 ms | 14.04 | 194.43 ms | 27.63 ms | 17.32% |
| 2 | 2,086.66 ms | 14.38 | 193.24 ms | 29.78 ms | 18.62% |
| 3 | 2,094.15 ms | 14.33 | 206.65 ms | 34.40 ms | 18.35% |
| 4 | 2,040.26 ms | 14.70 | 194.69 ms | 29.35 ms | 18.96% |

共提交 3,984 个 GPU 蒙皮 Mesh，创建 34、复用 3,950。结束时 Mesh 条目、Fence、持有引用和读回均为 0，日志没有 `VK_ERROR_DEVICE_LOST`。这证明 v35 的渐进创建和固定 32 Mesh 负载解决了上一轮的设备丢失；四个窗口都没有达到 24 FPS。

性能瓶颈尚不在已计时的转换代码内。稳定窗口平均约 69.65 ms/帧，模型作用域约 6.58 ms/帧，Present 约 0.11 ms/帧。即使考虑嵌套作用域不能相加，仍有约 55～60 ms/帧没有被 Host 的 Draw/模型/Present 计时覆盖。现有证据只说明这段时间可能位于 Source 客户端工作、Bridge IPC／唤醒或 Host 线程等待，不能仅凭约 18% 的 GPU Engine 采样认定是 GPU 未吃满。

本轮还暴露了配置覆盖。启动日志读到 `rtx.integrateIndirectMode = 0`，但 RT 激活时明确记录 `[RTX] Integrate Indirect Mode: ReSTIR GI - activated`，并提示 Automatic Graphics Preset 正在使用。固定 runtime 在非 NVIDIA 适配器上把 Auto 派生为 Low；该预设层重新选择 ReSTIR GI，覆盖了较低层的请求。因此 v35 是有效的 B580 稳定性证据，但不是预期的 importance-sampled 直接光性能基线。

v36 在 runtime 创建前设置 `rtx.graphicsPreset = 4`（Custom），并通过 API 再次固定同一值。收集器要求实际日志出现 `Importance Sampled - activated` 且没有 `ReSTIR GI - activated`。同一轮还新增 Host 设备线程 kernel/user CPU 时间与游戏/Host 两进程 CPU 增量，以把未解释的帧间隔分到客户端计算、Host 计算或等待一侧。[结构化复核](../../research/rt-performance-214618-review.json) 保存本结论的机器可读依据。
