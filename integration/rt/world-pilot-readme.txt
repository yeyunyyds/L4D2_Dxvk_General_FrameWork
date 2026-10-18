LDB 1.3.2：受限世界 Remix 接入试验 v12（天空矩阵精度修正、更大模型与其余材质输入合并验证）

一个入口、一次 F9、一个最终结果 ZIP：
1. 解压到独立目录，建议放在旧测试包旁边。保留旧包的 backups 和 installed-observer.json，供自动继承原始恢复备份。
2. 双击 run-test.cmd。脚本校验并复用固定 runtime 缓存，等待已有游戏正常退出，备份/安装配套 x86 client 与 x64 Host，再通过 Steam 启动。
3. 主菜单应有正常光栅画面。进入上次地图，选择同时能看到天空、墙体和多种场景道具的位置，按一次 F9 后保持镜头静止。总计 240 次 Present，自动停止；首次 RT shader 初始化可能停顿，请等待。
4. 按提示正常退出游戏，等待控制台显示最终 COMPLETE / INCOMPLETE 和 ZIP 路径，上传 results 中一个 rt-world-*.zip。不需要手动压缩中间目录。

自动完成四阶段对照，保持同一强球形诊断灯（半径5、辐射度3000）：
- 1–30：普通 Remix 光栅控制段，RT 关闭。
- 31–120：已验收的 v7 世界/Unlit/切线布局与 Neko HUD。
- 121–180：增加 v8 NekoSky 作为映射对照，并预检六个天空面的原始输入。
- 181–240：六面全部通过时重建天空所需的全部 UV；合并修正模型摆放与刚性单骨骼输入，并保留原世界路径及其他静态家族。
30/120/180/240 保存640×400颜色、投影深度、解码法线及原始RGBA32F，共12份原始读回。诊断读回不是全分辨率画质或性能验收。

v12 模型扩展：在v11基础上加入已审查的另一份完整刚性VS程序，扩大几何范围；对实际回包已核对的完整 VS 字节码 SHA256，按当次 c58–c60 恢复摆放，按 c48–c49 重建 UV。直接替换 snapshot world，避免原 world 已包含该变换时重复应用。无骨骼模型与经逐顶点验证的刚性 bone0 输入分开处理；后者必须 SHORT2 权重(32767,-1)、D3DCOLOR索引全0，解码权重(1,0,0)。morph必须关闭，相机必须匹配。未知程序、其他骨骼权重/索引及任意人物蒙皮继续回退，不按具体道具名称猜测变换。

模型只用READONLY读取实际Draw的有界顶点/索引，模型单次16384个绝对顶点、65536个索引；NekoToon仅审计允许16384个顶点/131072个索引，以覆盖实际约1.5万顶点/2.7万三角形输入；天空和其他静态场景仍为4096/16384。重建buffer保留原索引、base/start参数；原buffer不改写。超预算或验证不通过则保留原Draw，并记录原因。模型材质仍为第一层漫反射，法线为几何面回退；完整PBR、原地图照明、人物/武器与动画未完成。

v12 天空修正：新增POSITION完整数据流检查，仅当四分量均由未修改的POSITION0 DP4产生，且VS矩阵各系数与主相机view×projection在浮点乘加/存储误差界内相等，才直接保留原世界顶点。此检查避免大天空顶点相消产生近零clip.w时，被逐顶点相对误差误拒绝；不删除反投影齐次比例限制，不允许任意变形。受限SM2/SM3直线VM按PS语义/分量保留TEXCOORD0–7中实际需要的输出，本次NekoSky为五组UV；不再只提供一组。若原VS位置投影与本帧主相机一致，则保留原世界坐标，避免约58万量级顶点经反投影放大浮点误差。其他位置路径仍要求可逆主相机及一致齐次缩放，不放宽旧门槛。未知指令、相对寻址、控制流、未定义输入、非法数据或不支持的PS接口拒绝；六面预检失败时整套保留v8天空。

天空仍通过固定Remix的reserved RenderState42 Sky bit4分类，清除原VS、保留原PS，关闭FOG。替换声明/stream0及所需UV路由，临时关闭固定功能vertex/indexed blend，防止继承原VS下无效的固定功能状态。原PS常量、纹理和sampler保留；每Draw恢复全部修改。没有额外猜测90度旋转、天空相机或普通遮挡穹顶。

其他批量静态路径沿用v10语义检查：LightmappedGeneric/DX9、UnlitGeneric、WorldVertexTransition/DX9、WorldTwoTextureBlend；支持正行列式仿射world、无alpha混合且写深度的HDR主视图候选，保留原alpha-test函数/阈值、cull、深度与sampler。第一层漫反射通常选stage0，WorldVertexTransition选stage7；第二层混合、顶点色、通用原VS形变仍未完成。原世界路径符合原分类时优先保留。

world-runtime.jsonl新增model_rebuild_counts/sample，保留sky_rebuild_counts。scene-analysis.json与summary.json区分重建成功、未知程序/预算拒绝及v8天空回退；提交成功仍需结合颜色、深度、法线及遮挡审查，不能当作物体已恢复。static-coverage.json保留全部候选和拒绝分组，不提供全地图覆盖百分比。

同一F9保存每阶段最多34份源输入：天空6、VertexLit模型12、其他场景10、NekoToon审计6，共最多136份，附shader、常量、声明、几何及SHA256。NekoToon本版仅保存原始输入并列入覆盖清单，不转换未知自定义shader；已知静态材质和模型扩展与这项审计在同一次F9完成。辅助stream1–3各最多256字节仅为前缀证据，不代表完整形变数据。原始位置粗投影不会执行模型shader，不是实际可见性证明。trace version9、coverage revision12，snapshot wire仍为v2，保留原CULLMODE/LIGHTING/FOG/category42及查询结果。

新增路径均要求本轮先成功转换世界，单Draw捕获失败不修改，应用失败先恢复再回退，恢复失败阻止继续Draw。Windows x86/x64检查包含原生NULLREF状态、重建stream/UV路由及逐项失败恢复；系统测试只模拟Remix的reserved42字段，不能替代B580实机画面验收。没有写Source线程CVar，没有移植全局prim context；Source准备线程实际并行、资源生命周期和生产性能仍待验收。

安装只替换 bin\dxvk_d3d9.dll 与 bin\.l4d2bridge\L4D2Bridge64.exe，原备份逐哈希核对并继承。Host只在本次测试请求下加载固定SHA的Remix；backend与启动参数不改。退出后可运行restore.cmd恢复，保留backups到恢复完成。

菜单若黑屏可正常退出并等最终ZIP，无需盲操作。超时/崩溃仍保留INCOMPLETE结果。不需要F8、capture.trigger或环境变量。等待启动/F9最多15分钟，F9后最多5分钟；有限IPC容量等待60秒、保留peer检查。记录和同步读回会降低诊断帧率。
