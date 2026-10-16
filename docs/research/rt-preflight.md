# 游戏接入前置检查

已发布的 [rt-preflight-dev.zip 文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/zip-host-capture/downloads/rt-preflight-dev.zip) 将当前游戏绑定证据与固定自定义 Remix 的静态 RT 冒烟测试合为一次运行、一个结果 ZIP。它不是 R0／R1／R2 综合包 A。游戏 Hook、实际引擎队列插入、世界转换、线程及性能仍未验收。

当前 v6 已完成 [B580 独立静态 RT 视觉验收](rt-preflight-b580-static-pass.md)：13 项、26 次读回及自发光/独立灯光的轮廓和平移均通过，原像素标准未放宽。无需重复此类三角形采集；下一步转入实际游戏队列与静态世界接入，资源释放警告仍需调查。此前 [v5 alpha 修正](rt-preflight-alpha-v5.md) 和 [v6 后处理配置](rt-preflight-postfx-v6.md) 的诊断记录保留。

当前源码检查得到两个具体结论：

- 固定参考 mod 在 `CMeshDX8` 实际绘制前使用 ShaderAPI 的 `GetBufferedState`，并从该状态取 world／view／projection；源码明确说明在较早的 `on_renderview` 设置相机曾造成一帧落后。因此，移植优先取实际消费位置的数据；主线程产生的视图身份仍需通过同一 context 的队列有序传递。
- 固定公开 SDK 声明 `VClient015`，参考 mod 实际请求 `VClient016`；引擎接口同为 `VEngineClient013`。mod 的材料系统与 ShaderAPI 还包含私有布局。这些声明不能证明当前游戏 ABI，不能据此调用 SDK 候选槽位。

采集器在原有七个 Hook 特征／材料 context RTTI／接口注册表检查之外，同批增加：

- 已发布 Client、EngineClient、RenderView、MaterialSystem、ShaderApi 的注册版本候选；不执行 factory。
- 从固定参考源码提取三个全局指针特征：material system、ShaderAPI、D3D device。唯一匹配时读取指针及所属模块；材料系统保留候选槽 95／96，ShaderAPI 保留参考候选槽 250／255 的目标和有界代码。
- 对象可以位于堆中；虚表和目标代码需落入已采集模块才能继续读。无法读取、外部虚表、多匹配分别记录，始终保留 `interface_binding_verified=false`。没有旧 RVA 回退，也不调用 `GetBufferedState`。

## 打包和运行

`package_rt_preflight.py` 从实际 native Release 的 `_output` 收集全部部署文件，排除 PDB／链接中间文件；附带原生静态诊断、VC++ x64 可再发行 CRT，以及固定 SHA 的 CPython 3.12.10 NuGet 包中的标准库和运行时。不依赖用户安装 Python。runtime 许可和依赖声明随入口包保留。

Git 文件大小限制通过若干小于 95 MiB 的**有效 ZIP** 依赖包解决。用户只需下载入口 ZIP；首次运行自动从同一下载分支取得依赖，逐包及逐文件校验 SHA／大小，之后复用缓存。入口与所有依赖一次 Git 提交发布，避免入口先于依赖上线。结果 ZIP 不重复包含这些大依赖。

native CI 会解压入口包，使用**包内 Python**执行 `--verify-package`，从本次依赖 ZIP 解包、逐文件验证、真正加载自定义 DLL 并检查 API 导出。该检查不初始化 GPU；只有这个检查通过才发布入口包。GPU 冒烟、游戏模块证据、B580 身份／图像仍需用户机器执行和分析。

最终打包提交 `6181472` 已通过上述原生检查并发布：入口约 10 MiB，首次自动取得四个依赖 ZIP，共约 251 MiB，运行目录包含 173 个 runtime／依赖文件。已复核入口 ZIP 的 CRC、SHA 和脚本源码，以及四个已发布依赖的有界读取及长度；[完整验证记录](../../research/rt-preflight-validation.json) 保留原生构建与打包的分别身份。

完整 runtime 的原生构建来自 `9c3b2f8`。最初加载失败的原因是无 GPU 的 Windows 构建机缺少 `vulkan-1.dll`；PE 依赖图确认后，复用既有二进制，增加固定提交的 Khronos Vulkan-Loader 1.4.313、匹配头文件和许可。loader 放在应用目录，不安装或替换系统驱动。最终普通导入无缺失，六个关键 DLL 分别加载通过，runtime API 导出存在；API 初始化与 GPU 光追依然记为 unrun。修复打包没有重新编译 runtime。

用户步骤：

1. 解压入口 ZIP 到独立目录，打开 `download-dependencies.html`，在浏览器下载三个依赖 ZIP 并放在 `run-test.cmd` 旁边，不解压依赖；让目前正常工作的 LDB 游戏保持在地图中。
2. 双击 `run-test.cmd`，等待独立 RT 窗口和模块检查结束。
3. 上传 `results/rt-preflight-*.zip`。失败也上传该 ZIP。

不需要 F8、F9、trigger 文件或规定动作。脚本只读游戏，不部署 DLL、不更改 CVar，也不关闭游戏。RT 窗口可能临时获得焦点。独立诊断超时 180 秒；游戏模块等待最多 180 秒。首次联网下载另计。

RT 失败不跳过游戏证据，游戏读取失败不删除 RT 结果；日志／结果有大小上限，超限文件在 summary 中逐项列出并将采集标为 incomplete。只终止脚本自己创建的诊断进程树。`game_rt`、`source_threading`、`performance` 均保持 unrun；静态场景通过仍需核对实际 adapter ID 和图像，不能仅凭 Intel vendor ID 认定 B580 验收通过。

## 首次用户检查的失败与入口修正

`rt-preflight-20261015-163255-a87c254e.zip` 只有 summary 和包清单。runtime 下载返回 `HTTP Error 404: Not Found`，因此 GPU 测试完全没有执行；游戏模块等待 180 秒超时，但旧版未保存进程列表或已加载模块，不能据此推断用户未进图，也不能确定缺少哪个模块。

修正版复用同一原生 runtime；三个大依赖绑定到已经发布并核对长度的固定提交 `fbcf82c9bcdec9aa4fc0fe9a7b3caa5f99eb08e0`，下载失败时尝试 GitHub 的另一入口，保持大小和 SHA 校验。小型 Vulkan loader 依赖随入口 ZIP 提供。控制台和 `progress.log` 记录进度、URL、具体异常及后续阶段；`game-discovery.json` 保留相关进程、PID 和实际模块列表。发现 Client/Engine 后立即采集；渲染或材料模块缺失由采集器单独记录，不再当作未进图的依据。没有找到目标时依然有等待上限。

本机 RT 回归 42 项，其中 40 通过、2 项需要 Windows 而跳过。发布工作流还会用真实 x86 fixture（进程名 `left4dead2.exe`）验证 x64 进程发现，并使用最终包内 Python 校验并加载原生 DLL；额外从 Windows runner 无仓库凭据探测依赖 URL，区分本环境可访问与用户匿名下载可访问。测试结果不等于当前游戏模块 ABI 或 B580 光追通过。

后续 Windows 工作流在无仓库凭据时，对三个已发布的固定依赖地址均复现 HTTP 404，而云环境授权访问可读取相同对象。因此仅改变地址或增加下载备用入口不足以解决此次交付问题；本次默认改为浏览器下载三个依赖 ZIP。新版入口接受同目录的 ZIP，逐包及逐文件 SHA 校验不变；小型 loader ZIP 已随入口包提供。Windows 16 项检查全通过，包含实际 x64→x86 进程发现；独立 DLL 加载与 API 导出检查通过。上述检查不包含 GPU 执行和实际游戏 ABI。

最终打包提交 `97ff840` 的 Actions 事件明确确认 `repository_private=true`，并复现三次匿名 HTTP 404：原方案遗漏了私有仓库鉴权条件，不能把本环境授权可读取当作用户脚本匿名可下载。当前通过用户已有 GitHub 登录的浏览器取得 ZIP，脚本不收集用户凭据，也不改变仓库可见性。最终发布文件与原生校验的 SHA、规范化脚本内容、README、内置 loader ZIP 和 CRC 已复核；见 [此次失败与修复验证记录](../../research/rt-preflight-user-validation.json)。
