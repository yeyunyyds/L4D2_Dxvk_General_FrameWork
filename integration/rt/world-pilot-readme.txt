LDB RT Performance v36 — Arc 有效配置与 CPU／IPC 定位

本版继续暂停五分钟 Beta。v35 已完整跑完 128 帧且没有 VK_ERROR_DEVICE_LOST，但四个稳定窗口只有 14.0～14.7 FPS，GPU 3D 约 17～19%。本轮保持相同负载，修正实际配置并定位每帧尚未解释的等待时间；人物完整度不在本轮验收范围内。

测试步骤（一次 F9）
1. 解压到新文件夹，运行 run-performance.cmd。程序会备份并安装配套的 Client、Host 和实验 Remix 运行时，然后启动游戏。
2. 进入与 v35 相同的高压测试地图，让人物和普通感染者保持在视野内。回到游戏后按一次 F9。
3. 保持同一个场景，正常小幅转头或移动；不要再次按 F9。程序先用 8 帧把 GPU 蒙皮网格上限从 4 逐步升到 32，再固定 32 测试 120 帧。
4. 控制台要求退出时正常退出游戏，保持窗口开启，上传最终一个 rt-performance-*.zip。

v35 结论
- 3,984 次 GPU 蒙皮 Mesh 提交全部完成，创建 34、复用 3,950；回收后 Mesh、Fence、引用和读回均为 0，没有设备丢失。
- 四个 30 帧窗口为 14.04、14.38、14.33、14.70 FPS，低于 24 FPS 门槛；GPU 3D 采样平均约 17～19%。
- Host 已计时的模型转换约 6.5 ms/帧，Present 约 0.1 ms/帧，而整帧约 69 ms。剩余约 55～60 ms 位于已计时作用域之外。
- world.conf 虽请求 importance sampled，Remix 实际日志却显示 ReSTIR GI。Arc 的 Auto/Low 预设在更高层覆盖了请求，因此 v35 不能作为目标直射配置的性能基线。

v36 策略
- 在运行时创建前固定 graphicsPreset=Custom，并再次通过 Remix API 设置；最终日志必须显示 Importance Sampled 已激活且没有 ReSTIR GI 激活，否则采集自动判为不完整。
- 保持 v35 的 TAAU 0.75、直接光、关闭二次反弹、关闭 RTXDI、8 帧渐进预热和固定 32 Mesh，形成可比较结果。
- 每个窗口新增 Host 设备线程 kernel/user CPU 时间；一秒采样同时记录游戏进程与 Host 进程 CPU 增量，并与 GPU Engine 时间对齐。
- 若帧时间下降，主要差异来自修正 ReSTIR 覆盖；若帧时间仍约 69 ms，而两进程 CPU 和 Host 设备线程都很低，则下一步直接检查 Bridge IPC 唤醒/批处理。若游戏进程单核接近满载，则优先处理 Source 客户端提交路径。

窗口化或全屏均可，测试过程中不要切换显示模式。不改物理组件，不新增 Source 线程设置。restore.cmd 按安装收据恢复并保留备份。
