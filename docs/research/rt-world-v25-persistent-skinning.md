# v25：持久基础网格与 GPU 蒙皮

021159 的 v24 实机结果把主要瓶颈定位到 Host 模型供给：模型路径占预览墙钟 66% 至 73%，其中 CPU 蒙皮占 32% 至 35%；姿态几何哈希约占 8% 至 10%，几何构建约占 10%。Present 仅约 0.11 ms/帧，Host 3D engine 平均约 49% 至 55%。因此 v25 不再用显示覆盖或分辨率换性能，而是减少每个动画姿态的重复 CPU 工作。

## 接入边界

仅完整 shader 摘要已审核的 `Skin3` 与 `Neko3`、三权重、32 字节输入、无 morph Draw 进入新路径。它保留未蒙皮基础位置，按原 shader 的 SHORT2 解码生成三个权重，保留 D3DCOLOR 的三个骨索引，并应用已审核的 UV 常量。使用到的骨矩阵必须有限且位于固定 256 骨上限内。相机矩阵、Neko depth-bias 和原有材质程序门禁继续生效。

基础位置、UV、权重、骨索引、拓扑、剔除和材质共同形成网格身份。逐帧骨矩阵通过 `remixapi_InstanceInfoBoneTransformsEXT` 提交，不进入网格键。这样同一基础网格的动画姿态复用 `MeshHandle`，同时给后端稳定的几何身份。带 morph 的模型继续使用逐顶点 CPU 重建；未知 shader、无效权重、无效索引、非有限矩阵和其他布局也保持原回退。

## 固定 Bridge 的传输修正

固定公共头把 `blendWeights_count` 与 `blendIndices_count` 定义为数组的标量总数，固定 runtime 的 `CreateMesh` 也按标量总数分配和复制。Bridge 原序列化器却再次乘以顶点数和每顶点骨数，会造成消息尺寸与 runtime 读取尺寸不一致。`remix-skinning-transport.patch` 使计算、序列化和反序列化统一使用 `count * sizeof(element)`；准备源码时补丁本身进入固定输入指纹，并继续核对 runtime 的权重、索引和骨矩阵消费代码。

## 合并验收

仍是一次 F9。原 600 Present 诊断和 60 秒无 AA/TAA 预览不变，新增累计及预览窗口内的 `skinned_submitted`、`skinned_created`、`skinned_reused`。分析器要求 v25 实际走过该路径，并满足 `created + reused == submitted`。正确性仍以原生图像和用户观察为准：计数无法证明骨矩阵方向、动画姿态或人物部件正确。

性能判定同时看相同分项。成功信号是 CPU skin、geometry build、mesh create 和 hash 每帧下降，持久网格复用非零，Host GPU 3D 利用率上升；由于动态场景负载不同，不能只比较两个屏显 FPS。第一轮若发现拉伸、爆炸或部件错位，应按 shader/模型回退并使用同包中的矩阵与计数定位，不能扩大启用范围。

