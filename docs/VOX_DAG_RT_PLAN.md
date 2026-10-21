# VOX DAG 与硬件三角形 BLAS：可行性与开发计划

日期：2026-10-20。基线：`e81ce7c` + 本地 Brick 改造。

## 1. 目标修订

明确约束：**仍由硬件追踪三角形；DAG 用于降低几何与加速结构的常驻内存。**

上一版“粗 AABB + Shader DAG 遍历 + 体素叶求交”不满足该约束，撤出本计划主线。现有 Brick 保留作独立实验，不作为本方案的前置依赖。本轮只修订分析和计划，不实现运行时代码，不宣称已有显存收益。

这里必须区分三种能力：

| 目标 | 当前标准 Vulkan KHR 路径可否做到 | 结论 |
|---|---|---|
| 自定义 BLAS 的内部节点为 DAG，由现有硬件完整遍历 | 没有应用层接口 | 不能将自定义节点写入 AS buffer；这需要驱动/硬件/专用扩展支持 |
| DAG 找重复几何，每个唯一子块一个普通三角形 BLAS，多处实例共享 | 可以 | 推荐首个原型；所有求交仍是硬件三角形 |
| 压缩磁盘 DAG，运行时完全展开后建普通 BLAS | 可以 | 只降低包体/部分加载成本，不能据此宣称降低最终 BLAS 常驻内存 |

**若“特化 BLAS”严格要求内部节点共享，当前应用层方案无法按字面实现。** 以下计划是可落地的替代：DAG 驱动的三角形 BLAS 去重/实例化，而非冒称驱动 BLAS 已变成 DAG。若不接受该替代，应先做驱动/厂商接口研究，再决定引擎方案，不继续实现软件体素求交。

## 2. API 边界与可选硬件方向

### 2.1 标准 Vulkan

加速结构格式是不透明、由实现决定的。应用提供三角形、构建参数和目标存储，不控制内部 BVH 节点布局或跨 BLAS 节点共享。

`VkAccelerationStructureBuildGeometryInfoKHR` 的约束规定：TLAS 使用实例几何；BLAS 不能包含 `VK_GEOMETRY_TYPE_INSTANCES_KHR`（VUID 03789/03791）。因此不能把任意深度的 DAG 用“BLAS 引用另一个 BLAS”递归提交。

标准路线需要把 DAG 的一个不重叠覆盖切面展开为：

```text
CPU DAG / 重复模式分析
             ↓ 选择块粒度与覆盖切面
唯一三角形子块 A、B、C → 各自普通、压缩后的 BLAS
             ↑
TLAS 实例：A@位置1、A@位置2、B@位置3、C@位置4……
             ↓
硬件 BVH 遍历和硬件 ray-triangle 求交
```

DAG 上层结构用于编译、选择与更新；GPU 不必常驻完整 CPU DAG，也不在 shader 内遍历 DAG。不能同时展开父节点与子节点而重复提交同一表面。

### 2.2 厂商压缩三角形格式

`VK_AMDX_dense_geometry_format` / DGF 是更接近“压缩几何仍由硬件使用”的方向：它提供从预压缩三角形数据构建 AS 的接口，而不是开放任意自定义 DAG 节点。它目前是 provisional 扩展，必须查询设备、驱动和 feature，不能因设备是 AMD 就默认可用，也不能因 SDK 有枚举就认为硬件支持。

AMD 官方把 DGF 描述为面向未来架构直接支持的硬件友好几何压缩。本计划尚未验证 Radeon 780M 对应扩展和原生实现的可用性；即使存在软件解码/兼容路径，也不能把它当作硬件原生压缩收益。DGF 与重复子块共享可以独立评估，不等于当前 VMDA。

把 DGF 列为能力探测/未来后端，不作为首个跨设备原型的依赖。厂商 cluster/多级实例扩展亦需单独证明支持与共享语义，不能将 NVIDIA 专用能力假定存在于 AMD。

真正修改 BLAS 内部为 DAG 属于驱动/硬件研究：需要公开或受支持的构建、节点格式、遍历与兼容接口。不能修改 Vulkan 不透明 AS 的字节、借序列化数据打补丁后声称合法可移植；本次没有授权开展驱动改造。

## 3. 与现有代码的关系

当前 `RayTracingScene.cpp` 已对同源完整组件共享 `VoxRayTracingGeometry`/BLAS。因此“同一个城堡组件出现十次”已经有收益，不能重复计入 DAG 改造。

新的收益来自：**不同源组件或同一组件内部含有相同三角形子区域，但目前作为不同几何构建。** 将这些重复区域变成同一个唯一子块，才可能进一步减少 BLAS。

当前 `VoxRayTracingGeometry.cpp` 已有半精度顶点/索引选择、BLAS 压缩和构建输入 fence 后释放。新方案的比较基线必须是压缩完成后的现有路径；不能把已释放的顶点、索引、scratch 当成常驻节省。

主要改动在子块提取、BLAS 缓存、TLAS 实例展开和属性映射。无需为此启用 Brick 的 3D 纹理注册、局部 DDA 或 `intersectHardwareBrick`。全量组件 BLAS 被选定子块和残余 BLAS 替换，而非同时保留两套。

## 4. 现有 DAG 实验能复用多少

- `VoxSvdDagStats.py` 可估计占用/颜色重复率，作为候选发现工具；相同占用并不保证实际外露三角形相同。
- `VoxMeshDagPack.py` 记录位置无关的 quad 内容，提供规范化/往返思路；它按 quad 锚点归组。跨节点 quad 的完整几何不能错误丢弃或重复放入多个块。
- `.vmda` 当前支持 v5 叶和 v4 包装，未覆盖完整 v3 组合容器；计划先从已解析的组件表面构建候选，不要求先扩展这个包格式。
- 本轮之前 CPU 检查：Menger 的脚本 SVO 结构估算 1,043,880 → 57,746 B，减少 94.47%。这不是本方案的 BLAS 压缩率：将 DAG 展开为 TLAS 出现列表会产生实例成本，驱动仍为每个唯一块构建自己的 BVH。

不必把既有 VMDA 变成运行时 ABI；首版可直接在 cook/资产准备阶段产生唯一块字典和出现表。

## 5. 三角形子块设计

### 5.1 按实际表面归一化

先提取已有贪心/回填后真实提交的三角形或等价 quad，保留 canonical face ID。共享键最少包含相对几何坐标、面向、覆盖范围、格式版本和会影响 AS 构建的几何 flags。

首版只做精确、平移等价去重。不做近似容差、有损量化、任意缩放或旋转同构识别。现有量化格式保持其原有正确性范围；位置归一化不能引入新的裂缝。

不要为每个块添加边界封口面。相邻原体素的内部面仍不可见；相同 occupancy 在不同邻接环境中可能有不同外表面，必须按最终实际表面判断是否可共享。独立组件不新增跨组件焊接规则。

### 5.2 切分与跨界 quad

先测试 8/16/32/64 体素尺度及较大自然重复区域；这些只是候选，不预设 8³ 最优。

首版优先不分裂已有大 quad：完整落入候选块的表面参与去重；跨越块边界的 quad 留在父级/组件残余 BLAS。选择一个互不重复的几何覆盖切面，确保每个原始 primitive 恰好归属一次。

这样保留高效减面平面，避免为了 DAG 切分把六个大面重新变成数千小三角形。若后续允许裁切，必须单独统计新增三角形、边界一致性和材质映射，不把裁切当作免费操作。

共享的 BLAS 必须包含完整唯一块的三角形。仅共享源顶点缓冲、把相同 geometry 重复放入一个大 BLAS，不保证驱动共享加速节点，不能据此计算 BLAS 去重。

### 5.3 几何与属性分离

首版可将材质内容也计入共享键，以降低接入风险。第二阶段再允许几何相同、颜色不同的子块共享 BLAS：

- `BlockGeometryKey` 对应唯一三角形 BLAS。
- 每次出现有独立 `BlockAppearanceRef` 和 canonical primitive 映射。
- 不能使用局部调色板编号直接比较不同调色板域的颜色。
- 不同 alpha/不透明性、面向裁剪或需要不同几何 flags 的资产不能无条件共享同一个构建结果。

块的局部几何范围/原点确定后，TLAS 变换组合为实体变换 × 组件变换 × 块到组件变换；每次出现仍有独立空间身份。变换及 primitive 映射要覆盖负缩放和非均匀缩放，不改变原表面。

## 6. 命中、发光与生命周期

硬件命中仍返回普通 `instanceCustomIndex`、geometry index、primitive index 和 barycentric。为每个 TLAS 叶出现维护：

```text
{entityId, componentId, blockGeometryId, appearanceRef,
 primitiveMapBase, originalSurfaceRef, transform/generation}
```

利用这个映射恢复原 canonical 面/材质。共享 BLAS 的 primitive ID 只在块内有意义，不是场景唯一面 ID。NEE/emitter key 还要包含空间出现身份，不能因为两个块引用同一 BLAS，就被当成同一盏灯。

保留现有硬件三角形的主命中、阴影、GI、反射和重建 guide 流程；修改的是命中索引解码而非求交方式。不要为 emitter 身份再创建完整组件 helper BLAS；其数据映射应随切分一起完成。残余 BLAS 只存未被共享块覆盖的表面。

唯一块 BLAS 在资产池按引用共享，静态 geometry 不按实例/帧槽复制。TLAS/实例表仍需要按实际出现和 frame slot 分配，不能忽略这部分成本。实体移动仅更新实例，修改几何重建受影响的块；旧 BLAS 和映射在所有消费者 fence 完成后回收。

DAG 任意深度不转化成嵌套硬件实例；选定切面后平铺。检查设备 `maxInstanceCount`、现有 24 位 custom index、geometry/primitive 数与 buffer 限制；超预算的区域保留组件级 BLAS，不静默漏画。

## 7. 收益与反收益模型

```text
M_base = Σ唯一组件的压缩 BLAS + 原实例/TLAS/属性映射
M_new  = Σ唯一子块的压缩 BLAS + Σ残余 BLAS
       + 所有新增 TLAS/实例/映射与分配开销
       + 必须保留的属性、工作缓冲、运行时字典
净节省 = M_base - M_new
```

仅作为构建参考的 CPU DAG 或可释放上传缓冲，单独报告，不混入 GPU 常驻。光栅仍需要的几何不因为 RT 切块而自动消失。UMA 分配同样计入资源账本，不把系统内存视图与 GPU heap 视图重复相加。

这条路线可能明显节省重复几何 BLAS，也可能退化：

- 块太小，BLAS 固定开销、对齐和每次 TLAS 出现的成本吞掉节省。
- 只出现一次的块没有复用收益，整块三角形 BLAS通常更合适。
- 小 BLAS 让光线更频繁切换实例，TLAS 访问与重叠增多；不能因为求交仍是硬件就保证不变慢。
- 边界/材质差异减少真实可共享表面，即使二进制 occupancy DAG 很小，BLAS 去重也可能有限。
- 把每个体素做成一个共享立方体 BLAS会造成实例爆炸，也可能恢复内部面，不作为默认方案。

选择应以实际构建的块 BLAS 大小和出现次数、总 TLAS 成本为依据。只统计 DAG 唯一节点数或三角形数量不足以预测分配字节。按区域/组件选路，允许整块、共享块和残余 BLAS 混合。

## 8. 分阶段开发计划

| 阶段 | 交付 | 验收/止损 |
|---|---|---|
| P0：能力与基线 | 确认 KHR 路径；探测 DGF/其他扩展但不启用；统计唯一组件压缩 BLAS、实例与 heap | 明确现有共享、API 能力及真实内存分母 |
| P1：CPU 子块分析 | 按最终表面提取平移等价块；输出共享字典、无重复覆盖切面和残余表面；增加损坏/边界测试 | 原 primitive 恰好覆盖一次；报告真实复用次数、残余占比和新增实例，禁止拿 SVDAG 比率替代结果 |
| P2：小型硬件原型 | 每个唯一块构建一次普通三角形 BLAS；平铺 TLAS 出现；接通材质、canonical face 和 emitter 映射 | 全部射线仍走 triangles；主/阴影/反射/GI结果正确；未额外保留组件完整 BLAS |
| P3：净显存测量 | 完整资源账本、压缩完成后的常驻与加载峰值、frame-slot/多实例去重 | 真正包含 TLAS/映射的净节省为正；否则调整粒度或回退 |
| P4：资产/区域选择 | 基于实测的块粒度与复用收益选择，支持组合模型、卸载/重载 | 无收益部分保留原整块 BLAS；性能回退在可接受范围内 |
| 可选 DGF 后端 | 仅设备/驱动明确支持时加入 DGF 编码与建 AS 对照 | 实测 AS 存储和构建峰值，不承诺任意 DAG 或跨设备能力 |
| 独立驱动研究 | 真正的 DAG 化 BLAS 内部节点与硬件支持评估 | 需要单独范围/接口与维护方案；不作为上述应用层功能的隐含承诺 |

最先值得实现 P1：它能回答“现有实例共享之外，究竟还有多少可重复的最终三角形表面”，无需先改 shader 或驱动。若候选出现列表本身已经太大，就止损，不先造完整运行时系统。

### 涉及文件

- `tools/standalone/vox-dag/`：添加最终表面子块分析；现有 VMDA 工具继续作离线往返参考。
- `VoxRenderer.cpp` / `VoxCompiledSurface.inl`：提供唯一组件的最终 quad、属性和残余映射；不改写作者 VOX。
- 新 `VoxTriangleBlockCompiler.*`：候选提取、几何 key、覆盖与残余验证、出现表。
- `VoxRayTracingGeometry.*`：唯一块标准三角形 BLAS、压缩、临时输入释放和资产池。
- `RayTracingScene.cpp`：将组件实例展开为块实例与残余实例，限制与 fence 生命周期。
- `RayTracingViewport*.cpp` / 相关 GLSL：只改命中与材质/emitter索引映射，不加入 Shader DAG 遍历或体素求交。
- `tests/`：覆盖、边界、材料、重复实例及资源回收回归。

## 9. 验证与验收口径

基准包含 Knight/Teapot/Dragon/Menger、平面房间、重复模块场景、`castle.vox` 组合资产、随机不可复用几何。私有资产不提交仓库。

正确性比较原组件三角形与共享块三角形的世界表面：覆盖、距离、面向、材质、barycentric 对应、实体/组件和 emitter 身份；裁切/边界等价规则独立定义。测试透明性分类、阴影开关、镜面/发光、负/非均匀缩放以及实例移动、隐藏、卸载/重载。

在同设备、相机、分辨率、采样、光栅消费者和重建设置下比较。等 BLAS 构建、压缩和旧 generation 释放完成后再测；截图/原子计数和性能运行分开。分别列主命中、光照、重建、整帧 GPU与 CPU；旧的 6.2 ms 仅作历史参考，不能当新实现已达标的当前结果。

报告压缩后的唯一 BLAS、TLAS、每槽实例/映射、属性、加载峰值和整场景绝对 MiB，不能只报一个块的压缩率。可把“净 RT 几何/实例内存下降至少 30%，主命中回退不超过 10%”设为首轮试行工程门槛，但这是待验证目标，不是预测；显存优先模式与性能优先模式分开决策。

## 10. 参考

- [Vulkan 加速结构说明](https://docs.vulkan.org/tutorial/latest/courses/18_Ray_tracing/02_Acceleration_structures.html)：AS 内部实现不透明。
- [VkAccelerationStructureBuildGeometryInfoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkAccelerationStructureBuildGeometryInfoKHR.html)：TLAS/BLAS 的 geometry type 限制。
- [VK_AMDX_dense_geometry_format](https://docs.vulkan.org/features/latest/features/proposals/VK_AMDX_dense_geometry_format.html)：预压缩三角形输入的 provisional 接口，不是自定义 DAG 接口。
- [AMD DGF SDK](https://gpuopen.com/dgf/)：硬件友好几何压缩方向及 SDK。
- [当前 DAG 实验说明](../tools/standalone/vox-dag/README.md)。本地 CPU 观察数据仍保存在 `out/dag-runtime-plan/20261020_131720/evidence.json`，不构成共享 BLAS 或 GPU DAG 的性能证据。


## 11. 2026-10-20 首轮落地

本轮用户将目标调整为“以 DAG 思想减少场景显存”，允许选择实际可行路径。先采用无损的组件叶节点内容寻址共享：几何 key 包含有序 quad、平面解码、面组及局部坐标参数；不包含材质。不同来源但几何相同的表面引用同一份普通硬件三角形 BLAS，各自保留原属性和 hit/emitter 身份。不拆分组件，不增加 TLAS 实例；这不是自定义硬件 BLAS 内部 DAG，也未实现细粒度 SVDAG。

候选实测，`vox model` 当前 `castle.vox`，RTX 4090 Laptop，1280×720，453 份表面、1309 实例：压实 BLAS 201.90625 → 183.88086 MiB（433 个唯一 AS），另外在构建 fence 后回收 15.2167 MiB quad 输入。BLAS/TLAS/实例/DDA/残留输入这一统计范围合计节省约 33.24 MiB；不包含光栅副本、纹理及视口重建图像，不能称全进程显存下降 15%。材质和 RT 表不进行有损处理。

对照与候选 GPU frame600（warmup360，240 样本）：主命中 5.821 → 5.779 ms，光照 12.949 → 12.844 ms，视口 26.108 → 25.905 ms。单轮小幅时间差仅支持“未见明显退化”，不构成稳定提速结论。同 frame30 的 17 个原始 BIN 全部逐字节一致，包含深度/法线/材质/光照/RR；未覆盖所有隐藏、热重载、设备和资产组合。初次运行因旧 SceneModels 目标文件未重编而崩溃，定向重编修正；不将失败运行计入性能。

默认启用共享和构建输入回收，分别用 `MIKAN_VOX_SHARED_BLAS=0`、`MIKAN_VOX_RELEASE_BUILD_BUFFER=0` 回退。`MIKAN_VOX_MEMORY_STATS=1` 输出包含 frame-slot 保留旧 AS 的请求字节账本。现场证据：`out/dag-scene-memory/20261020_134008/`；外部备份：`D:/Engine project/backup/dag_scene_memory_20261020_134008`。这是粗粒度共享的可验证起点，细粒度块切分仍需按额外 TLAS/映射净成本评估。


## 12. 合并 quad 粒度 DAG 原型（2026-10-20）

已加入 `VoxQuadDag.h`：叶子记录方向和宽高，位置保存在父子边上；分组节点以完整、有序的子节点与相对位置内容做驻留，材质和出现身份不进入几何 key。空间分组为三叉步长；节点步长表示锚点分组，不是可直接用于软件遍历的保守 AABB。选作硬件共享单元的块另行保证 quad 完全容纳；跨边界的长 quad 保持原样，进入残余 BLAS。CPU DAG 保留结构，硬件后端仅展开选中 cut 的出现列表，不是 Vulkan BLAS 内部自定义 DAG。

选路按复用次数、唯一 quad 数和估计的实例/AS成本选择共享节点；其余合并为一个残余 BLAS。加入实例预算、唯一块上限及全场景 converter 描述符预算保留。最终验收仍使用实际压实 AS、TLAS 与 scene table 的请求字节账本，而非估计值。没有收益的资产退回整体 BLAS。

`VoxRayTracingGeometry` 保存该出现的局部 quads、planes、source quad 索引与重排后的 palette/R8描述；属性未重新量化。共享几何后仍保留自己的 R8采样与材质。Scene View/RT灯表通过几何包装的 DecodeQuad 读取子集；不能直接将局部下标传给原始 Source renderer。硬件三角形方向与对角线保持原规则，现有 GLSL 接口不变。视口按共享 AS 复用 quad 几何缓冲，属性 header 仍逐出现保留。

默认关闭本实验模式（`MIKAN_VOX_QUAD_DAG=1` 可启用），默认 cut步长9、实例预算256。不会因为内存下降就默认替换更快的整体 BLAS。已测试 Menger 单资产；多资产、动态重载及其他 GPU 仍未全面验收。源文件不修改，不新增作者资产格式。CPU 节点与映射也占主机内存，本轮GPU字节账本不把这些算成显存。

当前实验数据与逐轮退化/改善记录位于 `out/quad-dag-rt/20261020_161400/REPORT.md`；主命中、光照和 NRD/TAA总视口分开报告。测试加入多色 Menger 合成副本，专门检查相同几何不同外观的共享，避免仅凭单色模型确认材质正确。外部备份：`D:/Engine project/backup/quad_dag_rt_20261020_161400`。本轮没有提交或推送。
