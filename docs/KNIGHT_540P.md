# 骑士 540p：物理大气与引擎 GI

入口 `MikanVoxCompletePreview` 为独立 Linux 原生 C++ 验证宿主，在 CPU Lavapipe 上执行引擎实际 Shader；不运行完整编辑器或 EngineMain。

## 对齐的渲染路径

- 原 VOX 加载、减面及四边形展开，原 VulkanBuffer、BLAS/TLAS 构建和 Ray Query。
- 原 `AtmosphereLUT.cpp`、原 Bruneton Shader 和四阶大气散射：透射率 256×64，散射 256×128×32，辐照度 64×16；全景天空当前对齐主线为 128×64，同时运行环境 cubemap 和 SH 投影（下方首张历史实测曾使用 1024×512）。
- 原 `vox_rt_realtime.comp` NEE/MIS：光追主可见性、物理太阳及大气透射、太阳阴影、遮挡天空采样、漫反射 GI、原材质的镜面/粗糙反射与透射分支。
- 原 `vox_rt_composite.comp` 合成直接光、间接光和反射。Knight 模型本身为漫反射材质，不能用此图声称所有材质分支均被覆盖。
- 引擎当前源码 `MAX_PATH_BOUNCES=1`，即一次漫反射续射，终点还估计直射光和可见天空；`MAX_MIRROR_BOUNCES=2`。保持该深度，不将“完整 GI”解释为无限多跳。
- 质量档每帧 4 个 GI 样本；使用原 STBN 128×128×64 数据，Halton 相机抖动及 32 帧线性 HDR 累积，当前显示出口采用原 `hardware_rt_present.frag` 所用的峰值 Reinhard 与标准 sRGB 函数；下方历史首轮图片使用的是 ACES2/gamma/Bayer，区别见 [SKY_DECODE_AUDIT.md](SKY_DECODE_AUDIT.md)。

本入口没有接入 NRD、ReSTIR PT 时空复用、生产 TAA、Bloom、云或编辑器场景系统。静态累积用于静帧降噪，不代表已复现生产实时降噪或所有渲染能力。天空及物理太阳采用同一方向和原 `kSceneExposure=10`。当前入口统一按 `max(camera.y+200,0)` 传递海拔；下方首张历史实测固定使用 200 米。

## 验证方式

先执行 64×36 GI 探针，再执行 960×540 最终累积。输出同时保留 GI 贡献和去掉该贡献的直接光诊断图；检查实际非零 GI、非空几何、全部像素写入和 HDR 数值有限，并启用 Vulkan Validation。
`knight-direct-only` 保留原天空背景和非漫反射间接项；对本漫反射 Knight 模型，它用于观察移除漫反射 GI 后的变化。GI 图采用同样色调映射显示，不能将两张 LDR 图直接相加恢复最终图。
Python 只管理进程、环境和文件导出，不计算大气、求交、GI 或色调映射。

## 复现

按 [SOFTWARE_VULKAN.md](SOFTWARE_VULKAN.md) 配好本地依赖并启用 `MIKAN_SOFTWARE_VULKAN=ON`：

```sh
/workspace/.linux-tools/cmake/data/bin/cmake --build out/build/linux-cpu --target MikanVoxCompletePreview
python3 tests/VoxCompletePreview.py out/build/linux-cpu/MikanVoxCompletePreview \
  --runtime-root /workspace/.software-vulkan --size 960 540 --frames 32 \
  --output out/knight-540p
```

STBN 是 Git LFS 文件，需要将实际二进制放在
`out/build/linux-cpu/full-preview/engine/textures/stbn/stbn_vec2_128x128x64.rgba8.bin`。
来源是固定上游提交 `07413151712b83b7bccbae9c86563cc36ea3ec7b` 的同名资源，实际大小 4,194,304 字节，SHA256 为
`e2c8e1156baba84e7d7c47c9fab5c70d6628c328a3dcad48d0394758b0729841`。
来源和许可证见仓库 `engine/textures/stbn/SOURCE.json`、`License.txt`。
此重型渲染不注册到默认 CTest，避免让一般 CPU 回归测试反复执行大气预计算。

## 产物与源码修复

产物目录为 `out/knight-540p/`：最终 PNG/BMP、GI 与直接光诊断图、线性 RGBA32F HDR、`complete-report.json`、`render.log`。
报告包含每帧计时、完整大气计算耗时、实际 Shader 哈希、设备、GI 贡献和 Validation 计数。

本次暴露并修复原 `AtmosphereLUT::Init` 的两个图像用途声明：辐照度及 delta 辐照度图像由 `vkCmdClearColorImage` 清零，创建时需要 `VK_IMAGE_USAGE_TRANSFER_DST_BIT`。
验证设备新增默认关闭的完整 Shader 特性开关，以启用设备支持的描述符索引和图像格式等能力；普通验证入口保持原默认行为。

## 本机实测

CPU 为 llvmpipe (LLVM 19.1.7, 256 bits)，2 个线程。最终图为 **960×540**，32 帧抖动累积；每像素 32 个主可见性样本，命中表面时 GI 预算为 128 个样本。
本次原始共享场景日志首行仍带旧验证入口的“4 primary samples/pixel”文字，该标记已在源码修正；实际完整路径每帧主采样 1 次，4 次指 GI 样本，具体以完整 Shader 和报告为准。

- 四阶大气 LUT：52.81 秒。
- 最终 32 帧累积：41.89 秒，平均每帧 1.309 秒。
- 整次原生运行：166.41 秒，包含初始化/JIT、大气、天空及 cubemap、低分辨率探针和图像读回。
- GI 非零，几何覆盖并集 82,317 像素，HDR 全部有限。
- Vulkan Validation：0 错误、0 警告；16 项已有 CTest 回归全部通过。

已目视检查最终图和 GI 贡献图。仍有 Monte Carlo 噪点，天空可见带状轮廓；当前保留原大气 Shader、LogLuv32 全景图及其读取路径，色带原因尚未单独定位，未通过图像编辑或模糊掩盖。
此结果验证 NEE/MIS GI 和物理大气在 CPU 上出图，不建立与完整编辑器或硬件 GPU 的逐像素一致性。

GLSL 天空路径解读及新的 2° 平视图见 [SKY_RAY_QUERY.md](SKY_RAY_QUERY.md)。

CPU Vulkan 上的 NRD 双信号实测与同帧对照见 [SOFTWARE_NRD.md](SOFTWARE_NRD.md)。
