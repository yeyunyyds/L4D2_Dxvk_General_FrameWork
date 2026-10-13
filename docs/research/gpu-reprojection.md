# LDB v1.3.1：GPU 相机重投影诊断

同批替换 ZIP 内三个二进制：`bin/dxvk_d3d9.dll` 和 `bin/.l4d2bridge/L4D2Bridge64.exe`、`L4D2Bridge32.exe`。安装位置与上一版相同；先完全退出 Steam、游戏和 Host，并备份原文件。当前测试使用 x64 Host，沿用原 DXVK 2.6.1 与游戏配置。基线固定于官方 v1.3.1 源码 `9e3ee1292f901b0bb9352e36bba93a4cd5cfbda4`，不是官方发布二进制。

本版在 Host 实时识别 shader CTAB 中名为 `cViewProj` 的 4×4 浮点常量，只接受全尺寸 HDR 目标、深度写入和 0..1 viewport 的场景绘制。同一目标的已识别矩阵若不一致，会跳过该帧；屏幕 shader、武器的 0..0.1 viewport 和未命名矩阵不自动认定为相机。

输出上一帧重投影到当前帧的预览、运动和深度一致性。不会修改游戏画面，不生成中间帧。GPU Vulkan compute 使用已回读的 stride=3 样本重新上传，历史保存在 CPU；这是检查 GPU 算法和 Host 状态选择的同步诊断，尚未实现全尺寸 GPU 常驻历史，也不能用于评估生产性能。

## 一次合并测试

在 CMD 中执行（路径可换，但必须是绝对路径）：

```bat
mkdir D:\LDB-captures
if exist D:\LDB-captures\capture.trigger del D:\LDB-captures\capture.trigger
set LDB_CAPTURE_DIR=D:\LDB-captures
set LDB_CAPTURE_TRIGGER=D:\LDB-captures\capture.trigger
set LDB_CAPTURE_START=0
set LDB_CAPTURE_FRAMES=8
set LDB_CAPTURE_SNAPSHOTS=8
set LDB_CAPTURE_STRIDE=3
set LDB_GPU_REPROJECT=1
start "" "C:\Program Files (x86)\Steam\steam.exe"
```

进入地图后，在上述设置过变量的 CMD 执行包内 `trigger_capture.cmd` 的完整路径。脚本等待 5 秒再创建触发文件；这 5 秒内切回游戏，开始并保持动作。也可沿用手动 `type nul > D:\LDB-captures\capture.trigger`。同一进程只采一组，下一组先完全退出、删除触发文件、重启。

用同一个包采静止、慢转头、慢平移三组。额外切屏/调整分辨率可检查 Reset，但只有事件确实记录 Reset 才算覆盖。每组保留整个采集目录，连同 Bridge/DXVK 日志压缩上传。不需要自己运行分析工具，也不需要上传触发文件。

## 新文件怎么看

- 原 `*-color.ppm`、`*-color.rgba16f`、`*-depth.f32` 等仍保存。
- `*-gpu-warp.ppm`：上一帧的场景颜色重投影到当前帧，HDR 显示仍用 Reinhard；无效位置为黑。
- `*-gpu-valid.ppm`：绿=设备深度差≤0.0001，红=深度不一致，蓝=越界、前帧天空/近景或无效 W；黑=当前天空或 depth≤0.2。
- `*-gpu.f32`：每像素 8 个 little-endian float32，顺序 `warped RGBA, dx, dy, depth_error, status`。运动是当前减前帧位置，单位为原 1920×1200 等渲染尺寸的像素，不是导出像素。
- `events.jsonl`：增加 `gpu_history`、`gpu_reprojection`、`gpu_reprojection_skipped`。明确记录真实矩阵来源 draw sequence、shader 状态文件、寄存器、矩阵、前后快照与状态计数。

第一帧、成功 Reset、目标/尺寸/格式变化、失败 Present、缺失场景帧都会使历史失效或重新预热，缺少一张 GPU 输出通常可由日志解释。第一张场景快照才尝试 GPU 对齐，每帧最多一次；武器、HUD 阶段继续观察，但不参与此相机重投影。天空和动态物体仍需后续方案，绿色也不是所有运动正确的证明。

采集最多 16 帧；原图像预算 256 MiB、状态 64 MiB；GPU 输出另限 128 MiB、每帧最多 1024×1024 个样本。超过预算会明确跳过。`LDB_GPU_REPROJECT=0` 可仅做原连续帧采集；不开 `LDB_CAPTURE_DIR` 则整个诊断关闭。

## 可选离线检查

只需要 Python 3：

```bat
python scripts\analyze_gpu_capture.py "D:\LDB-captures\采集目录"
```

它检查 GPU 文件/矩阵来源/前后帧，使用 CPU 高精度参考核对有效点的运动、深度差和原始 HDR 颜色。`no_gpu_pairs` 表示没有真正执行可比较的 GPU 帧对，不是验证通过。

[GitHub 文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/zip-host-capture/downloads/ldb-host-capture-dev.zip)
