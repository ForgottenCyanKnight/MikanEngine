# 构建与游戏移植文档

从内置 Pixel Hop Twins 示例开始验证本机环境，再使用相同流程构建自己的项目。下列指南中的命令默认在引擎仓库根目录执行；项目路径、工具链与输出目录应按实际环境替换。

## 推荐阅读顺序

| 文档 | 内容 |
| --- | --- |
| [Linux 构建、运行与打包](LINUX.md) | 依赖安装、桌面编辑器与独立游戏、Linux 分发目录、截图与玩法测试、CPU 验证及排错 |
| [Android 构建与 APK 打包](ANDROID.md) | SDK/NDK/JDK/Gradle、原生依赖、项目选择、资源收集、APK 生成、安装与包体检查 |
| [游戏移植指南](PORTING.md) | 来源与许可、项目结构、跨平台 C++、输入和时间步、2D 画面与 UI、按需加载、行为对照及验收 |
| [Pixel Hop Twins 示例说明](../templates/pixel-hop-twins/README.md) | 可直接构建的内置模板、操作方式、素材与许可证目录 |

Linux 的 `MIKAN_PROJECT_PATH` 和 Android 的 `mikanProjectPath` 都要指向含 `project.json` 的项目目录。构建玩法、选择运行资源和安装分发包是不同步骤，具体命令见对应平台指南。

`project.json` 的 `runtimeProfile` 选择运行档，`assets[]` 声明项目资源与许可。Linux 与 Android 当前的项目资源收集方式有差异，不能仅凭编辑器运行成功判断打包完整。两端都应从独立产物检查画面、输入、声音、资源和许可文件。

## 现有渲染与资源专项记录

下列文档记录特定配置下的实现或验证，不代表所有平台和 GPU 都支持同样的功能。

| 主题 | 文档 |
| --- | --- |
| CPU Vulkan 渲染、Ray Query 与 NRD | [SOFTWARE_VULKAN](SOFTWARE_VULKAN.md)、[SOFTWARE_RAY_QUERY](SOFTWARE_RAY_QUERY.md)、[SOFTWARE_NRD](SOFTWARE_NRD.md) |
| 场景渲染与天空 | [KNIGHT_540P](KNIGHT_540P.md)、[SKY_RAY_QUERY](SKY_RAY_QUERY.md)、[SKY_DECODE_AUDIT](SKY_DECODE_AUDIT.md) |
| 体素导入、测试与规划 | [CXW_IMPORT](CXW_IMPORT.md)、[VOX_TESTING](VOX_TESTING.md)、[VOX_LOD](VOX_LOD.md)、[VOX_DAG_RT_PLAN](VOX_DAG_RT_PLAN.md) |
| 软件 DDA 与降噪掩码 | [SOFTWARE_DDA](SOFTWARE_DDA.md)、[NRD_SPECULAR_MASK](NRD_SPECULAR_MASK.md) |

需要核对当前实现时，以 [CMake presets](../CMakePresets.json)、[Linux 项目打包规则](../cmake/LinuxProject.cmake)、[Android 构建入口](../android/app/build.gradle) 和 [Android 资源收集器](../android/mikan_project_assets.gradle) 为准。
