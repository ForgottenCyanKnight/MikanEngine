# 光追管线中的天空背景与平视测试

核对的是当前主线源码；这不保证用户实机采用同一提交、相同配置或相同渲染模式。

## GLSL 与宿主之间的数据流

1. `AtmosphereLUT.cpp` 和 `atmo_*.comp` 预计算 Bruneton 四阶散射 LUT。
   `atmo_sky.comp` 根据太阳方向和相机海拔查询物理散射，生成非线性投影的天空全景图，乘原 `kSceneExposure` 与灯光颜色，编码为 RGBA8 LogLuv32。
2. `AtmosphereRenderer::Init` 当前写死全景图 **128×64**，并非 1024×512；散射 LUT 本身仍是 **256×128×32**。全景生成和 RT 使用同一海拔 `max(camera.y+200,0)`。
3. `RayTracingViewport.cpp` 把全景图绑定到 binding 5，把 SH 绑定到 6、颜色/强度调制绑定到 7，把透射率 LUT 绑定到 18；`pc.eye.w` 传海拔，`pc.extent.z` 标记天空可用，`pc.extent.w` 为曝光。
4. `vox_rt_environment.glsl::skyPanoramaUV` 将世界方向变换为经纬方向，再按地球曲率、海拔移动地平线，用四次幂的逆映射展开地平线附近区域，最后做 0.002 UV inset。不能把它当普通等距柱状图直接按 atan/acos 读取。
5. `skyBackground` 使用 `textureLod(...,0)` 读取全景图，经 LogLuv32 解码得到线性 RGB，再乘环境颜色/强度。实际 sampler 来自 `AtmosphereLUT`，为线性过滤、U 环绕；它不是测试入口额外创建的 nearest sampler。
6. `vox_rt_realtime.comp::shadePixel` 主射线未命中时，直接把 `skyBackground(direction)` 写入 HDR 输出，并返回。空场景的 `vox_rt_sky.comp` 使用同一函数。
7. `vox_rt_gi.glsl` 的漫反射续射未命中、终点可见天空采样，以及 GGX 反射未命中，都读取同一个 `skyBackground`。`sampleVisibleSky` 先追踪遮挡射线，遮挡时返回零。当前 NEE 路径不以无条件 SH 环境光替代遮挡测试；`skyIrradiance` 虽然定义了，在这些 RT Shader 中没有调用。
8. 太阳直接光由 `vox_rt_sun.glsl` 独立估计：太阳圆盘采样、透射率 LUT、地球遮挡和场景阴影；全景图中的散射天空不等于解析太阳圆盘。RT 背景直接读取 panorama，不直接读取预过滤 skyCube。
9. 天空的运动引导以 `previousViewProjection*vec4(direction,0)` 重投影，去掉相机平移；完整视口可以据此运行 TAA。NRD 的漫反射/镜面信号降噪与主射线未命中的天空背景不同，不能把天空条纹简单解释为未接 NRD。

## 本次平视渲染

沿用骑士、太阳方向、960×540、32 帧线性累积、原始四阶散射与原 NEE GI Shader。
相机保持原来的水平朝向，向下俯视 **2°**，注视模型中心；垂直视场角为 34°。
为对齐宿主，天空全景图改为 **128×64**，大气生成及读取统一采用相机 y+200 米。
没有改动生产 GLSL，没有对结果作模糊或图像修补。

复现命令：

```sh
/workspace/.linux-tools/cmake/data/bin/cmake --build out/build/linux-cpu --target MikanVoxCompletePreview
python3 tests/VoxCompletePreview.py out/build/linux-cpu/MikanVoxCompletePreview \
  --runtime-root /workspace/.software-vulkan --output out/knight-540p-level \
  --size 960 540 --frames 32 --pitch 2
```

STBN 和依赖要求见 [KNIGHT_540P.md](KNIGHT_540P.md)。输出保留在新目录，上一张俯视图仍在原目录。
`complete-report.json` 记录实际相机坐标、俯角、大气海拔、天空尺寸、Shader 哈希和 Validation 结果。

此前俯视测试用了 1024×512 的天空全景图，而且海拔固定 200 米；那张图保留为历史结果，不能当作参数完全对齐的 GPU 对照。
本次同时改了相机、天空尺寸和海拔，因此两图的区别不能用于单独证明条纹的根因；判断 CPU/GPU 一致性仍需同参数硬件 GPU 对照。

## 实测结果

最终输出 `out/knight-540p-level/knight-540p.png`，CPU 为 llvmpipe (LLVM 19.1.7, 256 bits)。
相机坐标为 (22.5823, 1.2961, 29.4552)，注视原点，实际俯角 2°，海拔 201.2961 米。
960×540、32 帧，原生执行共 84.24 秒，其中大气 LUT 41.17 秒，最终帧平均 0.992 秒。
Validation 为 0 错误、0 警告；64×36 探针、非零 GI、HDR 有限及全部像素写入检查通过。

已目视检查最终图。天空仍有明显带状和块状边界；这次没有修复 CPU/GPU 画面差异，不能把输出称为 GPU 一致性验证通过。保留原始 PNG、HDR 和诊断图供继续定位。

后续核对发现历史截图的最终显示出口使用 ACES2，与仓库提供的 RT present 函数不同；解码源码相同不等于显示链相同。修正后的对照与边界见 [SKY_DECODE_AUDIT.md](SKY_DECODE_AUDIT.md)。
