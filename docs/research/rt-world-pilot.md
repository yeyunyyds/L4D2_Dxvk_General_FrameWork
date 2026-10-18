# 受限静态世界 Remix 接入试验

本版沿用 [首轮实际 Draw](rt-draw-232229-result.md) 的消费端关联证据，开始尝试把实际世界候选交给 Remix。它不移植参考 mod 的全局 prim context，不写 `mat_queue_mode` 或其他 Source 线程 CVar，也不调用未确认的 GetCallQueue。Source producer Hook、准备线程并行和完整游戏光追仍未验收。

已发布 [rt-world-pilot-dev.zip 文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/c557e24718f780d7dca0727135fb413e5f6e152b/downloads/rt-world-pilot-dev.zip)。当前为 [v11 原模型变换与天空接口合并修正](rt-world-v11-inputs.md)，构建 `ed656a6cdeb68b8ec439046a93e85575949a2f69`，包大小 11,940,088 字节，SHA256 `f9c8728c10b7d004a9cb37b3501f75581d5a554c643ef0683494b4ccc8676379`。固定发布树的全部成员、配套PE架构/身份、源码收据和脚本已独立核验；[验证记录](../../research/rt-world-pilot-v11-validation.json) 包含Windows双架构原生检查、固定DLL/API加载和两架构fixture重析。48份完整实际输入的CPU回归及六轮共587,528 Draw兼容性回归通过；本版B580最终图像、覆盖和遮挡仍待一次F9实机验收。

此前 [v10 实机复核](rt-world-191200-result.md) 确认采集完整、原世界/HUD保留，但静态模型扩展未通过；实际输入定位到原VS模型摆放变换遗漏，以及天空多组UV和大坐标精度问题。v11据此合并修正并交付，不再要求原样重采。

[v4 实机完整回包](rt-world-130102-result.md) 已独立复核通过 240 Present、193,262 Draw、11,805 次受限世界转换，9 份读回齐全、无状态恢复错误、无本轮 IPC fault。按实际 primary GBuffer 编码离线解出的法线/深度轮廓与地图表面相符，受限静态世界的 RT 输出链路已跑通。读回仍偏暗，武器/HUD 等内容缺失；完整场景、照明材质正确性、Source 准备线程并行、资源释放和性能继续待验收。基础数据已经足够，下一步转向画面正确性与覆盖补全。

本版 [v5 灯光与材质合并验证](rt-world-v5-lighting.md) 用同一次 240 Present 比较原球灯、较强同型球灯与定向灯，同时保存有界纹理阶段样本及正确解码的法线预览。该对照要求按一次 F9 后保持镜头静止。

[v5 实机灯光对照结果](rt-world-165351-result.md) 已完成独立复核：240 Present、76,825 Draw、2,730 次世界转换，12 份读回齐全；相机一致、36 份材质样本无状态变化，较强球灯使原始颜色加权均值提高约 14.13 倍。确认旧诊断灯对偏暗有显著影响。近处白墙与武器/HUD/天空仍有覆盖缺口，下一阶段转向缺失场景与合成，不再重复相同灯光采集；Source 并行及性能继续待验收。

此前 [v6 覆盖扩展与 UI 接入](rt-world-v6-coverage.md) 在同一强球灯下，比较原世界路径与新增浮点 Unlit 场景候选及 Neko 后处理处的原生 UI 合成入口；仍为一个 F9、一个结果 ZIP。

[v6 实机回包](rt-world-192551-result.md) 已独立确认 240 Present、83,920 Draw、3,630 次世界转换和 120 次 HUD 合成；HUD 图像恢复，但近处白墙仍无预期遮挡。该版格式白名单排除了每帧六次 stride64/80 的带切线 LightmappedGeneric 场景绘制，成为 v7 精确扩展的依据；当时尚未锁定白墙的具体 Draw。

此前 [v7 精确切线布局扩展](rt-world-v7-tangent-coverage.md) 已发布：31–120 保留 v6 世界/Unlit/HUD，121–240 只额外增加严格验证的 stride64/80 LightmappedGeneric；灯光、HUD 与镜头对照不再同时变化。原始 trace 记录 CULLMODE，新增有界材质/布局/RPC 归属表，仍为一次 F9、一个结果 ZIP。两种新布局的 Windows 双架构状态/回滚检查及成品核验已通过。[v7 B580 实机结果](rt-world-202526-result.md) 进一步确认本镜头内白墙和左侧结构恢复颜色、RT 深度/法线及遮挡，新增 720 次转换成功，HUD 两段均正常；这处缺墙无需重复采集。

此前 [v8 天空与有限刚性道具](rt-world-v8-sky-rigid.md) 已通过构建和打包核验并发布：保留完整 v7 为基线，同一轮依次加入 NekoSky 与两种无骨骼声明的 VertexLitGeneric。仍只需一次 F9。[本轮实机分析](rt-world-141921-result.md) 已完成：720次天空转换后背景恢复，但云层映射待校正；60次道具转换尚未显示明确新增表面。无需原样重采。

此前 [v9 受限原始天空 VS 重建与场景输入诊断](rt-world-v9-sky-input.md) 已通过双架构构建、原生检查和成品核验并发布：保持v8天空对照，六面预检完整成功后尝试重建原VS位置/UV，同时采集成功道具及受限候选的实际输入。仍为一个F9，未知shader保留v8并自动记录原因。

此前 [v10 成批静态场景扩展](rt-world-v10-static.md) 已构建、核验并发布。基于 v9 实现更多声明家族、正尺度/平移变换及透明裁切，并扩大分组输入采样；Windows x86/x64 新增及既有检查均通过。仍为一次 F9，完整蒙皮/PBR与Source并行不随覆盖扩展自动验收。

## 从数据到实际转换

实际 Draw 的同一 RPC 携带 v2 快照：原有矩阵/材质/viewport，以及当次读取的 64 位格式、解码状态和有界 shader 名称。v1 trace 继续可读。格式 getter 必须与已采到的 `mov eax,[ecx+0x38]; mov edx,[ecx+0x3c]; ret` 匹配；shader getter 必须与已采到的 `this+0x14` 路径匹配。随后识别返回字符串字面量、读取对象内小偏移指针，或 v3 补充的 `A1 <absolute>; ret` 全局字符串指针叶函数。全局指针槽和指向的名称都复读，发生变化则拒绝；槽地址从当前已验证代码读取，不硬编码 ASLR 地址。代码必须位于已映射的可执行 image page，名称最多 63 字节，字段复读不一致或未知实现均拒绝分类。不会执行私有材质函数。

选择条件为：有效快照、非空材质身份、`0x480003`、明确的 LightmappedGeneric 名称、单位 world、刚性 view、有限的右手透视投影、near 4～32、零起点/full-Z viewport、同尺寸 HDR 目标、不混合/不 alpha test 且写深度。它是消费端世界候选策略，不能当作完整 Source 主视图 ID 或静态对象来源证明；反射等特殊视图仍需结合实际画面审查。

`rt_static_draw.h` 的状态作用域增加消费端输入，直接使用 D3D9 行向量矩阵，没有制造 `ViewKind::Main` 或伪造 Source producer 标签。它接受原始 stream 0 声明，以及本次实测的 48 字节 stride/附加 stream 2 声明。FVF 使用 stream 0 的前 40 字节，保留实际 stride、buffer、offset 和其他流；每次 Draw 之后恢复原 shader、矩阵及完整声明。未知布局回退原绘制；getter、setter、物理 Draw 和恢复错误分别记录。

## 后端与设备所有权

Host 启动时读取本次 `LDB_RT_WORLD_1` 请求，只在该模式下加载包外缓存中的 Remix。DLL 必须匹配已通过 B580 独立试验的 SHA `21c826bac9fe6a27b28d0b1300f838efa21b010b24216c44f4728b266d8a8ec7`；完整依赖也逐文件校验。加载后核对真实模块路径，取得固定 0.6.4 API，逐项记录配置请求与返回值。

标准 `Direct3DCreate9` 入口保留 draw-call conversion。参考运行时的 API `dxvk_CreateD3D9` 会关闭 draw-call conversion，因此本路径不使用它创建游戏设备。API 只注册实际游戏设备、设置配置和读回结果，独立翻译单元避免与 Bridge 原有 API 版本混用。参考 API 的 `Shutdown` 会对注册的设备/Direct3D 对象反复 Release 到零，不能用于 Bridge 所拥有的设备；测试保留正常 Bridge 生命周期，资源释放验收继续未完成。

显式关闭 `rtx.useVertexCapture`，避免自动采集未知的游戏 VS。**这不会在光追开启时保留这些绘制的 raster 路径**：固定运行时会将它们判为 Ignored，并移除原 Draw。v1 从启动就打开 RT，导致菜单也被移除，详见 [启动黑屏回包分析](rt-world-093012-result.md)。v2 启动及 F9 前 30 Present 均关闭 RT，只有成功应用第一个合格世界 Draw 的状态作用域才请求开启；请求由运行时在帧边界应用。采集结束、失败或 Reset 请求关闭，并禁止继续转换。完整混合合成未完成，光追开启后动态模型、武器、天空、透明和 HUD 仍可能缺失。

暂不移植原地图灯光：设置名义 Source 英寸比例 `sceneScale=1/2.54`，并用相机球形诊断灯观察室内几何。该光照不是原版游戏效果。

## 一个入口的一次测试

发行包 `rt-world-pilot-dev.zip` 的 `run-test.cmd` 复用既有 preflight 缓存或下载固定 SHA 依赖，等待正常退出游戏，校验并继承相邻旧观察包的原始备份，安装 x86 client/x64 Host，再由 Steam 启动。没有改 backend 文件、Source CVar 或 Steam 启动选项；请求仅对本次 Host 启动选择 Remix。

进图后面向同时包含地面与墙壁的位置，按一次 F9 后保持镜头静止。前 30 次 Present 保留未转换光栅观察，之后尝试选定世界转换并请求开启 RT，总共 240 次 Present；这些都是同一个 Remix 后端，不能冒充普通 DXVK 的 R0 对照。30/120/180/240 后分别用 API 读回颜色、primary depth、primary world shading normal，输出为 640×400 最近邻缩放的 RGBA32F 和预览。目标先清零：参考 API 在没有源图时也可能返回成功，因此检查实际缓冲内容，不能只检查返回值。

v2 在 F9 前独立保留有界启动模块清单、首次 Draw 到达 Host/调用计数和 client 的请求/F9/前台 PID 记录。菜单若仍黑屏可直接正常退出，不要求盲操作；脚本会保存 INCOMPLETE 部分包。最终的完成提示和退出码区分完整采集与部分结果；模块清单成功或库加载成功都不表示 240 Present 已完成。用户主动退出与退出阶段 ReShade/驱动异常也分别记录，不能倒推成游戏此前自动崩溃。

采集完成先读取仍在运行的模块和 0～255 context 槽位，再提示正常退出游戏，最终日志与数据合并为一个 ZIP。ConVar 只保存 SDK 布局候选、原始字段及模块身份，不把读到一个整数当作 ABI 或并行通过。退出、异常、模块失败和预算截断保留分项状态。旧 40,952-Draw 数据仍能由新版分析器复核。

诊断期持续逐 Draw getter/内存读取，并同步读回，因此不评价 60 FPS 预算。结果约 192 MiB 原始文件、每 trace 128 MiB，最多等待 15 分钟启动/F9、5 分钟采集；游戏退出或明确 runtime 错误会提前保存部分包。`restore.cmd` 按原始备份恢复配套文件，并拒绝覆盖测试后的用户更新。

## 发行前检查及实际边界

检查包括有界矩阵/名称读取与旧协议兼容、世界/武器/控制段过滤、精确布局、getter/应用/恢复失败、原始备份继承和升级回滚、配置缺项、API 返回成功但目标全零的拒绝，以及旧真实 trace 的回归。Windows 同时执行 x86/x64 的模拟设备/真实文件用例和系统 D3D9 NULLREF 状态检查，另执行固定 DLL 的 CNG/真实路径/0.6.4 配置初始化正反例；后者不创建 GPU 或游戏设备。

发布构建和包内 Python/依赖加载验证以本版交付记录为准。云端没有 L4D2/B580 的实际运行，不能据此宣布游戏 RT、完整静态世界、Source 准备线程并行或性能通过。下一次用户结果用于同时审查世界转换、实际输出、材质失败原因和缺失模块信息，不再补一次独立三角形或相同的纯观察段。

v1 的 [历史交付核验记录](../../research/rt-world-pilot-validation.json) 只覆盖当时构建与用例，不代表菜单或实际游戏通过。其中已完成：从固定 GitHub 提交重新下载成品，验证全部 ZIP 成员哈希、发行脚本/配置/README 与源码一致、配套 PE 架构和编译身份、原生源码输入指纹；分别重析 x86/x64 的观察与世界转换 trace。每种架构的世界用例均为 240 Present/720 Draw，其中 210 次符合条件的世界绘制转换，控制段、武器和 HUD 候选保持原绘制，未出现状态恢复错误。这些是模拟设备用例，不能充当实际游戏分类或 GPU RT 证据。另已通过双架构系统 D3D9 NULLREF 状态测试、固定运行时错误 SHA 拒绝与真实 DLL/API 0.6.4 配置初始化、包内 Python 入口与完整配置检查、40,952 次原始游戏 Draw 的兼容性回归。本地 Python 检查为 75 项通过、2 项需 Windows 而跳过。

发行验证曾发现 Windows 依赖 DLL 被父 Python 进程保留，导致临时目录清理失败；现将加载/导出检查放在独立进程执行，退出后释放其依赖，重新完成 Windows 打包、目录清理和发布。此修正不调用 GPU 或私有游戏方法。

[v2 历史交付核验记录](../../research/rt-world-pilot-v2-validation.json) 已核对固定 GitHub 成品的 CRC、全部成员哈希、源码/配置/README、配套二进制与原生指纹，并重新分析双架构观察/转换 trace。Windows x86/x64 均实际执行启动开关保护、拒绝布局、开启失败时恢复原 Draw、完成后关闭且禁止重新开启的用例；系统 NULLREF 状态、固定 DLL/API 初始化、包内 Python 与完整配置检查也通过。本地 Python 为 76 项通过、2 项需 Windows 而跳过。以上不替代菜单画面、API 帧边界实际生效、游戏 RT 或 Source 并行的实机验收。

v2 的 [实际回包分析](rt-world-100133-result.md) 确认 240 Present/97,190 Draw 完整、地图最终画面正常，但所有世界格式候选的标准 shader getter 是尚未支持的 A1 指针读取，导致零转换、RT 未请求开启。v3 补齐该只读解码，并记录名称槽/地址。另将法线原始 uint32 与 alpha 分开统计，避免把固定 `(0,0,0,1)` 字节模式当有效几何；固定运行时的 primary normal 源为 R32_UINT 打包编码，旧 PNG 的 float 通道映射不能作为向量法线验收，原始字节始终保留供后续离线解码。

[v3 历史交付核验记录](../../research/rt-world-pilot-v3-validation.json) 已从固定 GitHub 提交重新下载并校验 CRC/全部成员哈希、包内脚本和配置、配套二进制/源码指纹。Windows x86/x64 均执行了有界 A1 名称读取、全局指针变化、溢出、空名称和自定义 shader 拒绝用例，启动开关/状态恢复与 NULLREF 检查继续通过；包内 Python、固定 DLL/API 和完整依赖加载检查通过。本地为 77 项通过、2 项需 Windows 而跳过，实际 v2 原始回包由新版分析器重析后仍明确报告零 RT 开启、零世界转换、全零深度及 alpha-only 法线不构成几何信号。v3 的实机复核见下段。

[v3 实际回包复核](rt-world-104917-result.md) 已确认标准材质名称解码成功、176 次世界绘制完成转换；采集在第 33 次 Present 后中断，最后 Present 记录间隔约 17.6 秒，同期开始初始化 Ray Query 管线。手动压缩的中间目录缺失 Bridge 日志，不能据此确认原生异常或通信超时根因，实际 RT 图像仍未通过。补充的等待退出前日志及 incomplete 摘要快照已进入 v4，最终仍合并一个 ZIP。后补 Bridge 日志的归因见下段。

后补同次 Bridge 日志后，已确认首条错误为 client 的 SetStreamSource 命令提交中止，Host 随后清理并以 0 退出；没有 `reply-deadline` 或原生异常报告。v4 将有效 world 启动请求下的 full IPC 生产端容量等待由 2 秒设为有限的 60 秒，保留 peer/故障检查和普通观察模式的 2 秒预算，并补记底层 transport fault。生产队列回归复现了默认超时，也验证了消费者暂停超过 2 秒后仍完整有序提交、peer 关闭及时取消。实际 v3 底层 fault 未记录，不能将机制复现等同于实机原因完全坐实；实际 RT 图像和本次崩溃是否恢复仍待实机验收。

[v4 交付核验记录](../../research/rt-world-pilot-v4-validation.json) 已核对固定 GitHub 成品的 CRC、全部成员哈希、配套 PE 架构/身份、源码指纹，以及包内脚本/配置/README 一致性。Windows x86/x64 均执行默认队列超时、暂停消费者 2.3 秒后 65 条命令完整有序传递、peer 关闭及时中断扩展等待；新增门槛与既有状态恢复/NULLREF、真实固定 DLL/API、包内 Python/完整依赖检查均通过。分别重析双架构 240 Present/720 Draw/210 转换模拟 trace，旧实际 40,952 Draw 兼容性回归通过。本地 Python 82 项中 80 项通过、2 项需 Windows 跳过，生产队列在 Linux 实际执行和 MinGW 严格交叉编译通过。v3 原生源码输入全部保持相同，只新增 transport patch/policy；Source producer、实际 RT 和性能仍未验收。


[v5 交付核验记录](../../research/rt-world-pilot-v5-validation.json) 已检查固定提交成品的 CRC/全部成员、源码输入与配置、配套 PE 身份及 native 证据。Windows x86/x64 均通过新增灯光阶段顺序/重复或缺失控制段/部分设置失败阻断用例，以及既有世界作用域、启动保护、IPC 背压、材质解码与系统 NULLREF 检查；固定 DLL/API 初始化、包内 Python/配置与依赖校验也通过。本地完整 Python 84 项中 82 项通过、2 项需 Windows 而跳过，最终分析器专项 5 项全部通过。新分析器重析旧实际 RGBA32F 数据及独立法线预览回归通过。v5 的实际三段灯光效果、HUD/武器与完整游戏光追、Source 并行和性能仍未验收。


[v6 成品核验](../../research/rt-world-pilot-v6-validation.json) 已检查固定发布提交的全部成员哈希、源码输入、配套 PE 与原生用例。Windows x86/x64 均通过新增 Unlit 输入/状态恢复与 UI 作用域检查：非空编译 VS/PS、纹理、声明、stream0、矩阵、渲染/纹理阶段状态均与之前相等；26 个应用 setter、27 个显式恢复 setter 的修改后失败注入与设备 getter/capture/draw/restore 失败均检查通过。NULLREF 使用显式离屏目标及仅限测试的 backbuffer 别名，并验证另一 surface 身份被拒绝；不作为实际 swapchain 或 Remix HUD 注入证据。已有全部 native gates、包内 Python/配置与固定运行时 API 加载通过。本地 Python 85 项中 83 项通过、2 项需 Windows 而跳过；v4/v5 实际 trace 和读回回归通过。后续 [v6 实机回包](rt-world-192551-result.md) 已确认本次采样的 HUD 恢复；近墙与完整场景、Source 并行及性能继续待验收。

[v7 成品核验](../../research/rt-world-pilot-v7-validation.json) 已核对固定 GitHub 成品全部成员、配套 PE/身份与源码指纹，并独立重析两架构 native trace。新增切线布局 gate 在 Windows x86/x64 均通过：实际捕获字节声明、阶段/错配拒绝、13 个 getter、6 个应用及 6 个恢复 setter 的失败处理，以及系统 NULLREF 上非空 VS/PS、非零 buffer offset、辅助 stream2 和真实 Draw 后的原绑定/矩阵恢复。既有 UI、启动保护、IPC、ABI/队列、固定 DLL/API 及包内 Python 检查通过。本地 86 项中 84 项通过、2 项需 Windows 而跳过；v4/v5/v6 实机 trace/读回兼容性回归通过。NULLREF 不代表 Remix/B580 图像验收；后续 [v7 实机回包](rt-world-202526-result.md) 已通过本镜头两处静态结构的覆盖恢复和 HUD 对照，完整地图/模型/天空、光照/PBR、Source 准备并行与生产性能继续待验收。

[v9 成品核验](../../research/rt-world-pilot-v9-validation.json) 已逐项检查固定发布提交的全部成员哈希、包内脚本/配置/README、配套 PE 架构与身份、源码指纹及固定后端合约。Windows x86/x64 均通过新增受限原 VS 解析和系统 D3D9 NULLREF 重建：实际编译非平凡位置/UV 着色器、读取原常量和带 offset 的 VB/IB，核对重建结果，并执行有符号 base、INDEX16/32、范围拒绝及绑定/恢复失败用例。完整既有门槛、固定 DLL/API 和包内 Python 检查继续通过。本地 Python 91 项中 89 项通过、2 项需 Windows 而跳过；五轮 v4–v8 实际 trace 共 512,769 Draw 及原始读回回归通过。系统检查不执行 Remix/B580；真实 NekoSky 是否通过六面预检、映射/颜色及道具可见恢复仍需本版一次 F9 的回包。

[v10 成品核验](../../research/rt-world-pilot-v10-validation.json) 已检查固定 GitHub 发布提交的全部成员哈希、包内脚本/配置/README、配套 PE 架构与身份、源码收据及固定 DLL/API 初始化。七个 shader 名称在 Windows x86/x64 系统 D3D9 NULLREF 上均执行仿射变换、非零字段偏移、透明裁切阈值/函数保留、首层贴图选择、真实 Draw 和逐项 getter/setter/恢复失败检查；所有原门槛保持通过。显式请求 `rtx.enableAlphaTest=True` 的固定运行时加载检查通过。完整本地 Python 94 项中 92 项通过、2 项需 Windows 跳过，最终配置修订后的5项输出分析检查通过；v4–v8 五轮实际 trace 共512,769 Draw及读回回归通过。新增几何/材质、透明裁切和天空映射在真实游戏中的效果仍待验收，骨骼路径继续拒绝。


[v11 成品核验](../../research/rt-world-pilot-v11-validation.json) 已验证固定提交的成员哈希、源码/配置/配套PE身份、Windows x86/x64原模型数学/拒绝、天空完整PS插值与大坐标、重建stream及UV路由的逐项失败恢复，既有门槛均通过。主世界/HUD基线保留，六面预检失败仍回退v8；CNG完整程序身份、逐顶点bone0和相机检查阻止未知输入被当作模型。系统NULLREF与CPU输入回归均不替代实际Remix/B580图像验收，测试仍合并一个入口、一次F9、一个最终ZIP。
