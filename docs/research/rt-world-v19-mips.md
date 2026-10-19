# v19：贴图容量、源mip、低照明与退出审计合并

[131549实机](rt-world-131549-result.md)已证明采样人物的原贴图可进入RT画面，64 MiB累计导出额度耗尽后有6,335次合成材质回退，基础级DDS缺少mip；强灯仍亮白，低照明人物配色改善。采集完整，但客户端退出阶段经过necola调用栈后发生执行访问异常。

v19继续一次F9、600 Present，不增加用户操作。两端重新编译，固定Remix0.6.4运行库保持原已验证版本。

## 本轮实现

- 静态sampler0纹理读取GetLevelCount给出的全部源mip，逐级校验描述、尺寸、格式、行距并只读锁定/恢复。DXT压缩块原样保留，X8每级alpha固定255。DDS DX10正确标记mip数量、caps和大小，哈希覆盖全部级别。源本身不足完整链时明确报告，不生成或宣称不存在的mip。
- 累计导出上限128 MiB、256个引用，每帧4次新读取、24 MiB链载荷加文件头；基础级仍最多16 MiB，单链最多24 MiB。显式回退与几何预算保留；该调整不保证所有场景都无回退，也不是常开模式的最终缓存设计。128 MiB与原512 MiB归档原始数据额度共同约束，归档遗漏仍显式报告。
- 默认头灯辐射改为30，301–360降为3用于中性材质对照，之后恢复30用于金属/半透明/自发光和恢复段。自动曝光不变，跨不同相机姿势不作严格逐像素因果推断。
- Infected新增独立审计组，每阶段24份；前三段每段最多512 KiB，扩展模型段最多4 MiB。额外记录原VS/PS、常量和有界几何，最多32768顶点/131072索引。该shader未获渲染准入，本轮不能宣称感染者已完成RT接入。
- 最终Bridge日志按库存哈希复核，单独写exit-analysis.json，区分异常、清理警告、日志报告正常与未验证；summary的exit_status独立于collection。客户端异常在可用时绑定游戏PID，记录命名栈帧；旧PID的异常不能被当作本次干净退出。异常退出保留完整ZIP，控制台返回码3。
- 启动模块列表额外保留necola.dll/studiorender.dll磁盘大小与SHA256元数据，不复制DLL内容，便于后续比较组件版本和退出兼容。

## 退出问题的边界

复核了配套Bridge的RemixDetach、DLL_PROCESS_DETACH与DirectInput解除链：原客户端在通知Host退出、清理对象和输入Hook之后进入其他模块卸载，与本次日志顺序相符。故障目标0x104b0不在可识别模块中，仅凭necola栈帧和现有日志无法证明具体悬空对象或唯一责任，当前没有该模块二进制/符号可定位其析构。

因此本轮增加可复核的退出判定和组件身份，不加入未经证明的Hook跳过、模块固定、不释放游戏设备等所谓修复。后端40个common device objects警告仍未修复。完整采集、稳定退出、实际画面和Source并行准备继续分别验收。

## 验证合同

新增系统D3D9 NULLREF四级DXT5源链→DDS→独立SHA256检查，覆盖小于4×4的压缩mip、后续级锁定失败后的解锁与截断拒绝；既有4/16 MiB边界和1024/2048真实导出检查保留。Python检查覆盖三级DDS、错误mip元数据、光照阶段、容量越界、大Infected输入审计、异常/旧PID/后端清理警告分类；兼容既有v18资料。

Trace version16、coverage revision19、snapshot wire v2。Windows构建和最终发布校验结果另行记录，B580本轮实际mip读取、覆盖提升和低照明效果仍待实机验证。

## 已验证交付

源码 `656dc682ee4d4a82595e417f72e64cc3ab766df9`，编译身份 `ldb-rt-observer+268b27835af951af`。121项本地Python检查通过119项、跳过2项Windows专用检查；两个修改的C++目标严格MinGW交叉编译通过。Windows原生x86/x64 Release、系统NULLREF、实际四级DXT5源链导出/独立哈希/后续级失败解锁与全部既有门槛通过；新mip检查成为纹理打包必需门槛。固定GLI的[8组多mip格式解析](../../research/rt-world-v19-dds-loader-check.json)通过。旧v18的37份DDS仍可校验，131549实际退出异常可被新分析器正确单独识别，原始上传保持不变。

[独立发布复核](../../research/rt-world-pilot-v19-validation.json)通过ZIP CRC、清单全部成员、配套PE架构/编译身份、Release编译参数、源码回执、本机输入哈希、原生fixture重分析、固定API初始化（默认头灯30），并确认HTTP下载文件等于Git blob。下载分支仅保留ZIP。

[v19固定文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/dc2bda2874e1b5b77494d489582674c0a3d40569/downloads/rt-world-pilot-dev.zip)。包11,984,355字节，SHA256 `e87a0ad8c7caaa1c159661ddfcb2b420302396741bee806be406e0e99d458f61`。保留旧测试目录及备份，新目录解压后运行run-test.cmd，进入原测试地图按一次F9；恢复ZIP提示后退出游戏，保持控制台至最终ZIP路径显示。大结果可继续分卷上传。

本轮实际B580源mip可读性、贴图回退减少幅度、灯光画面及退出兼容均未实机验收。Infected仅新增审计，退出异常根因尚未修复；不将原生检查或成功打包当作这些结果已通过。
