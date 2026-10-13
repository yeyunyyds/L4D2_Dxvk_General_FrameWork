# L4D2_Dxvk_General_FrameWork

目标：在 L4D2 + DXVK + Intel Arc B580 环境实现插帧，随后评估光追。

当前阶段是插帧研究，尚未提供可安装的插帧 DLL。

已实现[独立 Host 采集原型](docs/research/host-capture.md)：颜色/深度像素与 Reset 已通过软件 Vulkan 及用户上传的 B580 实机日志检查。

本版合并[可显示生成帧的相机插帧原型](docs/research/frame-generation-prototype.md)：全尺寸 GPU 历史、场景/武器/UI 阶段掩码、中间视角合成和真实 Present。武器/UI 保留当前帧，动态物体、透明边缘与帧节奏仍属原型限制。用户之前的 B580 GPU 重投影验证见[报告](docs/research/gpu-reprojection-user-results.md)。

- [第一阶段研究结果](docs/research/phase1.md)：源码接入点、必要改动与验证证据。
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
