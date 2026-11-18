# 第三方源码与参考项目

本页区分“当前帧生成构建的直接输入”和“光追/架构研究参考”。固定提交以仓库内的 `integration/*/*.json` 与 `research/sources.lock.json` 为最终机器可读依据。

## 当前帧生成路径的直接输入

| 项目 | 固定版本/提交 | 本项目中的用途 |
| --- | --- | --- |
| [L4D2_Dxvk_32to64_Bridge](https://github.com/yeyunyyds/L4D2_Dxvk_32to64_Bridge) | v1.3.4 / `78f8c17aa8eb6c752a6d790e166c9ba55ed4a91e` | LDB Client、Host32/64 基线；GF Host hooks 在此版本上重放 |
| [NVIDIA dxvk-remix](https://github.com/NVIDIAGameWorks/dxvk-remix) | `9aa74f8dfad2188efbd0f717c64d9f8fa909787e` | LDB 使用的 Bridge/public 上游源码基线 |
| [DXVK](https://github.com/doitsujin/dxvk) | v2.6.1 / `2b1a284f8453baa2bd193709b67e5183074c74ba` | 未修改的 x64 D3D9 后端与标准 Vulkan interop |
| [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) | v1.1.4 / `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55` | Optical Flow、Frame Interpolation 与 Vulkan backend |
| [L4D2 Asteria Graphics Enhancement](https://github.com/Anokianx/L4D2-Asteria-Graphics-Enhancement) | `5c78e76b08171cfa1e797e4eece0b4a42bf9018b` | Vulkan replacement-swapchain 路线；以 MIT 源码为基础移植为 x64 Layer，并加入 Host V2 输入 ABI |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `234c4b7370a8ea3239a214c9e871e4b17c89f4ab` | 与固定 DXVK 匹配的 Vulkan 头文件 |
| [Microsoft Detours](https://github.com/microsoft/Detours) | `ea6c4ae7f3f1b1772b8a7cda4199230b932f5a50` | LDB/实验 Hook 构建依赖 |

当前 Windows 包从官方 DXVK v2.6.1 Release 归档取得 `d3d9.dll`，不交付 GF 私有 DXVK fork。Asteria 源码通过 [`integration/fg/asteria-x64.patch`](integration/fg/asteria-x64.patch) 和 [`integration/fg/async-hud-x64.patch`](integration/fg/async-hud-x64.patch) 转换为本项目使用的 x64 Layer。

## 已实际用于设计或验证的参考项目

| 项目 | 固定版本/状态 | 参考范围 |
| --- | --- | --- |
| [L4D2 Danyria Integration Tool](https://github.com/Anokianx/L4D2-Danyria-Integration-Tool) | 当时公开仓库与用户提供二进制的研究记录 | D3D9 语义采集、Asteria/FSR3 接入路线。未把闭源二进制或反编译产物提交到本仓库 |
| [l4d2-rtx](https://github.com/xoxor4d/l4d2-rtx) | `1fc8ded63a8491e9c1972eb2940d6cebbc20ad9e` | Source Draw、模型、材质、Remix 回调与 L4D2 兼容行为 |
| [xoxor4d/dxvk-remix](https://github.com/xoxor4d/dxvk-remix) | `game/l4d2_rebase1` / `b73eb23053aa50d3787b0c6c8b4b8ae49962880e` | L4D2 专用 Remix runtime 和扩展 render-state 研究；不属于当前 FG 运行路径 |
| [eblenix_csgo_public](https://github.com/xastrix/eblenix_csgo_public) | `54a04b5f3873e35a68d7f99d2656c54251fb098d` | 早期 D3D9/Source Hook 与注入点研究 |
| [HL2SDK](https://github.com/alliedmodders/hl2sdk) | `2a31cd007b2d7d2f964dc093eedcf7a812cf9dd6` | Source 材质系统、call queue 与引用生命周期接口核对 |
| [Vulkan-Loader](https://github.com/KhronosGroup/Vulkan-Loader) | RT 诊断包固定 v1.4.313 | 独立 RT 运行时诊断/打包，不属于当前 FG 算法本身 |

## 许可与来源规则

- 每个项目仍受其上游许可证约束；请在对应仓库查看完整许可证和再分发条件。
- 准备脚本按提交哈希取源，构建包将实际随包组件的许可证放入 `licenses/`。
- `.research/` 是本地构建与源码缓存，不作为本仓库提交内容。
- 研究参考不等于代码复制。未进入构建输入的项目不会被描述为运行时依赖。
