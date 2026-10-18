# v13：模型相机证明、三权重骨骼与 NekoToon/morph 合并覆盖

依据 [v12 实机复核](rt-world-004208-result.md)，本版保留已通过的天空修正，把模型相机精度误拒绝、多骨骼、共同 NekoToon 位置程序及必要的完整 morph 流一起接入最后阶段。仍为一个入口、一次 F9、240 Present、一个最终 ZIP。

## 从实际程序重建位置

普通模型沿用完整 VS SHA256 身份门槛，原三份骨骼程序扩展为 SHORT2 三权重调色板路径，并加入本轮已审核的 `76ed20af…ecbb4b` 骨骼程序和 `dbb52fd1…e27b91e` 刚性程序。材质名和 stride 不能代替程序审核。

NekoToon 只接受完整 VS `f29d14d0f408c16686dbba5b391c48a8e7445de0d47162b7118d0baaca5519a3`，并同时验证本轮两个完整 PS 身份。原 VS 的 POSITION0 是 o7，UV 来自 TEXCOORD3；其位置与 UV 计算已按实际声明和常量分别审核，不能把 o0 世界位置 TEXCOORD 当 clip 输出。c12.w 深度偏移目前只接受零。

三权重按 `(SHORT2+1)/32768` 和 `1-w0-w1` 解码，D3DCOLOR 经原 `.zyx` 选择内存前三个骨骼索引；对应相对地址为三倍索引，从 c58 起取三行矩阵。每个顶点按原顺序混合三组矩阵，再计算世界位置。三个引用均须落在已读取的 256 个 VS 常量中且有限，包括零权重引用。权重超范围、非有限位置及未知程序继续拒绝。

蒙皮输出使用单位 world，避免再次应用 snapshot world。刚性输入仍使用原 VS 模型矩阵作为 world。UV 按 c48/c49 恢复。已知完整程序的 c8–c11 必须通过与主相机 `view×projection` 的逐系数误差界比较；相消前验证系数，消除本轮四组输入的逐顶点精度误拒绝，同时保留不同相机、结构零项变化及非有限系数的拒绝。

## morph 与有界几何

morph 关闭时不需要额外位置数据；开启时必须取得完整的 stream2 POSITION1 FLOAT4 引用范围，按原顶点索引、stream offset/stride 和 frequency1 只读提取，在蒙皮前应用 xyz 增量。错误声明、越界、实例频率、失败的 Get/Desc/Lock/Unlock 或非有限数据均拒绝，不以辅助流前缀代替、不强制忽略 morph。

模型与自定义输入上限统一为 32,768 个绝对顶点、131,072 个索引。主顶点单文件最多 1 MiB、完整 morph 输出最多 512 KiB，原 Draw 的索引及 base/start 参数不变。此范围包含本轮 16,977 顶点模型与约 2.7 万三角形的 Neko 输入；不是取消预算。天空及其他静态上限保持原值。源缓冲只读，上传使用独立重建 VB。

## 材质与原状态

Neko 不可直接进入只按名称选择的通用静态转换，必须先通过完整 VS/PS、布局、相机和位置重建。已审核 PS 变体没有 TEXKILL；本版按 sampler0 首层贴图显示诊断材质，仍要求 HDR 主相机、无 alpha 混合且写深度，保留原 alpha-test 函数/阈值、剔除和采样器。完整 Toon 光照、多贴图材质、PBR 和原地图照明不在本版完成范围。

通用静态家族与既有世界/HUD 优先路径继续保留。替换的 shader、声明、stream0、固定功能 blend、矩阵及纹理阶段均按每次 Draw 恢复；捕获失败不修改，应用失败先恢复再回退，恢复失败阻止绘制。morph 源流只读，不改变绑定。

## 同一次功能测试保留更完整输入

PS 常量由前 32 个 float4 改为完整 224 个，覆盖实际 Neko PS 读取的 c184。需要的 morph 位置单独保存，记录绝对顶点、stream、offset、stride、频率、源 buffer 大小、HRESULT 和文件哈希；其余辅助流仍是各 256 字节前缀。

每阶段最多天空6、普通模型48、其他静态96、自定义48份。前三阶段各2 MiB，最终阶段12 MiB，其中天空256 KiB、模型4 MiB、其他1.75 MiB、自定义6 MiB，遗漏继续显式统计。模型成功样本上限由32增加到64，避免新增 Neko 挤掉其他模型证据。trace version10 对应 coverage revision13；分析器兼容旧回包，并分别验证新范围、PS224 和完整 morph 元数据。

## 验证与实机步骤

本轮旧回包的126份相关完整主输入，在新生产数学路径中118份可重建；8份启用 morph 的输入缺失完整辅助流，明确拒绝。它们不是8个对象。CPU 回放不证明 GPU 图像、三角形可见性或完整人物恢复。

原生检查新增三权重不同骨骼/字节通道、逐顶点 morph 顺序、索引与非有限拒绝、相机系数和真实不同相机；系统 NULLREF 检查覆盖有偏移/超过256字节的完整 morph 读取、读边界失败、源内容不变、PS224，以及 Neko 必须具备 prepared 输入、逐 getter/setter/restore 回滚。大网格检查使用16,977顶点、有符号 base 和非零 offset/start。Windows x86/x64 已实际通过这些新增及既有检查；固定 DLL/API、完整依赖和包内 Python 入口验证通过。不能把这些检查等同于 Remix/B580 验收。

下载新包后解压到旧包旁独立目录，运行 `run-test.cmd`。进入同一测试地图，使天空、墙体、大小道具和人物尽量同屏；按一次 F9 后保持镜头静止，人物可正常播放动画。等240 Present完成，按提示正常退出游戏，等最终 ZIP 路径并上传一个结果 ZIP。无需先补一次纯采集，也无需 F8 或 capture.trigger。

本版不修改 Source 线程 CVar，不移植全局 prim context；Source 准备线程并行、生产性能、完整人物/武器和材质外观仍待实机验收。此前 client 清理后的退出异常尚未修复。

## 已发布成品

[固定 ZIP 文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/9855ee856210ebc53de8b0d21f4d32acc14af9b9/downloads/rt-world-pilot-dev.zip)，11,945,511字节，SHA256 `0dfb76f50f95f40b19c65380b3adff911066eabb8cac6f12cec0003431779379`；构建 `18cdf6eacf68a6cd96fd512df119030b7aa92fed`，身份 `ldb-rt-observer+3f514b2619cb03ad`。成品CRC、全部成员哈希、包内脚本/README/配置、配套x86 client/x64 Host、源码收据与Windows执行记录已独立匹配。[交付核验](../../research/rt-world-pilot-v13-validation.json) 与 [实际输入回归](../../research/rt-world-v13-input-regression.json) 分别保存成品及离线证据。

Windows两架构均执行相机系数/不同相机拒绝、三权重不同骨骼及通道、逐顶点morph、范围和非有限输入拒绝；系统NULLREF上完整morph引用范围/超过256字节/最后顶点、读取失败和源缓冲不变、PS224以及Neko必须具备prepared输入、逐getter/setter/restore回滚均通过。两架构原生trace由分析器独立重析通过。固定下载树仅包含ZIP。

本地最终Python共101项，99通过、2项需要Windows而跳过；便携C++模型数学检查实际执行，runtime/overlay/native draw/Host通过严格MinGW交叉编译。八轮实际747,346 Draw兼容回归通过。当前回包94份模型、171,423个顶点经原VS字节码独立位置/UV执行核对，UV完全一致，最大640×400投影差约0.0033像素；其余8份morph缺失输入没有伪造数据补齐。详细边界见前文，实际Remix/B580画面仍待本版一次F9验证。
