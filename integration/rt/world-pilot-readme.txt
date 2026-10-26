LDB RT Performance v46 — 持久拓扑身份与 BLAS refit 验证

v43 已把 CPU 姿态重建移到四个 Host worker，并把兼容的 Source 部件合并成材质/变换批次。v44-v45 证明仅复用外层 Mesh 句柄几乎没有性能收益，因为 Runtime 仍为每次更新生成新的拓扑哈希，Remix 会创建新的场景几何/BLAS。v46 从持久批次第一帧起使用专用 upsert，并在索引和布局未变时保持拓扑身份，顶点代次独立变化，使 Remix 进入既有 kUpdateBVH 与动态 BLAS refit 路径。可预期的不支持模型 Pass 继续只回退该 Draw。静态和刚体批次尚无稳定资源 ID，继续走逐帧批次，避免错误共享。

测试步骤（一次 F9）
1. 解压到新文件夹，运行 run-performance.cmd。程序会安装配套 Client、Host 和包含持久 Mesh 导出的实验 Remix Runtime，然后启动游戏。
2. 进入以往的测试地图，让一个完整人物和普通感染者保持在视野内。回到游戏后按一次 F9。
3. 保持人物可见并小幅转头。程序依次运行 30 个 GPU-80 对照帧、30 个普通帧批次、30 个持久句柄更新帧；不要再次按 F9。
4. 观察人物身体和衣服是否全部显示，有无拉伸、碎片、黑屏、长时间卡帧或崩溃。三段结束后会自动关闭 RT、回收资源并验证 60 个普通 Present。
5. 控制台要求退出时正常退出游戏，保持窗口开启，上传最终一个 rt-performance-*.zip，并描述人物完整度和三段的大致帧率。

验收说明
- 持久段第一帧必须通过专用 upsert 建立动态批次槽；其余 29 帧保持动态 CreateMesh 为零并更新同一拓扑身份。
- 单个 model_pipeline_unsupported 或 model_pipeline_special_pass 可以计入 frame_batch_input_fallbacks，但不得关闭 RT 或中止其余批次。
- 所有接受的 Source Draw、API 批次和持久 Create/Update 计数必须闭合，预算遗漏必须为零。
- 设备丢失、Fence 失败、资源回收超时、模型重新缺失或运行时导出缺失均失败。
- Runtime 目前仍为每次更新分配输入 Buffer，但稳定拓扑必须让场景缓存复用 BlasEntry，并让动态 BLAS 走 update/refit。结果将决定下一层是否继续改成环形输入 Buffer。
- 24 FPS 暂不作为通过门槛。

窗口化或全屏均可，测试过程中不要切换显示模式。不改物理组件，不新增 Source 线程设置。restore.cmd 按安装收据恢复并保留备份。
