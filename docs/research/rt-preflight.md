# 游戏接入前置检查

已发布的 [rt-preflight-dev.zip 文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/zip-host-capture/downloads/rt-preflight-dev.zip) 将当前游戏绑定证据与固定自定义 Remix 的静态 RT 冒烟测试合为一次运行、一个结果 ZIP。它不是 R0／R1／R2 综合包 A。游戏 Hook、实际引擎队列插入、世界转换、线程及性能仍未验收。

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

1. 解压入口 ZIP 到独立目录，使用目前正常工作的 LDB 启动游戏并进入地图。
2. 双击 `run-test.cmd`，等待独立 RT 窗口和模块检查结束。
3. 上传 `results/rt-preflight-*.zip`。失败也上传该 ZIP。

不需要 F8、F9、trigger 文件或规定动作。脚本只读游戏，不部署 DLL、不更改 CVar，也不关闭游戏。RT 窗口可能临时获得焦点。独立诊断超时 180 秒；游戏模块等待最多 180 秒。首次联网下载另计。

RT 失败不跳过游戏证据，游戏读取失败不删除 RT 结果；日志／结果有大小上限，超限文件在 summary 中逐项列出并将采集标为 incomplete。只终止脚本自己创建的诊断进程树。`game_rt`、`source_threading`、`performance` 均保持 unrun；静态场景通过仍需核对实际 adapter ID 和图像，不能仅凭 Intel vendor ID 认定 B580 验收通过。
