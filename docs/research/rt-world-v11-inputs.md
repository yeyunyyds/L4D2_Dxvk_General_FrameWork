# v11：合并修正原模型变换与天空接口

本版直接使用 [v10 实机输入](rt-world-191200-result.md) 修正模型局部坐标误当世界坐标、骨骼布局一律拒绝及天空多组 UV/大坐标反投影问题。保留 v7 世界与 HUD，仍为一个入口、一次 F9、一个最终 ZIP。

## 实现边界

VertexLitGeneric 使用四份已审查的完整 SM3 程序 SHA256 验证位置/UV合约，未知程序保留原Draw；不是按材质名称无条件套常量。每Draw读取原矩阵、常量、声明与有界几何，按c58–c60恢复模型摆放，直接替换snapshot world避免双重变换。按c48–c49重建首层UV，原始VB/IB不改写。独立buffer保留绝对顶点/base/start索引关系，应用和恢复均检查HRESULT。

单骨骼路径仅接受SHORT2(32767,-1)、D3DCOLOR索引全0的每个已读取顶点，实际权重(1,0,0)，morph c13.x=0、齐次控制c0.y=1；其余权重/索引拒绝。矩阵必须有限、正行列式且与当次主相机匹配。4096个绝对顶点/16384个索引的预算维持不变，超预算明确回退，不作为状态API失败。完整蒙皮、顶点形变与动画未实现。

天空VM将输出按语义映射至TEXCOORD0–7，按原PS需要保留分量；本次NekoSky实际需要五组UV。若原位置的clip坐标（包括w）与主相机投影在浮点舍入预算内一致，保留原世界坐标，避免大坐标反投影放大误差；其他路径仍使用原逆矩阵与一致齐次w门槛。保留原PS/常量/纹理/sampler，临时声明使用独立140字节stride，显式归一化UV路由并关闭固定功能vertex/indexed blend。单Draw后恢复原绑定、矩阵、TSS及状态。六面预检不全则整套保留v8。

## 同一轮验证

1–30原光栅；31–120原世界/HUD；121–180增加v8天空并做六面预检；181–240合并启用完整天空接口、模型矩阵/UV及受限bone0，保留其他v10静态家族。读回仍为30/120/180/240四张640×400画面及原始颜色/深度/法线。

trace version8、coverage revision11，snapshot wire保持v2。新增model_rebuild_counts及有界sample，记录实际应用矩阵、重建次数和拒绝原因；分析器要求阶段、计数和样本一致。成功提交仍不能当作屏幕覆盖或BLAS证明。

当前48份完整实机输入的CPU回归已通过：24份天空保留原位置且五组UV与独立公式一致，24份模型通过原矩阵/UV和相机匹配。大模型的预算拒绝维持不变。新原生门槛覆盖双架构模型数学/拒绝、天空多UV/大坐标以及NULLREF下重建绑定和逐项getter/setter/恢复失败。系统测试只模拟reserved42，不能代替Remix/B580的图像验收。

本版最终颜色/遮挡、完整地图、动态人物/武器、材质/PBR、Source准备线程并行及生产性能仍需分别验收。当前不写Source线程CVar、不移植全局prim context，退出资源生命周期问题没有随输入修正自动解决。

## 测试操作

解压到旧包旁的独立目录，运行run-test.cmd；进上次地图，选择天空、墙和多种道具可见的位置，按一次F9后保持镜头静止，按提示退出游戏，等控制台输出最终ZIP路径后上传。无需F8或capture.trigger。

## 已发布与独立核验

[下载文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/c557e24718f780d7dca0727135fb413e5f6e152b/downloads/rt-world-pilot-dev.zip)，构建 `ed656a6cdeb68b8ec439046a93e85575949a2f69`，身份 `ldb-rt-observer+4da2cbaa992b55ba`，11,940,088 字节，SHA256 `f9c8728c10b7d004a9cb37b3501f75581d5a554c643ef0683494b4ccc8676379`。

[成品独立核验](../../research/rt-world-pilot-v11-validation.json) 已重新下载固定发布树中的主ZIP、原生与交付证据，核对全部成员、包内脚本/配置/README、配套PE架构与身份、源码指纹和固定DLL/API初始化。Windows x86/x64的新旧原生门槛均通过；分别独立重析两架构的观察和240 Present世界fixture。NULLREF只验证标准D3D9状态/提交，reserved42由测试适配器模拟，不执行Remix/B580。

本地完整Python检查96项，94通过、2项因Windows环境而跳过；Host/Runtime/Overlay/Native四处严格MinGW交叉编译及min/max宏污染检查通过。六轮实际回包共587,528 Draw重析保持通过。[48份完整实机输入CPU回归](../../research/rt-world-v11-input-replay.json) 包含24份天空、4份无骨骼模型、20份单骨骼模型，均通过主相机匹配/重建；天空五组UV匹配独立公式。CPU投影不证明三角形可见、遮挡或BLAS接入。

本轮交付不宣称模型/天空的B580实机画面已经通过；请使用同一次F9回包核对最终覆盖、映射及遮挡。原大模型预算拒绝、完整蒙皮/PBR、Source并行、退出生命周期和生产性能仍保留各自的未验收范围。
