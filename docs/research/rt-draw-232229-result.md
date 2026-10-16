# 首次真实游戏 Draw 消费端实测

输入 `rt-draw-20261015-232229-4cce94bf.zip` 的原始 Draw 段已独立解析通过。配套 client/Host 的安装 SHA 与已发布的 `73b834a` 构建一致。ZIP CRC 正常，无文件被预算策略省略。机器记录见 [本轮复核](../../research/rt-draw-232229-review.json)。

| 项目 | 结果 |
| --- | --- |
| 完整 Present | 120 |
| 实际 Draw / 有效 Source 状态 | 40,952 / 40,952 |
| Draw 类型 | 全部 DrawIndexedPrimitive / triangle list |
| 漏记、失败、Reset | 0 / 0 / 0 |
| Client/Host viewport 不一致 | 0 |
| Host pipeline getter 失败 | 0 |
| 不同顶点声明/步长组合 | 14 |
| 相机/目标组合 | 42 |
| 材质对象 | 117，均可与 Draw 的对象/vtable 对应 |

Host 记录线程 23904，client Draw 线程 23416。这证明本次消费端快照和实际跨进程 Draw 的关联检查通过；一个消费线程本来就可以接收多个生产线程的工作，不能据此判定 Source 单线程或并行准备已经验收。Host QPC 跨度约 1.4265 秒只是诊断记录的消费区间，不能用来宣称游戏 FPS、GPU 时间或性能达标。

## 从数据确定的接入差异

主分辨率为 1920×1200。日志可见 HDR 目标格式 113 的透视绘制、viewport MaxZ 约 0.1 的另一组透视绘制，以及多个尺寸的正交/屏幕目标。它们提供了主场景、武器及屏幕阶段的候选区分条件；没有 Source view ID、材质名称和几何身份，尚不能把这些候选写成可靠分类。

117 个材质的 slot 20 均为 `8b41388b513cc3` 开头，即从 `this+0x38/0x3c` 返回 64 位值的简单 getter。只读对象快照能提取与参考 VertexFormat 相符的值，不需要执行该方法。这里使用的是每个地址第一次出现的对象快照，不是每次 Draw 的独立格式值，也没有证明地址对应的对象生命周期不变。slot 0 的名称路径跳到窗口之外的符号/字符串解码，slot 49 则经由 `this+0x14` 的 shader 对象，现有文件没有所需的指向数据；没有臆造材质或 shader 名称。

在参考 l4d2-rtx 对 `0x480003` 的 LightmappedGeneric/world 分支附近，可找到 2,127 次符合全深度范围、HDR 目标、不混合且写深度条件的候选，涉及 18 个材质，覆盖全部 120 帧。其 stream 0 为：

| 字段 | 类型 | 偏移 |
| --- | --- | --- |
| position | FLOAT3 | 0 |
| normal | FLOAT3 | 12 |
| texcoord 0 | FLOAT2 | 24 |
| texcoord 1 | FLOAT2 | 32 |

实际 stride 为 **48**，声明还包含 stream 2 的附加元素。现有 `rt_static_draw.h` 仅接受 stride 40 和仅含 stream 0 的精确声明，因此即使给它可信分类，该布局仍会被拒绝。参考 mod 的 SetFVF 路径可以作为适配依据，但不能把多余流的语义、材质纹理映射或静态世界身份直接假定为已解决。本次观察也没有保存顶点/索引数据和纹理绑定内容，不能仅用该 ZIP 离线构建世界光追场景。

## 收尾错误及修复

上传包中 controller 的 `collection=incomplete` 有真实原因：发行包漏放 `rt_game_probe.collect()` 启动时读取的 `integration/rt/baseline.json`。它发生在原生 120-Present 段完成之后、打开目标进程采集模块之前。旧流程又先做 live probe，才做离线分析，故本包缺少 `game-binding.json` 和 `draw-analysis.json`。这不是原生 Draw 失败，也不能当作扩展队列窗口已经收集成功。

修复包括：

- 将 baseline、audit、SDK 配置纳入同一打包依赖列表，并在安装前校验必需项、JSON 和文件 SHA。
- 最终打包检查实际执行隔离 Python 的 `--verify-only`，不只检查 `--help` 是否可启动。
- 先独立持久化 Draw 分析，再采集 live 模块；失败时分别保留结果与明确状态。
- 回归检查覆盖真实打包文件布局中的 collect 配置读取、缺项、篡改、模块失败后的有效 Draw 保留，以及损坏 trace 不阻断模块采集。

原始 Draw 已在云端复核，不为该收尾错误安排一次单独 F9 复测。扩展模块证据留到下一次综合测试。

## 下一步与验收边界

消费端关联这一小项已有真实游戏证据。下一步利用本轮实际声明适配 48 字节世界候选布局，补充有界的材质名称/shader 数据解码与排除武器/屏幕阶段的条件，再将首次静态世界 RT、缺失的模块/ABI 信息及线程状态验证合成一个测试入口。

若仍需 Source producer 队列接入，必须验证当前模块的函数语义和调用约定；不能因为旧 SDK 的槽位可执行就调用它。消费端观察实现没有添加强制单线程的 CVar，但本次日志没有生产线程或线程 CVar 实测，因而 **Source 队列执行、并行准备、静态世界分类、游戏 RT 与性能均未验收**。独立 B580 最小 RT 的通过记录仍是另一项证据。
