# 多线程 RT 的版本审查与有序接入基础

状态：已实现并验证协议、LDB Device 接入、Host 观察和开发采集协调器。Source 游戏队列 Hook、几何转换、实际 Remix RT 初始化和 B580 验收尚未完成；当前不是综合测试包 A。

## 本轮实现

RT 使用独立的 LDB 1.3.2 基线 `cafc0442446c502a0de0ebb52d5e73ccaae67516`，没有改变旧 FG 的基线或加入 FSR 资产。固定来源见 [RT baseline](../../integration/rt/baseline.json)。

已找到 mod README 指定的自定义 runtime：`xoxor4d/dxvk-remix` 的 `game/l4d2_rebase1`，固定为 `b73eb23053aa50d3787b0c6c8b4b8ae49962880e`。该分支确实解释 mod 使用的 render state 42／149／150／169／177／196／197／210／211／220，不能用通用 Remix 主分支作等价替换。

mod 与该 runtime 的 API 均为 0.6.4，Bridge 为 0.6.5，22 个接口函数的顺序相同。Bridge 的 opaque material 多出四个尾部字段，并有读取 0.6.4 调用方的适配。runtime 的开发版兼容检查以 minor 版本匹配；这些是源码证据，实际 DLL 初始化、材质调用和二进制 ABI 仍待验证。不能只因版本号接近就认定完整组合已通过。

已提取 7 个关键 Hook 的固定源码模式和模块归属，未把旧静态 RVA 当作当前游戏可用地址。参考 `IMatRenderContext` 只定义了部分矩阵方法，没有完整 `GetCallQueue` ABI。该槽位和 queued mesh 的布局仍未确定；当前没有向游戏安装猜测的队列 Hook，也没有关闭游戏线程来绕过它。完整证据见 [源码审查](../../research/rt-source-audit.json)。

另外固定了 AlliedModders `hl2sdk/l4d2` 的 [专用 SDK 参考](../../research/rt-source-queue-sdk.json)，并已实现直接使用其 `CFunctor`／`ICallQueue` 的有序回调。原生 x86 的 512 帧回调／COM／Host 开发用例通过；实际游戏 Hook 仍未安装。编译提取的 SDK `GetRenderContext` 为第 95 槽，而参考 mod 的部分接口声明为第 96 槽（调用点已被注释），不能直接把完整 SDK 当作游戏 ABI。最新证据与边界见 [队列回调与静态诊断](rt-source-queue.md)。

## 协议与 Device 接入

| 文件 | 当前行为 |
| --- | --- |
| `integration/rt/rt_protocol.h` | 显式小端编码的 v1 包；帧／视图／draw 身份、相机、实例变换、受限资源版本、Reset／地图失效和消费验证 |
| `integration/rt/rt_bridge_api.h`、`rt_client.h` | 从代理 device QueryInterface 获取 `IMetadataStream`；复制调用方数据，Hello 取得 Host 应答，其他元数据走原有 Device 通道 |
| `integration/rt/bridge-hooks.patch` | 固定 LDB 基线上增加 `0x1e10`／`0x1e11` 命令；在实际 Draw、Device／Swapchain Present、Reset 和 Destroy 位置观察顺序 |
| `integration/rt/rt_host.h` | 单一消费线程验证、有界二进制 trace、实际 API 返回值、Reset 失效和退出统计；不修改游戏画面或执行光追 |
| `scripts/prepare_rt_bridge.py` | 从固定 Git 对象构建独立源目录，校验 LDB 补丁和输入指纹，保留缓存中的本地改动，不拉取 FSR 资产 |

包头固定为 104 字节，不依赖 x86／x64 的 C++ 内存布局。包含 session／run／device、device／map generation、frame／view／sequence、数据产生线程和时钟。命令最大 1 MiB，消费端临时资源默认限 16 MiB；这是开发观察数据的边界，尚不是完整游戏场景的 GPU 资源协议。

资源版本使用独立存储和共享只读引用。更新、retire 或 Reset 不会释放仍被任务保留的 CPU 快照；这些引用仍计入预算。它没有实现 GPU fence／显存生命周期，后者必须由实际 RT 接入另外验证。

`Submit` 的非 Hello 成功结果只表示客户端序列化／入队成功，不代表 Host 校验、实际绘制或画质成功。Host 拒绝事件、实际 Draw 返回值、Present 和退出统计必须一起分析。只能在经过确认的 Source 执行位置提交相应数据；Device 通道顺序不能弥补 Source 层的错位。

未标注的菜单 Draw／Present 单列，不冒充世界帧。设备失效期间跳过的元数据有记录；成功的物理 Reset 形成明确屏障，允许恢复时跨过已经取消的序号和帧，清空旧视图／资源状态。屏障之外仍严格拒绝跳号。这种恢复不表示丢弃区间已渲染或连续呈现。

当前 trace 只证明 Host 与元数据的关联，不证明 Source 工作线程确实并行：游戏线程和 Host 属于不同进程，线程 ID 不同本身没有该证明能力。

## 统一采集基础

`scripts/rt_suite.py` 已能在一个开发会话中顺序运行多个子测试、检查文件 SHA、保留各段输出，并在成功、失败、依赖失败、超时或日志超限后输出一个 ZIP。它仅管理自己启动的子进程，拒绝覆盖已有会话。画面与性能可作为不同子段执行，结果不会合成一个“通过率”。

当前支持的 scope 为 `developer_foundation`。它尚不启动 Steam／L4D2、不安装 DLL、不录制或回放游戏，也没有新 RT F8 控制。完整用户入口 `start_rt_suite.cmd` 仍待后续实现，旧包的 F8／F9 语义没有变化。

`scripts/analyze_rt_trace.py` 检查包身份、序号、generation、消费线程、元数据与物理 Draw／返回值／Present 的关联、预算和退出完整性。它会拒绝残缺数据；Host 异步事件与 32 位 UID 回绕不会被当成时间排序的替代品。输出始终把游戏队列、画质、B580 和性能验收保留为未验证。

## 已执行验证与限度

| 验证 | 结果 | 能证明的范围 |
| --- | --- | --- |
| Linux C++17，严格警告及 ASan／UBSan | 5 组执行场景通过，每次包含 3000 帧生产／消费重叠 | 编码、嵌套视图、拒绝输入、资源保留、预算与跨帧身份；没有 Source 游戏 |
| MinGW x64 + Wine | 协议及 COM／Host observer 执行通过 | Windows 目标代码的开发用例，没有 GPU 绘制／真实 Bridge IPC |
| 原生 Windows MSVC 14.29 | x86 client、x64 Host Release 构建成功 | 实际 LDB 1.3.2 补丁与新 Device 接入可以编译，不等于游戏运行通过 |
| 原生 x86／x64 协议和 COM／Host 执行 | 各通过 3000 帧协议用例及 4 帧 Host 用例，包含失败／成功 Reset | 两种架构上的生产头文件执行；Host 用例由测试设备适配器调用，不是跨进程 D3D9 验收 |
| 原生 trace 离线检查 | 两个架构各 22 个包、4 个关联 Draw／Present 记录通过 | 观察记录的完整性与关联，不能证明这些测试帧是实际游戏图像 |
| Python | 56 项检查通过，其中新增 9 项协调器／trace 检查 | 失败保留、单 ZIP、超时、预算、版本／线程／generation 错误与 UID 回绕 |
| 重复准备和原生产物核对 | 输入指纹一致；PE 架构、SHA 与嵌入 build ID 一致 | 测试／源码／产物身份明确 |

量化记录见 [基础验证结果](../../research/rt-foundation-validation.json)，其中固定原生提交、源码指纹、Build ID 与产物 SHA。后续代码变动需要对应的新证据，不能只沿用文件名。

## 开发复现

已有 Python 3、Git 和 g++ 的云环境：

```bash
mkdir -p .research/rt-foundation
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread -Iintegration/rt \
  tests/rt_protocol_cases.cpp -o .research/rt-foundation/rt_protocol_cases
.research/rt-foundation/rt_protocol_cases
python -m unittest discover -s tests -v
```

新输出目录的首次准备与原生 Windows 构建：

```text
python scripts/prepare_rt_bridge.py
python scripts/prepare_rt_source_sdk.py
powershell.exe -NoProfile -File scripts/build_rt_foundation.ps1
```

重复准备时通过 `--output` 指定新目录，并将该目录交给构建脚本的 `-Source`；脚本会保留已有输出，不自动清理用户改动。源码审查脚本 `audit_rt_sources.py` 使用固定的 compat、runtime 和 upstream Git 缓存。

CI 单独提供 `rt-foundation-build-status.zip`，含状态、构建日志、指纹和开发产物。它没有 Source 插件及 Remix RT runtime，不能安装为综合包 A。正式 A 包仍需引擎队列 Hook、匹配 runtime 的实际执行和最小静态 RT；届时按 [统一采集规格](rt-unified-capture-spec.md) 一起实测，无需现在额外收集三组游戏数据。
