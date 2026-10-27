LDB RT Performance v51 — 实际渲染上下文恢复

v50 已完成 90 个 RT 帧和 60 个栅格帧且没有崩溃；稳定两段为 28.8–29.0 FPS，Arc 3D 引擎单秒采样约 94%。v50 的 materialsystem 文件和入口校验均通过，失败原因是首次 GetRenderContext 返回的 vtable 不属于代码仅允许的两个主 vtable，随后被永久标记为版本不匹配。

v51 仍是只读安全探测。它记录绑定通过的具体阶段／失败点、最多四种实际 context vtable、出现次数，以及每种 context 的 128–159 槽目标和前 16 字节。未知 context 会有限重复观测；不调用任何候选虚函数，不提交人物模型，不改变 mat_queue_mode。

测试步骤（一次 F9）
1. 解压到新文件夹，运行 run-performance.cmd；程序会安装配套 Client、Host 和实验 Remix Runtime，然后启动游戏。
2. 进入以往测试地图，让一个完整人物和数个普通感染者保持在视野内。回到游戏后按一次 F9。
3. 保持人物可见并小幅转头，等待三个 30 帧窗口和随后 60 个普通 Present 自动完成；不要再次按 F9。
4. 控制台要求退出时正常退出游戏，保持窗口开启，上传最终一个 rt-performance-*.zip，并说明是否有黑屏、冻结或崩溃以及大致帧率。

本轮验收要求
- model hook 必须捕获有效人物输入，绑定阶段至少到达 context vtable 读取。
- 至少记录一种实际 context vtable、命中次数和不少于 20 个完整候选代码前缀。
- 不执行候选队列函数，不产生人物、CPU pose 或 frame batch 提交。
- 90 个 RT 帧、资源归零、60 个栅格帧和安全卸载必须全部完成。

窗口化或全屏均可，测试过程中不要切换显示模式。不改物理组件，不新增 Source 线程设置。restore.cmd 按安装收据恢复并保留备份。
