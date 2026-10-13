# 连续帧、shader 状态和矩阵候选诊断

这一版将边界、连续帧、shader 字节码、常量和离线矩阵分析合并采集。原 HDR 版本已在 B580 上取得非清屏场景深度；新版状态/连续帧仍需独立实测。它不自动认定相机矩阵，不执行插帧。

## 一次测试采集什么

每个观察到的 Draw 直接从实际 Host device 查询 VS/PS shader、float/int/bool 常量、FVF/顶点声明、旧式 World/View/Projection 变换、viewport、深度/混合状态。字节码和状态按原始字节精确去重，存成 `state-N.bin`；不保留额外 COM 引用。读取实际状态覆盖 StateBlock Apply 的恢复结果。旧式变换只表示 GetTransform 值，在 programmable shader 中可能完全不使用。

事件 schema=2，附带 Host CPU QPC 与频率、帧号、UID、generation。快照关联最近成功绘制的 sequence，不能把边界时当前 shader 自动视作整张图的唯一生产者。QPC 包含诊断停顿，不是游戏模拟时间、GPU 时间或生产性能。

默认连续 8 帧，最高 16 帧；每帧最多 8 个图像尝试。默认 stride=3，每个输出像素取原图坐标 `min(index*stride+floor(stride/2), dimension-1)`，不是平均深度。1920×1200 输出为 640×400，事件 `color/depth` 保留原尺寸，`export` 记录输出尺寸和采样映射。GPU 回读仍是原尺寸。可设 stride=1 保存全尺寸，但总图像预算 256 MiB，可能无法保存所有帧；shader/state 文件另限 64 MiB。超过预算会明确跳过图像或停止诊断，不能把缺失数据当作完整验证。

## 推荐游戏测试设置

完全退出 Steam、游戏和 Host，备份并同批替换三个桥二进制，沿用 x64 Host 和原 DXVK/配置。新的包内 README 主入口是本文件。

在 CMD 设置，然后从同一 CMD 启动 Steam：

```bat
mkdir D:\LDB-captures
if exist D:\LDB-captures\capture.trigger del D:\LDB-captures\capture.trigger
set LDB_CAPTURE_DIR=D:\LDB-captures
set LDB_CAPTURE_TRIGGER=D:\LDB-captures\capture.trigger
set LDB_CAPTURE_START=0
set LDB_CAPTURE_FRAMES=8
set LDB_CAPTURE_SNAPSHOTS=8
set LDB_CAPTURE_STRIDE=3
start "" "C:\Program Files (x86)\Steam\steam.exe"
```

进入地图后，在另一个 CMD 执行 `type nul > D:\LDB-captures\capture.trigger`。同一进程只采集一个区间。每次重测完整退出、删除触发文件并重新启动。

用同一个包做三组：静止观察、缓慢转动视角、缓慢平移。为了降低诊断等待对动作的干扰，转头/移动时保持连续操作。另做一次切屏/分辨率改变：采集跨度中若确实记录 Reset，分析工具应拒绝跨 generation 的重投影；若没有 Reset 事件，不能宣称覆盖了该情况。每组保留整个独立采集目录；需要 Bridge/DXVK 错误诊断时同时保留对应日志。

## 离线分析

只需要 Python 3，无额外 Python 包：

```bat
python scripts\analyze_ldb_capture.py "D:\LDB-captures\你的目录"
python scripts\analyze_capture_series.py "D:\LDB-captures\你的目录" --output "D:\LDB-captures\inventory.json"
```

第一项检查事件/图像/常量文件长度、时间顺序、帧/Reset 关联及深度内容。第二项解析 SM2/SM3 的 shader 常量读取，排除 DEF 覆盖的寄存器，列出有限可逆的 4×4 寄存器块及跨帧重复情况。候选可能是物体变换、骨骼或相机矩阵；相对寻址和未支持 shader 模型会标出限制。不能只凭可逆或重复，就选作 view-projection。

## 显式矩阵假设的重投影

只有结合 shader 使用方式及游戏数据挑选矩阵之后才使用此工具。mapping.json 示意如下，SHA/寄存器必须替换为实际数据：

```json
{
  "shader_sha256": "实际VS字节码SHA256",
  "register_start": 8,
  "storage": "register_rows",
  "pixel_center_offset": 0.0,
  "evidence": "说明从shader/常量变化得到此假设的依据"
}
```

寄存器 8 只是示例，不是 L4D2 已确认位置。`storage` 可选 `register_rows` 或 `transposed_register_rows`；D3D9 通常使用整数像素中心，若矩阵已含半像素修正需显式验证 0.5。工具使用深度 0..1 和记录的 viewport 范围，不知道引擎 jitter、模型空间或相机语义。

```bat
python scripts\reproject_capture.py "D:\LDB-captures\你的目录" --previous 0-0-0 --current 1-0-0 --mapping mapping.json --output reprojection
```

输出稀疏运动 CSV、深度一致性 PPM 和结果 JSON。运动单位为原渲染表面像素，方向为当前减上一帧；重投影越界、W 无效等标为无效。跨 Reset 拒绝运行。深度一致性绿色表示在 0.001 的设备深度容差内，不是正确相机或正确运动的充分证据。结果始终标为 `diagnostic_matrix_hypothesis`，不会冒充已验证游戏运动矢量。

[GitHub 下载文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/zip-host-capture/downloads/ldb-host-capture-dev.zip)
