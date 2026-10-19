# v15：模型缓存兼容修正与材质、灯光合并验证

[v14 实机](rt-world-145737-result.md) 已在合并阶段显示完整人物与武器，但 Neko 单独阶段仍白屏，并出现实际深度/法线中的异常长条。33份配对输入的生产重建、上传核验和独立原VS位置/UV比较通过；仍缺少全体同帧部件和后端 BLAS/实例的直接证明。

本版默认请求 `rtx.enablePreservePath=False`，走固定后端完整的动态几何/实例处理路径，并保留 `rtx.useBuffersDirectly=False` 的 staging 复制。固定后端源码将 preserve 明确列为可关闭的兼容路径；尚未通过 B580 证明它就是本例根因，不把这项修改预先写成白屏已修复。不修改 Source 线程 CVar，不移植参考 mod 的全局 prim context。

一次 F9 内自动完成以下对照，总600 Present：

| 取图帧 | 模型与条件 |
| ---: | --- |
| 30 | RT关闭的光栅 |
| 120 | 既有世界与HUD |
| 180 | 加入完整重建天空，无扩展模型 |
| 240 | 仅Neko，preserve关闭，原球灯、首层贴图 |
| 300 | 同上，仅临时开启preserve |
| 360 | 仅Neko，再次关闭preserve |
| 420 | 普通模型与Neko合并，preserve关闭 |
| 480 | 合并模型改为中性灰RGB，保留贴图alpha及原裁切/深度/剔除 |
| 540 | 模型恢复首层贴图，仅切换定向诊断灯 |
| 600 | 撤去扩展模型，恢复球灯，检查背景恢复 |

曝光参数不变。球灯仍为半径5、辐射3000；定向灯辐射3、方向`0,0,-1`，是诊断照明，不代表原地图光照。所有配置请求在旧阶段取图之后提交，下一次取图前留出59帧以上。成功请求不等于已经取得有效 GPU 状态；日志明确标记 requested。部分配置失败保留旧阶段标签并停止RT/采集，不得继续宣称新阶段正常。

v15阶段顺序与v14不同，跨版本单张画面改善不能单独确认根因；复用开关的可逆差异仍需结合完整同帧输入和几何读回判断。

中性灰只用于模型材质对照：作用域内将首阶段RGB来源切为`TFACTOR=0xff808080`，保留原贴图alpha。保存、应用、回滚都检查返回值；不改变世界材质。Windows原生检查覆盖每个getter/setter失败及实际NULLREF设备上的原状态恢复。

模型数学和索引重映射不变。补齐诊断交接：

- 取图帧逐次转换Draw记录最多64份，取消按材质去重，避免共用材质的不同部件遗漏。
- 三段Neko各预留12 MiB同帧原始输入/重建VB和IB配对；合并贴图阶段16 MiB。中性灰与定向灯阶段保存摘要/实际状态，不重复完整几何。超额及样本数截断显式记录。
- 所有记录带完整VB/IB哈希、Draw参数、实际首阶段颜色/alpha来源、texture factor及sRGB；原状态审计补入CLIPPLANEENABLE和CULLMODE。
- 几何摘要采用独立4 MiB上限，避免原shader哈希函数64 KiB上限拒绝大部件；保留shader原上限，非索引Draw的空IB摘要有定义。
- trace version12 / coverage revision15 / snapshot wire2。新解析器保留旧版解析，并检查600 Present、所有阶段、配置顺序/返回值、实际材质绑定与配对哈希。整体结果仍需人工颜色/深度/法线验收。

本机可以做CPU回放、结构检查与Windows交叉编译；配套x86 client/x64 Host及原生NULLREF/入口检查由Windows构建执行。发布前须核验固定下载树、二进制身份、源码收据与全部包成员。B580实际画面、完整材质/原照明、全地图覆盖、Source准备线程并行、生产性能及后端资源清理继续待验收。

用户操作仍只有：解压新包、运行`run-test.cmd`、进同图按一次F9并保持镜头不动，完成后正常退出，上传脚本生成的最终ZIP。无需F8、trigger文件或环境变量。

已发布 [v15 ZIP 文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/521f8725c4e7c9b00e5cce1e23a251d270cb108c/downloads/rt-world-pilot-dev.zip)。构建 `3a29404fcb8c9d5e5386d9926eecf5aa07d5278e`，身份 `ldb-rt-observer+c71012573905d521`；[独立交付核验](../../research/rt-world-pilot-v15-validation.json) 已检查双架构原生执行、新增故障回滚标记、固定runtime/API、源码收据、全部成员、ZIP-only发布树及HTTP文件与Git blob一致。发行核验自身不含B580图像；随后的[v15实机复核](rt-world-155210-result.md)已确认采集完整，但六组模型画面均失败。105份完整同帧配对及341,038顶点通过原VS/提交字节核验，后续转向实际GPU资源交接与最小重放，不能宣称本版已修复白屏/异常几何。
