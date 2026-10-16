# B580 前置检查的第二版诊断

后续结果已确认 v2 的非黑像素判据过宽：实际 B580 命中、读回和退出成功，画面却接近全黑。当前入口已更新为 [七项合并视觉验收 v3](rt-preflight-visual-v3.md)，以下保留 v2 的修复与验证历史。

用户结果 `rt-preflight-20261015-170826-79905162.zip` 确认依赖校验通过，实际选中 Intel Arc B580（8086:e20b），API 初始化、材料/网格创建及 600 次相机/物体提交成功。拾取请求没有完成回调；旧测试把颜色读回放在拾取命中之后，因此没有独立图像证据。旧测试随后报告失败并以 0xc000041d 退出，未留下 status.json，不能据此把故障归为 GPU device lost。

本版复用 `9c3b2f8` 原生构建的同一个 runtime DLL（SHA256 `21c826bac9fe6a27b28d0b1300f838efa21b010b24216c44f4728b266d8a8ec7`）和三个大依赖 ZIP。只重新编译小型 x64 诊断程序，随入口包放在 `bin/rt-runtime-smoke.exe`，由 manifest 单独校验 SHA、大小、源码和 API 头文件身份；旧 cache 中的诊断 EXE 不再运行。

## 游戏证据

用户实际加载 `shaderapivk.dll`，材料接口为 `VMaterialSystem080`，Client 为 `VClient016`。采集器接受实际加载的 `shaderapivk.dll` 或 `shaderapidx9.dll`，同时加载两个候选时记录歧义并停止选择。跨模块读指针也使用同一实际选择；采集前后核对 renderer 选择稳定性。

对 Vulkan renderer 使用参考模式只会得到候选。报告保留参考 renderer 名称和差异标志，始终禁止 Hook 安装，不能将单个模式匹配、接口版本、RTTI 或槽位候选视作 ABI 验证通过。

## 独立渲染与清理

- 相机/几何仍通过同一固定 Remix API 提交。Present 改用该设备自身的 `PresentEx`：固定版本 API 包装函数每次 `GetSwapChain` 后未释放引用，诊断避开该路径，不修改 runtime。
- FINAL_COLOR 包装函数复制的源就是 backbuffer，但其 `GetBackBuffer` 引用也未释放。诊断直接取得同一 backbuffer，用 `GetRenderTargetData` 读回，并对每次取得的 COM 引用使用局部释放。
- 第 96 帧后定期读取最终画面，即使拾取没有回调；记录 RGB 范围、中心 RGB、非黑像素数、读取成功次数和独立错误。拾取命中后会再读取一次，不能拿此前的图像替代命中后的图像。
- 只有预期 object ID 命中、命中后画面有信号、进程正常结束且清理完整才通过。只读回图像或只提交成功均不通过。
- 渲染阶段不再固定为 600 帧；最多 240 秒和 20000 帧，整个自有子进程上限 360 秒、日志上限 64 MiB。每 15 秒显示进度。游戏读取仍独立执行，任何 RT 失败不跳过它。
- 先保存结果，再释放网格/材料、关闭仍有运行库支持的窗口，最后 Shutdown。DLL 映射保留到诊断进程结束，防止窗口或异步回调进入已卸载代码。清理前后分别保存 pending/complete；父进程要求 complete。
- 原生异常处理器尽力保留本诊断进程的普通 minidump 和异常码；不读取或生成游戏进程 dump。超时仍只停止诊断自有进程。

## 就绪状态的证据范围

只对自有诊断设置 `RTX_TEST_TRACE=1`、`DXVK_DUMP_SCREENSHOT_FRAME=90`、`DXVK_METRICS_PATH` 和禁用阻塞对话框的自动化选项。固定 runtime 已包含 opt-in trace，不需要重编译 DLL。

`frame_trace.jsonl` 只覆盖 runtime 第 90 帧前后的小窗口，包含相机有效性、场景表面数、shader 编译数量及 async 状态。它不是全程时间线；若 trace 缺失，也必须保留缺失这一事实，不能推断具体渲染门槛。整个诊断另持续记录自身帧数、回调数与读回次数。上述配置不改变正在运行的游戏。

## 验证与交付

本机 RT Python 回归 47 项，45 通过、2 项需要 Windows 而跳过；诊断与原生测试已用 MinGW x64 交叉编译。Windows 工作流使用固定 SHA 的 API 头文件和 MSVC 14.29 `/O2 /MT /W4 /WX`，编译诊断及无 GPU 的测试：实际 DLL 窗口回调的关闭顺序、状态先落盘、独立颜色数据写入、崩溃证据和标准 D3D9 的负向拒绝。窗口/CPU 图像 fixture 通过不等于 B580 光追通过。

最终入口仍由包内 Python 验证并真正加载相同原生 runtime。GitHub 仓库为私有；已有用户只需覆盖解压入口包并保留 cache/三个大 ZIP，首次用户仍从登录浏览器取得大依赖。用户运行一次 `run-test.cmd`，返回一个结果 ZIP，无 F8/F9 或规定运动动作。实际 B580 图像、游戏 Hook/队列、多线程及性能仍需真实机器分别验收。

最终打包提交 `0673ced` 已通过 Windows 验证并发布：协调器 19 项全通过，游戏采集器 16 项中 15 通过、1 项 Linux 专用检查跳过；独立诊断、真实 DLL 窗口清理、CPU 颜色写入、普通 minidump 和标准 D3D9 拒绝均通过。最终包约 10.4 MiB，新诊断 EXE 297472 字节；已从发布分支取回核对 CRC、包/EXE SHA、脚本与 README 的逐字节一致性，三个大依赖不变。完整记录见 [发布与原生验证](../../research/rt-preflight-v2-validation.json)。这些原生检查的 GPU 执行均为 unrun。
