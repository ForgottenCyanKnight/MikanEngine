# Enhanced ReSTIR PT

参考 Prime `e1917423d0c35f57742a2ca08e0f3258d0ff23c0`，算法来源为
Falcor 9.0 `759aad033ff610fb0d82c74f7e0a508d0096d5f2`。
Enhanced 是内置 ReSTIR 的唯一 GLSL/Vulkan 路径，NEE/MIS 仍为默认基线。
旧自实现 DI/GI、整路径 PSS 对照和 RTXDI SDK 分支已删除，包括入口、
reservoir 分配、描述符绑定、独立管线和着色器。保留独立 NEE 辐照度缓存、
NRD、DLSS 和 TAA；它们不属于旧 ReSTIR 实验。

## 已接入的核心

一条完整新路径的 RIS、Hybrid PSS/reconnection、双 footprint、实际 BSDF PDF
roughness 分类、混合路径度量、几何 Jacobian、双向 pairwise MIS、时间与三邻域
空间重采样、Compact prefix retrace、原始 reciprocal LUT 和 DupMap。
沿用 Mikan 求交、真实材质、漫反射/GGX VNDF/理想镜面、有限反弹预算、
发光三角形 power-distance tree、天空与 NRD，引擎 albedo 未修改。

这不是 Prime 全部功能逐项等价：未接入可选 RR 输出去相关、解耦着色、随机
重投影、运行时可变阈值/多轮配置。场景变化保守失效整份历史，没有局部
identity/suffix refresh；没有 area reservoir、DoF、splatting、MCMC、robust
temporal、介质或透射。当前验证范围是本引擎不透明表面传输。

## 数学和资源合同

`vox_rt_enhanced.glsl`：TEA16/Morton/TinyUniform、材质响应、正反向光源 PDF。
无辐照度的太阳不占 NEE proposal；天空与太阳 terminal RIS 域分开。
光源采样固定消耗五个随机数，prefix replay 可跳过阴影查询而保留随机图。

`vox_rt_enhanced_path.glsl`：路径生成、RC 选择、prefix replay。
距离阈值为 primary footprint 乘 `0.0002 * (1 + 0.2 * Gaussian)`，
rough 使用实际 PDF ≤25。两个相邻面积 footprint 必须满足原判定。
reservoir 保存混合度量 integrand，初始 RIS 使用物理路径贡献，sum/target
归一化；不是简单把 source/destination BSDF PDF 比率当 Jacobian。

`vox_rt_enhanced_shift.glsl`：重算相邻 BSDF、连接可见性、接收点相关 light PDF，
几何 Jacobian 为 `G_destination/G_source`。正反 shift 各验证源 chart 的可逆
支撑，再进行双向 pairwise MIS。缓存 suffix 仅在已接受的静态场景有效；场景、
材质和光照变化使用已有全局失效条件。原地更新外部天空纹理但没有内容版本
通知仍是已有接口限制。

`vox_rt_enhanced_dispatch.glsl`：工作筛选、Compact 队列、LUT 和合并。
64线程工作组前缀和、每组一次原子预留，无需 subgroup arithmetic；直接 RC
不做 prefix ray query。二维间接 dispatch 避免1080p三候选队列越过 Vulkan
最低65535组的单维限制。互反候选共享反向 shift，避免重复追踪。

| 阶段 | 输入/输出 |
| --- | --- |
| DupMap | 前帧17×17相同 seed 计数 |
| generate | fresh reservoir、当前 primary identity |
| temporal workload/retrace | 双向 prefix 工作筛选、间接追踪 |
| temporal merge | 双向 shift、MIS、重复度历史 cap |
| spatial workload/retrace | 三互反邻域筛选、必要 prefix 追踪 |
| spatial shift/merge | 每有向候选一次 shift、互反 MIS 合并 |
| resolve | 去调制 diffuse/specular 与当前 receiver 的 NRD guide |

每 view：四份160B/像素 reservoir、三份128B/像素 work、12B/像素队列加16B头、
44B/像素辅助（DupMap、两份精确 primary identity）。合计1080B/像素，不含
原有 RT/NRD：640×360约237.3MiB、720p约949.2MiB、1080p约2135.7MiB。
这是未压缩移植。单个 buffer 超过设备 storage range 时回退 NEE。
实际 SPIR-V 检查160B reservoir、128B work。

关闭时仅初始占位；首次使用才完整分配，之后保留至尺寸重建/清理。升级共享
缓冲前等待已提交 GPU 工作，稳态没有该等待。compute/indirect 之间有 barrier，
空间读写独立银行。Enhanced 分开编译 SPV 并逐阶段特化，NEE 无其大结构 ABI。

## 控制与验证

控制面板只提供 NEE/MIS、ReSTIR Enhanced 两项。启动参数：

```powershell
$env:MIKAN_HWRT_RESTIR_PT='1'
$env:MIKAN_HWRT_RESTIR_TEMPORAL='1'         # 0 关闭时间复用
$env:MIKAN_HWRT_RESTIR_PT_SPATIAL='1'       # 0 关闭空间复用
$env:MIKAN_HWRT_RESTIR_PT_DUPLICATE_CAP='1' # 0 固定 cap20
```

默认一条新路径、三邻居、一轮空间、历史 cap20。DupMap 指数0.1把 cap 从20
向1降低；M 不等于实际独立样本数。旧 `MIKAN_HWRT_RESTIR`、`RESTIR_GI`、
`UNBIASED_RESTIR`、`RTXDI`、`RESTIR_PT_PROFILE` 不再选择其他路径。
使用 `tools/run_rt_enhanced.ps1` 可启动 Enhanced + NRD/TAA。

`tools/test_restir_enhanced_math.py` 七项独立 oracle 涵盖非单位 Jacobian、漏项
反例、相邻 PDF 消去、完整 LUT 互反、PDF/footprint、队列覆盖、重复 cap。
Python oracle 不执行 GPU shader，不能证明生产无偏。

2026-10-11 夜间真实 GPU 证据在 `out/enhanced_port_20261011/REPORT.md`。
线性 BIN 用于均值/方差，PNG 仅预览；性能独立运行排除抓图读回和冷启动。
静态夜景不能替代运动、遮挡、动态光照和极端材质验收。

## 许可证

四个新 Enhanced GLSL 是 Prime-derived 适配，保留 GPL-3.0-only、additional
permissions 和完整 NVIDIA BSD-3-Clause 通知；原配对 LUT 由 u16 等值展开
为 u32。许可证原文在 `docs/licenses/prime-enhanced`。
参考：[Prime](https://github.com/bWFuanVzYWth/prime)。
