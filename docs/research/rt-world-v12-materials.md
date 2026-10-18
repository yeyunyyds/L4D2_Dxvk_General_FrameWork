# v12：修正天空数值边界，合并更大模型与其余材质审计

依据 [v11 实机复核](rt-world-204924-result.md)，本版一起处理天空未启用、更大模型预算拒绝和自定义材质输入空白。仍为一个入口、一次 F9、240 Present、一个最终 ZIP；不改变 Source 线程 CVar。

## 天空：比较矩阵系数，避免近零 w 的相消误拒绝

v11 在新相机下三个天空面的 clip.w 相对差约 2.354e-6，略超 2e-6；其余分量在共同尺度下差约 5.176e-8。此前逐顶点判断让正确的主相机矩阵转入逆矩阵路径，随后齐次比例检查拒绝，六面整体回退 v8。

v12 新增有界 POSITION 数据流证明：输出四分量必须各有且只有一条不带修改的 DP4，由声明的 POSITION0 和直接常量矩阵产生；不允许额外写入、饱和、swizzle、modifier 或相对寻址。将这四行常量与 snapshot view×projection 的各系数比较，误差界取浮点乘加及存储的八个 epsilon 乘各乘积绝对值之和。系数符合时保留源世界顶点，再按原 shader 重放全部所需 UV。

该证明在顶点乘法相消之前判断，避免巨大天空顶点产生小 w 时放大相对误差；不是简单提高逐顶点阈值。其余程序保留旧投影/反投影路径，齐次比例限制不变，六面全通过门槛不变。实际 UV 由原始 VS 产生、原 PS/纹理/采样器保持。新增真实相机数值回归及矩阵变形、重复输出写入和饱和输出拒绝检查。

## 更大的已知模型，以及新增刚性程序

模型独立上限由 4096 顶点/16384 索引提升到 16384 顶点/65536 索引；自定义 NekoToon 仅审计上限独立为16384顶点/131072索引；天空及其他静态场景上限不变。范围、signed BaseVertex、MinVertexIndex、StartIndex、原 buffer 大小和索引引用仍逐次检查，源 Lock 仍为 READONLY，重建后保留原 Draw 参数。顶点单文件最多 1 MiB，最终重建模型 stride32、最大 512 KiB；任何超预算继续回退。

新增完整 VS SHA256 `3566b4c8bed75da732f7c8cdb1bce1bb1b5025c932916b2d739ec748f9b8526b` 的刚性 profile。已保存的实际程序中：POSITION0→morph(c13.x)→c58–c60→c8–c11；TEXCOORD0→c48–c49；最终 POSITION 写入在直线路径完成，后续照明分支不再写 POSITION。这与现有刚性位置/UV 合同一致，仍逐 Draw 要求 morph 关闭、常量/布局合法及主相机匹配。

这份输入的 8951 顶点/8979 三角形此前同时超出索引上限。原回包没有保存其顶点，不能声称它已离线恢复或实机可见。扩大上限让下一次合并测试能同时尝试转换并保存完整几何；带非受限权重的程序仍拒绝，本版不宣称任意蒙皮完成。

Windows 系统用例增加 8951 顶点、26937 索引、非零 StreamOffset/StartIndex 和负 BaseVertex 的真实 VB/IB 只读提取、越界拒绝与源缓冲不变检查。既有逐 getter/setter/restore 失败注入继续验证重建流及原状态恢复。

## 其余材质：保留现有扩展，同时补齐未知自定义程序输入

LightmappedGeneric/DX9、UnlitGeneric、VertexLitGeneric、WorldVertexTransition/DX9、WorldTwoTextureBlend 的现有受限扩展继续开启；alpha-test、cull、depth 和采样器按原状态保留，第一层漫反射仍是诊断材质。第二层混合、PBR、原 shader 的完整照明不在本次完成范围。

NekoToon 在 v11 有大量 Draw，但没有完整原 shader/几何采样，不能按材质名套普通模型矩阵。v12 将它作为独立的“仅审计”家族：在有效 HDR 主相机候选中保存原 shader/常量/声明/几何，并纳入 static-coverage 分组，保留原 Draw，不转换未知程序。原始 trace 中有15189顶点/27456三角形及12621顶点/12460三角形的NekoToon；独立扩大审计预算，避免再次只收集到shader而漏掉大块几何。不能把新增采样计数当作材质已经进入 RT。

每阶段配额改为天空6、VertexLitGeneric12、其他静态10、自定义 NekoToon6，总共34；四阶段最多136份。原来8份模型配额容易在大模型和首批小道具后耗尽，扩大并单独预留自定义配额，可在同一次 F9 同时定位剩余权重、未知程序和材质覆盖缺口。辅助流仍仅前缀证据，不能当完整 morph。

trace version9 对应 coverage revision12，snapshot wire2 不变；分析器兼容全部旧版本，并仅对 v12 模型允许扩大范围。产品 manifest、collector、README 和报告共同记录配额与范围。

## 验收边界

离线重放和系统 NULLREF 检查验证数学、范围与状态合同；B580 实机天空朝向、较大模型可见性/遮挡以及剩余材质画面需由下一次 F9 验证。武器/人物、完整地图、PBR/原照明、Source 准备线程并行和生产性能仍未验收。上一轮退出清理后访问异常是独立生命周期问题，本版不宣称已修复。
