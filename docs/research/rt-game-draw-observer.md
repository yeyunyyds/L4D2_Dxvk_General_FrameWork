# 真实游戏 Draw 消费端观察接入

首次真实游戏结果已到：[20261015-232229 复核](rt-draw-232229-result.md)。120 次 Present、40,952 次 Draw 的消费端关联检查通过；收尾配置缺失导致模块采集未执行，已修复打包与分项分析流程。世界材质候选实际 stride 为 48，仍需适配，游戏 RT 和 Source 并行未验收。

独立 B580 最小 RT 视觉已通过；本次转入实际游戏绘制链。实现不通过猜测的 GetCallQueue 调用，也不移植 l4d2-rtx 的单线程 CVar。

当前上传的 shaderapivk.dll SHA 为 `f3611dcb95625bb41dc38edd799607f85f18142bc23eab4e23162bcd50d72adb`。捕获的 GetBufferedState 代码从 world/view/projection 三个矩阵栈复制数据，参考 mod 同样在绘制前使用该状态，而不是在主线程提前写全局相机。另一方面，当前 context 的 SDK 候选槽位 146 不能被确认为无参数队列 getter；现有代码窗口包含输出参数和 `ret 4`。没有把槽位可执行或模式唯一当作 ABI 通过。

## 接入位置

`consumer-hooks.patch` 在 LDB client 四种实际 Draw 的入口取得自有状态快照，并使用新的显式命令 ID，把快照和原始 Draw 参数放进**同一条 RPC**。Host 在对应实际 Draw 的同一 UID 下记录快照、实际 D3D9 pipeline 状态和 HRESULT。原有命令 ID、无采集时的原始绘制及既有有序元数据接口继续保留。

这解决的是消费端的数据与实际命令关联，不等于已经验证了 Source 准备线程到材质消费线程之间的帧/视图队列。数据没有被伪装成 `ViewKind::Main`，也没有生成尚未确认的静态世界分类；现有静态转换在这条新观察路径中不启用。

矩阵解码先核对磁盘模块 SHA、实例 vtable RVA、250/255 两个方法的 RVA 和代码形状，再在当前 Draw 调用线程有界读取对应矩阵栈和当前材质指针。它不执行任何私有虚方法。索引、地址范围、有限值、view/projection 可逆性及矩阵栈字段复读均检查；这些是有界观察，不声称多字段读取原子化或已证实 Source 并行。profile 与出处见 [当前游戏数据解码配置](../../research/rt-consumer-game-profile.json)。

Client 采集对象只保护本设备的诊断数据；没有全局引擎渲染锁，也没有在 Draw RPC 的构造/执行期间持有该采集锁。Host 的 getter 使用临时 COM 引用并释放，不设置 D3D9 渲染状态。逐 Draw 的读内存/getter 开销使这个区间不能用于目标帧率验收。

## 一次收集的内容

- 四种 Draw 的参数、同一 RPC 内的矩阵/材质快照、client Draw 线程/QPC、实际 Host UID/线程、Draw 结果和 Present/Reset 边界。
- Client/Host viewport 对照、render target 尺寸/格式、顶点声明、stream 0 偏移/步长/频率、混合和深度状态。
- 采集段中出现的材质对象与 0/20/49 槽位代码窗口；最多 1024 个材质，每个对象最多 512 字节，方法窗口最多 128 字节。只读证据，不调用这些方法或据此直接判定 shader。
- 正在运行的游戏模块身份、上下文 RTTI 及 0～255 槽位窗口，保留已有唯一性与稳定性检查，未验证的候选仍标为候选。

观察数据使用独立 trace 格式，不改变原 `rt-host` 协议的帧/视图约束。离线分析器会拒绝身份/版本错误、重复或乱序的 client Draw、UID 错配、遗漏结果、client/Host viewport 不一致、getter 失败、非有限/奇异相机、缺少 footer、预算截断或 Reset 中断。原始游戏绘制仍执行；任何采集失败只降低诊断结论，不能被“正常 Present”覆盖。

## 交付和验收边界

包名 `rt-game-observe-dev.zip`，附固定 SHA 的 Python，不再需要下载 RT 大依赖。`run-test.cmd` 自动寻找游戏、在必要时等待用户正常退出、备份并安装配套的 x86 client/x64 Host，随后通过 Steam 启动。保留其他 LDB 文件、backend 和游戏启动选项；不是把整个 l4d2-rtx 插件装入游戏。

用户进图后只按一次 F9，观察 120 次 Present 后自动停止，模块/队列/材质证据一并封为一个 ZIP。`restore.cmd` 校验备份并恢复原来两个文件，拒绝覆盖测试后的用户更新。安装失败回滚及恢复可重复执行已纳入测试。

本地新增的有界矩阵/协议用例、Python Draw 分析与安装恢复用例已执行；Windows 构建同时执行两个架构的 CNG、请求解析、真实文件落盘、Host getter 与 COM 引用检查，并独立解析生成的 trace。这些使用受控内存和模拟 D3D9 对象，**不是实际游戏跨进程执行**。后续用户实测已单独确认消费端跨进程 Draw 关联，见文首复核；游戏队列、准备线程并行、世界分类、游戏 RT 和性能继续标为未验收。

如果本轮游戏数据对应正确，下一步直接利用已收集的视图/材质/声明与队列代码证据完成静态世界绑定，不再追加独立三角形或逐场景单项采集。本包是综合包 A 的消费端观察接入，尚不是完整 R0/R1/R2 静态世界 RT 综合包 A。

## 构建与发布记录

代码 `73b834aeee95550d3cc9f906d5c733bc3ad38303` 已完成 MSVC Release x86 client / x64 Host 构建；两个架构的有界矩阵、真实 CNG、请求文件、Host getter/COM 引用及 120 次 Present 的 fixture 落盘检查均通过。已将两份原生二进制 trace 取回云端重新解析，均通过。该用例是同进程模拟设备，不能替代游戏内跨进程执行。

本地 Python 64 项中 62 通过、2 项要求 Windows；矩阵核心 Linux 严格编译/执行与 Windows x64 Werror 交叉编译通过。最终包内固定 Python 的隔离启动检查已在 Windows 执行，源码输入 SHA、PE 架构/build ID、包内每个文件的 SHA、脚本/入口/README 字节一致性以及 ZIP CRC 均已核验。首次 Host 编译发现的宏生成长度变量名错误已修正，最终配套构建与发布验证使用上述代码版本。

首版发布记录（收尾配置有缺项）：11263270 字节，SHA256 `9355d690a7e4bb164a7d02a7f399569611987aed9eef3af2da91ff9b2809d12b`，download ref `8330fe189a01a499f008f52497f04659f5559388`。当时的构建/包内文件验证见 [观察包验证](../../research/rt-game-draw-observer-validation.json)；它未覆盖 live probe 的配置读取，不能解释为配置完整性通过。实测 Draw 通过与缺项问题分别见文首复核。Source 队列/并行、游戏 RT 与性能仍待验证。
