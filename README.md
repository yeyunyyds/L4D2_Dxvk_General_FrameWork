# L4D2_Dxvk_General_FrameWork

目标：在 L4D2 + DXVK + Intel Arc B580 环境研究光追与插帧。当前优先大量复用 l4d2-rtx，保留 Source 的多线程渲染能力。

[多线程光追完整实施计划](docs/research/multithreaded-rt-implementation-plan.md)：重构有序帧／绘制数据和线程相关 Hook，沿用匹配 Remix 后端；默认把用户采集合并为 A（兼容性／静态 RT／队列）与 B（动态／画面／生命周期／性能）两批，每批一个入口、一个结果 ZIP。方案已保存，RT 接入和新测试工具待实现。

[光追优先的路线评估](docs/research/ray-tracing-route-review.md)：若跳过内置 FG，先验证匹配的 l4d2-rtx／Remix 在 B580 的兼容性与高压预算；限定硬件 RT 效果与完整路径追踪分别评估。当前尚无 B580 光追验收。

具体体验目标：原生高压约 60～80 FPS 时，锁定 60 个真实帧，通过插帧稳定呈现 120 帧；以高压最低帧和实际送帧节奏验收。

内置插帧扩展暂缓，现有实验包保留，尚未完成 B580 游戏插帧质量验收；当前也尚无 B580 光追验收。

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
