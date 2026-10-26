# L4D2_Dxvk_General_FrameWork

目标：在 L4D2 + DXVK + Intel Arc B580 环境研究光追与插帧。当前优先大量复用 l4d2-rtx，保留 Source 的多线程渲染能力。

[v47 人物资源／实例架构](docs/research/rt-model-resource-architecture-v47.md)：停止逐帧 CPU pose、几何拼接和动态 Mesh 更新，改为 bind-pose 拓扑只上传一次、每帧只提交实例和骨骼 palette，直接使用 Remix GPU skinning。R0 的资源／帧事务和 R1 的 `DrawModelExecute` 身份观察已落地。R2A 已让具有稳定模型身份的多个角色复用同一 surface Mesh，并通过现有 ObjectPicking 扩展向固定 runtime 传递稳定实例 ID；动画 bone hash 继续驱动 BLAS 更新，但不再每帧制造新实例身份。下一步将同一人物的多个 surface 聚合为一次资源定义和一次 `DrawInstance`。

[v43 四 worker 帧批处理原型](docs/research/rt-world-v43-frame-batch.md)：同一次 F9 先保留 30 帧 GPU-80 正确性 oracle，再把 60 帧 CPU 姿态与几何准备交给四个 Host worker，并在 HUD 注入前将同材质／变换的 Source surface 合并为较少的 Remix Mesh。Source Draw 完整性与 API 批次压缩分别验收，任何部件缺失都会失败。[v42 后的架构重审](docs/research/rt-architecture-reassessment-v42.md)与[实测结果](docs/research/rt-performance-232019-result.md)记录了切换路线的依据。

[多线程光追完整实施计划](docs/research/multithreaded-rt-implementation-plan.md)：重构有序帧／绘制数据和线程相关 Hook，沿用匹配 Remix 后端；默认把用户采集合并为 A（兼容性／静态 RT／队列）与 B（动态／画面／生命周期／性能）两批，每批一个入口、一个结果 ZIP。

[RT 接入基础进展](docs/research/rt-foundation.md)：已固定 LDB 1.3.2 和自定义 Remix 源码候选，实现有序协议、Device 通道观察与开发采集协调器；x86 client／x64 Host 原生构建和两种架构的执行用例通过。Source 队列 Hook、游戏 RT 和综合包 A 尚未完成。

[B580 独立静态 RT 已通过](docs/research/rt-preflight-b580-static-pass.md)：v6 实测 13/13 项、26/26 次读回通过，自发光与独立灯光的轮廓/平移均满足原严格判据。游戏 RT、Source 多线程、性能及资源释放仍待验证；下一步转入实际游戏队列与静态世界接入。

[真实游戏 Draw 观察接入](docs/research/rt-game-draw-observer.md)：配套 client/Host 把当前矩阵、材质和原 Draw 放进同一 RPC，保留原绘制；一次 F9 合并采集消费顺序、顶点布局与队列 ABI 证据。世界分类与主视图尚未确认，不启用游戏 RT。

[首轮真实 Draw 复核](docs/research/rt-draw-232229-result.md)：120 帧、40,952 次 Draw 的矩阵/视口/跨进程关联通过；发现世界候选实际 stride 48，与原转换器的 40 字节假设不同。收尾打包漏配置已定位，模块/Source 并行和游戏 RT 仍待验证；无需单独重复本段采集。

[受限静态世界接入试验](docs/research/rt-world-pilot.md)：适配真实 48 字节布局，按实时材质/相机/深度条件筛选世界候选，以同一版固定 Remix 尝试实际绘制转换；一次 F9 合并控制段、转换段、颜色/深度/法线和模块证据。未改 Source 线程 CVar，实际游戏画面与并行仍待验证。

[光追优先的路线评估](docs/research/ray-tracing-route-review.md)：若跳过内置 FG，先验证匹配的 l4d2-rtx／Remix 在 B580 的兼容性与高压预算；限定硬件 RT 效果与完整路径追踪分别评估。当前已通过独立最小场景，游戏光追仍未验收。

具体体验目标：原生高压约 60～80 FPS 时，锁定 60 个真实帧，通过插帧稳定呈现 120 帧；以高压最低帧和实际送帧节奏验收。

内置插帧扩展暂缓，现有实验包保留，尚未完成 B580 游戏插帧质量或游戏光追验收。

[60→120 路线重评](docs/research/project-reassessment-60to120.md)：当前输入可信度、画质、GPU 预算与双呈现仍未达标。建议先验证成品方案是否满足目标；内置路线按明确门槛研发，停止仅围绕通过率小修。

已实现[独立 Host 采集原型](docs/research/host-capture.md)：颜色/深度像素与 Reset 已通过软件 Vulkan 及用户上传的 B580 实机日志检查。

本版合并[可显示生成帧的相机插帧原型](docs/research/frame-generation-prototype.md)：全尺寸 GPU 历史、场景/武器/UI 阶段掩码、中间视角合成和真实 Present。武器/UI 保留当前帧，动态物体、透明边缘与帧节奏仍属原型限制。用户之前的 B580 GPU 重投影验证见[报告](docs/research/gpu-reprojection-user-results.md)。

- [第一阶段研究结果](docs/research/phase1.md)：源码接入点、必要改动与验证证据。
- [官方 FSR 组件接入](docs/research/fsr-component-backend.md)：官方光流与插帧组件的移植和验证依据。
- [区域保护版 B580 分析](docs/research/fsr-regional-b580-result.md)：1534 次生成帧调用成功，接受率 81.47%；人物／机枪残影与墙体错位仍未通过画质验收。
- [FSR 区域保护修正](docs/research/fsr-regional-protection.md)：局部保留细亮区域，采集成功／拒绝候选并统一日志；本轮 B580 画质待验收。
- [B580 首次 FSR 游戏运行](docs/research/fsr-b580-first-run.md)：174 次生成帧 Present 成功，但约 95% 候选被保护规则拒绝，画质尚未验收。
- [地图加载诊断](docs/research/fsr-startup-diagnostics.md)：统一收集 Bridge、DXVK 与 FG 日志，包含关闭 FSR 的进程级对照。
- [官方 FSR Host 接入](docs/research/fsr-host-integration.md)：实际 Present 接入、安装和一次综合测试。B580 已有游戏执行记录，整体画质验收待完成。 [集成验证记录](research/fsr-regional-validation.json) · [Windows 包校验](research/fsr-regional-package-validation.json)。
- [Windows / B580 验证步骤](docs/research/windows-validation.md)：GPU 探测与官方 FSR Vulkan 样例验收。
- [帧输入契约草案](docs/research/frame-input-contract.md)：深度、相机、运动矢量、HUD 与资源生命周期。
- [固定研究版本](research/sources.lock.json)及[本轮验证记录](research/validation-2026-10-12.json)。

Linux 云环境复现（Python 3、Git、g++、Debian trixie）：

```bash
bash scripts/setup_research_tools.sh
python3 scripts/prepare_research.py
python3 scripts/build_probe.py
python3 scripts/verify_fsr_shaders.py \
  --glslang .research/tools/root/usr/bin/glslangValidator \
  --spirv-val .research/tools/root/usr/bin/spirv-val
python3 -m unittest discover -s tests -v
```

工具、依赖源码和完整输出放在被忽略的 `.research/`；准备脚本拒绝覆盖已有依赖改动或错误版本。源码研究缓存不是附加项目仓库。shader 检查只证明 FP32 GLSL 编译及 SPIR-V 验证，不证明 GPU 插帧或整个 SDK 构建成功。
