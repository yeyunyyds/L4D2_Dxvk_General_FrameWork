# v24：原生TAA对照和GPU供给诊断

220654测试已获用户确认所有部件显示，剩余描述明确为人物边缘小方块。本次合并原生TAA对照、原生像素证据、Host分段耗时、Windows进程GPU Engine采样和SHA256提供器复用，保留一次F9、一个最终ZIP。性能诊断不以降低显示覆盖换取指标。

## 边缘画质

600 Present原诊断不变，仍trace20/coverage23；交付版本24以单独pilot_delivery_revision记录。随后的60秒中性材质预览前30秒无重建，后30秒请求rtx.upscalerType=3（TAAU），明确resolutionScale=1.0。固定运行库None模式不缩放；切换TAA不能沿用默认0.75。队列请求成功不证明TAA已实际改善图像，动画对象逐姿态新Mesh的运动历史/拖影尤其需验收。

两个时点以背缓冲同尺寸目标调用FINAL_COLOR复制，不缩放，保存BGRA8。单份不超过16MiB，超出时保存中心1024×768裁剪。尺寸超过4096×2160或不支持时独立记录证据不可用，保留主要诊断结果。原640×400诊断和三份预览回读保留。动态场景先后两段不是同相机/同姿态的严格A/B；原生图像仍须人工检查。

## GPU利用率低：已知工作和未知瓶颈

代码可确认当前模型需同步只读源VB/IB、CPU执行位置/蒙皮重建、哈希、构造API几何并对新姿态调用CreateMesh；贴图首次导出需同步读取全部提供的mip及写DDS；Infected有有界CPU底色烘焙。GPU负责后端RT，但这些工作可能让GPU收到命令较晚。当前没有实际分项时间，不能由低GPU占用直接确定CPU蒙皮、IO、RPC等待或GPU渲染中的唯一瓶颈。

新增QPC窗口记录metadata、draw、static、model、prepare、source_geometry、skin、texture、texture_read、texture_persist、infected_bake、hash、geometry_build、material、mesh_create、instance、retire、original_draw、readback、present。作用域含调用内部等待、部分相互嵌套；比如prepare包含source_geometry/skin，draw包含model。不能相加当总CPU时间，未计入的帧间隙也不能直接定为RPC等待。窗口内诊断回读开销可单独辨认。开始窗口含启动/等待，预览是更有参考价值的已加载区间。

另用Python独立后台线程每秒采样Windows英文GPU Engine PDH计数器，限定游戏与Host PID；最多960行，每行128引擎，单次数组4MiB上限。统计可能涉及3D、Compute、Copy等引擎，同一GPU多引擎可并行；不直接相加。计数器不可用不挡主要采集，记录明确status，不把缺失记录当零GPU使用。其时间戳用于与Host时间窗关联；这不是逐Vulkan pass GPU计时。

固定运行库已有dxvk_gpu_idle_time_ms指标，但本次metrics.txt为空，不能据此虚构运行期idle比例。固定API已有remixapi_MeshInfoSkinning和remixapi_InstanceInfoBoneTransformsEXT，可作为持久几何加逐帧骨骼实例的接入口；还需核对蒙皮/形变、法线和运动历史，不能直接视为可无条件替换。未来若metadata/draw耗时占主导，再选择GPU蒙皮、持久几何/骨骼实例和减少复制；若source reads/persist占主导，则优先原纹理复用和异步准备；若Present明显阻塞，应核对呈现限制/后端等待。所有选择以新包数据为依据。

## 已落实的一项优化与覆盖信息

bytesSha256此前每次都打开CNG SHA256提供器并查询对象尺寸。本版复用不可变算法提供器，每次调用仍拥有独立hash和内存对象；不缓存可变资源结果，也不移除文件完整性保护。已知向量、并发独立状态及现有大DDS独立哈希门禁验证输出。它移除了确定重复初始化，实际游戏收益尚未测量。

源shader普查覆盖诊断及预览，最多128种、超额计数明确保留，帮助同一测试辨认未接入材质。普查不会新增水、粒子、折射或加法材质的渲染支持；特殊pass继续保持原审核边界，避免破坏本次已通过的部件显示。游戏全覆盖、画质和正常退出仍需各自验收。
