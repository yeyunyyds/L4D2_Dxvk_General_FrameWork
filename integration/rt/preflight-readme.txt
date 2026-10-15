LDB 多线程 RT：接入前置检查

这是当前游戏接口证据 + 独立静态光追测试，不是综合包 A，也不会让游戏获得光追。

操作（Windows x64）：
1. 解压本 ZIP 到独立文件夹。打开 download-dependencies.html，用浏览器下载列出的三个依赖 ZIP，放在 run-test.cmd 旁边，不要解压它们；无需安装 Python。
2. 用你现在正常可用的 LDB 启动 L4D2，进入任意地图。
3. 双击 run-test.cmd。等待独立 RT 测试窗口结束及模块检查完成，上传 results 目录中新生成的一个 rt-preflight-*.zip。

不需要 F8、F9、capture.trigger 或转头/平移。测试窗口会临时获得焦点。
独立 RT 最多运行 180 秒；等待游戏模块最多 180 秒。下载时间另计。
失败也上传结果 ZIP；不要只发送“成功/失败”。结果包含实际路径、模块 SHA 和候选代码。

脚本只读取正在运行的游戏；所有 runtime、缓存和日志写在本解压目录。
它不安装游戏 Hook，不更改游戏目录、CVar、注册表或驱动，不关闭游戏。
若有多个 L4D2 进程，请只保留本次测试的一个。游戏与测试脚本使用相同权限运行。

之前用户与无凭据 Windows runner 均遇到依赖下载 HTTP 404，因此本次推荐先用浏览器下载依赖。已通过 SHA 校验的依赖会缓存，下一次无需重复下载。
本版三个大依赖使用已发布的固定提交地址，并在失败时尝试 GitHub 备用入口；小型 Vulkan loader 依赖随入口包提供。
控制台会显示下载字节进度、错误、游戏 PID 和核心模块情况；详细信息保存在结果包的 progress.log、game-discovery.json 中。
缺少材料/渲染模块时仍收集现有证据，不再仅因四个模块未齐而等待 180 秒。
如果自动下载仍失败，可用浏览器打开 download-dependencies.html 中的文件页面，下载三个大依赖 ZIP 放在 run-test.cmd 旁边，不要解压，然后重跑。脚本仍会校验 SHA 和大小。
脚本和依赖版本见 preflight-manifest.json；第三方声明见 licenses 和 python/LICENSE.txt。
通过仅表示独立场景命中和颜色读回；B580 身份、图像、游戏队列、多线程与性能仍需分别审查。
