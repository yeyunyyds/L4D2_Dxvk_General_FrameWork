# v6：场景覆盖扩展与原生 UI 合成入口

v5 实机已确认同一相机下灯光响应，见 [结果](rt-world-165351-result.md)。下一包保持强球灯不变，在一个 240 Present 段中比较扩展前后的世界覆盖与 HUD。没有再次增加用户的灯光采集轮次。

## 从现有数据选择接入范围

v5 trace 有 960 次主视图、不透明写深度的 `UnlitGeneric / 0x80503` 绘制。stream 0 是完整 FLOAT3 position、FLOAT3 normal、FLOAT2 UV，步长 32；附加 stream1 COLOR1、stream2 POSITION1/NORMAL1、stream3 POSITION2 的精确声明已保留。它不同于 VertexLitGeneric/NekoToon 的压缩法线及 blend weight/index 布局，可在不解码蒙皮或伪造骨骼的前提下构造固定功能输入。

选择仍要求单位 world、有效且刚性的右手透视视图、near 4～32、完整 viewport/depth、同尺寸 HDR 目标、不混合/alpha test 且写深度。只在扩展阶段接受上述 shader、格式及逐字节声明。FVF 读取 stream0 前 32 字节，排除额外流；VS/PS 同时置空后提交，再恢复完整原绑定和矩阵。shader/材质分类是受限候选策略，不是已验证的 Source 静态对象标签；未知/压缩布局继续拒绝。上次白墙的具体 Draw 尚未确定，因此本版是覆盖扩展试验，不能提前承诺白墙已修好。

旧 LightmappedGeneric 路径保持 v5 的作用域。一个需要收紧的分析结论是：固定 runtime 的 `processRenderState()` 依据 **PS 是否启用** 选择 programmable/FFP 纹理提取；v5 中原 PS 仍绑定，所以仅见 TSS stage1 DISABLE 不足以证明 programmable 路径未读取绑定的其他纹理。v5 的三段状态一致和真实亮度变化结论仍成立。本版新增 Unlit 路径同时清除 PS，才进入已核对的固定功能纹理提取路径；仍不宣称完整 PBR 已正确。

## HUD 注入点

现有 v5 原始 trace 中，52,365 次 backbuffer `UnlitGeneric` 绘制均位于每帧 `Neko_Engine_Post` 之后。这个顺序允许复用固定 runtime 已有的 UI 合成路径，而不重新解释这些 HUD 的可编程顶点、字体或纹理。

固定 backend `b73eb23053aa50d3787b0c6c8b4b8ae49962880e` 的 `d3d9_rtx.cpp`：

- `makeDrawCallType` 对关闭可编程 VS、正交且不写深度的 primary-target UI 返回 Rasterized 并请求注入。
- `internalPrepareDraw` 在该点调用 `triggerInjectRTX`；此后默认 `skipDrawCallsPostRTXInjection=False` 保留原绘制及状态。
- 显式请求 rtx.enableAlphaBlend=True：固定 backend 在识别 UI 之前会先过滤全局禁用的 alpha-blended draw，透明触发三角形需要通过此门槛。
- 未触发注入时，可编程 VS 又关闭 vertex capture 的绘制会被移除。这正是旧 HUD 缺失的机制。

v6 仅匹配实际观察到的 `Neko_Engine_Post / 0x80001`、单位 world/view/projection、TList、非 UP、完整 viewport、A8R8G8B8、无深度/混合、stride32 及五元素精确声明，额外核对 GetRenderTarget 与 GetBackBuffer 返回同一实际 surface。匹配后用 alpha=0 的 XYZRHW|DIFFUSE 三角形触发原生 UI 注入，不把它加入世界几何。成功后跳过该次 Neko 后处理，再让后续游戏 HUD 按其原 VS/PS 光栅绘制。

跳过后处理是必要的覆盖控制：若继续执行原 fullscreen Neko draw，未完成的光栅 HDR 输入可能把刚合成的 RT 覆盖掉。它也意味着本版没有恢复 Neko 全套色调、受伤 fade 等效果。这里只保证边界条件和状态作用域，实际 HUD 图像仍待验收。

使用 D3DSBT_ALL 额外备份，同时显式捕获/恢复所有被修改的 public 状态：VS/PS、声明、stream0、三组矩阵、13 个渲染状态、stage0 纹理与四项阶段状态、stage1 的 color/alpha op。stream0 会被 DrawPrimitiveUP 清除；不能仅凭 StateBlock::Apply 的 S_OK 判定状态均已恢复。所有 setter 失败后都尝试完整恢复；恢复失败返回错误而不在未知状态下执行原绘制。捕获/应用/透明 draw/恢复分别记录。透明 draw 的 S_OK 仅证明该调用完成，不是 Remix 已实际注入或 HUD 图像通过的证据。

## 同一次 F9

| Present | 覆盖 | 灯光 |
| --- | --- | --- |
| 1–30 | 原光栅控制段 | 固定配置，RT 关闭 |
| 31–120 | 原 LightmappedGeneric 接入 | 球灯半径5、辐射度3000 |
| 121–240 | 加入精确 Unlit 场景候选及 Neko HUD 入口 | 同一球灯，不改变曝光参数 |

30/120/180/240 三类读回保持不变，继续记录最后世界相机、材质样本及一包最终日志。按一次 F9 后保持镜头静止，可以在同一位置对比几何、颜色与 HUD。

原始 Draw trace 的世界 profile 标记为 version3（snapshot wire 仍为 v2），分析器只在该版本且第120次 Present 后允许新增 Unlit 声明和 HUD decision9。HUD replacement 明确标为未调用原 post draw；新增透明 draw 在 runtime JSONL 另记，避免把跳过原调用伪装成原 draw 执行。旧版本 trace 继续保留旧白名单验证。

## 构建与验收

新增 Windows x86/x64 原生用例在真实系统 D3D9 NULLREF 上检查 StateBlock 和 DrawPrimitiveUP：原 shader、纹理、声明、stream0、矩阵及渲染状态恢复；每一个 setter 先修改状态再返回失败；各 getter、capture、draw、restore 失败分别检查。另以模拟设备验证 Unlit 精确布局、PS/VS 清除与恢复以及压缩布局拒绝。原有 startup、transport、材质解码及作用域用例继续执行。系统 NULLREF 没有默认颜色目标，状态用例显式创建离屏 RT，并仅在测试代理中把它作为 backbuffer 身份；另用第二个真实 surface 验证身份不符时不修改/绘制。生产代码仍调用真实 GetBackBuffer。系统 NULLREF 不运行 Remix，也不验证实际 swapchain 注入或替代 B580 的效果验收。

Python 新增 profile/阶段/声明越界拒绝、HUD 丢失/恢复失败的判定，并回归 v4/v5 原始数据。成品必须通过双架构新增 gate、配套 PE 身份、包内 Python、固定运行时 API 初始化与全部成员哈希验证后发布。

本版没有修改 Source CVar、没有移入参考 mod 全局 prim context、没有调用私有 game 方法或替换固定 Remix DLL。武器、蒙皮、天空、透明、完整地图光源、多线程准备任务和性能仍未验收。不能由保留线程设置推导出准备任务已实际并行。
