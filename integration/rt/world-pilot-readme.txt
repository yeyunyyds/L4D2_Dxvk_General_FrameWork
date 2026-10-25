LDB RT Performance v44 — 持久 Remix Mesh 更新验证

v43 已把 CPU 姿态重建移到四个 Host worker，并把兼容的 Source 部件合并成材质/变换批次。v44 进一步修改固定 Remix Runtime：动态 CPU 姿态批次在预热时只创建一次 Mesh，之后通过项目专用 remixapi_LdbUpdateMesh 导出更新同一句柄。静态和刚体批次尚无稳定资源 ID，继续走逐帧批次，避免错误共享。

测试步骤（一次 F9）
1. 解压到新文件夹，运行 run-performance.cmd。程序会安装配套 Client、Host 和包含持久 Mesh 导出的实验 Remix Runtime，然后启动游戏。
2. 进入以往的测试地图，让一个完整人物和普通感染者保持在视野内。回到游戏后按一次 F9。
3. 保持人物可见并小幅转头。程序依次运行 30 个 GPU-80 对照帧、30 个普通帧批次、30 个持久句柄更新帧；不要再次按 F9。
4. 观察人物身体和衣服是否全部显示，有无拉伸、碎片、黑屏、长时间卡帧或崩溃。三段结束后会自动关闭 RT、回收资源并验证 60 个普通 Present。
5. 控制台要求退出时正常退出游戏，保持窗口开启，上传最终一个 rt-performance-*.zip，并描述人物完整度和三段的大致帧率。

验收说明
- 持久段第一帧可以创建动态批次槽；其余 29 帧必须保持动态 CreateMesh 为零并产生同句柄 UpdateMesh。
- 所有接受的 Source Draw、API 批次和持久 Create/Update 计数必须闭合，预算遗漏必须为零。
- 设备丢失、Fence 失败、资源回收超时、模型重新缺失或运行时导出缺失均失败。
- 这一步先验证生命周期和吞吐；Runtime 内部仍可能为每次更新分配新 Buffer 并重建 BLAS，结果将决定下一层是否改为环形 GPU Buffer 和真正的增量 AS 更新。
- 24 FPS 暂不作为通过门槛。

窗口化或全屏均可，测试过程中不要切换显示模式。不改物理组件，不新增 Source 线程设置。restore.cmd 按安装收据恢复并保留备份。
