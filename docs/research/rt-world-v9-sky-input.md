# v9：受限原始天空 VS 重建与场景输入诊断

[v8 实机结果](rt-world-141921-result.md) 显示天空可见但云层映射与原光栅不符；道具60次转换没有明确新增RT表面。本版针对原VS被替换后丢失的位置/UV处理，增加可拒绝未知输入的窄重建路径，并在同一次F9补齐原始几何和shader证据。没有根据最终图片直接指定某个固定旋转角。

| Present | 实际路径 | 读回 |
| --- | --- | --- |
| 1–30 | 原光栅控制，自动记录原场景输入 | 30 |
| 31–120 | 已验收的v7世界/HUD | 120 |
| 121–180 | v8天空对照，同时预检六个天空面 | 180 |
| 181–240 | 六面均可识别时尝试原VS重建天空；否则全部保留v8。加入原有受限道具路径与输入诊断 | 240 |

保持强球形诊断灯、曝光配置和既有世界/HUD入口。原始snapshot wire2不变，world trace header6区分v9，pipeline字段沿用v8；旧记录继续兼容。一个入口、一次F9、一个最终ZIP，镜头选择天空和前景道具同时可见，触发后保持静止。

## 有明确拒绝边界的原VS重建

`rt_sky_shader.h` 仅支持SM2/SM3、最多512条直线指令、16K DWORD字节码：MOV、ADD、MUL、MAD、RCP、DP3/4、MIN/MAX、ABS、M4x4，加声明和float常量定义。解析寄存器类型、源swizzle/修饰、目标分量mask/saturate和本地DEF常量。临时寄存器逐分量追踪定义；POSITION0和TEXCOORD0来自本次精确声明的stream0，其他输入未定义。相对寻址、分支/循环、纹理访问、未知运算、超范围寄存器、非有限值及读未定义分量均拒绝。PS须为SM2/3且只声明可提供的TEXCOORD0.xy及sampler输入；原PS仍执行。

读取原VS当次256组float常量，计算各顶点clip position和UV。clip通过主view×projection逆矩阵回到世界坐标，检查有限范围、正的同向齐次尺度，并要求各顶点齐次尺度一致；这样避免除以不同w后改变纹理透视插值。计算为CPU float重放，不能承诺与GPU指令融合/舍入逐bit一致。

新几何使用独立DEFAULT临时VB，保留原Draw参数、IB及stream0频率；前导顶点按实际绝对索引保留有界padding。内层v8作用域验证完整声明/管线并生效后才绑定临时stream0；准备失败走v8。绑定失败则先恢复stream与作用域，成功恢复后回退原Draw；恢复失败返回错误，禁止在污染状态下继续绘制。捕获和文件诊断不写原buffer，不改变Source线程CVar或全局prim context，不开启全局vertex capture。

六个实际天空身份必须在121–180阶段全部预检成功，最后阶段才允许重建，避免已知不支持的面与已重建的面混用。逐Draw仍重新读取本次数据；某次异常和所有回退计数显式保留。实际NekoSky代码尚未在云端取得，因此本版是否能重建其全部运算、最终天空方向和HDR颜色是否正确，必须由实际回包验证；未知shader时保留v8并提供原始证据。

## 同轮保留几何和原着色器

通过Host公共D3D9接口读取，按阶段/材质身份有界采样，每阶段最多12份、共48份。优先顺序随实际Draw，不能保证采到所有模型。同一文件前缀包含原ordinal，事件包含material/table、format/shader、Draw kind/topology/参数、完整矩阵、stream offset/stride和绝对顶点起点。源VB/IB只使用READONLY Lock；失败记录HRESULT，无写锁重试。

上限：绝对顶点末端4096、索引16384、stride64、单shader64KiB、常量4096字节。64位计算offset/range，校验IB索引确实落在声明的MinVertexIndex/NumVertices范围；正确处理有符号BaseVertexIndex和INDEX16/32。拒绝UP/实例化与越界，源buffer不改写。最大组合输入负载低于24MiB，继续纳入最终ZIP现有总预算。

保存完整声明、原VS/PS、float常量、源顶点和索引，各文件带SHA256。`scene-analysis.json`核验预算/路径/大小/哈希/索引边界，列出shader版本/opcode，并对FLOAT3 position@0给出粗投影范围。投影没有执行模型原VS、骨骼、形变、裁剪或遮挡；它不是可见性、BLAS或逐物体ID证明，也不将成功材质自动标为TNT等具体物体。

`sky_rebuild_counts`在30/120/180/240记录attempts/ready/reconstructed/fallback和原因；collector明确显示完整重建、保留v8或部分/未运行，不把原Draw成功当作重建成功。已修正旧v6控制台阶段提示。普通道具/武器/蒙皮接入范围未扩大。

## 验证要求

纯C++执行两种shader版本的已知位置/UV变换、未定义输入、非法/相对操作、非有限值和逆矩阵拒绝。Windows x86/x64另用实际D3DCompile生成非平凡VS/PS，读取其真实bytecode/常量/带offset源VB，比较重建位置/UV与解析结果；真实系统D3D9 NULLREF提交，再检查原stream、VS恢复。覆盖有符号base/非零IB偏移、越界拒绝、绑定修改后失败、恢复失败、双失败阻止Draw及getter失败。系统测试只模拟reserved42，其他标准状态和Draw仍真实；它不执行Remix/B580。

新增 `world_sky_reconstruction_development_execution` 发布门槛，要求两架构的解析器与系统执行证据；所有既有门槛保留。Python核验新原始文件和回退统计，并回归已有trace/读回。真实天空/道具图像、原地图灯光、完整PBR、Source准备并行、资源生命周期和生产性能继续按各自证据验收。

同一诊断还记录VS的16组integer/16个boolean常量、PS前32组float常量及stream1–3各最多256字节的绑定起点前缀（各有HRESULT）。辅助流前缀不代表完整顶点或形变数据，未用于当前天空重建；可用来检查原shader是否依赖此前遗漏的输入。重建逐Draw只读取其必要的stream0与float常量，完整辅助采样按有界阶段执行。
