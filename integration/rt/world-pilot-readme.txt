LDB RT Source context 诊断包

本包用于核对模型调用期间是否同步发生 D3D Draw，并对照两侧线程的 TLS context。它只采集证据，不启用人物光追提交；画面和帧率不能作为光追成果验收。当前 materialsystem.dll 仅接受 SHA-256 09fd324cd977d5d4e2a216ee831c07ff8b882efd024137c27de4155fbdfcf0ba。文件不匹配会停止该诊断。

测试步骤
1. 解压到新文件夹，运行 run-performance.cmd。它会安装配套 Client、Host 和实验 Runtime，并启动游戏。
2. 进入此前使用的地图，让完整人物和普通感染者出现在视野内。按一次 F9。若 F9 无法触发，切出游戏运行 trigger_capture.cmd，并在它提示的 5 秒内切回游戏；两种触发方式只需选一种。
3. 保持人物可见，可缓慢转头；等待脚本自动结束采集。
4. 控制台要求退出时正常退出游戏。上传最终的 rt-performance-*.zip，并简述有无黑屏、冻结或崩溃。

这次主要检查有效模型调用中有多少次包含同步 D3D Draw，并记录两侧线程 ID、TLS context vtable 和候选槽位。不会调用未验证的队列方法。若游戏发生异常，保留生成的部分结果 ZIP。restore.cmd 可按安装收据恢复原文件。
