# Linux CPU 纯光追验证

在无 GPU 的 Debian 13 云端通过 Mesa 25.0.7 Lavapipe 实际运行原生 Vulkan Ray Query。
设备为 `llvmpipe (LLVM 19.1.7, 256 bits)`，线程数为 2。入口是独立 C++ 验证程序，尚非完整 EngineMain 或编辑器。

## 实际执行路径

`MikanVoxRayQueryPreview` 复用原引擎 VOX 加载、减面、VulkanBuffer、RendererUtils、AccelerationStructure 和光追设备初始化代码。
未经修改的 `vox_rt_expand.comp` 将四边形展开为三角形，随后构建真实 BLAS/TLAS；未经修改的 `vox_rt_validate.comp` 每像素发射 4 条主射线，并用阴影查询判断太阳光遮挡。
输出包含直接漫反射、原 Shader 的常量环境项及显示 gamma 编码。

主可见性与阴影均来自光追，没有光栅化、后处理 Pass、降噪或多跳 GI。
这是 compute shader 内的 inline Ray Query，不使用 `vkCmdTraceRaysKHR` 或 raygen/SBT，尚未接入完整 `vox_rt_realtime.comp` 多跳路径。
Python 只设置环境、启动原生程序、检查结果和将 BMP 转 PNG，不承担求交或着色。
脚本移除 DISPLAY，不需要 Xvfb、窗口或交换链。

## 结果

320×240、4 samples/pixel、每模型两个视角，每个视角 307,200 条主射线，另有命中后的阴影查询。
下表为缓存预热后一次运行的墙钟时间：

| 模型 | 三角形 | 展开与 AS 构建 | 两视角各自光追耗时 |
| --- | ---: | ---: | ---: |
| Knight | 626 | 4.42 ms | 68.37 / 65.11 ms |
| Teapot | 42,808 | 11.44 ms | 298.82 / 325.78 ms |
| Dragon | 64,856 | 14.03 ms | 869.44 / 795.25 ms |
| Menger | 275,976 | 87.89 ms | 1273.31 / 1255.23 ms |

全部原生执行约 7.61 秒，包括加载、减面和表面审计、AS 构建、8 个正向视角、8 个反向相机负对照及读回。
表中光追耗时不含前面的构建或减面，不能当作完整游戏帧率；冷启动 Shader JIT 和 CPU 调度会影响结果。

所有正向画面非空且所有像素被写入，反向相机均输出背景。Vulkan Validation 为 **0 错误、0 警告**。
这验证了执行和基本结果，尚未与硬件 GPU 逐像素对照。完整 CTest **16/16 通过**；此前光栅严格比较的已知像素差异仍然存在。

## 构建与复现

先按 [SOFTWARE_VULKAN.md](SOFTWARE_VULKAN.md) 配置依赖并启用 `MIKAN_SOFTWARE_VULKAN=ON`：

```sh
/workspace/.linux-tools/cmake/data/bin/cmake --build out/build/linux-cpu --target MikanVoxRayQueryPreview
python3 tests/VoxSoftwareRayQueryTests.py out/build/linux-cpu/MikanVoxRayQueryPreview \
  --runtime-root /workspace/.software-vulkan --size 320 240 \
  --output out/software-ray-query-320
```

该配置同时注册 `vox_software_ray_query` CTest，默认 128×96。测试使用仓库内四个固定版本的 MIT 模型。
产物在 `out/software-ray-query-320/`：overview.png、逐视角 PNG/BMP、ray-query-report.json 和日志。
报告记录设备、扩展、Shader SHA256、三角形数、BLAS/TLAS 大小、计时、负对照和 Validation 结果。

为了复用引擎，RendererBase.cpp 的宽泛全局头改为所需 Vulkan/RenderGlobals 头；RayTracingValidationDevice 新增默认关闭的 CPU 筛选、Validation 选项和可重复清理入口，原调用保持默认行为。
链接器移除未使用的图形辅助函数，未用假渲染器替代。

包含原引擎物理大气与 NEE/MIS GI 的 960×540 静帧见 [KNIGHT_540P.md](KNIGHT_540P.md)。
