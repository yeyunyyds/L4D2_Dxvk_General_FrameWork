LDB 1.3.2：世界与实时模型贴图接入试验 v18

一次 F9 自动完成，一份结果 ZIP，无需额外手动采集：
1. 解压到独立目录，放在旧测试包旁；保留旧包 backups 与 installed-observer.json。
2. 双击 run-test.cmd，自动安装配套 x86 client/x64 Host，通过 Steam 启动。沿用上次成功进图的游戏组件组合，固定 runtime 校验后复用。
3. 进入原测试地图，让世界、天空和人物同时可见，按一次 F9。可以缓慢移动、转头，让人物动画继续。自动完成600 Present，首次贴图读取可能停顿。
4. 控制台显示 Recovery ZIP saved 后，正常退出游戏，保持此窗口开启。脚本继续分析并更新同一份 ZIP；等待最终 COMPLETE / INCOMPLETE 和 Upload this ONE ZIP 路径，上传该 ZIP。

ZIP 在 results 下，与同名结果文件夹并列，不在文件夹里面。即使退出等待或后处理被中断，也尽量保留先前生成的 INCOMPLETE 恢复 ZIP；它不代表完整验收。最终写入使用临时文件后原子替换，保留已有有效 ZIP。请勿提前关闭控制台。

阶段（无需切换按键）：
1–30 原光栅；31–120 既有世界/HUD；121–180 天空。
181–240 Neko原始基础色/alpha；241–300 合并普通模型/Neko原始基础色/alpha，头灯保持3000。
301–360 相同模型材质，头灯降为30；361–420 金属；421–480 半透明；481–540 自发光，后三段沿用30头灯。
541–600 撤去API模型，头灯恢复3000，检查背景、Mesh、材质与本轮持有的原纹理引用恢复。

颜色、深度与解码法线各10份；239/240、299/300相邻姿态和纹理元数据自动保存。这轮低照明是查找人物亮白原因的对照，不能提前保证亮白已修复；自动曝光参数保持不变，移动中的不同镜头不能逐像素直接比较。

本轮新增：
- 读取当前模型sampler0静态基础色贴图，导出本地DDS DX10，供固定Remix API加载。支持DXT1/3/5、A8R8G8B8、X8R8G8B8，明确保存sRGB/linear语义、原D3D alpha测试函数/阈值及基础采样方式；X8转换为RGB不变、alpha固定255的BGRA8以兼容固定运行库；现为第0级，不是完整mip链。
- 最多128个纹理引用、64 MiB导出数据、每帧4次新读取，单纹理最高4096及16 MiB数据。动态、RT、未知格式、读取失败或预算不足时明确记录灰色合成材质回退，不能算完整贴图覆盖。
- 原始游戏纹理只读，不改纹理内容。新DDS只写进本次结果目录model-textures。API纹理创建成功不等于GPU最终加载与画面已验收。
- 材质变化保留稳定纹理/alpha/采样身份的几何；等待旧材质GPU使用完成后先销毁再重建相同材质句柄。重复CreateMaterial本身会被运行库忽略，不作为更新方法。真实API失败仍停止RT。
- 继续512次Draw、256 MiB总几何、96 MiB单帧新几何、512缓存项和8个未完成查询限制。预算不足只跳过受影响的模型并记录，剩余对照继续；COMPLETE不代表模型无遗漏。

summary.json分别报告model_budget_coverage、model_texture_coverage及灰色回退次数。场景和全部特殊材质、完整Toon/光照、原始平滑法线、shader内透明裁剪、动画纹理、长期稳定性及性能均未完成验收。Source线程设置和物理引擎不变。

安装仅替换bin\dxvk_d3d9.dll和bin\.l4d2bridge\L4D2Bridge64.exe；退出后restore.cmd可恢复。trace version15、coverage revision18、snapshot wire v2。无需F8、capture.trigger或环境变量。结果ZIP仍为无损LZMA，可用7-Zip查看。
