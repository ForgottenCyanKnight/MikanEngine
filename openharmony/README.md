# MikanEngine 鸿蒙体素沙盒

工程使用 HarmonyOS 6.0 SDK（API 20）。ArkTS 承载 SDL3 窗口，原生 `libmain.so` 运行 MikanEngine 的 Vulkan 光栅管线。包体面向 `arm64-v8a` 真机。

在仓库根目录构建。脚本同步体素场景、方块图集和引擎资源，首次构建会准备固定版本的 Assimp 源码。

```powershell
./tools/build.ps1 -Target CompileShaders
./openharmony/build_voxel_prototype.ps1
```

HAP 产物在 `out/harmony-voxel-sandbox/`。真机安装需要为 `com.mikanengine.openharmony` 配置适用的签名和设备授权。SDL 的许可证随子模块提供。 [SDL3 LICENSE](../dependencies/SDL3/LICENSE.txt)

构建 x86_64 模拟器测试包时，运行 `./openharmony/build_voxel_prototype.ps1 -Emulator`。
