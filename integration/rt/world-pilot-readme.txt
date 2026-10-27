LDB RT Performance v50 — Source 队列 ABI 恢复与 14 FPS 等待归因

v49 在 F9 后约 2.4 秒于 engine.dll+0xe0940 读取 0xc 崩溃，证明调用的 Source 队列候选并非已验证的 ICallQueue ABI。v50 完全移除该虚函数调用，只读记录 queued render context 的 140–150 槽地址与各 16 字节代码前缀，同时保留已验证的 DrawModelExecute 入口 Hook。人物提交在本轮有意关闭，避免错误身份污染；mat_queue_mode 不变。同一次测试自动开启 Client/Host API wait 诊断，以 1 秒快照拆分 Present、IPC submit、队列满、命令锁、Host backend 与未归因时间。

测试步骤（一次 F9）
1. 解压到新文件夹，运行 run-performance.cmd。程序会安装配套 Client、Host 和实验 Remix Runtime，然后启动游戏。
2. 进入以往测试地图，让一个完整人物和数个普通感染者保持在视野内。回到游戏后按一次 F9。
3. 保持人物可见并小幅转头。程序连续运行三个 30 帧窗口，分别记录资源预热、稳定提交和复用，同时记录两端等待；不要再次按 F9。
4. 观察人物身体和衣服是否全部显示，有无拉伸、碎片、黑屏、长时间卡帧或崩溃，并记录大致帧率。三段结束后会自动关闭 RT、回收资源并验证 60 个普通 Present。
5. 控制台要求退出时正常退出游戏，保持窗口开启，上传最终一个 rt-performance-*.zip，并描述人物完整度和大致帧率。

验收说明
- 每个完成的 Source model scope 必须恰好提交一个 GPU-skinned DrawInstance；最后一个 scope 必须在物理 Present 前完成。
- queued、begin、end 计数必须完全相等，surface 命中必须大于零，取消和队列失败必须为零；采集结束时函数钩子必须在引擎仍加载时成功解除。
- 多 surface 资源定义数必须大于零且小于实例数，证明同一拓扑已跨角色／帧复用。
- CPU pose、frame batch、worker 和资源失败计数必须全部为零；探索版不会用兼容路径掩盖新架构失败。
- 设备丢失、Fence 失败、资源回收超时、人物部件缺失或运行时导出缺失均失败。
- 24 FPS 暂不作为本轮结构验收门槛；api_wait 报告会先判断约 68 ms/帧来自 Client/IPC/Host 哪一段，再决定是否改传输、调度或 Runtime。

窗口化或全屏均可，测试过程中不要切换显示模式。不改物理组件，不新增 Source 线程设置。restore.cmd 按安装收据恢复并保留备份。
