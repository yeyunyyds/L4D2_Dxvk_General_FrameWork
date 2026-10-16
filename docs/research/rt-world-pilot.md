# 受限静态世界 Remix 接入试验

本版沿用 [首轮实际 Draw](rt-draw-232229-result.md) 的消费端关联证据，开始尝试把实际世界候选交给 Remix。它不移植参考 mod 的全局 prim context，不写 `mat_queue_mode` 或其他 Source 线程 CVar，也不调用未确认的 GetCallQueue。Source producer Hook、准备线程并行和完整游戏光追仍未验收。

已发布 [rt-world-pilot-dev.zip 文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/269b5663cc777004f34383e7b3d052eb590e631b/downloads/rt-world-pilot-dev.zip)。构建提交为 `1bcbb2678bb5e9a0b3550209aea81b6a28db30dc`，包大小 11,876,515 字节，SHA256 为 `d6e70e7c7fe222d6fd1f69ead5e32e0a03e9185c6d4c655ed716b658842f775a`。该链接固定到本次已核验产物，不随下载分支更新而改变。

## 从数据到实际转换

实际 Draw 的同一 RPC 携带 v2 快照：原有矩阵/材质/viewport，以及当次读取的 64 位格式、解码状态和有界 shader 名称。v1 trace 继续可读。格式 getter 必须与已采到的 `mov eax,[ecx+0x38]; mov edx,[ecx+0x3c]; ret` 匹配；shader getter 必须与已采到的 `this+0x14` 路径匹配。随后只识别两种叶函数：返回字符串字面量，或返回对象内的小偏移指针。代码必须位于已映射的可执行 image page，名称最多 63 字节，字段复读不一致或未知实现均拒绝分类。不会执行私有材质函数。

选择条件为：有效快照、非空材质身份、`0x480003`、明确的 LightmappedGeneric 名称、单位 world、刚性 view、有限的右手透视投影、near 4～32、零起点/full-Z viewport、同尺寸 HDR 目标、不混合/不 alpha test 且写深度。它是消费端世界候选策略，不能当作完整 Source 主视图 ID 或静态对象来源证明；反射等特殊视图仍需结合实际画面审查。

`rt_static_draw.h` 的状态作用域增加消费端输入，直接使用 D3D9 行向量矩阵，没有制造 `ViewKind::Main` 或伪造 Source producer 标签。它接受原始 stream 0 声明，以及本次实测的 48 字节 stride/附加 stream 2 声明。FVF 使用 stream 0 的前 40 字节，保留实际 stride、buffer、offset 和其他流；每次 Draw 之后恢复原 shader、矩阵及完整声明。未知布局回退原绘制；getter、setter、物理 Draw 和恢复错误分别记录。

## 后端与设备所有权

Host 启动时读取本次 `LDB_RT_WORLD_1` 请求，只在该模式下加载包外缓存中的 Remix。DLL 必须匹配已通过 B580 独立试验的 SHA `21c826bac9fe6a27b28d0b1300f838efa21b010b24216c44f4728b266d8a8ec7`；完整依赖也逐文件校验。加载后核对真实模块路径，取得固定 0.6.4 API，逐项记录配置请求与返回值。

标准 `Direct3DCreate9` 入口保留 draw-call conversion。参考运行时的 API `dxvk_CreateD3D9` 会关闭 draw-call conversion，因此本路径不使用它创建游戏设备。API 只注册实际游戏设备、设置配置和读回结果，独立翻译单元避免与 Bridge 原有 API 版本混用。参考 API 的 `Shutdown` 会对注册的设备/Direct3D 对象反复 Release 到零，不能用于 Bridge 所拥有的设备；测试保留正常 Bridge 生命周期，资源释放验收继续未完成。

显式关闭 `rtx.useVertexCapture`，避免自动采集未知的游戏 VS。**这不会在光追开启时保留这些绘制的 raster 路径**：固定运行时会将它们判为 Ignored，并移除原 Draw。v1 从启动就打开 RT，导致菜单也被移除，详见 [启动黑屏回包分析](rt-world-093012-result.md)。v2 启动及 F9 前 30 Present 均关闭 RT，只有成功应用第一个合格世界 Draw 的状态作用域才请求开启；请求由运行时在帧边界应用。采集结束、失败或 Reset 请求关闭，并禁止继续转换。完整混合合成未完成，光追开启后动态模型、武器、天空、透明和 HUD 仍可能缺失。

暂不移植原地图灯光：设置名义 Source 英寸比例 `sceneScale=1/2.54`，并用相机球形诊断灯观察室内几何。该光照不是原版游戏效果。

## 一个入口的一次测试

发行包 `rt-world-pilot-dev.zip` 的 `run-test.cmd` 复用既有 preflight 缓存或下载固定 SHA 依赖，等待正常退出游戏，校验并继承相邻旧观察包的原始备份，安装 x86 client/x64 Host，再由 Steam 启动。没有改 backend 文件、Source CVar 或 Steam 启动选项；请求仅对本次 Host 启动选择 Remix。

进图后按一次 F9。前 30 次 Present 保留未转换光栅观察，之后尝试选定世界转换并请求开启 RT，总共 240 次 Present；这些都是同一个 Remix 后端，不能冒充普通 DXVK 的 R0 对照。30/120/240 后分别用 API 读回颜色、primary depth、primary world shading normal，输出为 640×400 最近邻缩放的 RGBA32F 和预览。目标先清零：参考 API 在没有源图时也可能返回成功，因此检查实际缓冲内容，不能只检查返回值。

v2 在 F9 前独立保留有界启动模块清单、Host 首次实际 Draw/绘制总数和 client 的请求/F9/前台 PID 记录。菜单若仍黑屏可直接正常退出，不要求盲操作；脚本会保存 INCOMPLETE 部分包。最终的完成提示和退出码区分完整采集与部分结果；模块清单成功或库加载成功都不表示 240 Present 已完成。用户主动退出与退出阶段 ReShade/驱动异常也分别记录，不能倒推成游戏此前自动崩溃。

采集完成先读取仍在运行的模块和 0～255 context 槽位，再提示正常退出游戏，最终日志与数据合并为一个 ZIP。ConVar 只保存 SDK 布局候选、原始字段及模块身份，不把读到一个整数当作 ABI 或并行通过。退出、异常、模块失败和预算截断保留分项状态。旧 40,952-Draw 数据仍能由新版分析器复核。

诊断期持续逐 Draw getter/内存读取，并同步读回，因此不评价 60 FPS 预算。结果约 192 MiB 原始文件、每 trace 128 MiB，最多等待 15 分钟启动/F9、5 分钟采集；游戏退出或明确 runtime 错误会提前保存部分包。`restore.cmd` 按原始备份恢复配套文件，并拒绝覆盖测试后的用户更新。

## 发行前检查及实际边界

检查包括有界矩阵/名称读取与旧协议兼容、世界/武器/控制段过滤、精确布局、getter/应用/恢复失败、原始备份继承和升级回滚、配置缺项、API 返回成功但目标全零的拒绝，以及旧真实 trace 的回归。Windows 同时执行 x86/x64 的模拟设备/真实文件用例和系统 D3D9 NULLREF 状态检查，另执行固定 DLL 的 CNG/真实路径/0.6.4 配置初始化正反例；后者不创建 GPU 或游戏设备。

发布构建和包内 Python/依赖加载验证以本版交付记录为准。云端没有 L4D2/B580 的实际运行，不能据此宣布游戏 RT、完整静态世界、Source 准备线程并行或性能通过。下一次用户结果用于同时审查世界转换、实际输出、材质失败原因和缺失模块信息，不再补一次独立三角形或相同的纯观察段。

本次 [交付核验记录](../../research/rt-world-pilot-validation.json) 已完成：从固定 GitHub 提交重新下载成品，验证全部 ZIP 成员哈希、发行脚本/配置/README 与源码一致、配套 PE 架构和编译身份、原生源码输入指纹；分别重析 x86/x64 的观察与世界转换 trace。每种架构的世界用例均为 240 Present/720 Draw，其中 210 次符合条件的世界绘制转换，控制段、武器和 HUD 候选保持原绘制，未出现状态恢复错误。这些是模拟设备用例，不能充当实际游戏分类或 GPU RT 证据。另已通过双架构系统 D3D9 NULLREF 状态测试、固定运行时错误 SHA 拒绝与真实 DLL/API 0.6.4 配置初始化、包内 Python 入口与完整配置检查、40,952 次原始游戏 Draw 的兼容性回归。本地 Python 检查为 75 项通过、2 项需 Windows 而跳过。

发行验证曾发现 Windows 依赖 DLL 被父 Python 进程保留，导致临时目录清理失败；现将加载/导出检查放在独立进程执行，退出后释放其依赖，重新完成 Windows 打包、目录清理和发布。此修正不调用 GPU 或私有游戏方法。
