LDB 1.3.2：受限静态世界 Remix 接入试验 v3（补齐实际世界 shader 名称读取）

本版开始尝试在真实游戏中转换选定的 LightmappedGeneric 世界绘制，并加载已在 B580 独立测试通过的同一版 Remix。它不是完整 l4d2-rtx 移植；动态模型、武器、天空、透明与 HUD 合成尚未验收，画面可能缺失这些内容。Source 队列和性能也不以本包的诊断帧率验收。

操作仍是一个入口、一个结果 ZIP：
1. 解压到独立目录，建议放在之前 rt-preflight-dev / rt-game-observe-dev 文件夹旁边。保留旧包的 backups 和 installed-observer.json。
2. 双击 run-test.cmd。脚本会优先复用已校验的旧 preflight runtime；没有时自动获取固定 SHA 的依赖，不需要手动安装 Python。等待提示后正常退出已有游戏，脚本会备份/安装配套 client 与 Host 并通过 Steam 启动。
3. 主菜单应先有普通光栅画面。进入地图，回到游戏按一次 F9，缓慢转头或移动。采集先观察 30 次 Present，再在材质/相机/布局和状态转换均通过后请求开启光追，总计 240 次 Present。首次编译光追 shader 可能较慢；不要反复按 F9。
4. 按控制台提示正常退出游戏。脚本会先读取仍在运行的游戏模块，再等游戏/Host 退出并收集最终日志。结束时明确显示 COMPLETE 或 INCOMPLETE；后者是部分证据，不代表测试通过。上传 results 中一个 rt-world-*.zip。

若主菜单仍黑屏，直接正常退出游戏，等待生成这个结果 ZIP；无需盲操作 map 或 F9。启动模块、Host 首次绘制、F9/焦点轮询与最终日志会一并保留。

不需要 F8、capture.trigger、设置环境变量、重复独立三角形测试或分三组采集。不要在该次运行中切换 DXVK/Remix 或 mat_queue_mode。

安装仍只替换 bin\dxvk_d3d9.dll 和 bin\.l4d2bridge\L4D2Bridge64.exe；同目录/相邻旧观察包的原始恢复备份会校验后继承。找不到旧观察包恢复凭据时会停止，不把旧测试版本当作原版备份。
游戏 backend 文件、启动参数和 Source 线程 CVar 不改。Host 只在启动时看到本次请求文件才从测试包 cache 加载固定 SHA 的 Remix；请求会在收尾时删除。测试结束、游戏退出后运行 restore.cmd 恢复原配套文件；请保留本目录和 backups 直到恢复完成。

测试内容及边界：
- 验证 shaderapivk 模块和矩阵栈布局，矩阵、实时 VertexFormat、可解码的 shader 名称与实际 Draw 同一 RPC 传输；未知代码只留证据，不执行私有方法。
- 支持实际 stdshader_dx9 的 A1 全局字符串指针 getter：只读取指针和有界名称，复读代码、指针与材质身份，发现变化即拒绝。LightmappedGeneric 白名单不放宽，NekoToon 等自定义 shader 不会因格式相同而被当作世界。
- 只选 0x480003、LightmappedGeneric 白名单、单位 world、刚性透视相机、完整深度范围、HDR 同尺寸目标、不混合/不 alpha test 且写深度的候选。
- 支持已观察到的 48 字节步长及附加 stream 2 声明。FVF 只读取 stream 0 的前 40 字节，不改顶点数据或绑定步长；每次 Draw 后恢复原矩阵、shader 和完整声明。
- 使用标准 D3D9 draw-call conversion，固定 API 0.6.4 只做配置和输出读回，不与 Bridge 原有 API 混用，不调用会强制释放游戏设备的 standalone Shutdown。
- 启动时显式关闭光追。F9 的前 30 Present 也是 Remix 光栅；成功转换首个合格世界 Draw 才请求开启。开关由固定运行时在帧边界应用，不能把请求成功当成实际 GPU 光追通过。采集结束/失败/Reset 时请求关闭并停止再次转换。
- 该运行时在光追开启后会丢弃未转换的可编程绘制，故武器、动态模型和 HUD 仍可能缺失；关闭 vertex capture 本身不提供混合光栅合成。
- 按 Source 名义英寸单位设置 sceneScale，并启用随相机的球形诊断灯，让室内几何可观察。原地图灯光尚未移植；此灯光配置只用于本次正确性试验。
- 30/120/240 Present 后分别保存 640×400 的颜色、深度、法线 RGBA32F 诊断数据和 PNG 预览；这些经过明确的最近邻缩放，不能作为全分辨率画质验收。
- 读回目标先清零，避免 API 返回成功而没有源图时把未初始化显存当有效图。最终会检查实际内容，仍需结合画面与线程证据复核。
- 新旧 trace 均可分析，记录所有转换拒绝/状态恢复结果。模块采集失败仍保留独立 Draw 分析。完整日志与原始数据放在同一个 ZIP。
- 240 Present 的首轮冷启动与逐 Draw getter/读回是正确性试验，不能据此判断目标 60 FPS 或 Source 准备线程是否并行。

采集等待启动/F9 最长 15 分钟；F9 后诊断最多持续 5 分钟，单 trace 约 128 MiB，结果原始文件约 192 MiB 上限。超时或崩溃也会留下部分结果；不要无限重试，上传这一个 ZIP 即可。
