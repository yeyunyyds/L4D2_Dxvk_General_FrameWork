LDB RT Performance v47 — 人物资源／实例架构验证

v43–v46 的 CPU pose、几何拼接、frame batch 和动态 Mesh 更新路线已经停止。v47 将同一 Source model scope 的可用 surface 定义为一个不可变的多 surface bind-pose 资源；之后每个角色每帧只提交一次实例和骨骼 palette，交给 Remix GPU skinning。性能窗口不会启动 Host worker pool，也不会回退到 CPU pose 或 frame batch。

测试步骤（一次 F9）
1. 解压到新文件夹，运行 run-performance.cmd。程序会安装配套 Client、Host 和实验 Remix Runtime，然后启动游戏。
2. 进入以往测试地图，让一个完整人物和数个普通感染者保持在视野内。回到游戏后按一次 F9。
3. 保持人物可见并小幅转头。程序连续运行三个 30 帧窗口，分别记录资源预热、稳定提交和复用；不要再次按 F9。
4. 观察人物身体和衣服是否全部显示，有无拉伸、碎片、黑屏、长时间卡帧或崩溃，并记录大致帧率。三段结束后会自动关闭 RT、回收资源并验证 60 个普通 Present。
5. 控制台要求退出时正常退出游戏，保持窗口开启，上传最终一个 rt-performance-*.zip，并描述人物完整度和大致帧率。

验收说明
- 每个完成的 Source model scope 必须恰好提交一个 GPU-skinned DrawInstance；最后一个 scope 必须在物理 Present 前完成。
- 多 surface 资源定义数必须大于零且小于实例数，证明同一拓扑已跨角色／帧复用。
- CPU pose、frame batch、worker 和资源失败计数必须全部为零；探索版不会用兼容路径掩盖新架构失败。
- 设备丢失、Fence 失败、资源回收超时、人物部件缺失或运行时导出缺失均失败。
- 24 FPS 暂不作为本轮结构验收门槛；结果用于判断下一步是消除重复拓扑采集/哈希，还是调整 Runtime 的 surface／BLAS 粒度。

窗口化或全屏均可，测试过程中不要切换显示模式。不改物理组件，不新增 Source 线程设置。restore.cmd 按安装收据恢复并保留备份。
