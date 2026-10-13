# LDB v1.3.1：可显示生成帧的相机插帧原型

这一包合并了全尺寸 GPU 历史、场景/武器/UI 阶段处理、中间帧计算和真实 Present 接入。不是上一版只输出重投影图片的诊断。它保存真实渲染帧的完整颜色和场景深度，在两帧之间建立相机姿态，在 GPU 上合成中间视角，再写入 DXVK 的真实背缓冲。

## 一次集中测试

1. 完全退出游戏、Host 和 Steam。备份原三个桥文件，按原位置同批替换 `bin/dxvk_d3d9.dll`、`bin/.l4d2bridge/L4D2Bridge64.exe` 和 `L4D2Bridge32.exe`。使用 x64 Host，沿用原 DXVK 2.6.1、配置和地图。此包是基于官方 v1.3.1 源码编译的实验版本。
2. 将 ZIP 解压到可写目录，运行根目录的 `start_fg_test.cmd`。它设置环境并直接启动 Steam/L4D2；如果 Steam 仍在运行，会提示先退出。无需 capture.trigger，也无需重做三轮采集。
3. 进地图后正常玩，缓慢转头、平移、观察武器和 HUD，并开关菜单。**F8** 启停插帧；**F9** 在双 Present 与“仅显示生成帧”预览间切换。预览能直接看中间帧的内容，恢复双 Present 再比较。F9 切换时会再保存几张图片，全进程最多保存 16 张生成结果。
4. 可在同一次游戏中切屏/改变分辨率检查恢复。结束后上传解压目录下完整 `fg-results` 文件夹，遇到启动/显示错误同时带上 Bridge/DXVK 日志。

F8/F9 仅在游戏窗口取得焦点时响应。如果按键未响应，可通过下方变量选择模式后完全退出再启动。菜单、加载画面、尚未取得场景矩阵、相机突变或不支持的 Present 会直接使用原帧。

## 输出与实际行为

- `fg-events.jsonl` 记录 GPU 初始化、场景/近景/UI 边界、历史失效、生成 Present、原游戏 Present、错误和总数。
- `*-generated.ppm`、`*-current.ppm`、`*-previous.ppm` 是少量检查图片；保存时降采样，**实际历史和合成是全尺寸**。`*-mask.ppm`：绿=两帧匹配并混合，青=使用当前帧重投影，白=天空旋转，黄=保留当前武器/UI，红=当前画面回退。边缘遮挡会产生当前帧回退区域，不输出黑洞。
- 默认 `paired` 会先提交中间帧，再恢复并提交原真实帧。桥协议、游戏模拟帧和原来的响应/信号量仍按一个游戏帧处理。
- `generated` 预览每游戏帧只提交一次，显示中间帧；它用于直接验收生成内容，**不是帧率翻倍模式**。
- Reset、目标/格式/尺寸变化、失败 Present、缺失场景和 F8 切换会使历史失效或预热。开启失败时记录 `fg_error`，停止进一步生成并沿用原帧。

历史使用自身的 Vulkan 图像，不长期持有游戏表面的 COM 引用，不改变 D3D 绘制状态。生成计算没有 CPU 像素回读/上传；只有少量图片取证会回读。当前实现仍用 GPU fence 同步等待，并额外调用 Present，尚未完成生产帧节奏/异步调度。因此 Present 次数增加不能直接当作实际显示帧率或性能提升，垂直同步和驱动也可能影响交付。

## 分层范围

场景通过同目标 cViewProj、全尺寸 HDR 与深度写入识别；武器依据近景深度范围建立保留掩码；UI 依据背缓冲上的屏幕 alpha 绘制边界及绘制前后差异保留。生成帧中的武器/UI使用当前真实帧像素，避免相机重投影把它们拖动。

这是阶段和掩码处理，**不是引擎提供的三张独立透明 RGBA 图层**。UI 边界可能包含后处理，透明边缘、动态角色、粒子和光照变化仍可能有接缝或拖影。当前没有骨骼/物体运动矢量或光流，颜色/深度不一致时优先使用当前帧；这些限制不应被理解为已验证完整动态场景插帧。

## 手动启动（可选）

必须从同一 CMD 启动已经完全退出的 Steam：

```bat
set LDB_FG_ENABLE=1
set LDB_FG_MODE=paired
set LDB_FG_LOG_DIR=D:\LDB-FG-results
set LDB_FG_CAPTURE_FRAMES=4
set LDB_FG_HOTKEYS=1
set LDB_CAPTURE_DIR=
set LDB_CAPTURE_TRIGGER=
set LDB_GPU_REPROJECT=0
start "" "C:\Program Files (x86)\Steam\steam.exe" -applaunch 550
```

`LDB_FG_ENABLE=0` 完全关闭新路径；`LDB_FG_MODE=generated` 直接进入生成帧预览；`LDB_FG_CAPTURE_FRAMES=0` 关闭图片回读，仅留日志。

[GitHub 文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/zip-host-capture/downloads/ldb-host-capture-dev.zip)
