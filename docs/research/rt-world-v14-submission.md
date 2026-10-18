# v14：模型独立提交与分组恢复对照

[v13 133627 实机结果](rt-world-133627-result.md) 的最终颜色约92%像素亮度超过0.95，深度/法线未恢复正常角色轮廓。对完整 morph 和原 VS 位置/UV 的独立字节码回放支持 CPU 重建数学基本正确，不能据此断言上传、绑定、后端实例与最终画面正确。白屏根因尚未证实。

本版实际修改模型提交：把捕获的 declared vertex range 紧凑化，重映射16/32位索引并归零 base/min/start；新建可读 DEFAULT VB/IB，上传后 READONLY 逐字节核对，转换作用域绑定自有 IB 并在物理 Draw 前复查绑定。作用域未应用时必须使用原 Draw 参数，任何 getter/setter/恢复失败均分别记录。原始游戏 VB/IB/morph 不写入。

固定后端 `b73eb23053aa50d3787b0c6c8b4b8ae49962880e` 的 `src/d3d9/d3d9_rtx.cpp::processVertices` 在 `rtx.useBuffersDirectly=False` 时把几何复制到 Remix staging，并获取读引用；本版请求该选项，保留现有几何处理线程。此举收紧资源交接边界，不是已经证明 v13 存在生命周期错误。可读映射一致性也不等于实际 GPU BLAS 内容证明。

仅对无 alpha blend、ZENABLE/ZWRITE=1、COLORWRITEENABLE 的 RGB 位完整（7或15）、LESS/LESSEQUAL、无 stencil/用户 clip plane 的普通模型通道重建。其他通道保留原始 Draw，不把面向原 shader 的特殊状态直接套给 FFP。已审核的三权重、Neko、morph、相机与贴图门槛保留。扩展 render state 数值只审计，不在无证据时清零，也不调整曝光。

一次 F9 扩为420 Present：30光栅、120世界、180完整天空、240普通模型、300 Neko、360合并、420撤去模型。每个边界保存颜色/深度/法线。181起其他静态家族作为共同背景，恢复阶段表示撤去扩展模型，并非整个场景回到120。动画随时间变化，分组图片不是同一动画时刻的逐像素证明。

原始输入仍按四个大阶段及独立家族预算采样，最后在420汇总。额外提交样本对齐到240/300/360取图前的Draw，以Present编号关联，同次Draw同时保存原始输入和重建结果；这不是GPU fence时间戳证明。普通/Neko/合并分别最多24个材质，同一 Draw 原始输入与完整重建载荷预算1/2/1 MiB，哈希和遗漏明确记录。设备状态样本也按子阶段限额。420 trace上限256 MiB，结果打包上限384 MiB，以免新增恢复对照被旧的240帧预算截断。

原生检查覆盖实际 NULLREF 上的自有 IB 绑定/恢复、每个 getter/setter故障、上传只读校验的失败锁/null/错字节/失败Unlock、特殊通道拒绝，以及大共享 VB 的 signed base/offset/start 与16/32位局部索引对应。发布要求两架构新检查各成功一次并校验配套二进制源码身份。Linux 交叉编译不能替代这些 Windows 执行检查，Windows NULLREF 也不能替代 B580 画面验收。

## 交付核验

最终构建 `d1ed8a0e23bd117b1b90612693f6c2aa517bf51c`，身份 `ldb-rt-observer+ca9009cca3123af0`。下载：[ZIP 文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/bd736ea9f9a96970bd3f9d1a9ba69efe0ec79a8d/downloads/rt-world-pilot-dev.zip)。大小 11,954,685 字节；SHA256 `26eb3d601e464c5ebd254aed2dff59a3f0fddfad9e589bbddcfe94930fdf21fd`。

[独立发布核验](../../research/rt-world-pilot-v14-validation.json) 已验证归档CRC、所有成员哈希、配套架构、二进制编译身份、全部源码收据、发布/构建状态同一提交、内置脚本/配置/README与源码一致、双架构原生测试标记与系统NULLREF、固定运行时/API正负加载检查、原生trace的420 Present与世界状态恢复。发布分支仍只包含ZIP。

[历史输入兼容检查](../../research/rt-world-v14-input-regression.json) 的九份采集共836,955 Draw全部通过结构/读回/原始输入检查；最新v13保存报告经JSON统一后与重新计算一致。位置/UV数学头文件未改变，最新137份相关主输入CPU回放135份有效，2份未知程序对应运行时已拒绝的特殊混合通道。

[v14 B580 实机复核](rt-world-145737-result.md) 已完成：合并模式恢复完整人物和武器，世界/天空可辨，撤去模型可恢复；Neko 单独阶段仍白屏及异常长条，故模型路径尚未全面通过。59份提交样本上传/绑定核验成功，33份完整配对通过逐字节和独立原VS位置/UV比较。后端缓存/实例/材质交互为后续候选，尚未确定唯一根因。Source准备并行与性能仍为unrun。
