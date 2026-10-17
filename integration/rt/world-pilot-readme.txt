LDB 1.3.2：受限世界 Remix 接入试验 v8（天空与有限刚性道具）

一个入口、一次 F9、一个最终结果 ZIP：
1. 解压到独立目录，建议放在旧测试包旁边。保留旧包的 backups 和 installed-observer.json，供自动继承原始恢复备份。
2. 双击 run-test.cmd。脚本校验并复用固定 runtime 缓存，等待已有游戏正常退出，备份/安装配套 x86 client 与 x64 Host，再通过 Steam 启动。
3. 主菜单应有正常光栅画面。进入上次地图，选择同时能看到天空和场景道具的位置，按一次 F9 后保持镜头静止。总计 240 次 Present，自动停止；首次 RT shader 初始化可能停顿，请等待。
4. 按提示正常退出游戏，等待控制台输出最终 COMPLETE / INCOMPLETE 和 ZIP 路径，上传 results 中一个 rt-world-*.zip。不需要手动压缩中间目录。

自动合并三段对照，使用相同的强球形诊断灯（半径 5，辐射度 3000）：
- 1–30 Present：普通 Remix 光栅控制段，RT 关闭。
- 31–120：完整 v7 世界/浮点 Unlit/64与80字节切线布局，以及已恢复的 Neko HUD 合成。上次已验收的白墙修复从本段启用。
- 121–180：上述路径加严格匹配的 NekoSky 天空。
- 181–240：保留天空，再加入两种无骨骼声明的 VertexLitGeneric 刚性道具候选。
30/120/180/240 各保存 640×400 颜色、投影深度、解码法线预览和原始 RGBA32F，共 12 份原始读回。180 与 240 现在分别用于天空和道具对照，不再是相同阶段的重复画面。诊断读回不是全分辨率画质或性能验收。

天空只接受 NekoSky / 0x80001、单位 world、精确实测声明、stride32、位置 FLOAT3@0 与 UV FLOAT2@12、同尺寸 HDR 目标、主透视候选、无深度/混合/alpha test。清除 VS，保留原 PS，使用固定 Remix 的 reserved RenderState42 Sky bit4，让后端处理天空；FOGENABLE 暂时关闭，rtx.fogIgnoreSky=True。原 category42 的未使用值 0xfefefefe 在添加 bit 前归零，单 Draw 后恢复精确原值。不会把天空当作遮挡世界的普通穹顶，也没有另外猜测天空相机。

道具只接受 VertexLitGeneric / 0x80503 或 0xa0403 的两种精确无骨骼声明、stride32、刚性 world/view、完整 depth/viewport、不透明写深度和非 UP/无实例化三角形。规范声明仅读取 FLOAT3 位置@0 与 FLOAT2 UV@16；清除 VS/PS、关闭固定功能 LIGHTING。保留原 buffer/offset/stride，不锁定或改写顶点，不解码压缩法线/切线或骨骼/形变辅助流。固定后端缺少法线输入时使用几何三角面法线，所以道具可能有平面感；不是完整平滑法线、蒙皮或 PBR。即便声明不含骨骼，也不能据此证明对象永远静态或辅助形变无效，仍需看实机图像。

新增两路均要求本轮已成功转换世界，再按阶段启用；不会自行触发初次 RT。每 Draw 恢复原矩阵、完整声明、VS、受修改的 PS/LIGHTING/FOG/category42。捕获失败不修改；应用失败后恢复，再回退原绘制；恢复失败返回错误。CULLMODE 保持原值。未知布局、带骨骼/蒙皮、缩放/镜像变换、武器相机与透明路径继续拒绝。

world trace 升为 version5，snapshot wire 保持 v2。记录原始 CULLMODE、LIGHTING、FOGENABLE 和 category42，附独立查询失败位；材质样本记录恢复后的对应状态。按阶段、shader/格式/声明/材质及 RPC 汇总有界候选和转换计数，最多256组，材质样本每阶段最多64份。统计不等于逐物体屏幕 ID，天空和道具是否正确必须结合颜色、深度、法线与遮挡审查。

HUD 沿用已验收的 Neko_Engine_Post 严格入口，控制段后所有阶段都启用；仍未恢复全部 Neko 后处理、受伤效果、武器和蒙皮模型。原地图照明、透明、完整动态场景/PBR、资源生命周期与性能未验收。没有写 Source 线程 CVar，没有移植全局 prim context；Source 准备线程实际并行仍待验证。状态查询、记录和同步读回会降低诊断帧率。

构建检查覆盖 Windows x86/x64 状态作用域、应用/恢复失败和真实系统 D3D9 NULLREF 提交。系统 D3D9 没有 reserved42 合约：测试适配器只模拟此扩展字段，其余标准状态、COM 和 Draw 使用真实 D3D9；这不能代替 Remix/B580 实机图像验收。源码收据绑定固定后端的天空分类和几何面法线合约。

安装只替换 bin\dxvk_d3d9.dll 与 bin\.l4d2bridge\L4D2Bridge64.exe；原始备份逐哈希核对并继承，不把旧观察包当原版。backend、启动参数不改。Host 只在启动时看到本次请求才从缓存加载固定 SHA 的 Remix。测试退出后可运行 restore.cmd 恢复原配套文件；保留本目录和 backups 到恢复完成。

菜单若黑屏可正常退出，等待最终结果 ZIP，无需盲操作。超时/崩溃仍保留 INCOMPLETE 结果，请等最终 ZIP 路径。不需要 F8、capture.trigger 或环境变量。
采集等待启动/F9 最长15分钟，F9后最多5分钟；世界模式保留60秒有限 IPC 容量等待与 peer 检查；单 trace128MiB、结果原始文件约192MiB。
