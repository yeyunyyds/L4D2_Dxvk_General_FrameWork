LDB 1.3.2：受限世界 Remix 接入试验 v6（场景覆盖扩展 + Neko HUD 合成入口）

一个入口、一次 F9、一个结果 ZIP：
1. 解压到独立目录，建议放在旧测试包旁边。保留旧包的 backups 和 installed-observer.json，供自动继承原始恢复备份。
2. 双击 run-test.cmd。脚本校验并复用固定 runtime 缓存，等待已有游戏正常退出，备份/安装配套 x86 client 与 x64 Host，再通过 Steam 启动。
3. 主菜单应有正常光栅画面。进入上次地图，面向缺失白墙和地面，按一次 F9 后保持镜头静止。总计 240 次 Present，自动停止；首次 RT shader 初始化可能停顿，请等待。
4. 按提示正常退出游戏，等待控制台输出最终 COMPLETE / INCOMPLETE 和 ZIP 路径，上传 results 中一个 rt-world-*.zip。不需要手动压缩中间目录。

这次不再切换三种灯光。全过程使用 v5 已能看清场景的同型强球灯（半径 5，辐射度 3000），自动曝光与 tone mapping 参数保持一致：
- 1–30 Present：普通 Remix 光栅控制段。
- 31–120：保留此前 LightmappedGeneric 世界转换，作为亮度一致的覆盖对照。
- 121–240：增加经过精确布局检查的 UnlitGeneric 不透明场景候选，并尝试在 Neko_Engine_Post 处触发运行时原生 UI 合成；180/240 保存扩展后的结果。
30/120/180/240 各保存 640×400 颜色、投影深度、解码法线预览和原始 RGBA32F，共 12 份原始读回；用于正确性验证，不是全分辨率画质或性能验收。

新增场景路径只接受实机记录的 0x80503 / UnlitGeneric、单位 world、刚性主视图候选、完整深度范围、同尺寸 HDR 目标、不透明且写深度、stream0 步长 32 的完整 FLOAT3 位置/FLOAT3 法线/FLOAT2 UV 及精确附加声明。使用固定功能 XYZ|NORMAL|TEX1；原 VS、PS、完整声明和矩阵逐 Draw 恢复。压缩法线、骨骼/蒙皮声明、武器相机、未知材质和布局继续拒绝。它不保证上次缺失的白墙恰好属于这一路径，也不是所有模型接入。

HUD 路径仅匹配本次实际记录的 Neko_Engine_Post、0x80001、单位矩阵、精确声明、无深度/混合的完整 backbuffer 绘制，并核对 RT 就是实际 swapchain backbuffer。仅在扩展阶段且已请求开启 RT 时尝试。
它用 alpha=0 的固定功能屏幕三角形触发固定 Remix 的 orthographic UI 注入，再保留其后原生 UI 绘制。成功后跳过该次原 Neko 后处理，避免它用未完成的光栅 HDR 画面覆盖 RT。状态由 D3DSBT_ALL 保存/恢复，包含 DrawPrimitiveUP 会清除的 stream0；任何捕获、setter、绘制或恢复错误都记录。
本版不等于恢复全部 Neko 后处理效果；原 Neko 色调/受伤淡入淡出、武器和蒙皮模型仍未移植。HUD 是否实际显示，必须由本轮图像确认，不能只看注入 draw 的 S_OK。

原地图灯光、天空、透明与全部动态模型未验收。没有写 Source 线程 CVar，没有移植参考 mod 的全局 prim context；Source 准备线程并行和性能继续待验收。逐 Draw 观察、状态 getter、文件记录与同步读回会影响诊断帧率。

安装只替换 bin\dxvk_d3d9.dll 与 bin\.l4d2bridge\L4D2Bridge64.exe；原始恢复备份逐哈希核对并继承，不把旧观察包当原版。游戏 backend、启动参数不改。Host 只在启动时看到本次请求才从缓存加载固定 SHA 的 Remix。测试结束并退出游戏后可运行 restore.cmd 恢复原配套文件；请保留本目录和 backups 到恢复完成。

主菜单若黑屏可直接正常退出，等待一个最终结果 ZIP，无需盲操作进图。F9/模块/Draw/注入日志、材质样本和最终 Bridge 日志会一起保留。不需要 F8、capture.trigger、环境变量或重复独立三角形测试。
采集等待启动/F9 最长 15 分钟，F9 后最多 5 分钟；世界模式保留 60 秒有限 IPC 容量等待与 peer 检查；单 trace 128 MiB、结果原始文件约 192 MiB。超时或崩溃会保留 INCOMPLETE 结果，请仍等最终 ZIP 路径。
