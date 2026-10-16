# v3 已转换世界绘制，但首次 RT 管线初始化附近中断

输入 `rt-world-20261016-104917-4f080893.7z`，1,656,468 字节，完整性检查通过。用户说明这是手动压缩的中间结果目录，游戏在 240 帧前崩溃。[结构化复核记录](../../research/rt-world-104917-review.json) 保留原包成员清单、SHA、独立 trace 分析和时间间隔。安装回执中的 client/Host SHA 均匹配已发布 v3 构建 `b93b2c068af1d525e3b06543ab89905f07f6982e`。

## 已确认的进展与中断位置

F9 记录与游戏前台 PID 一致。24,314 次 Draw 的快照全部 valid，347 个记录材质均解码出名称；54 个 `0x480003` 材质全部为 LightmappedGeneric。v2 的名称阻塞已经解除，没有执行私有材质函数。

独立分析得到 33 次 Present：前 30 次为光栅控制段，之后分别转换 60、59、57 次世界绘制，共 176 次。176 个转换报告均为 Applied，capture/apply/restore HRESULT 全为零；24,314 次物理 Draw 和 33 次 Present 的已记录 HRESULT 均为 S_OK。没有 Reset、未关联 Draw、viewport 不匹配或管线 getter 失败。这只能说明已经记录的同步调用没有报错，不能排除异步 GPU 错误或随后进程异常。

| Present | 转换 Draw | 与上次 Present 记录的间隔 |
| --- | ---: | ---: |
| 31 | 60 | 81.05 ms |
| 32 | 59 | 64.53 ms |
| 33 | 57 | 17,625.60 ms |

第 33 帧最后一次 Draw 结果记录到 Present 结果记录之间约 17,561.92 ms；随后约 1,186.83 ms 写入 footer，reason 为 3。源码中 3 表示 Recorder 析构收尾，不是异常码，不表示 Reset，也不能单独判定是谁先退出。模块探测时游戏 PID 11272 仍存活，与用户观察到的崩溃窗口/Host 退出并不矛盾；尚不能将其精确归因于游戏原生异常。

同一时段 `remix-dxvk.log` 在 10:51:20.116 记录三种 Ray Query 管线初始化，10:51:20.124 记录 ReSTIR GI activated。此前 runtime API 请求开启 RT 返回 0，收尾请求关闭也返回 0。这比 v2 的零转换推进了一步，但不证明实际 ray dispatch 或正确光追画面。

## 仅有中间结果包时的归因边界

现有包没有 `bridge32.log`、`bridge64.log`、异常报告、最终 `summary.json` 和日志清单。Remix 日志没有明确的 Vulkan 设备丢失或原生异常记录；缺少错误不等于排除该错误。40 个 common device objects 未释放属于收尾记录，上一轮正常退出也出现过 39 个，不能据此认定为本次首发故障。

一个待核实方向是首次 RT 管线初始化阻塞 Host，继而触发 Bridge 的同步应答截止时间：固定 LDB 源码的部分带 UID 应答有默认最多 10 秒的硬等待，实际重试上限也可能更早结束，而本次最后间隔超过 17 秒。只有 Bridge 日志里的 `reply-deadline`、通信错误、`L4D2_BRIDGE_PEER_EXIT` 及异常报告才能检验该顺序。trace 没有 Present 入口时间戳，也没有记录当时正在等待的同步 RPC，所以不能把该假设写成已确认超时，更不能直接将其判为驱动 TDR。

只有第 30 帧的三种原始读回：颜色是控制段内容，depth 全零，normal 的 uint32 为固定 `(0,0,0,1)`。120/240 读回缺失，无法审查开启 RT 后的深度、法线或颜色。用当前分析器独立重析并生成预览成功，没有复现 Python 分析异常；独立生成的分析文件和预览没有冒充原始上传成员。

## 采集器修正与下一步

原流程在等待游戏/Host 正常退出之后才复制 Bridge 日志和写最终摘要。此次手动压缩发生在等待期间，因此漏掉最关键的退出证据。源码现改为等待前先复制可用日志，并写明确标为 incomplete 的摘要；退出后仍刷新日志、分析和生成同一个最终 ZIP。预先复制日志失败会保留原因并继续后续分析，不将中间摘要标成验收完成。回归覆盖了 Host 已退出而游戏仍存活、等待超时后仍可打包，以及日志复制失败不阻断独立世界分析。

最初这一项是采集器源码修正，未改变运行时、世界选择、状态转换或 Bridge 等待上限。当时优先索取本次已有的最终 ZIP 或 Bridge 日志；后补日志和随后的 v4 交付见下文。

修正后的本地 Python 全套 82 项中 80 项通过，2 项要求 Windows 而跳过；其中采集器 16 项全部通过。云端没有游戏/B580，本次测试不构成实机崩溃修复验收。

实际游戏 RT、完整世界分类、Source 准备线程并行、资源释放和性能均未验收。

## 补充 Bridge 日志后的定位与 v4 修正

后补 `bridge32.log` 为 9,659 字节，`bridge64.log` 为 1,084,812 字节。两份日志的构建身份均为已核验 v3 的 `ldb-rt-observer+61d74af0008b8f80`，GUID 均为 `8fe642f1-28ed-4eec-ae64-8c246fc15151`；client PID 11272、Host PID 17040 与原 trace 一致，确认属于同次运行。日志 SHA 和逐项结论追加在结构化复核记录中。

最早故障是 client 于 10:51:06.080 写出的 `IPC_FULL_FAULT ... command=148 uid=2 ... result=command-aborted`。148 是 `IDirect3DDevice9Ex_SetStreamSource`，不是等待应答的 `reply-deadline`。该路径把对象句柄作为日志 uid 字段传入，因此 `2` 是 device handle，不能误读为第二个 RPC。Host 到 10:51:21.640 才开始清理资源，10:51:22.825 记录正常收尾；client 于 10:51:23.460 记录 Host 退出码 `0x00000000` 并弹出 CPU crash 窗口。这里的 CPU 类型是通用 peer-exit 处理器分类，没有对应 CPU 异常报告。

源码链路是命令提交失败后 `Command::abort` → `Bridge::reportFault` 关闭 client Bridge 并标记共享通道故障；Host 从阻塞的后端工作返回后观察故障、正常清理退出；游戏的 peer-exit 回调显示崩溃窗口。首发故障应定位于 client 提交/IPC 路径，不能把收尾对象清单或弹窗分类认定为 RT/CPU 原生异常。

固定 full IPC adapter 的命令环生产端与上传池容量等待均硬编码 2,000 ms，命令环只有 64 个 4 KiB 块。首次 RT 初始化使消费端长时间不能推进，而 client 继续提交直至容量耗尽，随后 2 秒等待失败，与本次时间关系吻合。旧日志没有输出共享 transport fault，尚不能从实机日志单独证明具体是哪一个底层条件；先前“10 秒同步应答截止时间”的假设不符合实际首条错误，已撤回。

v4 只在校验通过的 `LDB_RT_WORLD_1` 启动请求下，将命令环和上传池的生产端容量等待设为有限的 60,000 ms；普通观察仍为 2,000 ms。保持同一 IPC 顺序、容量、peer 检查、关闭/故障唤醒和 Source 线程配置，不增加无限重试。日志增加启动 profile/预算，以及覆盖 shared fault 前的读/写 transport fault，若仍失败可区分 Timeout、PeerExited 和其他故障。之前的退出前日志快照也一并进入 v4。

回归直接使用准备后的生产 full IPC adapter、实际共享映射和事件：填满 64 个命令块，复现默认 2 秒超时；world profile 下暂停消费者 2.3 秒后逐条验证 65 个 SetStreamSource 的 FIFO/完整参数，确认没有丢失或重复；对端关闭时不到 2 秒就中断扩展等待。Linux 实际执行通过、Windows x64 严格交叉编译通过；双架构 Windows 执行作为发行门槛，不能用上述结果代替 B580 实际 RT 验收。

本次已有日志已足够选择这项有限等待修正，不再要求补采一轮纯日志。已发布 [v4 文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/90598699fda203b1d6cbb1e0725136bda7c38ebf/downloads/rt-world-pilot-dev.zip)，[交付核验记录](../../research/rt-world-pilot-v4-validation.json) 确认固定成品的全部成员哈希、配套二进制、源码指纹、双架构 Windows 新增队列用例、既有状态/NULLREF、包内 Python 与固定 DLL/API 加载检查通过。仍用一个入口、一次 F9、一个 ZIP 同时检查是否跨过初始化、240 Present 及实际读回。尚未确认修复本次实机故障。
