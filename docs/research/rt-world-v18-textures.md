# v18：模型基础贴图、alpha与照明对照

[v17实机](rt-world-014959-result.md)首次完成全部600 Present、六组材质与恢复段。人物仍亮白，56次模型提交因单帧新几何预算遗漏，结果目录停在等待退出的归档前检查点。v18把下一轮纹理、照明、材质切换压力和ZIP可靠性合并在一次F9中。

## 原始基础色路径

固定Remix 0.6.4的MaterialInfo只接受贴图文件路径，没有原D3D9纹理注册入口。实现读取已通过模型VS/PS和主通道筛选后的sampler0静态2D纹理，使用只读LockRect复制第0级；DXT1/3/5和BGRA/BGRX8编码为DDS DX10，显式保留采样器sRGB语义及原D3D alpha测试函数、阈值、基础过滤与寻址方式。压缩块原样保留，不做有损重编码。独立执行固定运行库的GLI解析器发现，直接使用BGRX DXGI88/93会得到非对应Vulkan格式编号207/208；X8R8G8B8因此转换为RGB不变、alpha固定255的BGRA8（DXGI87/91），避免将GLI扩展格式直接传给Vulkan。读取锁通过RAII恢复，即使CPU分配抛异常也不能留住游戏纹理锁。

上限为128个纹理引用、64 MiB累计导出、每帧4次读取，单级纹理最多4096及16 MiB数据。缓存持有COM引用防止指针复用；键包含颜色空间，生成的DDS使用内容哈希命名。纹理属性和alpha/采样状态组成稳定材质身份。动态/渲染目标、未知格式、读取失败、预算不足明确记录合成材质回退，不将API接受路径写成GPU已加载贴图。结果DDS仅写入用户本次结果目录，不加入GitHub测试安装包。

当前仍只接基础色和D3D alpha测试，未复现完整像素shader的色调、透明裁剪、Toon、原始法线/光照、动画贴图和完整mip链。基础色来自sampler0，不把其他绑定贴图一律当成基础色。

## 材质更新和几何身份

核对固定运行库后确认，重复CreateMaterial会被忽略，不能把它当更新接口。Scene保留稳定材质句柄与身份，变换材质前要求旧材质的最后成功使用帧已被EVENT查询确认完成；未完成则记录预算遗漏，继续余下采集。完成后按DestroyMaterial → CreateMaterial重建同一身份的材质，再DrawInstance。几何缓存保留纹理、alpha和采样身份，去掉的是本轮统一切换的合成材质变体。

固定后端每次外部实例提交会重新按句柄查找MaterialData，因此同一几何可获得新的材质。源准备回执新增DDS解析、材质注册/删除和逐Draw材质查询的源码哈希合同。真实API失败仍关闭RT，不吞异常，也不销毁GPU尚在使用的资源。

## 一次600帧对照

30原光栅、120世界/HUD、180天空保持原顺序。240 Neko贴图、300合并模型贴图使用原3000头灯；之后将辐射请求降为30，360取相同中性材质的低照明图，420金属、480半透明、540自发光；随后恢复3000并于600取恢复图。自动曝光参数不变。两次低照明/恢复请求单独记录，不能把API返回0或不同相机的画面直接当成灯光效果证明。

239/240、299/300的相邻几何和原始输入审计继续保留。增加DDS哈希/格式/颜色空间、材质绑定、贴图成功提交/灰色回退、导出字节、持有纹理引用和材质重建计数。分析器独立校验，summary分开报告几何预算与贴图覆盖，GPU材质验收仍标为未运行。Trace version15、coverage revision18，snapshot wire v2保持不变，保留旧数据兼容性。

## 结果归档

捕获和活进程探针完成后，先保存标为INCOMPLETE的恢复ZIP，再等待游戏/Host退出，后处理结束后更新同一份ZIP。ZIP与结果文件夹并列，控制台明确提示退出游戏、保持窗口开启和最终上传路径。写入临时文件后原子替换，失败时保留已有有效ZIP；最终COMPLETE与图像、覆盖、完整退出和性能验收仍是不同结论。

本轮不修改Source线程CVar或物理引擎，不声称解决已知的后端退出对象清理警告。实际B580贴图加载、亮白改善、完整材质外观和本轮完整游戏流程仍待新包实机验证。

## 交付验证

修正版源码 `d6d9428f879f045678d4b77863cd782b0b560a37`，编译身份 `ldb-rt-observer+973d8b35492f9f0d`。Windows首轮在新增测试的alpha类型转换警告C4244处停止；修正测试类型及X8格式映射后，x86/x64配套Release、系统NULLREF、材质完成标记/销毁重建/纹理身份、DDS行距/sRGB/alpha/异常恢复及既有原生检查全部通过。保持原有/W4 /WX。固定0.6.4运行库初始化和内嵌Python入口、LZMA导入也通过。

本地117项检查通过115项、跳过2项Windows专用检查；四个关键C++目标严格MinGW交叉编译通过。合成600帧的Draw/世界/场景/纹理分析联检通过。[固定GLI解析器的10组格式检查](../../research/rt-world-v18-dds-loader-check.json)验证DXT1/3/5、BGRA8、转换后X8在linear/sRGB下均映射到预期Vulkan格式；此项为CPU文件解码，不是GPU验收。

[实际目录归档复核](../../research/rt-world-v18-archive-check.json)对用户上一轮本地结果目录重新打包：除summary外的3,703个文件逐项哈希匹配，CRC通过，无遗漏；保留INCOMPLETE退出等待状态，不改写原始上传摘要。ZIP 24,749,950字节，生成及完整复核约145秒，因此大包压缩期间需要保持窗口开启。

[独立发布复核](../../research/rt-world-pilot-v18-validation.json)通过ZIP CRC、全部清单成员、两端PE架构/编译身份、Release参数、原生fixture重分析、固定API初始化、源码回执与当前输入哈希、HTTP下载与Git blob一致性检查；下载分支仍仅包含ZIP。

[固定版本文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/46ad79be1a3a5723a9c959ecf99def6417c197c7/downloads/rt-world-pilot-dev.zip)。测试安装包11,980,397字节，SHA256 `199ce71a1bbd0041cf26915cdf7dc9cf90460ce222a06edfca4dda992b775440`。实际v18 B580画面、纹理覆盖、亮白改善与完整退出仍未验收。

## 124445实机结果与纹理哈希修正版

[124445结果](rt-world-124445-result.md)在180 Present后首个NekoToon模型阶段触发通用C++异常，未导出DDS或成功提交API模型。发现允许16 MiB基础级的DDS导出误用4 MiB几何哈希上限；修正版使用16 MiB+148字节的独立DDS边界，加入完整尺寸哈希及系统NULLREF真实导出检查，并保留具体异常上下文。旧日志未记录异常文字和目标纹理尺寸，因此不将此缺陷断言为唯一根因。

修正版源码 `985ed8830b4991c2e18a798e00f5925cc65b4e5d`、编译身份 `ldb-rt-observer+e77327d1980ca3d9`，Windows原生和发布复核通过。[使用修正版文件页面](https://github.com/yeyunyyds/L4D2_Dxvk_General_FrameWork/blob/ad457c4a4597fb413be82185b5704587c0ae9d35/downloads/rt-world-pilot-dev.zip)，仍一次F9合并测试；B580贴图和低照明外观尚未验收。
