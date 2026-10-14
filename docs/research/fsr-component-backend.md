# 官方 FSR 组件接入

## 当前结果

采用锁定 FidelityFX SDK v1.1.4（FSR 3.1.4 文档对应版本）的官方 Optical Flow、Frame Interpolation 主机组件和全部 18 个对应 GLSL pass。算法着色器保持原样，新增的主机适配单独记录。直接记录计算命令，不链接 SDK 的交换链代理。

云端 Mesa llvmpipe 实际执行已通过：二维物体运动的中间位置、静止、显式场景重置、真实帧编号跳变后的自动重置、HUD-less 场景输入合成、三次完整上下文创建/释放。核心与同步校验均开启。将原帧作为输出的反向对照会被中点检查拒绝；错误运动矢量纹理格式在调度前被拒绝。

这不是 B580 游戏验收。当前 Windows 游戏实验包仍使用原有自研后端；新组件尚未接入 Host 的游戏调用链。本轮没有更新下载 ZIP。

## 与 l4d2-rtx 的关系和线程边界

固定参考 `xoxor4d/l4d2-rtx@1fc8ded63a8491e9c1972eb2940d6cebbc20ad9e`：

| 参考位置 | 可复用信息 | 本轮实现边界 |
|---|---|---|
| `main_module.cpp` 主视图和 `RenderView` 接入 | 主相机、视图身份及帧边界 | 保留现有 Host shader 观测；增加矩阵到 FSR 相机参数的适配，尚未新增引擎 Hook |
| `model_render.cpp` 视图模型和 HUD 分支 | 武器独立投影、首个 HUD 绘制边界 | 官方组件支持 HUD-less 输入；测试用已知分层，不代表游戏分层已完整 |
| Remix `rtx_instance_manager.cpp` | 实例变换和前帧顶点历史 | 尚未移植，仍是动态模型运动矢量的主要工作 |
| Remix `RtxContext::dispatchDLFG` | 相机、深度、屏幕运动矢量交给插帧调度 | 改为直接使用官方 FSR FI/OF 组件做独立验证；没有运行 DLSS-FG |

不移植其 `mat_queue_mode=0`、`r_threaded_particles=0`、`r_threadeddetailprops=0` 设置，也不引入整套固定功能转换、CPU 蒙皮。本轮没有修改游戏线程设置，尚不能据此宣称新的引擎 Hook 已经兼容并发绘制。

组件由单个渲染设备所有者顺序调用，GPU 提交仍需遵守原设备队列锁和依赖。SDK 该版本全局保存设备上下文，适配器明确拒绝第二个同时存活的组件所有者，避免跨设备覆盖。这里的设备调用顺序不要求把整个游戏渲染器改成单线程。

## 已实现的数据接口

- `integration/ldb/fsr_camera.h`：从现有 row-major `cViewProj` 恢复相机位置、正交方向、有限近远平面和垂直 FOV。限定未抖动、常规 D3D `[0,1]` 深度；不支持的透视、正交、反向 Z、无限远或无效矩阵会失败。单位转换需由调用方提供。
- `tools/fsr_component_backend.h`：验证尺寸、格式、相机参数和资源别名；调用官方光流、FI prepare、FI dispatch；首帧和编号非连续时重置历史；可传入 HUD-less 颜色。
- 当前最小配置为 FP32、SDR、全分辨率运动矢量、无抖动、有限常规深度。颜色输入/输出为 RGBA8 UNORM，深度为 R32 float，运动矢量为 RG32 float。
- 运动矢量方向为当前到前一真实帧，单位为显示像素，`motionVectorScale=(1,1)`。测试使用已知物体运动真值，不能把它称为已获取游戏人物运动矢量。

另外复用了之前上传数据中的 24 个相机矩阵：全部可被适配器解析。跨组近裁面存在约 0.8 与 7 游戏单位的差异，远裁面约为百万单位且对矩阵浮点误差敏感。因此这项检查只证明现有输入可解析；视图身份及准确裁面仍需与引擎参数交叉核对，不能把估计值当作已验证真值。

## SDK 主机端必要适配

上游缓存保持干净，构建产物在 `.research/fsr-vulkan`。构建脚本生成明确且受固定文本检查约束的主机代码覆盖层：

1. Linux `wchar_t` 为 4 字节，私有上下文超过 SDK 按 Windows 宽字符布局预留的公开容量；增加 Linux 试验的 opaque 容量，保留大小断言。不是 Windows ABI 修改。
2. 官方 Vulkan 内存选择排除所有可见的 device-local heap，使仅有统一内存的软件设备无法创建资源。检查全部请求的内存标记，保持不可见本地显存优先，并增加可见本地 heap 回退。
3. SDK 使用 `vkGetBufferMemoryRequirements2KHR` 名称；在 Vulkan 1.2 设备上缺少 KHR 别名时解析到等价的 1.1 核心入口。

另外约束 SDK 能力回调选择实际构建的 FP32、不强制 wave64 配置；生成的 shader blob 表只接受该配置。Linux 的 SDK 断言报告会终止进程，未禁用断言或 Vulkan 校验。

## 复现

前提为仓库现有 Debian 云环境工具链。准备脚本会保留有改动的依赖并报错，不能强行覆盖。

```bash
bash scripts/setup_research_tools.sh
python3 scripts/prepare_research.py --only fidelityfx vulkan_headers
git -C .research/sources/fidelityfx sparse-checkout add sdk/src/backends/shared
bash scripts/setup_software_runtime.sh
python3 scripts/build_fsr_vulkan.py
python3 scripts/run_fsr_vulkan.py

c++ -std=c++17 -O2 -Wall -Wextra -Werror -Iintegration/ldb \
  tests/fsr_camera_cases.cpp -o .research/fsr-vulkan/fsr_camera_cases
.research/fsr-vulkan/fsr_camera_cases
python3 -m unittest discover -s tests -v
```

`sparse-checkout add` 用于补齐旧轮次留下的稀疏缓存，新缓存执行也是幂等的；先运行准备脚本确认锁定版本与工作树干净，再扩展所需目录。

相机单测包含 12 个正向、8 个拒绝用例。`--stdin` 模式可接收每行 16 个数的历史矩阵，输出估算相机参数。完整 GPU 图片、日志和报告保存在本次唯一运行目录；可携带结果见 [验证报告](../../research/fsr-component-backend-validation.json)。

## 游戏接入仍需完成

将 Host 颜色/深度格式、相机姿态、HUD-less 颜色及运动矢量按同一真实帧和 Reset 代次关联；为 DXVK 逻辑设备启用所需 image features；完成 GPU 输入转换与资源同步；在 B580 上构建运行并验证窗口、Reset、原帧回退及呈现时序。动态人物、武器和粒子的运动输入需要独立处理。

直接调度组件避免了 SDK proxy 对多个专用队列和交换链替换的要求，代价是本项目必须负责生成帧调度和显示节奏。独立测试的 CPU 上传、图像回读与逐帧等待仅用于验证，不作为游戏路径或性能结论。
