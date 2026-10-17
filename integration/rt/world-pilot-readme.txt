LDB 1.3.2：受限世界 Remix 接入试验 v10（在 v9 上扩展静态场景家族与透明裁切）

一个入口、一次 F9、一个最终结果 ZIP：
1. 解压到独立目录，建议放在旧测试包旁边。保留旧包的 backups 和 installed-observer.json，供自动继承原始恢复备份。
2. 双击 run-test.cmd。脚本校验并复用固定 runtime 缓存，等待已有游戏正常退出，备份/安装配套 x86 client 与 x64 Host，再通过 Steam 启动。
3. 主菜单应有正常光栅画面。进入上次地图，选择同时能看到天空、墙体和多种场景道具的位置，按一次 F9 后保持镜头静止。总计 240 次 Present，自动停止；首次 RT shader 初始化可能停顿，请等待。
4. 按提示正常退出游戏，等待控制台输出最终 COMPLETE / INCOMPLETE 和 ZIP 路径，上传 results 中一个 rt-world-*.zip。不需要手动压缩中间目录。

自动合并三段对照，使用相同的强球形诊断灯（半径 5，辐射度 3000）：
- 1–30 Present：普通 Remix 光栅控制段，RT 关闭。
- 31–120：完整 v7 世界/浮点 Unlit/64与80字节切线布局，以及已恢复的 Neko HUD 合成。上次已验收的白墙修复从本段启用。
- 121–180：上述路径加 v8 的 NekoSky 天空，作为映射对照，同时预检六个天空面的原始着色器。
- 181–240：六个天空面均通过预检时，尝试按原 VS 重建天空位置与 UV；否则整套保留 v8 天空。同时批量尝试更多静态场景候选、平移/正尺度物体和透明裁切表面，并记录实际几何；保留已验证的世界路径。
30/120/180/240 各保存 640×400 颜色、投影深度、解码法线预览和原始 RGBA32F，共 12 份原始读回。180 与 240 分别用于 v8 天空和受限重建/道具对照，不再是相同阶段的重复画面。诊断读回不是全分辨率画质或性能验收。

天空只接受 NekoSky / 0x80001、单位 world、精确实测声明、stride32、位置 FLOAT3@0 与 UV FLOAT2@12、同尺寸 HDR 目标、主透视候选、无深度/混合/alpha test。v8对照路径清除 VS，保留原 PS，使用固定 Remix 的 reserved RenderState42 Sky bit4，让后端处理天空；FOGENABLE 暂时关闭，rtx.fogIgnoreSky=True。原 category42 的未使用值 0xfefefefe 在添加 bit 前归零，单 Draw 后恢复精确原值。不会把天空当作遮挡世界的普通穹顶，也没有另外猜测天空相机。

v9 新增受限原 VS 重建：只在可完整识别的 SM2/SM3 直线运算下，读取当次原 VS/PS 字节码、256组float常量以及实际顶点/索引，执行位置与UV计算，再用本帧主view/projection的逆矩阵恢复世界位置。保留原PS，使用独立临时vertex buffer，原始绘制参数、索引和stream0频率不改；逐Draw恢复原stream0的buffer/offset/stride与全部原v8作用域状态。不会直接猜测旋转90度或UV翻转。未知指令、相对寻址、控制流、未定义输入、额外PS插值输入、非法/越界数据或不一致齐次缩放均拒绝重建；完整六面预检失败时保留v8路径，并记录原因。这是窄范围诊断修正，不是通用shader模拟器，也未接入原VS的蒙皮或顶点纹理读取。

同一F9自动保存有界场景输入：每阶段最多24份，最多96份（天空6、VertexLit模型8、其他场景10，分开计额）；每份最多4096个绝对顶点、16384个索引、stride不超过128；原buffer只用READONLY锁，失败不重试写锁。保存源顶点/索引、完整声明、VS/PS字节码、VS float常量及原矩阵、Draw参数和ordinal关联，文件附SHA256。scene-analysis.json核验文件，并提供原始位置的粗投影范围；模型VS/骨骼/形变、三角形裁剪和遮挡未应用，不能把这个范围当成实际可见性或RT实例证明。

world-runtime.jsonl记录各阶段sky_rebuild_counts；最终scene-analysis.json和summary.json明确区分实际重建次数与保留v8的回退。即使重建计数成功，映射和颜色仍需实际图像审查。遇到不支持的真实shader，本轮也会保留诊断文件，不需要再手动触发另一套采集。

v10 的批量静态路径按语义验证完整声明，支持 LightmappedGeneric / LightmappedGeneric_DX9、UnlitGeneric、VertexLitGeneric、WorldVertexTransition / WorldVertexTransition_DX9、WorldTwoTextureBlend。接受 stream0 的 FLOAT3 POSITION0 和 FLOAT2 TEXCOORD0（可有非零偏移），stride 4～128 字节且按4对齐；其他已识别字段校验大小和范围，所有骨骼权重/索引及未知辅助流拒绝。支持有限、可逆、正行列式的仿射 world，包括平移、正尺度和剪切；主 view 保持刚性，武器相机和非完整 viewport 拒绝。

原有世界分类/目标检查通过时优先保持原路径。新增路径要求 HDR 目标、非UP/无实例化三角形、无alpha混合且写深度；允许原ALPHATESTENABLE，并精确保留 ALPHAFUNC / ALPHAREF、cull、深度、sampler和buffer。清除VS/PS，使用原stream0位置/UV的独立声明、关闭LIGHTING；纹理阶段显式选择纹理颜色/alpha并关闭后续阶段，逐Draw恢复全部修改。法线采用几何三角面回退。

材质先显示第一层漫反射贴图：通常来自原stage0；WorldVertexTransition按固定参考实现使用stage7第一层，stage12第二层暂不混合。原VS的UV变换、顶点形变、完整地形混合、顶点色烘焙和PBR尚未接入，不能据此验收材质完全正确。声明无骨骼也不证明物体静态；实际位置和遮挡仍需读回审查。

新增两路均要求本轮已成功转换世界，再按阶段启用；不会自行触发初次 RT。每 Draw 恢复原矩阵、完整声明、VS、受修改的 PS/LIGHTING/FOG/category42。捕获失败不修改；应用失败后恢复，再回退原绘制；恢复失败返回错误。CULLMODE 保持原值。未知布局、带骨骼/蒙皮、镜像/奇异变换、武器相机与alpha混合继续拒绝；新增批量路径支持正尺度和透明裁切。

world trace 升为 version7，snapshot wire 保持 v2。记录原始 CULLMODE、LIGHTING、FOGENABLE 和 category42，附独立查询失败位；材质样本记录恢复后的对应状态。按阶段、shader/格式/声明/材质及 RPC 汇总有界候选和转换计数，最多256组，材质样本每阶段最多64份。统计不等于逐物体屏幕 ID，天空和道具是否正确必须结合颜色、深度、法线与遮挡审查。

HUD 沿用已验收的 Neko_Engine_Post 严格入口，控制段后所有阶段都启用；仍未恢复全部 Neko 后处理、受伤效果、武器和蒙皮模型。原地图照明、透明、完整动态场景/PBR、资源生命周期与性能未验收。没有写 Source 线程 CVar，没有移植全局 prim context；Source 准备线程实际并行仍待验证。状态查询、记录和同步读回会降低诊断帧率。

构建检查覆盖 Windows x86/x64 状态作用域、应用/恢复失败和真实系统 D3D9 NULLREF 提交。系统 D3D9 没有 reserved42 合约：测试适配器只模拟此扩展字段，其余标准状态、COM 和 Draw 使用真实 D3D9；这不能代替 Remix/B580 实机图像验收。源码收据绑定固定后端的天空分类和几何面法线合约。

安装只替换 bin\dxvk_d3d9.dll 与 bin\.l4d2bridge\L4D2Bridge64.exe；原始备份逐哈希核对并继承，不把旧观察包当原版。backend、启动参数不改。Host 只在启动时看到本次请求才从缓存加载固定 SHA 的 Remix。测试退出后可运行 restore.cmd 恢复原配套文件；保留本目录和 backups 到恢复完成。

菜单若黑屏可正常退出，等待最终结果 ZIP，无需盲操作。超时/崩溃仍保留 INCOMPLETE 结果，请等最终 ZIP 路径。不需要 F8、capture.trigger 或环境变量。
采集等待启动/F9 最长15分钟，F9后最多5分钟；世界模式保留60秒有限 IPC 容量等待与 peer 检查；单 trace128MiB、结果原始文件约192MiB。

同一诊断还记录VS的16组integer/16个boolean常量、PS前32组float常量及stream1–3各最多256字节的绑定起点前缀（各有HRESULT）。辅助流前缀不代表完整顶点或形变数据，未用于当前天空重建；可用来检查原shader是否依赖此前遗漏的输入。重建逐Draw只读取其必要的stream0与float常量，完整辅助采样按有界阶段执行。

static-coverage.json 汇总全部已知场景shader家族在主透视HDR目标上的候选，包括带骨骼声明、alpha裁切/混合及未转换绘制，附原声明和分类结果。它不把Draw占比当作物体/像素或全地图覆盖率。无需为了不同静态材质另外触发采集。
