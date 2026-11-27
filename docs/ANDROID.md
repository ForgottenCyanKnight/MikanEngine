# Android APK 构建

本文以 Linux 主机上的 Bash 为例，命令从引擎仓库根目录执行。Android 工程将选定项目的 C++ 玩法编入 `libmikanengine.so`，通过 SDL3 和 Vulkan 运行。相关文档见[文档入口](README.md)、[Linux 构建](LINUX.md)、[移植说明](PORTING.md)及 [Pixel Hop Twins 模板](../templates/pixel-hop-twins/README.md)。

## 工具版本与依赖

| 项目 | 当前仓库配置 |
| --- | --- |
| JDK | AGP 最低需要 JDK 17，本云环境使用 JDK 21 |
| Gradle | Wrapper 固定为 8.13 |
| Android Gradle Plugin | 8.9.1 |
| SDK | `compileSdk 36`、`targetSdk 34`、`minSdk 26` |
| Build Tools | 本次构建使用 35.0.0，工程未固定 `buildToolsVersion` |
| NDK | r28c，`28.2.13676358` |
| CMake | Android SDK 中的 3.22.1 |
| ABI | `arm64-v8a` |
| Assimp | 当前配套头文件与已验证动态库采用 6.0.4 |

`compileOptions` 的 Java 8 源码目标与运行 Gradle 所需的 JDK 版本分别配置。依赖下载使用 `google()`、`mavenCentral()` 和 Gradle Plugin Portal。

安装 Android Command-line Tools 后，设置本机路径并安装组件。

主机还需要 Git、Git LFS、CMake、Ninja、glslangValidator；检查 APK 时会使用 unzip 和 ripgrep。Debian/Ubuntu 可安装 `cmake ninja-build git git-lfs glslang-tools unzip ripgrep`，不必为了构建 APK 安装 Linux 桌面的 X11 开发包。

```sh
export JAVA_HOME="/path/to/jdk-21"
export ANDROID_HOME="$HOME/Android/Sdk"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"
sdkmanager --licenses
sdkmanager "platform-tools" "platforms;android-36" "build-tools;35.0.0" "ndk;28.2.13676358" "cmake;3.22.1"
```

也可通过 Android Studio 安装同版本组件，并在 `android/local.properties` 设置 `sdk.dir`。该文件使用本机 SDK 路径。

初始化源依赖和 Android 预制资源。

```sh
git submodule update --init --depth 1 -- \
  dependencies/JoltPhysics dependencies/glm dependencies/box2d \
  dependencies/FastNoiseLite dependencies/SDL3 dependencies/SDL3_image \
  dependencies/vulkan dependencies/meshoptimizer dependencies/zlib
git lfs install
git lfs pull --include='android/app/libs/SDL3_image-3.4.0.aar,android/app/libs/arm64-v8a/libassimp.so,engine/fonts/simhei.ttf,engine/ui/mikan_engine_splash.png,templates/pixel-hop-twins/**' --exclude=''
```

SDL3 原生库与 Java 桥接共同使用 `dependencies/SDL3`。SDL3_image 通过 `android/app/libs/SDL3_image-3.4.0.aar` 提供 Prefab 包。项目和引擎资源清单引用的其他 LFS 文件也需要完整内容，例如字体和玩法素材。

打包前应准备资源清单中的 SPIR-V。Pixel Hop Twins 等独立 2D 项目使用 `engine/runtime_2d_assets.list`；其三个 UI 着色器可用主机上的 `glslangValidator` 编译。

```sh
mkdir -p engine/shaders/spv/ui
for shader in ui2d.vert ui2d.frag ui2d_sdf.frag; do
  glslangValidator -V --target-env spirv1.3 "engine/shaders/glsl/ui/$shader" -o "engine/shaders/spv/ui/$shader.spv"
done
```

完整运行时还需要相应引擎资源与 `shader-build-plan.txt`，可在 Linux 主机使用共享配方编译全部着色器；Windows 的 PowerShell 入口见[主 README](../README.md)。修改着色器后重新生成，编译成功后再打包。

```sh
cmake -DGLSL_DIR="$PWD/engine/shaders/glsl" \
  -DSPV_DIR="$PWD/engine/shaders/spv" \
  -DGLSLANG_VALIDATOR="$(command -v glslangValidator)" \
  -P cmake/CompileShaders.cmake
```

上面的 LFS 下载范围用于内置 2D 示例。构建 `full` 项目或其他玩法时，还应下载其资源清单需要的文件；也可用 `git lfs pull` 获取仓库当前版本的全部 LFS 内容。

## Assimp 动态库

默认路径是 `android/app/libs/arm64-v8a/libassimp.so`。Gradle 属性 `mikanAssimpLibrary` 可以指定其他同名动态库；设置后，原生链接与 APK 收集均使用该文件。当前 Android 共享运行时仍链接 Assimp，2D 项目也需要满足该链接依赖。

已有兼容库时，可在构建命令中传入绝对路径。

```sh
gradle -p android :app:assembleDebug \
  -PmikanProjectPath="$PWD/templates/pixel-hop-twins" \
  -PmikanAssimpLibrary="/path/to/arm64-v8a/libassimp.so"
```

需要从源码生成时，下面的配置对应当前已验证的 arm64、API 26、共享 C++ 运行库及 16 KB 段对齐。在仓库根目录获取官方 Assimp 6.0.4 源码并构建。

```sh
mkdir -p out/deps
git clone --depth 1 --branch v6.0.4 https://github.com/assimp/assimp.git out/deps/assimp-6.0.4
export ANDROID_NDK_ROOT="$ANDROID_HOME/ndk/28.2.13676358"
cmake -S out/deps/assimp-6.0.4 -B out/build/assimp-android \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  -DASSIMP_BUILD_TESTS=OFF -DASSIMP_BUILD_ASSIMP_TOOLS=OFF \
  -DASSIMP_BUILD_SAMPLES=OFF -DASSIMP_BUILD_DOCS=OFF -DASSIMP_NO_EXPORT=ON \
  -DASSIMP_BUILD_ZLIB=ON -DASSIMP_BUILD_M3D_IMPORTER=OFF -DASSIMP_BUILD_M3D_EXPORTER=OFF \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384"
cmake --build out/build/assimp-android --target assimp --parallel 2
```

生成库位于 `out/build/assimp-android/bin/libassimp.so`，将这个绝对路径传给 `mikanAssimpLibrary`。动态库需与仓库头文件的接口及所选 ABI 兼容。[Assimp 6.0.4 源码](https://github.com/assimp/assimp/releases/tag/v6.0.4)

## 构建与版本

安装 Gradle 8.13 后可直接使用 `gradle -p android`。仓库也提供 Wrapper，下例的 `gradle` 可替换为 `bash android/gradlew`；Windows 使用 `android\gradlew.bat`。

```sh
gradle -p android :app:assembleDebug \
  -PmikanProjectPath="$PWD/templates/pixel-hop-twins" \
  -PmikanAssimpLibrary="$PWD/out/build/assimp-android/bin/libassimp.so" \
  -PmikanVersionCode=1 -PmikanVersionName=1.0 \
  --max-workers=2 --console=plain
```

将项目路径换成自己创建的项目目录即可。相对 Gradle 属性路径以 `android/` 工程根为基准，绝对路径便于复现。`mikanVersionCode` 默认 `1`，`mikanVersionName` 默认 `1.0`；更新安装时递增版本号并沿用原签名。

`assembleDebug` 生成的 APK 位于 `android/app/build/outputs/apk/debug/app-debug.apk`，使用本机 Android Debug 签名。当前 Gradle 明确传递 `CMAKE_BUILD_TYPE=Release`，因此 Debug APK 的 C++ 部分使用 Release 配置和 `-O2 -DNDEBUG`。判断 native 配置时查看 `android/app/.cxx/Release/` 下的缓存。

`assembleRelease` 使用启用资源压缩的 Release APK 配置，工程中的该配置仍设为 `debuggable true`，发布签名需另行配置。

可在依赖齐备后追加 `--offline`。离线构建要求 Gradle 8.13 分发包、AGP 与 Maven 依赖、SDK、NDK、CMake、AAR、Assimp、子模块和资源均已缓存或安装。首次联网构建完成后保留对应 `GRADLE_USER_HOME`。

改变项目绝对路径可能改变 Android CMake 配置并触发较多原生代码重新编译。日常修改保留项目路径、Gradle 与 `.cxx` 缓存，优先增量构建；缓存目录不属于运行资源。

## 资源与许可随包收集

Android 将 `projectRoot/resourceRoot` 作为 `scene` 和 `assets` 的收集根，并将文件放入 APK 的 `assets/`。桌面端的 `ResolveAssetPath` 按项目根解析；跨平台项目沿用模板的 `"resourceRoot": "."` 可以保持两端路径一致。

`project.json` 中的 `codeRoot` 保存 C++ 源码，`assets` 声明运行文件。Gradle 会跟踪场景的 UI、贴图及 TMX、glTF 等已支持的引用。玩法代码自行读取的关卡、音频描述、PCM 和 `fonts.masks` 等文件应显式加入 `assets`。许可目录同样加入清单；模板已包含完整 MIT、CC0、Skia 和字体声明。

`"runtimeProfile": "2d"` 选择独立 2D 启动和所需的引擎资源集合。使用引擎 Box2D 或音频管理器的项目可声明 `"runtimeFeatures": ["physics2d", "audio"]`。Pixel Hop Twins 的音效直接使用 SDL 音频接口，构建 SDL 时需要 `SDL_AUDIO=ON`；当前 Android SDL 配置启用音频。构建结果仍包含共享运行时代码与所链接的动态库。

Gradle 的 `syncMikanProjectAssets` 任务将文件同步到 `android/app/build/generated/mikan-project-assets`，生成启动场景与运行配置标记。它会检查文件缺失、LFS 指针、越界路径和保留文件名。资源失败时先修复日志中的首个错误，再处理后续配置报错。

## 检查 APK 与设备运行

下列工具来自已安装的 Build Tools 和 Platform Tools。

```sh
apk="$PWD/android/app/build/outputs/apk/debug/app-debug.apk"
build_tools="$ANDROID_HOME/build-tools/35.0.0"
"$build_tools/apksigner" verify --print-certs "$apk"
"$build_tools/aapt" dump badging "$apk"
unzip -Z1 "$apk" | rg '^(assets/|lib/)'
```

核对实际包名、版本、最低系统版本和 `arm64-v8a`，检查玩法素材和 `assets/licenses/` 是否齐全，再用 `unzip -l` 查找体积较大的条目。模板的默认包名为 `com.mikanengine.pixelhoptwins`；其他项目的后缀由项目目录名移除非字母数字字符后生成，可用 `aapt` 确认。

```sh
adb devices -l
adb install -r "$apk"
adb shell am start -n com.mikanengine.pixelhoptwins/com.mikanengine.MikanEngineActivity
adb logcat -v threadtime | rg 'mikan|Mikan|SDL|Vulkan|AndroidRuntime|FATAL'
```

设备需要支持 APK 的 ABI，以及 Manifest 声明的 OpenGL ES 3.0 和 Vulkan 版本。日志用于定位初始化、资源与驱动问题。构建、签名校验和桌面截图完成后，触控、音频、前后台切换及具体设备驱动仍需在真机验证。

## 16 KB 页对齐

项目的链接配置使用 `-Wl,-z,max-page-size=16384`。运行 ZIP 对齐检查后，再逐个检查 APK 内的 ELF 动态库；每个 `LOAD` 段的 `Align` 应至少为 `0x4000`。下面的 NDK 工具路径适用于 Linux 主机。

```sh
"$build_tools/zipalign" -c -P 16 -v 4 "$apk"
inspection_dir=$(mktemp -d)
unzip -q -j "$apk" 'lib/arm64-v8a/*.so' -d "$inspection_dir"
for library in "$inspection_dir"/*.so; do
  printf '\n%s\n' "$library"
  "$ANDROID_HOME/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" -lW "$library" | rg 'LOAD'
done
```

当前 `useLegacyPackaging=true` 使原生库按传统方式打包并在安装时解压，ELF 装载段仍需兼容设备页大小。逐一检查 Assimp、SDL3_image、SDL3 和 C++ 运行库的实际文件。预编译库出现 4 KB 段时，应使用 16 KB 对齐重新生成对应库，并再次打包检查。

## 常见问题

- `Unsupported class file` 或 Gradle 启动失败时，核对 `JAVA_HOME` 与 `gradle --version`，使用 JDK 17 或已验证的 JDK 21。
- `SDL3_image` Prefab 找不到时，检查 AAR 完整性、LFS 下载和 SDK CMake 版本。SDL3 的 Java 桥接与 native 源码共同来自同一子模块版本。
- `libassimp.so` 缺失、ABI 不符或符号找不到时，核对 `mikanAssimpLibrary` 指向的文件、NDK ABI、头文件版本和库的依赖。
- `Required Android asset is missing` 时，核对 `project.json`、大小写、`resourceRoot` 和代码直接读取的文件。修改源码目录后检查 `codeRoot` 与 `game` 对应的 `games/<game>/*.cpp`。
- 更新安装报签名或版本冲突时，使用原签名并递增 `mikanVersionCode`。音效无声时检查 SDL 音频构建开关、静音状态和设备音频日志。

## 本云环境示例

本任务已准备了专用工具目录，可使用下列环境文件和 Assimp 路径。它们属于当前云工作区，在其他机器上采用前文的本机安装路径。

```sh
source /workspace/.mikan-android/env.sh
gradle -p android :app:assembleDebug \
  -PmikanProjectPath="$PWD/templates/pixel-hop-twins" \
  -PmikanAssimpLibrary=/workspace/.mikan-android/build-assimp-android/bin/libassimp.so \
  --offline --max-workers=2
```

该环境文件配置 JDK 21、Gradle 8.13、SDK 36、NDK r28c 及缓存目录。文档命令覆盖构建与检查流程，真机结果以对应设备的运行记录为准。
