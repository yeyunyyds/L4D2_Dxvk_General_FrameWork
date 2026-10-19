LDB RT World Pilot v19 — source mip / texture coverage / low light / exit review

测试：
1. 将新包解压到独立目录，沿用上一轮成功进图的游戏组件组合。
2. 双击 run-test.cmd，启动游戏并进入原测试地图。
3. 按一次 F9，可以缓慢转头、移动和让人物动画继续。自动完成600 Present，不用切换其他按键。
4. 出现 Recovery ZIP saved 后正常退出游戏，保持控制台开启，等最终 Upload this ONE ZIP 路径显示。上传该 ZIP；需要分卷时全部上传。

ZIP位于results下，与同名文件夹并列。先保存INCOMPLETE恢复ZIP，再等待游戏退出，完成分析后原子替换同一ZIP。压缩可能需要数分钟，请保持控制台开启。

本版合并：
- 贴图累计导出128 MiB、最多256个源纹理引用；每帧最多4次新读取、24 MiB加文件头，保持显式灰色回退。上限仍可能不足，结果不会将回退当作完整覆盖。
- 复制静态sampler0纹理已有的全部mip级，DXT1/3/5压缩块原样保留，BGRA/X8保持原颜色空间；X8 alpha固定255。保留原D3D alpha和基础采样方式。单基础级最多16 MiB，整条链最多24 MiB。源纹理本身没有完整mip时单独报告，不生成假mip。
- 默认头灯改为30；301–360段降为3作对照，随后恢复30用于金属、半透明和自发光段。自动曝光配置不变。相机可移动，跨阶段不能当作严格同机位对照。
- 自动额外审计Infected shader的VS/PS、常量及有界几何，每阶段独立24项额度。这是接入前证据，不宣称感染者已进入RT路径。
- 最终exit-analysis.json明确报告Bridge异常和后端未释放对象；summary中的exit_status与collection独立。COMPLETE表示采集完整，不能当作稳定退出。退出异常时控制台返回码3，结果ZIP仍保留。

阶段：
1–30原光栅；31–120世界/HUD；121–180天空；181–240 Neko原贴图；241–300合并模型原贴图。
301–360相同中性材质、头灯3；361–420金属、421–480半透明、481–540自发光（头灯30）；541–600撤去API模型、检查恢复。
每阶段自动保存颜色、深度和解码法线，239/240、299/300相邻姿态及原始输入保留。无需F8、capture.trigger或环境变量。

原游戏纹理只读，DDS只写在本次结果目录。保持512 Draw/帧、256 MiB几何、96 MiB单帧新增几何、512 Mesh和8个待完成查询限制。失败/遗漏和采集完整性、实际画面验收分别报告。
本版尚未修复或证明necola退出异常的唯一原因；不关闭插件、不吞异常、不调用游戏拥有设备的Remix Shutdown。完整Toon/像素shader、原始平滑法线、动画贴图、全部材质、长期常开和性能仍待后续验收。

安装只替换bin\dxvk_d3d9.dll与bin\.l4d2bridge\L4D2Bridge64.exe，退出后restore.cmd可恢复。Source线程设置和物理引擎不变。Trace version16、coverage revision19、snapshot wire v2。ZIP为无损LZMA。
