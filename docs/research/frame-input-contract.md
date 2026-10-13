# 第一阶段帧输入契约（设计草案）

本页定义后续捕获与静态场景原型需要的数据，不代表 Bridge 已实现这些命令，也不代表游戏已输出运动矢量。

## 帧与资源身份

`renderFrameId` 是单个主视图真实渲染帧序号，每帧严格递增 1；生成帧不递增它。设备、swapchain 或视图重建增加独立 `generation`。输入尺寸变化、相机切换、跳号、暂停恢复、换图或无效矩阵均使历史失效。

维护独立的 `presentFrameId` / 生成帧计数，不能拿 FG 显示数量替代真实渲染帧数量。FSR `frameID` 应由有效真实输入序列驱动；不能直接假定 DXVK 的 WSI frameId 与 Source 主视图一一对应。

资源引用包含 owner device、generation、格式、尺寸、samples、mip/layer/aspect、默认 layout 和完成/复用同步点。不能仅保留裸 VkImage，并假设下一帧仍有效。异步 FG 引用应保持到相应 GPU 工作结束；Reset 前排空，释放 COM 与 GPU 引用，再重建。

## 最小输入

| 数据 | 产生位置 | 第一版处理 |
|---|---|---|
| 最终颜色 | Host 实际 D3D9 backbuffer / DXVK PresentImage 的源图像 | 固定 SDR、固定尺寸；明确 UNORM/sRGB 与 transfer function |
| HUD-less 颜色 | 游戏 HUD 绘制前的主场景快照 | 候选边界需要实机确认；EndScene/Present 不天然等于 HUD 开始 |
| 世界深度 | 主视图有效深度，在 clear、复用或不同投影视图覆盖前 | 第一版关闭 MSAA；深度 clear 值/反转、near/far 和单位统一 |
| 当前/历史相机 | x86 游戏侧主视图接入 | 当前/前一帧 view/projection、相机位置/基向量、垂直 FOV、near/far |
| 当前→上一帧运动 | Host GPU 计算或后续对象历史重放 | 静态世界先用深度重投影；动态物体明确不保证正确 |
| 帧时间 | 同一真实帧序列的采样时钟 | FSR `frameTimeDelta` 单位为毫秒，不能混用秒或计时 tick |
| 历史标志 | 输入完整性与生命周期管理 | 不完整输入回退原生，不以全零/旧运动数据冒充有效输入 |

纯光栅第一版不引入 TAA jitter，jitterOffset=(0,0)。后续引入抖动时重新验证相机矩阵和运动矢量约定。FSR 的 prepare / optical flow / interpolation 路径可以独立于超分运行。

## 静态世界重投影

统一使用**像素坐标**定义运动方向，当前像素为 p，当前主视图深度为 z：

1. 用像素中心 `(x+0.5, y+0.5)` 构造 clip 坐标。明确图像 Y 向下、矩阵乘法方向与深度范围 [0,1]；Source/D3D 与 Vulkan 约定只在入口转换一次。
2. 使用当前 ViewProjection 的逆矩阵重建齐次世界坐标，并除以 w。
3. 使用上一帧 ViewProjection 投影到上一帧像素位置 p_prev。
4. 写入 `motionPixels = p_prev - p`，格式建议 `R16G16_SFLOAT`。若改为 UV 单位，配套 motionVectorScale 要改变，不能重复乘分辨率。
5. 对 clear-depth、非有限数、w 接近零/投影到相机后方建立无效标记并按后端契约处理。遮挡区域交给 prepare/disocclusion 处理，不能随意把它们解释为静止。

必要可视化：相机静止时静态几何运动接近零；纯平移/旋转方向正确；边缘与远近深度一致；前后相机帧未错位。再与已知场景的解析投影作对照。

该方法只计算相机运动。移动角色、骨骼/布料、开门、粒子、透明物体与第一人称武器需要另外的历史数据或经验证的近似策略。不能把人物模型的线速度直接当作逐像素运动矢量。

## IPC 与执行顺序

元数据必须与对应主视图渲染命令、快照边界、Present 在同一可证明的有序序列上关联。当前 Bridge 的 Module 与 Device 队列没有统一全局顺序，单独低频消息通道也不提供 GPU 资源同步。

建议新增版本化的 Device 命令，表达主视图开始/相机信息、场景快照边界与历史失效。IPC 只传值和已有资源 ID，不跨进程传 COM 指针/VkDevice/VkImage。Vulkan 图像留在 x64 Host；在 Host 将资源 ID 解析为真实对象并查询 DXVK interop。

插帧失败、菜单/loading、多视图不支持或任一输入失配时，本帧走原生呈现并重置历史。生成帧只处理图像与显示，不再次执行游戏逻辑。
