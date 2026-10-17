# v10：在 v9 上成批扩展静态场景候选

本版响应“在 v9 上直接添加更多静态场景”的要求。沿用 v9 天空重建、v7 世界/HUD和同一强球形诊断灯；新增家族一起进入最后阶段，一个入口、一次 F9、一个最终 ZIP。它增加实际转换，不是仅发布另一套数据采集工具。

| Present | 路径 | 读回 |
| --- | --- | --- |
| 1–30 | 原光栅控制 | 30 |
| 31–120 | 已验证世界/HUD | 120 |
| 121–180 | 同上加 v8 天空，并完成 v9 六面预检 | 180 |
| 181–240 | 同上加 v9 受限天空重建及成批静态候选 | 240 |

## 成批接入边界

支持候选 shader：LightmappedGeneric / LightmappedGeneric_DX9、UnlitGeneric、VertexLitGeneric、WorldVertexTransition / WorldVertexTransition_DX9、WorldTwoTextureBlend。原有世界分类和目标检查通过时，优先沿用原路径；新增路径处理此前拒绝的家族、格式、仿射变换或透明裁切状态，不按具体道具名称补白名单。

完整声明必须有 stream0 的 FLOAT3 POSITION0、FLOAT2 TEXCOORD0，偏移按4对齐且字段完全位于4～128字节stride内。可有其他有界浮点UV、法线/切线/颜色字段；Source既有辅助stream1–3仅接受已识别的语义形状。任何骨骼权重/索引、POSITIONT、重复主位置/UV、未知辅助语义、越界字段或实例化均拒绝。没有执行Source morph辅助输入，不能用声明无骨骼证明物体静态。

world允许有限、非退化、正行列式的仿射矩阵，包括平移、正尺度和剪切；view保持刚性、主透视near 4～32、完整viewport、同尺寸HDR目标。仅支持非UP三角形、无alpha混合且写深度；原透明裁切开关、比较函数和阈值精确保留。初始化显式请求 `rtx.enableAlphaTest=True`，分析器要求成功事件，避免依赖用户缓存中的默认配置。镜像、武器视角、半透明和蒙皮仍拒绝。

新增作用域清除VS/PS，独立声明只引用原stream0的位置和UV，不锁定或改写buffer，不改变原Draw/索引/流绑定。原world/view/projection交给固定功能后端，缺法线使用固定运行时的几何面法线。首层材质通常选原stage0；WorldVertexTransition按固定参考实现选择stage7首层贴图，暂不混合stage12第二层。显式纹理颜色/alpha选择、RESULTARG=CURRENT及关闭stage1避免继承无关固定功能状态；固定功能vertex blend/indexed blend暂时关闭，避免继承原VS下不生效的混合状态；全部修改单Draw恢复，修改后失败也恢复，恢复失败阻止继续Draw。

这不是完整Source shader移植：原VS的UV变换、形变、顶点色烘焙、地形双层混合、平滑法线/PBR、原地图灯光仍未完成。新增几何的位置/遮挡、透明裁切与材质必须结合实机图像验收；不承诺当前镜头或全地图的大多数物体已经恢复。

## 同轮覆盖与输入证据

world trace header7标识coverage revision10；snapshot wire2、原pipeline字段和旧trace读取保持兼容。新增decision12分开统计bulk_static_converted，不能将其成功次数当作像素或物体覆盖率。

`static-coverage.json`按阶段/shader/格式/完整声明/混合/裁切/深度/仿射有效性汇总，包含未转换和带骨骼字段的候选，不再只统计既有不透明白名单。上限1024组，遗漏显式计数；它没有Source静态对象ID，不能给出全地图静态覆盖百分比。

输入采样每阶段按天空6、VertexLit模型8、其他场景10独立限额，共24份、四阶段最多96份；stride上限128，原绝对顶点/索引预算保持4096/16384，单文件1MiB、组合输入80MiB。只读失败清除未取得的占位payload并记录HRESULT，不尝试写锁。辅助常量/流仍为v9有界采样。原结果ZIP总预算192MiB保持不变，超限继续标INCOMPLETE并明确列出遗漏。

## 本版验证

新增发布门槛 `world_bulk_static_development_execution`。Windows x86/x64必须在系统D3D9 NULLREF上执行七个shader名称的非零位置/UV偏移、32/48/80字节stride、非均匀正尺度/平移、透明裁切函数/阈值保留、stage7首层选择、真实Draw及完整状态恢复。逐项注入每个getter和修改后失败setter、恢复失败与双失败阻止Draw，另拒绝骨骼、越界UV、镜像/武器相机、alpha混合、无深度写入和缺纹理。全部既有门槛保留。

这些系统用例不执行Remix/B580，原生成功不代替实机图像验收。没有写Source线程CVar或移植全局prim context；实际Source准备并行、资源生命周期与生产性能仍未验收。
