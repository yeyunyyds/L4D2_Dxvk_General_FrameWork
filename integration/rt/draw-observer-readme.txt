LDB 1.3.2：真实游戏 Draw 消费端观察包 v1

这次采集已经进入真实游戏绘制链。它用于确认当前 Draw 的矩阵、材质、顶点布局及跨进程命令对应，仍不会让游戏开启光追，也不等于 Source 队列/多线程验收。

前提：Windows x64，Steam，以及已正常运行的 LDB 1.3.2。沿用现有 backend/runtime；无需三个大依赖 ZIP，无需安装 Python。

操作（一个入口）：
1. 将本 ZIP 解压到独立目录，双击 run-test.cmd。
2. 首次运行会先等待你正常退出 L4D2，再备份并替换 client/Host 两个文件。Windows 无法替换正在使用的 DLL/EXE，因此必须先退出；脚本不会强制关闭游戏。随后会通过 Steam 启动游戏。
3. 进入地图、回到游戏后按 F9 一次，正常游玩即可；120 次 Present 后自动停止。不要关闭游戏，等待控制台收集模块并显示结果 ZIP。
4. 上传 results 中新生成的一个 rt-draw-*.zip。

若脚本未找到游戏目录，可将游戏根目录作为 --game 参数传入，例如 run-test.cmd --game "D:\Steam\steamapps\common\Left 4 Dead 2"。
不需要 F8、capture.trigger、独立三角形复测或三组分开采集。F9 仅启动这次观察，不是插帧开关。
默认等待启动/F9 最长 15 分钟。诊断数据最多持续 60 秒；单 Host trace 约 128 MiB 上限，完整结果原始文件约 192 MiB 上限。超时、退出和失败也保留一个 ZIP。

安装仅替换 bin\dxvk_d3d9.dll 和 bin\.l4d2bridge\L4D2Bridge64.exe；原文件、SHA 和恢复凭据保存在本目录 backups 及 installed-observer.json。其他 LDB 文件、图形 backend、游戏 CVar 和启动选项均保留。
结束测试后，退出游戏，运行 restore.cmd 恢复这两个原文件。请保留本目录及 backups，直到恢复完成。恢复程序会校验备份，并拒绝覆盖测试之后发生的用户文件更新。

技术范围：
- 四种 Draw 的状态快照与参数放在同一 RPC，Host 用同一个实际 Draw UID 记录，不靠相邻消息或主线程全局变量猜配。
- 对固定 SHA 的 shaderapivk.dll，核对实例 vtable、方法地址和代码形状后，只读取矩阵栈和当前材质指针。不会调用未知 GetCallQueue、GetBufferedState 或材质虚方法，不安装额外 Source Hook。
- 合并采集矩阵、viewport、render target 尺寸/格式、顶点声明/步长和深度/混合状态；同时采集出现过的材质对象与方法代码，以及 render context 的完整候选槽位窗口。
- 原始 raster Draw 始终保留，不修改线程 CVar，不把 LightmappedGeneric 或主视图分类猜测当作事实。静态转换与游戏 RT 暂不启用。
- 正确的矩阵和消费顺序不证明 Source 准备线程并行。当前包仍把 Source 队列、游戏 RT 和性能标为未验收。
- 逐 Draw getter 和内存读取仅用于正确性诊断，采集期间帧率不作为性能结果。

结果内的脚本成功/原生 fixture 通过，不代表实际游戏接口已经验收；以这次实际日志和独立分析为准。
