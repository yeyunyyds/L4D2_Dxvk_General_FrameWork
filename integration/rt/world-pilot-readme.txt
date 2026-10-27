LDB RT Performance v52 — 单次渲染上下文取证

v50 已完成 90 个 RT 帧和 60 个栅格帧且没有崩溃；稳定两段为 28.8–29.0 FPS，Arc 3D 引擎单秒采样约 94%。v50 的 materialsystem 文件和入口校验均通过，失败原因是首次 GetRenderContext 返回的 vtable 不属于代码仅允许的两个主 vtable，随后被永久标记为版本不匹配。

v51 虽未调用候选槽，但在未知 context 上反复执行 GetRenderContext/Release，按 F9 后约两秒再次触发 engine.dll+0xe0940 读取 0xc。v52 恢复为与无崩溃 v50 相同的单次 context 获取，并在第一次读取完成后立即写出实际 vtable、绑定阶段／失败点和 128–159 槽；不调用任何候选虚函数，不提交人物模型，不改变 mat_queue_mode。

测试步骤（一次 F9）
1. 解压到新文件夹，运行 run-performance.cmd；程序会安装配套 Client、Host 和实验 Remix Runtime，然后启动游戏。
2. 进入以往测试地图，让一个完整人物和数个普通感染者保持在视野内。回到游戏后按一次 F9。
3. 保持人物可见并小幅转头，等待三个 30 帧窗口和随后 60 个普通 Present 自动完成；不要再次按 F9。
4. 控制台要求退出时正常退出游戏，保持窗口开启，上传最终一个 rt-performance-*.zip，并说明是否有黑屏、冻结或崩溃以及大致帧率。

本轮验收要求
- model hook 必须捕获有效人物输入，绑定阶段至少到达 context vtable 读取。
- 单次 context 获取后必须立即记录实际 vtable、命中次数和不少于 20 个完整候选代码前缀。
- 不执行候选队列函数，不产生人物、CPU pose 或 frame batch 提交。
- 90 个 RT 帧、资源归零、60 个栅格帧和安全卸载必须全部完成。

窗口化或全屏均可，测试过程中不要切换显示模式。不改物理组件，不新增 Source 线程设置。restore.cmd 按安装收据恢复并保留备份。
