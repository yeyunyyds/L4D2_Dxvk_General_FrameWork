# v7：带切线世界布局扩展

[v6 实机结果](rt-world-192551-result.md) 已确认 HUD 恢复，但近处白墙仍未参与预期遮挡。同一 trace 中每帧有一个 stride64 和五个 stride80 的主视图、不透明写深度 LightmappedGeneric Draw 被格式白名单排除。本版针对这两种精确布局扩展，不把它们直接标为已经定位的白墙。

## 接入边界

只增加 `LightmappedGeneric / LightmappedGeneric_DX9` 的 `0x480033` 和 `0x2480033`。继续检查有效材质解码、单位 world、有效刚性透视主视图候选、完整 viewport/depth、同尺寸 A16B16G16R16F 目标、关闭混合及 alpha test、开启深度及写入、非 UP 三角形、stream0 frequency=1、offset 四字节对齐和非空 buffer。

完整 declaration 必须逐字节匹配 v6 原始记录：

| 格式 | stride | stream0 布局 | 额外流 |
| --- | ---: | --- | --- |
| `0x480033` | 64 | FLOAT3 position@0、FLOAT3 normal@12、FLOAT2 UV0@24、FLOAT2 UV1@32、FLOAT3 tangent@40、FLOAT3 binormal@52 | stream2 FLOAT4 POSITION1@0、FLOAT3 NORMAL1@16 |
| `0x2480033` | 80 | 前四项相同，另有 FLOAT2 UV2@40、FLOAT3 tangent@48、FLOAT3 binormal@60 | 同上 |

固定功能 `XYZ|NORMAL|TEX2` 仅读取已验证的前 40 字节；保持原 buffer、offset、stride 和辅助流绑定，不改写顶点内容。新路径同时清除 VS/PS，让固定 Remix 走已核对的 FFP 纹理提取，而不沿用原可编程 PS 的多纹理提取逻辑。它尚未把切线、法线贴图和 UV2 转为完整 PBR 输入；本次优先验证几何覆盖与遮挡。

所有修改仅在单次 Host Draw 内生效，恢复原三组矩阵、完整 declaration、VS 和 PS。捕获失败不修改；setter 可能先修改再失败，仍尝试完整恢复；恢复失败返回错误。未改变 CULLMODE，也没有用全局关闭剔除掩盖问题。未知、压缩法线、蒙皮、非单位 world、武器相机和不匹配布局继续拒绝。

## 一次采集隔离本次变化

| Present | 世界路径 | HUD | 光照 |
| --- | --- | --- | --- |
| 1–30 | 原 Remix 光栅控制 | 原始 | 固定配置，RT 关闭 |
| 31–120 | v6 原世界及精确浮点 Unlit | 保留已验收入口 | 固定强球灯 |
| 121–240 | 上述路径加两种切线布局 | 同一入口 | 同一强球灯 |

30/120/180/240 各保存颜色、投影深度、解码法线及原始 RGBA32F。HUD 与 Unlit 在前后两段均启用，避免把它们的恢复混入新布局效果。灯光、曝光参数与上次相同。一个 F9 后保持镜头静止，正常退出，等待一个最终 ZIP。

## 合并定位证据

world trace header 从 version3 升为 version4，snapshot wire 仍为 v2。每次 Draw 的原 pipeline 额外记录 CULLMODE；查询失败有独立 bit512，不会被值0伪装成有效状态。旧 observer/world version1/2/3 保持可解析，不给旧数据补造剔除值。

分析器新增有界归属表：按阶段、材质/table 身份、shader、格式、声明/stride/frequency 和原剔除状态分组，记录转换/拒绝计数、首末 ordinal/帧、首次 RPC uid 与 Draw 参数。最多 256 组，超出 Draw 单独计数；该表只筛选主视图、不透明 HDR 世界候选，不给未知 shader 强行贴对象标签。材质样本仍每阶段最多 64 份，增加 shader、格式和恢复后的 CULLMODE/HRESULT。

这些数据能说明具体哪些候选开始进入转换，并帮助关联原 RPC；没有获得光栅逐物体屏幕 ID、顶点/索引内容或 RT 实例/BLAS 身份。白墙是否恢复要检查实际颜色、深度、法线的遮挡；若本次精确扩展仍无效，再安排有界的屏幕归属/几何诊断，而不是无依据放行更多 shader。

## 构建与验收要求

Windows x86/x64 执行两种实际字节声明的转换、阶段门槛、错配/压缩/实例化拒绝、13 个 getter 失败、6 个应用及 6 个恢复 setter 的修改后失败检查。系统 D3D9 NULLREF 使用真实非空编译 VS/PS、两种 stride 的实际顶点 buffer、非零 offset 和有效辅助 stream2，提交固定功能 Draw，再检查原 shader/declaration COM 绑定、矩阵与两条流均恢复。

新增 `world_tangent_layout_development_execution` 打包门槛；缺少任一架构的执行证据不得发布。既有 UI 回滚、系统状态、Source ABI/队列、IPC、启动保护、加载和配套身份检查继续执行。Python 覆盖 version4 的阶段/精确布局/CULLMODE/归属表及两段 HUD 判定，回归已有实际 trace/读回。

NULLREF 不运行 Remix、不验证 B580 图像或真实墙面，跨平台交叉编译也不替代原生执行。本版未写 Source 线程 CVar、未移入参考 mod 的全局 prim context；Source 准备任务实际并行、完整场景、武器/天空、原地图照明/PBR、生命周期与生产性能仍待验收。

已发布 [固定成品文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/02d7b302de7db8dbc46afadc725cafc39e536369/downloads/rt-world-pilot-dev.zip)，构建 `fe87baac495262a94c58974fdf0f398210e4d696`、身份 `ldb-rt-observer+81aa9f5383e0df38`，11,894,431 字节，SHA256 `3658972b15db1bff14a4fcd48677b53f76955ab48cd58f89d1ca36161e4bf22e`。[独立核验记录](../../research/rt-world-pilot-v7-validation.json) 已确认两架构新增 gate、全部旧门槛、成品哈希/身份/源码输入、包内 Python/配置及固定运行时 API 加载。

[v7 B580 实机回包](rt-world-202526-result.md) 已确认新增 720 次转换后，近处白墙和左侧结构的颜色、RT 深度/法线与遮挡共同恢复；HUD 两段均正常，240 Present 完整、无状态恢复错误。该镜头的覆盖修复通过，完整地图、模型/天空、光照/PBR、Source 准备并行与性能继续待验收。
