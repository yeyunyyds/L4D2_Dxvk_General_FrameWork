# 不透明材质 alpha 初始化修正 v5

用户结果 `rt-preflight-20261015-204652-f0253de7.zip` 与已发布 v4 一致：完整完成 13 项、1088 帧、26 次读回，约 34.34 秒，实际设备为 Intel Arc B580。有效 UV/法线检查通过，13 项均有预期拾取；原始 albedo 正确，处理后 albedo 和自发光调试图全部为零，独立方向光也未恢复正常画面。NaN/Inf 调试图没有报告无效数值。UV 修正没有解决当前黑屏。

源码审查找到独立测试程序中的确定错误：`remixapi_MaterialInfoOpaqueEXT` 零初始化后，没有设定 `alphaTestType`。固定 runtime 的 [AlphaTestType](https://github.com/xoxor4d/dxvk-remix/blob/b73eb23053aa50d3787b0c6c8b4b8ae49962880e/src/dxvk/shaders/rtx/concept/surface/surface_shared.h#L46) 中 `0=NEVER`，`7=ALWAYS`。零不是“关闭 alpha 测试”。同时，零初始化的 `useDrawCallAlphaState=false` 会让 [calculateAlphaState](https://github.com/xoxor4d/dxvk-remix/blob/b73eb23053aa50d3787b0c6c8b4b8ae49962880e/src/dxvk/rtx_render/rtx_instance_manager.cpp#L678) 使用材质自身的 alpha 状态，覆盖实例侧的状态。这解释了为什么之前的显式 InstanceInfoBlendEXT 对照没有解除拒绝条件。

[实际着色器 alpha 判断](https://github.com/xoxor4d/dxvk-remix/blob/b73eb23053aa50d3787b0c6c8b4b8ae49962880e/src/dxvk/shaders/rtx/concept/surface_material/opaque_surface_material_blending.slangh#L27) 会把 NEVER 的不透明度判为 0。这与“候选物体拾取/原始材质可见，实际处理和照明画面全黑”相符。这里确认的是初始化错误及其 alpha 拒绝效果；修改后的最终 GPU 画面仍需实机确认，不能把 CPU 复现冒充 B580 通过。

v5 的修正与验证：

- 生产路径使用 `opaqueTriangleMaterial()`，显式设定 `alphaTestType=7`、`opacityConstant=1`、`useDrawCallAlphaState=false`、不启用混合覆盖。仅适用于本独立测试的不透明三角形，未修改游戏材质规则。
- 每次 CreateMaterial 前验证实际结构；`material-input.json` 保存三个实际提交的材质值，分别使用发光强度 4、64、0。Python 独立检查，错误 alpha 值不能被看似正确的截图覆盖。
- `prepare_rt_alpha_fixture.py` 从固定提交的两个源文件提取 AlphaTestType 与非混合 alpha 判断分支，先逐文件校验 SHA。仅把 Slang 的 inout 改为 C++ 引用，并标记被截去的混合分支所用参数；原 switch 与 early return 保留。源许可、源 SHA 和生成头 SHA 一并记录。
- CPU 复现实际固定着色器分支：旧参数 NEVER 输出 opacity=0，生产 helper 修正参数 ALWAYS 输出 opacity=1。Windows 原生 helper 同时检查旧初始化被拒绝，非透明输入与 alpha 状态所有权正确。它不是完整 Slang 编译或 GPU 执行检查。
- 关灯对照显式设置 `rtx.fallbackLightMode=0`。runtime 默认在无外部光源时自动补光；若不关闭，会污染真实关灯对照。
- 保留 13 项、1088 帧、26 次采样，不增加用户步骤。发光与独立光照分别报告；v5 总体静态视觉通过要求两条路径均通过，包含各自几何平移和关灯控制。仅调试图正确不算通过。

本地 RT 回归 55 项中 53 通过、2 项要求 Windows；生产诊断和包含真实 alpha 分支的 native helper 均通过 x64 交叉编译。Linux CPU 的固定着色器前缀实测输出 `NEVER(0)=0; ALWAYS(7)=1`。Windows 与发布记录见 [v5 验证](../../research/rt-preflight-v5-validation.json)；上传数据的独立像素复核见 [v4 实测审查](../../research/rt-preflight-204652-review.json)。

入口仍为 [rt-preflight-dev.zip 文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/zip-host-capture/downloads/rt-preflight-dev.zip)。复用原 runtime DLL 与三个大依赖，仅更新小型诊断和脚本；覆盖原目录、保留 cache，LDB 1.3.2 进图后运行 `run-test.cmd` 一次，上传一个结果 ZIP。原来的资源未释放警告仍需审查；游戏 RT、实际队列、多线程和性能未验收。

发布代码 `85df7f8` 已通过 Windows 原生检查：协调器 27/27，游戏采集器 16 项中 15 通过、1 项 Linux 专用检查跳过。MSVC 14.29 原生 helper 输出 `PINNED_SHADER_ALPHA_CONTRACT_PASS: old NEVER=0, fixed ALWAYS=1`；真实窗口/DLL 生命周期、CPU 图像写入、minidump、标准 D3D9 拒绝及包内实际 DLL 加载均通过。已从发布提交 `a6f837eaa104b68d1eef27a009cb9c9cac882231` 取回 ZIP 验证 CRC、EXE/SHA、四个脚本与 README 逐字节一致，包大小 10943429 字节，三个大依赖完全复用。原生构建/包校验的 GPU 执行仍为 unrun。
