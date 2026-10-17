# v8：天空与有限刚性道具

[v7 实机结果](rt-world-202526-result.md) 已确认近处白墙和左侧结构恢复颜色、RT 深度/法线及遮挡，HUD 正常。本版以完整 v7 为基线，将天空与有限道具合并进一次 F9 的分段对照，不重复同一缺墙调查。

| Present | 内容 | 读回 |
| --- | --- | --- |
| 1–30 | Remix 光栅控制，RT 关闭 | 30 |
| 31–120 | 完整 v7 世界、浮点 Unlit、切线布局、HUD | 120 |
| 121–180 | 上述内容加 NekoSky | 180 |
| 181–240 | 上述内容加无骨骼声明的刚性道具候选 | 240 |

固定强球灯保持辐射度3000，曝光配置相同。每次读回同时保存颜色、投影深度和解码法线。一个入口、一次 F9、一个最终 ZIP；进图选择可见天空和道具的镜头，触发后保持静止。

## 已有数据限定接入范围

v7 trace 的天空候选为 NekoSky / `0x80001`，stride32，主场景同一透视矩阵，单位 world，HDR113，无深度写入。道具候选为 VertexLitGeneric / `0x80503` 与 `0xa0403`，stride32，不透明写深度，刚性 world。其声明为位置 FLOAT3@0、压缩法线/切线 UBYTE4@12、UV FLOAT2@16，外加辅助流；两者区别是是否含 stream1 COLOR1。它们区别于已有带 BLENDWEIGHT/BLENDINDICES 的蒙皮声明。

实现逐字节比对完整实测声明，另检查材质、shader、矩阵、viewport、HDR目标、深度/混合状态、三角形拓扑、stream频率与offset。无骨骼声明只限定实验输入，不证明对象静态或形变辅助流无效。缩放、镜像、武器相机、未知声明及蒙皮路径拒绝。

## 固定后端合约

后端继续使用 `b73eb23053aa50d3787b0c6c8b4b8ae49962880e`，不重编运行时：

- `src/d3d9/d3d9_rtx_utils.cpp` 读取 reserved RenderState42，`0xfefefefe` 表示未使用。
- `src/dxvk/rtx_render/rtx_types.h` 的 InstanceCategories 按 WorldUI、WorldMatte、Sky 排列，Sky 为 bit4。
- `src/dxvk/shaders/rtx/concept/surface/surface_interaction.slangh` 在 normalBufferIndex 无效时使用 flippedTriangleNormal。

准备脚本校对上述源码模式，并将三个固定对象的 SHA256 写入构建收据及其 fingerprint。天空仅清除 VS，保留原 PS，用 position/UV 的规范声明和原快照矩阵，添加 Sky 分类、暂时关闭 FOGENABLE；启用 `rtx.fogIgnoreSky=True`，避免天空修改世界雾。后端将天空作为专用天空输入处理，而非普通遮挡穹顶；实际正确分类和图像仍需 B580 回包验证。

道具清除 VS/PS，规范声明仅包含 position/UV，关闭 LIGHTING；压缩法线/切线不作为浮点误读，不复制/改写顶点。没有 normal 输入时由固定后端使用三角面法线，可能表现为平面着色，尚无平滑法线、完整 PBR 或蒙皮。原 buffer、offset、stride 和辅助流绑定保留。

各作用域恢复全部修改字段及精确 COM 绑定。Sky 不改原 PS，道具恢复 PS；共同恢复 VS、完整 declaration、矩阵、LIGHTING，Sky 另恢复 FOGENABLE 与 category42。原 category sentinel 加 bit 前归零，恢复时保留原 sentinel。原剔除不改。只有本轮已有成功世界转换才能进入新增阶段；它们不触发初次 RT。

## 一轮内保留归因证据

world trace version5、snapshot wire2，增加原 LIGHTING/FOGENABLE/category42 与查询失败位，保留 CULLMODE 和 RPC 关联。转换 Decision10/11 分别标识天空/刚性候选，分析器校验阶段、完整声明、相机、状态与已有世界转换前提，单独汇总 sky/rigid 数量。三段 HUD 均要求成功，恢复后的材质样本覆盖三段。

Windows x86/x64 执行状态恢复与失败注入，系统 NULLREF 使用真实 HDR 目标、顶点 buffer、非空 shader 及实际 Draw；仅 reserved42 经适配器模拟，因为标准 D3D9 不支持该私有合约。新 `world_scene_scope_development_execution` 门槛必须有两架构证据才能打包；既有门槛全部保留。NULLREF 不执行 Remix 或验证 B580 图像，交叉编译只用于提前发现编译问题。

未写 Source 线程 CVar，未移植全局 prim context或调用私有游戏方法。Source 准备线程实际并行、原地图光照、武器/蒙皮、透明、完整材质与性能仍未验收。


## 已发布成品与验证

[固定成品文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/e78f5dc095f605b4cb75f0211ce622d297380593/downloads/rt-world-pilot-dev.zip)，构建 `ef5485f2c47546f46d15cb310f54ce9d0e181084`，身份 `ldb-rt-observer+75722b8bfc0efc34`。大小 11,900,168 字节，SHA256 `0c37ad7dcd2042f523200b92f167788498ededa22b0b04def6a52f0781ecf48c`。

[独立核验记录](../../research/rt-world-pilot-v8-validation.json) 确认全部743个成员的清单/哈希、成品与原生证据中的配套 PE 一致、源码输入及后端合约收据、独立包内 Python/配置和固定 runtime API 初始化。Windows x86/x64 的新增场景及全部既有门槛通过。Sky 作用域各17个 getter、8个应用/8个恢复 setter，道具两路各16个 getter、7个应用/7个恢复 setter，逐项失败注入通过。reserved42 仅在标准 D3D9 测试适配器中模拟，未当作实际 Remix 成功证据。

原生 Host 集成 trace 每架构包含240 Present、723 Draw、213次转换，其中天空1次、两种道具各1次；独立分析确认阶段与原状态归属。它仍是模拟设备和实际 Host 命令路由验证，不是游戏图像。本地87项 Python 测试完成，85通过、2项需要 Windows 而跳过；额外严格 x64 交叉编译通过。v4/v5/v6/v7 四份既有实机回包共430,784 Draw 及原始读回均回归通过，没有改写旧预览。v8 新增天空和道具实际画面尚待回包验收。
