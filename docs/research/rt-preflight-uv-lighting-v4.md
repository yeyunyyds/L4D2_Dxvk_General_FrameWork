# UV 修正与独立光照对照 v4

后续 v4 实测确认 UV 修正未解决黑屏：原始颜色正确，处理后颜色全零。当前入口为 [不透明材质 alpha 修正 v5](rt-preflight-alpha-v5.md)，修正新确认的 NEVER alpha 初始化遗漏。以下保留 v4 的输入修正与对照设计历史。

上一份实测 `rt-preflight-20261015-202040-3a98d4d7.zip` 对应 `20d4d6c`，在实际 Intel Arc B580 上完成 608 帧、14 次读回，约 20.84 秒。每项均命中自己的物体 ID，模块快照完整。原始材质视图正确显示三角形，中心 RGB 为 `[230,123,65]`；自发光视图的两张图全部为零，四种正常画面均不合格。固定曝光和将自发光强度 4 提高到 64 没有恢复画面。结果足以继续定位，无需原样重采。

源码审查确认旧测试的三个 UV 都为零。固定 runtime `b73eb230` 的 [genTangSpace](https://github.com/xoxor4d/dxvk-remix/blob/b73eb23053aa50d3787b0c6c8b4b8ae49962880e/src/dxvk/shaders/rtx/utility/math.slangh#L415) 中，`rawTangent` / `rawBitangent` 只在 UV 行列式非零时赋值，但在退化 UV 回退之前已被读取并用于 raw tangent 输出。输入全零 UV 会触发这条读取未赋值变量的路径。

这确认了输入触发的问题，**尚不能证明它是 B580 自发光全黑的唯一根因**。本次修正测试网格，避免依赖该退化分支；未修改 runtime DLL，也未解决任意游戏网格的通用退化 UV 处理。

修改如下：

- UV 改为 `(0,1)`、`(0.5,0)`、`(1,1)`，行列式为 1。提交前检查数值有限、法线单位长度和非退化 UV。`geometry-input.json` 保存实际提交的 UV/法线，Python 独立重算。
- 保留原来的正常画面、自发光和几何平移检查，加入处理后 albedo（23）及 NaN/Inf（260）调试视图，区分原始材质、实际表面求值与无效数值。NaN/Inf 视图中黑色表示未报告无效值，亮色表示无效；它与发光视图全黑的含义不同。
- 增加一个自发光强度为 **0** 的独立材质，用实际 Remix 光源 API 提交白色方向光，辐亮度 `(5,5,5)`，方向 `+Z`，角直径 0.5°。依次测试关灯、开灯、物体平移、销毁光源后关灯。
- 光照路径必须分别通过正确三角形轮廓、两次采样、预期拾取、随物体平移、关灯前后暗图与完整清理。两次关灯图最大通道值须不超过 24/255。创建光源只注册资源，只有每帧 `DrawLightInstance` 才激活；最后通过 `DestroyLight` 显式移除。
- `emission_path` 和 `lighting_path` 分别报告。独立光照成功不会替代发光验收；发光正常画面、对应平移、发光调试图和网格输入必须同时正确，才允许总体视觉通过。

一个进程共享一次 GPU 初始化，自动完成 13 项、1088 帧、26 次采样，仍使用全局渲染 240 秒和含初始化/清理共 360 秒上限。每项两张图间隔 16 帧。所有 RT 失败仍保留独立游戏证据和一个结果 ZIP，不安装游戏 Hook、不改变游戏线程配置。

本地回归 53 项中 51 通过、2 项需 Windows。生产网格 helper 的原生检查验证真实 API 的 64 字节顶点布局、有效 UV、旧零 UV 拒绝和非有限值拒绝；Python 检查覆盖“独立光照通过不能掩盖发光失败”“关灯残留画面不能通过”“看似正确的图像不能覆盖退化输入”。原生检查不代替新版 B580 实测。

入口仍为 [rt-preflight-dev.zip 文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/zip-host-capture/downloads/rt-preflight-dev.zip)。覆盖原目录并保留 cache 和三个大依赖，LDB 1.3.2 进图后运行 `run-test.cmd` 一次，上传一个结果 ZIP。源码审查记录见 [上一份实测审查](../../research/rt-preflight-202040-review.json)，本版构建与发布验证见 [验证记录](../../research/rt-preflight-v4-validation.json)。游戏 RT、队列、多线程与性能仍未验收。

发布代码 `17f22a3` 已通过 Windows 原生检查：协调器 25/25，游戏采集器 16 项中 15 通过、1 项 Linux 专用检查跳过；生产 UV helper、CPU 图像、真实 DLL 窗口生命周期、minidump 与标准 D3D9 拒绝均通过。最终包内 Python 实际加载 runtime、六个关键 DLL 分别加载、PE 依赖检查均通过。已从发布提交 `787933a10677805251f6a093c5d7b5970e3c07dd` 取回 ZIP 核对 CRC、EXE/SHA、四个脚本和 README 与源码逐字节一致，包大小 10942260 字节。三个大依赖完全复用。以上构建与发布检查的 GPU 执行为 unrun。
