# CPU 软件 Vulkan 的原生 NRD 测试

测试入口复用引擎 `NrdDiffuseDenoiser.cpp`、`DiffuseDenoiser.cpp` 和未经修改的
`denoiser_prepare.comp` / `denoiser_unpack.comp`，调用实际 NRD 4.18.0 REBLUR。
没有在 Python 中实现替代滤波器。

## 配置与数据路径

- SDK 配置与主线 `cmake/Nrd.cmake` 一致：嵌入 SPIR-V、静态链接、禁用 NRI，
  `NRD_NORMAL_ENCODING=4`、`NRD_ROUGHNESS_ENCODING=1`，关闭可选 quad intrinsics。
- 真实 RT 输出的漫反射/镜面 radiance + hit distance、线性 viewZ、世界法线/粗糙度、
  运动向量进入生产 prepare Shader，再运行 SDK 调度和生产 unpack Shader。
- 漫反射与镜面使用独立 REBLUR 实例和独立历史。第一帧 CLEAR_AND_RESTART，后续 CONTINUE；
  单帧槽在复用前等待 fence。场景和相机静止，仍提供实际相机采样抖动。
- 每一帧先保存相同的直接光/天空基底，分别合成原始 GI 和 NRD 输出。
  第 1 帧及最后一帧的原始/降噪图使用相同采样与相同色调映射。
- NRD 对照图输出最后一次实际降噪结果，**不经过测试入口额外的多帧平均**。
  NRD 自己仍然是包含历史累积、重投影与空间滤波的算法，不是无历史的纯单帧算法。
- 原有 `knight-540p.png` 继续保留为原始信号的多帧平均参考，不能混同于 NRD 输出。

不设置静默回退：要求两个实例初始化成功，且每帧 Record 成功，否则测试失败。
Knight 为漫反射模型，镜面后端虽执行，但此模型不能验证复杂镜面材质的降噪质量。
独立测试宿主不等于完整编辑器运行时，也未接入 DLSS。

## 构建与运行

已有软件 Vulkan 依赖和 STBN 要求见 [KNIGHT_540P.md](KNIGHT_540P.md)。额外配置：

```sh
/workspace/.linux-tools/cmake/data/bin/cmake -S . -B out/build/linux-cpu -DMIKAN_SOFTWARE_NRD=ON
PATH=/workspace/.linux-tools/bin:$PATH /workspace/.linux-tools/cmake/data/bin/cmake --build out/build/linux-cpu --target MikanVoxCompletePreview -j2
python3 tests/VoxCompletePreview.py out/build/linux-cpu/MikanVoxCompletePreview \
  --runtime-root /workspace/.software-vulkan --output out/knight-540p-nrd \
  --size 960 540 --frames 32 --pitch 2 --nrd
```

SDK 会获取固定版本的 ShaderMake、MathLib 及 ShaderMake 指定的 Linux DXC。
这些构建依赖需要联网；在本环境 Ninja 位于本地工具前缀，嵌套 ShaderMake 构建也需将其加入 PATH。
新增可选 `NRD_SHADER_JOBS` 控制 ShaderMake 并发，默认 0 保持上游行为；软件测试配置限制为 2。
NRD 本身的 HLSL 算法没有修改。
`MIKAN_SOFTWARE_NRD` 默认关闭，普通 CPU CI 不需要下载 DXC 或构建 NRD。

## 产物与计时解释

`out/knight-540p-nrd/` 保留：

- `knight-raw-first.png` / `knight-nrd-first.png`：第一帧相同输入的原始与 NRD 图。
- `knight-raw-last.png` / `knight-nrd-last.png`：最后一帧相同输入的原始与 NRD 图。
- `knight-540p.png`：独立的原始信号多帧平均参考。
- 原始平均 HDR、GI 诊断图、运行日志和 `complete-report.json`。

`nrd_frame_ms` 是 CPU 等待两个 REBLUR 实例执行、NRD 输出合成及必要导出的墙钟耗时；
不是单个 NRD 内核的 GPU timestamp。`frame_ms` 包含光追、原始参考和 NRD 对照工作，
不能直接当成生产游戏单帧性能。

天空未命中背景属于直接写入的基底，不是 NRD 漫反射/镜面输入。
因此这个 NRD 接入并不以修复天空条纹为验收条件；即使 NRD 成功，也不能据此认定天空误差原因。

## 本次实际结果

CPU 设备为 `llvmpipe (LLVM 19.1.7, 256 bits)`；相机俯角 2°，960×540，32 帧双信号 NRD。
原生总耗时 112.39 秒，包括大气预计算 43.33 秒和全部原始/NRD 对照工作。
NRD、合成及导出首帧 2.531 秒，后 31 帧平均 0.378 秒。
Vulkan Validation 为 **0 错误、0 警告**，每个实例各 32 次逐帧 Record 均成功，没有回退到原始 GI。

已目视检查同帧原始图与 NRD 图：表面的随机 GI 噪点明显减少，但没有硬件 GPU 基准来量化保真度，也未验证动态场景的拖影和遮挡揭露。
两图差异范围为 [316, 70, 691, 462]；左侧 200×540 的纯天空对照区域逐像素完全相同。
天空条纹仍存在，因此此次只证明 NRD 可以在 Lavapipe CPU 上实际执行，不证明天空条纹来自哪种模拟误差。

最终 NRD 输出：[knight-nrd-last.png](../out/knight-540p-nrd/knight-nrd-last.png)。
同帧原始输出：[knight-raw-last.png](../out/knight-540p-nrd/knight-raw-last.png)。

启用 NRD 构建后，已有 16 项 CTest 回归全部通过。NRD 重型渲染单独执行，不计入这 16 项。

后续核对发现历史截图的最终显示出口使用 ACES2，与仓库提供的 RT present 函数不同；解码源码相同不等于显示链相同。修正后的对照与边界见 [SKY_DECODE_AUDIT.md](SKY_DECODE_AUDIT.md)。
