LDB 1.3.2：受限世界 Remix 接入试验 v14（独立模型提交修正与一次 F9 分组对照）

一个入口、一次 F9、一个最终结果 ZIP：
1. 解压到旧测试包旁的独立目录，保留旧包 backups 和 installed-observer.json，供自动继承原始恢复备份。
2. 双击 run-test.cmd，等待脚本安装配套 x86 client/x64 Host 并通过 Steam 启动游戏。固定 runtime 校验后复用。
3. 进入相同地图，选天空、墙、道具与人物同时可见的位置，按一次 F9 后保持镜头不动。人物动画可继续；420 Present 自动结束，首次 RT 初始化可能停顿。
4. 按提示正常退出游戏，等控制台显示最终 COMPLETE / INCOMPLETE 和 ZIP 路径。只上传 results 中最终 rt-world-*.zip，不要手动压缩中间目录。

同一次采集自动完成：
- 1–30：原始光栅控制，RT 关闭。
- 31–120：既有 v7 世界/Unlit/切线布局与 HUD。
- 121–180：加入完整位置/UV 重建后的天空，作为不含扩展模型的对照。
- 181–240：增加已审核的普通模型，暂不加入 Neko。
- 241–300：增加已审核的 Neko，撤去扩展普通模型。
- 301–360：普通模型和 Neko 同时加入。
- 361–420：撤去扩展模型，检查画面能否恢复。其他静态家族保留作共同背景。
30/120/180/240/300/360/420 各保存 640×400 颜色、深度、解码法线及原始读回。阶段切换和取图均自动执行，不需要再次按键。

本次修正：
- 模型拥有紧凑 VB 和 IB，把原 signed base/min/start/stream offset 的寻址转成局部顶点与索引；原游戏缓冲继续只读。
- 新建模型缓冲可读，上传后只读比较全部字节；绘制前核对实际 VB/IB、声明、world、stream stride/frequency 与清除后的 VS/PS。失败则恢复并走原始 Draw，不使用未验证的新参数。
- 请求 Remix 复制几何到其 staging 路径，减少对短生命周期输入缓冲的直接引用；没有关闭 Source 准备线程。
- 仅在无混合、写深度、完整 RGB 写入、LESS/LESSEQUAL 深度比较、无 stencil/用户裁剪面的普通实体通道重建。特殊通道保留原绘制。
- 保留已审核完整 VS/PS 身份、主相机矩阵证明、三权重蒙皮、完整 stream2 morph 位置与 Neko 首层贴图。未知程序继续拒绝。

额外证据自动合入同一个 ZIP：
- 在240/300/360取图帧分别保存最多24份提交样本，记录上传/绑定核验、局部 Draw 参数和实际提交内容的文件哈希。
- 普通/Neko/合并阶段分别预留1/2/1 MiB同一 Draw 的原始输入与重建 VB/IB；超额明确标记 payload_omitted，不当成完整采集。
- 记录与 stencil、裁剪、颜色、深度及 Remix 自定义材质控制有关的原始 render state，查询失败也保留，不猜测其数值。
- 原始输入审计继续最多每大阶段198份，四个大阶段为 raster/baseline_world/expanded_sky/expanded_models；末阶段包含所有模型子阶段并最终汇总。PS 常量224个float4、完整 morph；其他辅助流仍为256字节前缀。

v13 最终模型阶段出现白屏；本版已修改提交路径并增加分组恢复对照，仍需 B580 实机证明白屏与角色轮廓是否修复。上传核验是设备可读映射核验，不是 GPU BLAS/TLAS 读回；转换计数、CPU 回放和 Windows NULLREF 检查均不等于画面验收。

trace version11、coverage revision14、snapshot wire v2。模型材质仍为首层贴图诊断，完整 Toon/PBR/原地图灯光、全地图/人物/武器覆盖、Source 并行和性能均未验收。保持诊断灯与曝光不变，避免用亮度修改掩盖几何问题。

安装仅替换 bin\dxvk_d3d9.dll 和 bin\.l4d2bridge\L4D2Bridge64.exe；正常退出后可用 restore.cmd 恢复。无需 F8、capture.trigger 或环境变量。此前 client 清理后退出异常尚未修复；崩溃或超时仍保存 INCOMPLETE 结果。
