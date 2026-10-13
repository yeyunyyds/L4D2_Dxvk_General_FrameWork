# LDB Host 游戏采集诊断接入

开发三件套通过 Windows runner 的 **MSVC 14.29 Release 编译检查**后发布，构建记录见包内 `capture-build.json`。包只更新三个桥二进制，并附带日志检查脚本，不包含 DXVK 或配置。原版已在 B580 游戏内完成颜色采集；本次增加 HDR 与更早的深度范围边界，仍需新的实机日志验证世界深度。插帧尚未实现。

[GitHub 下载文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/zip-host-capture/downloads/ldb-host-capture-dev.zip)

## 接入内容与证据

基线：LDB `c8d55b0f5d913d71488266469b4dca236439a4e9`（包含前次研究基线之后的 IPC/Host 修复），Remix Bridge `9aa74f8dfad2188efbd0f717c64d9f8fa909787e`。不修改 Client 命令结构或 IPC 协议。

Host 的 DrawPrimitive / DrawIndexedPrimitive / UP 调用记录实际绑定颜色、深度、shader、viewport、Z 写入和混合状态。Clear、SetRenderTarget(0)、SetDepthStencilSurface、SetViewport 前保存有限的快照；Device Present 与 SwapChain Present 均覆盖。成功 Reset 后增加 generation，失败 Reset 保留 generation；Destroy 删除诊断状态。

普通颜色导出为 PPM；RGBA16F 额外导出 `*-color.rgba16f`，按逐行 RGBA、每通道 little-endian binary16 保存原始 GPU 数据，每像素 8 字节，不裁剪大于 1 或负值。HDR 的 PPM 仅用于预览，映射为 `round(255 * max(c,0)/(1+max(c,0)))`，不做 gamma 校正，非有限值预览为 0。深度导出为 16 位 PGM 和 little-endian float32 原始数据。深度是设备归一化深度，**没有线性化为世界距离**。文件名包含采集帧、generation、快照序号，事件包含 UID、进程内对象标识、实际 VkFormat、尺寸、样本数和 usage。对象标识只在该进程的对象存活期间有意义，不能跨进程传递或视为永久资源身份。

viewport 深度范围改变前记录 `previous/next`，独立保留两个尝试名额。若同一绑定、绘制计数下已有快照，则以 `snapshot_prefix` 引用，避免重复导出；Clear/Reset 后引用失效。这些边界只标为候选，不直接认定为世界或武器。

所有游戏内容先标为 unknown；Present 处的绑定不自动等于主世界。没有取得完整游戏相机或 shader 常量解释，也没有运动矢量、FSR 调度或插帧。

诊断默认关闭，x86 Host 为无操作实现。启用后只在指定区间执行 getter、GPU 等待与回读；每次 getter 引用在回调内释放，不跨 Reset 保留 COM 引用。每帧快照上限 8（可设 1..16），最多 8 帧，总导出预算 256 MiB、单边最大 4096、每帧最多 12000 条 draw 记录，并有事件/日志总预算。默认 8 个名额分为深度范围变化 2、Clear 2、其他绑定边界 3、Present 1，跳过也占预算。MSAA、尺寸不一致、无深度、格式不支持等明确记录跳过。异常停止该设备诊断，并保留原 backend HRESULT；GPU/驱动卡死仍可能阻塞 Host，这不是生产异步采集路径。

同一份组件已在交叉编译的 Windows 测试程序中通过 Wine + llvmpipe 的真实 DXVK 调用。该程序交替执行 Device/SwapChain Present，校验导出像素，并测试失败/成功 Reset。**这证明组件测试场景，不证明完整 LDB IPC 或游戏集成。** 独立原型的 B580 实机通过证据也不能直接转移为本次集成通过。

## 从源码进行 Windows 构建

准备 Git、Python 3.12+、Meson、Ninja、VS 2019/2022 的 MSVC v142 14.29 工具集和 Windows SDK。工作目录为本框架仓库：

```powershell
python scripts/prepare_research.py
python scripts/prepare_ldb_capture.py
powershell -NoProfile -File scripts/build_ldb_capture.ps1
```

准备脚本只新建 `.research/ldb-capture-source`，已有输出拒绝覆盖；固定依赖提交和补丁 SHA256。它下载 Detours 的固定 gitlink，不下载完整 RTX 渲染 SDK。构建脚本在隔离目录编译 x86 Client、x64 Host、x86 Host，并使用共同的独立开发 build ID。打包检查实际 MSVC Release compile_commands、PE 位数、build ID 和源摘要。成功后才会生成 `.research/dist/ldb-host-capture-dev.zip`；不自动安装或发布。包内不包含配置、DXVK 或 ReShade。

## 游戏测试步骤（仅在完整构建成功后）

1. 完全退出 Steam、游戏和 Host，备份原来的桥三个二进制。将开发包的 `bin` 合并到游戏目录，同批替换三个桥文件；保留原 DXVK 与运行配置。
2. 确认 `bin/.l4d2bridge/bridge.conf` 的 `client.testX86Server = False`，使用 x64 Host。此阶段建议先使用已经验证的官方 DXVK 2.6.1。
3. 在 CMD 中设置以下临时变量，再从这个 CMD 启动 Steam。Steam 必须已完全退出，既有 Steam 进程不会继承新变量。

```bat
mkdir D:\LDB-captures
if exist D:\LDB-captures\capture.trigger del D:\LDB-captures\capture.trigger
set LDB_CAPTURE_DIR=D:\LDB-captures
set LDB_CAPTURE_TRIGGER=D:\LDB-captures\capture.trigger
set LDB_CAPTURE_START=0
set LDB_CAPTURE_FRAMES=2
set LDB_CAPTURE_SNAPSHOTS=8
start "" "C:\Program Files (x86)\Steam\steam.exe"
```

从 Steam 启动 L4D2，沿用桥的 `-vulkan -insecure -windowed` 启动选项。进入地图，看到正常游戏世界后，在另一个 CMD 输入：

```bat
type nul > D:\LDB-captures\capture.trigger
```

Host 在检测到标记后的下一帧开始观察两帧，采集期间可能明显卡顿。采集目录形如 `D:\LDB-captures\进程号-启动时间-设备号\`。保留完整目录，包括 `events.jsonl`、PPM、PGM、f32，同时保留同次桥/DXVK 日志。

4. 检查结构与图像文件：

```bat
python scripts\analyze_ldb_capture.py "D:\LDB-captures\你的采集目录"
```

通过只表示完整观察记录与文件一致；输出另含每个快照的深度范围、非清屏像素数与 `non_clear_depth_snapshots`。全为 1 的深度会明确标出，不视为世界深度通过；仍需查看画面、原始深度和绘制序列来确定世界、武器、HUD 边界。若都是 skipped 或没有 completed，保留日志排查，不能当成游戏深度取得成功。

一次进程只采集一个指定区间。重测请完整退出 Steam/游戏/Host、删除触发文件，再重新启动。停用时清除这些临时变量并重新启动 Steam；恢复正式版时同批恢复备份的三个二进制。

## 云端组件检查

```bash
python3 scripts/build_host_capture.py
python3 scripts/build_ldb_capture_fixture.py
python3 scripts/software_capture_smoke.py --exe .research/ldb-fixture/host_observer_fixture.exe --frames 8 --observer enabled --validation
python3 scripts/software_capture_smoke.py --exe .research/ldb-fixture/host_observer_fixture.exe --frames 8 --depth-format d24 --observer enabled --validation
python3 scripts/software_capture_smoke.py --exe .research/ldb-fixture/host_observer_fixture.exe --frames 8 --observer disabled --validation
python3 scripts/software_capture_smoke.py --exe .research/ldb-fixture/host_observer_fixture.exe --frames 8 --observer waiting --validation
python3 scripts/software_capture_smoke.py --exe .research/ldb-fixture/host_observer_fixture.exe --frames 8 --observer triggered --validation
python3 scripts/software_capture_smoke.py --exe .research/ldb-fixture/host_observer_fixture.exe --frames 8 --observer enabled --hdr-scene --validation
python3 scripts/software_capture_smoke.py --exe .research/ldb-fixture/host_observer_fixture.exe --frames 8 --depth-format d24 --observer enabled --hdr-scene --validation
python3 -m unittest discover -s tests -v
```

这些检查始终标记 `full_host_build=unrun` 和 `l4d2_game_capture=unrun`。原组件记录见 `research/ldb-host-capture-component-validation.json`，本次 HDR 组件验证见 `research/ldb-hdr-capture-component-validation.json`。
