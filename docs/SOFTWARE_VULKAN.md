# CPU 软件 Vulkan 几何渲染验证

已在 Debian 13 x86_64 云端使用 **Xvfb + SDL3 X11 + Mesa Lavapipe** 实际绘制并呈现画面。
Vulkan 设备类型为 `VK_PHYSICAL_DEVICE_TYPE_CPU`，设备名称为
`llvmpipe (LLVM 19.1.7, 256 bits)`，Mesa 版本为 25.0.7。
宿主会拒绝非 CPU Vulkan 设备，没有使用硬件 GPU。

## 实际运行范围

新建的 `MikanVoxRasterPreview` 是独立的几何验证宿主，复用：

- 引擎 `VoxFormat::LoadVoxFile`、材质编码和 `BakeVoxSurfaces`；
- 原有紧凑四边形编码、平面表与 `AppendVoxRasterAttributes` 属性重定位；
- **未经修改的生产 Shader**：`engine/shaders/glsl/voxel_quad.vert`、
  `engine/shaders/glsl/voxel_mesh.frag`，以及它们包含的解码代码。

宿主自行建立 SDL 窗口、Vulkan 设备、交换链、几何管线和读回资源，通过真正的
`vkCmdDrawIndexed`、`vkQueueSubmit`、`vkQueuePresentKHR` 执行绘制。
原始颜色 attachment 直接来自交换链截图。法线和深度 attachment 同步读回用于诊断。

**没有运行完整 `EngineMain`/`VoxRenderer` 生命周期，也没有光照合成、天空、后处理、
TAA、Bloom、色调映射、光追或 GPU 剔除。** 这里是移植的几何入口验证，不能据此声称
完整桌面引擎已经在 Linux 上运行。

Python 仅负责启动 Xvfb/原生程序、检查结果、将 BMP 无损编码成 PNG 及制作预览拼图；
不负责模型投影、三角形光栅化或 Shader 计算。法线预览是对真实法线缓冲的 CPU 解码显示。

## 验证结果

- 640×480，骑士、茶壶、龙、Menger 分形，各两个相机视角。
- 每个视角分别绘制普通合并与接触回填后的网格，共 **16 帧成功呈现**。
- Vulkan Khronos validation 开启：**0 个错误，1 个警告**。
- 该警告针对原有 Shader 中 `samplerBuffer` 产生的
  `OpTypeSampledImage` / `Dim Buffer` SPIR-V 声明，完整内容保留于 `render.log`。
- 15 项基础与渲染冒烟 CTest 通过，测试总耗时 13.56 秒；软件渲染测试约 4.62 秒。
  这不是引擎游戏帧率测量，包含加载、CPU 烘焙、校验、资源创建、截图和导出。

### 画面等价检查没有完全通过

| 模型 | 两视角合计颜色不同像素 | 轮廓覆盖不同像素 | 深度差 ≥ 1e-5 的像素 |
|---|---:|---:|---:|
| 骑士 | 2 | 0 | 0 |
| 茶壶 | 1 | 1 | 8 |
| 龙 | 0 | 0 | 12 |
| Menger 分形 | 0 | 0 | 31 |

深度是 `[0,1]` 的投影后深度，阈值不是世界距离。
颜色差异累计 3 个像素，轮廓差异 1 个像素。原因尚未定位，可能涉及不同矩形/
三角形划分在边缘采样时的精度；CPU 单位面覆盖审计通过并不保证光栅化逐像素相同。

**出图冒烟通过与严格画面等价是分开的结论。** 报告中
`albedo_pixel_identical=false`、`strict_comparison_pass=false` 明确保留这一结果。
不使用放宽后仍称“完全一致”的判断。

默认入口完成绘制、非空截图和 Vulkan 校验后成功退出，并保留比较数据；加上
`--strict-comparison` 会在这些差异存在时返回非零退出码。
该严格模式已实测返回 1，并保留了 16 帧及差异报告。

## 构建与运行

通常的 Debian/Ubuntu 依赖（需在有包管理权限的机器安装）：

```sh
apt-get install cmake ninja-build g++ git git-lfs python3 python3-pil \
  mesa-vulkan-drivers libvulkan-dev vulkan-tools vulkan-validationlayers \
  xvfb glslang-tools libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
  libxi-dev libxfixes-dev libxrender-dev libxkbcommon-dev
```

本次云端没有系统安装权限，依赖解包在 `/workspace/.software-vulkan`，构建工具在
`/workspace/.linux-tools`，没有修改系统库。模型和固定子模块的准备步骤见 [Linux 构建说明](LINUX.md)。

```sh
cmake -S . -B out/build/linux-software-vulkan -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMIKAN_CPU_ONLY=ON -DMIKAN_SOFTWARE_VULKAN=ON \
  -DSDL_X11_XSCRNSAVER=OFF -DSDL_X11_XTEST=OFF
cmake --build out/build/linux-software-vulkan --target MikanVoxRasterPreview --parallel 2

python3 tests/VoxSoftwareRasterTests.py \
  out/build/linux-software-vulkan/MikanVoxRasterPreview --output out/software-vulkan
```

云端已有构建的复现命令：

```sh
python3 tests/VoxSoftwareRasterTests.py out/build/linux-cpu/MikanVoxRasterPreview \
  --runtime-root /workspace/.software-vulkan --output out/software-vulkan
```

在该命令末尾追加 `--strict-comparison` 可重现当前严格比较失败的结果。

Xvfb 需要创建本地 Unix socket；受限执行环境需要允许该本地连接能力。
脚本固定选择 Lavapipe ICD，并且退出时终止自己创建的 Xvfb。

启用此选项时，`vox_software_raster` 会注册到 CTest；自定义依赖前缀通过
`MIKAN_SOFTWARE_VULKAN_ROOT` 传入测试脚本。默认 CPU 构建不启用该图形测试，
因此原有仅 CPU CI 无需安装窗口系统和 Mesa。

## 产物

本次完整测试产物在 `out/build/linux-cpu/software-vulkan/`：

- `overview.png`：四个模型的原始 albedo 截图拼图；单色模型没有光照，因此呈现平涂颜色。
- `normals-overview.png`：四个模型的几何法线方向拼图，便于观察没有光照时的表面结构。
- `*_albedo.png`：实际光栅化颜色，未添加后处理。
- `*_normals.png`：真实几何法线 attachment 的方向可视化。
- `*_depth.f32`：行优先、float32 深度读回，便于复核比较。
- `render-report.json`：CPU 设备、呈现帧数、Shader 哈希、校验计数和逐视角差异。
- `render.log`、`xvfb.log`：运行诊断。

多模型实例组装和设备/窗口重建不在这个截图入口的验证范围内。当前入口仅接受单个组装后模型，
不覆盖引擎多实体场景、动画、插件和编辑器。

纯光追的 CPU Vulkan 验证另见 [SOFTWARE_RAY_QUERY.md](SOFTWARE_RAY_QUERY.md)。
