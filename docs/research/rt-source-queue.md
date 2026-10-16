# L4D2 队列回调与静态场景诊断

当前已通过原生 Windows 开发验证的是 **SDK 队列回调 → 生产 COM adapter → Host observer**，尚未安装 Source 游戏 Hook。固定自定义 Remix runtime 的完整 x64 Release 与静态场景程序均已编译通过；后续 v6 的 [B580 独立静态 RT 视觉验收已通过](rt-preflight-b580-static-pass.md)。它们均不是综合测试包 A，游戏队列执行仍未验收。

Host 实际 Draw 旁的静态世界转换实现与验证边界见 [静态 Draw 转换](rt-static-draw.md)。

当前游戏绑定证据与独立 GPU 测试已合并到[接入前置检查入口](rt-preflight.md)。这个入口不是综合包 A；不要求用户分别运行旧检查器和静态诊断。

## 队列回调

`integration/rt/rt_source_queue.h` 直接继承固定 L4D2 SDK 的 `CFunctor`，经 SDK 的 `ICallQueue::QueueFunctor` 入队，没有手写猜测的 functor 虚表。SDK 由 `prepare_rt_source_sdk.py` 从固定提交归档，原始头文件不作修改。

- 入队前复制包头、相机、变换和资源数据；记录产生线程与 QPC。
- Source 消费时分配提交序号，经已绑定的 `IMetadataStream` 提交。调用方必须把回调插在对应绘制命令旁边的**同一个 material context** 上。
- SDK 入队增加引用；正常执行或 flush 释放队列引用。回调持有独立数据和 COM sink 引用，避免调用方修改／退出作用域后数据失效。
- 取消未执行回调会停止该 lane 后续标注；旧 token 的回调不能重新启用或破坏新的 Reset epoch。
- 仅在确认物理 Reset 成功后通过 `afterDeviceReset` 安排恢复。它不触发游戏 Reset，也不把失败 Reset 当成功。
- 空队列只用于已确认的即时模式；不以空指针为理由修改 `mat_queue_mode`。消费并发重入会停止标注，没有用全局渲染锁串行化工作线程。

默认上限为 4096 个待释放回调、16 MiB 编码数据，单包最多 1 MiB。它是观察数据的开发边界，尚不是完整场景 GPU 资源协议。

`post` 成功只表示本地回调已接受。Host 拒绝、物理 Draw／Present 返回值以及退出统计仍需另外核对。该组件不修改固定功能状态、不上传 RT 几何、不改变游戏线程 CVar。

`stop()` 只失效回调，**不会排空引擎队列**。持有回调代码的 DLL 必须等 SDK 释放全部队列引用后才能卸载。游戏 Hook 还需要处理局部视图已消费后发生 flush 的边界；不能把本组件的 Reset 用例当作任意游戏换图／取消均已验证。

## 编译确定的 SDK 候选 ABI

原生 MSVC x86 编译 `rt_source_abi.cpp`，由实际汇编中的间接调用提取槽位，避免以源码行数猜测继承方法或重载顺序。索引从 0 起算：

| 方法 | SDK 编译结果 | l4d2-rtx 的部分接口 |
| --- | ---: | ---: |
| `IMaterialSystem::GetRenderContext` | 95 | 96 |
| `IMatRenderContext::GetMatrix(matrix3x4_t*)` | 29 | 29 |
| `IMatRenderContext::GetMatrix(VMatrix*)` | 30 | 30 |
| `IMatRenderContext::GetCallQueue` | 146 | 未声明 |

矩阵重载声明的槽位一致；MaterialSystem 的 GetRenderContext 存在一个槽位的声明差异。参考 mod 中这两个 GetRenderContext 调用点已被注释，96 不能被当作当前游戏的已验证入口。因此不能把完整 SDK 接口直接转成当前游戏对象并调用。`146` 也只是固定 SDK 的候选值，尚不是当前游戏已验证的 GetCallQueue 入口。需要结合当前模块身份、模式唯一性和函数语义完成绑定，失败时停止安装 Hook。

固定 SDK、参考 mod 提交、汇编 SHA 和执行证据见 [队列验证记录](../../research/rt-source-queue-validation.json)。

## 原生执行结果

原生提交 `1e4fac2a93db50bdea1acca253a31b2f985746d2`：

| 检查 | 结果与范围 |
| --- | --- |
| LDB Release | x86 client、x64 Host 构建通过，PE、编译参数和 build ID 已核对 |
| 协议／COM／Host | x86、x64 各 3000 帧协议用例、4 帧 Host 用例通过 |
| SDK 回调 | x86 三组用例通过；512 帧生产／消费重叠、复制后修改调用方数据、资源／相机关联、取消、预算、COM 保留和 Reset 恢复 |
| SDK 回调 trace | 3073 个包、512 个 Draw／Present 关联通过；另有 14 包、2 帧恢复用例 |
| SDK ABI | 四个候选方法由原生汇编提取，保留完整汇编及 SHA |
| 云环境 | Linux 严格编译、ASan／UBSan、56 项 Python 检查通过 |

测试使用实现 SDK 引用规则的测试队列，以及同进程的测试 device 适配器；没有启动 Source 引擎、没有真实 32→64 D3D9 IPC，也不能证明游戏渲染线程已并行。

## 最小静态 RT 诊断

`rt_runtime_smoke.cpp` 是独立 x64 程序，针对固定自定义 runtime 的 API 0.6.4：

1. 只从显式绝对路径加载 DLL，检查初始化函数与函数表；逐步记录错误码。
2. 设置 RT 与无 DLSS 的诊断配置，创建真正的 D3D9Ex device，并记录 adapter vendor/device ID。
3. 创建自发光材质和一个固定三角形，逐帧提交明确的相机与实例。
4. 异步读回中心区域的 object ID；预热期间无命中会重试。
5. 命中预期 ID 后，将最终颜色复制到 D3D9 surface 并进行 GPU→CPU 读回，保存 `final-color.ppm`、事件和状态。

成功状态仅表示这个静态场景的预期命中和颜色读回；不等于 L4D2 世界转换、画质、线程或性能验收。异常退出、超时和无命中也必须保留进综合结果。程序没有 FG、F8／F9 或游戏安装功能。

`prepare_remix_runtime.py` 与 `build_remix_runtime.ps1` 准备固定 runtime 及递归子模块，在原生 Windows 中编译诊断和完整 Release runtime。对应 workflow 将失败日志也发布到开发证据 ZIP；依赖文件不齐全的证据 ZIP 不能用于安装。

原生提交 `d788ac3d08db4a7a927fdd5d14e1b1b82378359c` 的完整构建已成功；275 条 C++ 编译命令均为 O2／NDEBUG。诊断程序也通过了拒绝普通 Windows D3D9 DLL 的负面检查。该检查没有初始化 Remix 或执行 RT，详细证据见 [runtime 构建记录](../../research/rt-runtime-validation.json)。状态 ZIP 只包含小型诊断程序，210 MB 的 runtime DLL 与依赖保存在原生 CI artifact 中，不能把状态 ZIP 当作安装包。

## 当前游戏的只读绑定检查

`scripts/rt_game_probe.py` 接收目标 PID，将检查结果写入综合会话的子目录。它只申请查询／读取权限，读取实际映射的 PE32 模块；不注入代码，不执行游戏接口，也不修改 CVar。当前采集 `client.dll`、`engine.dll`、`shaderapidx9.dll` 和 `materialsystem.dll`：

- 保存磁盘 SHA、映射地址／大小、时间戳和相关内存段 SHA；检查采集前后模块列表与进程存活状态。
- 仅在可执行段查找七个固定参考特征，区分无匹配、唯一候选、多匹配和读取不全。旧 RVA 不作回退。
- 同批保存候选附近的有界机器码及槽位目标代码，便于离线检查指令边界和函数语义，减少只拿到地址后再补采代码的轮次。
- 若保留 MSVC x86 RTTI，收集材料系统／即时及排队 context 的虚表候选、对象偏移及候选槽指向的代码。RTTI 不存在时明确记为 unavailable。
- 同时按固定 SDK 的 `InterfaceReg` 三指针布局查找 `VMaterialSystem` 注册候选；仅静态读取两种简单 getter 所引用的对象和虚表，不执行 factory。这样 RTTI 被裁剪时仍可能取得接口候选；其他函数体保留代码并记为 unresolved。
- 95／96／146 以及矩阵槽位均只输出候选，始终保留 `hook_install_allowed=false` 和 `semantics_verified=false`。唯一字节匹配或 RTTI 名称不能验证调用约定、函数边界、队列所属关系或线程角色。

检查器会保留失败报告，已有会话目录拒绝覆盖。它将作为 A 的自动预检步骤；当前还没有游戏安装／启动协调器，不要求用户单独运行或再上传一次。Windows 原生开发检查使用独立 x86 fixture，由 x64 Python 外部读取，不能替代游戏绑定验证。

原生提交 `6662925e3a556809751559884733916d4a54bc60` 已通过该路径：11 项执行检查通过，另 1 项 Linux 专用检查跳过。MSVC x86 fixture 的实际虚表地址、RTTI 识别结果与接口注册 getter 读出的对象一致，源文件／PE／结果 SHA 已复核。WOW64 同时枚举两份模块列表时的相同身份记录会合并，不同地址或路径的冲突仍保留。完整记录见 [游戏检查器开发验证](../../research/rt-game-probe-validation.json)。

同一提交的完整 x86 client／x64 Host Release 回归也已通过；协议／COM／512 帧 SDK 回调和四份离线 trace 均通过。仍未启动 Source 引擎或执行实际跨进程 D3D9 渲染。

读取不是暂停所有游戏线程后的原子快照；模块列表稳定不等于内容全程不变。磁盘文件 SHA 也不能证明进程从未经过修补，因此记录实际内存段与候选代码，后续安装 Hook 仍需独立验证。

用户验证仍按 [综合采集规格](rt-unified-capture-spec.md) 合入 A：不单独要求运行这个程序，再补传一轮文件。当前游戏 Hook／静态世界转换／用户协调器未完成，A 尚未发布。

## 开发复现

```text
python scripts/prepare_rt_bridge.py
python scripts/prepare_rt_source_sdk.py
powershell.exe -NoProfile -File scripts/build_rt_foundation.ps1

python scripts/prepare_remix_runtime.py
powershell.exe -NoProfile -File scripts/build_remix_runtime.ps1
```

已有输出会被准备脚本保留；重复准备应指定新的 `--output`，并将其传给对应构建脚本的 `-Source`。SDK 包含目录另行指定时，应相应调整开发构建脚本中的 SDK 根路径。
