LDB RT Performance v37 — Arc 设备与进程生命周期修正版

本版继续暂停五分钟 Beta。v36 已证明 Custom + Importance Sampled + 32 个动态模型会在 Arc B580 上触发 VK_ERROR_DEVICE_LOST；随后画面冻结，不能算完成。v37 固定到 v35 实际跑完 128 帧的 ReSTIR GI 路径，移除没有干净生命周期证据的 32 模型档，并把关闭光追、资源归零和普通画面继续运行纳入同一次验收。

测试步骤（一次 F9）
1. 解压到新文件夹，运行 run-performance.cmd。程序会备份并安装配套的 Client、Host 和实验 Remix 运行时，然后启动游戏。
2. 进入以往的测试地图，让人物和普通感染者保持在视野内。回到游戏后按一次 F9。
3. 保持同一个场景，正常小幅转头或移动；不要再次按 F9。程序自动完成 30 帧静态 RT、60 帧上限 8、120 帧上限 16，然后关闭 RT、等待 Mesh/Fence 全部归零，再验证 60 帧普通光栅 Present。
4. 控制台要求退出时正常退出游戏，保持窗口开启。只有采集完成、没有设备丢失、资源归零、游戏和 Host 干净退出同时成立时，控制台才显示 Test acceptance: PASS。上传最终一个 rt-performance-*.zip。

v36 结论
- 第一段 30 帧完整，约 13.88 FPS；Host 设备线程仅消耗 78 ms CPU，而窗口墙钟为 2162 ms，大部分时间是等待或同步，不是 Host 线程计算饱和。
- Importance Sampled 激活约 4.15 秒后，Remix 报 VK_ERROR_DEVICE_LOST；运行时在第 91 个预览 Present 捕获 D3DERR_DEVICELOST。后续冻结且退出未经验证。
- 这说明此前显示“Collection COMPLETE”不能代表测试成功。v37 分开显示 Evidence collection 和 Test acceptance，未经干净退出一律失败。

v37 策略
- Custom 预设仍固定所有参数，但 integrateIndirectMode 明确设为 ReSTIR GI；这是选择已有存活证据的路径，不是宣称性能已解决。
- 动态模型只测 8 和 16 两档；0 模型控制段确认问题是否在动态 Mesh 供给。每档保留 Host 设备线程、游戏/Host 进程 CPU 与 GPU Engine 对齐数据。
- RT 阶段成功后主动关闭 RT，等待全部 API Mesh、GPU Fence 和纹理引用归零，再跑 60 帧普通画面。任何查询失败、设备丢失、回收超时、冻结或异常退出都会让验收失败。
- 本轮先建立不会冻结、不会以崩溃收场的 GPU 基线。通过后再依据 0/8/16 三档的等待变化优化 Bridge IPC 和 GPU 供给；人物完整度和最终帧率不在本轮验收范围内。

窗口化或全屏均可，测试过程中不要切换显示模式。不改物理组件，不新增 Source 线程设置。restore.cmd 按安装收据恢复并保留备份。
