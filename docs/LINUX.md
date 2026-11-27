# Linux 构建、运行与打包

[文档索引](README.md) · [Android 构建](ANDROID.md) · [游戏移植指南](PORTING.md)

以下命令在引擎仓库根目录执行，使用 Bash。先用内置的 `templates/pixel-hop-twins` 验证环境，再把项目路径替换为自己创建的项目。

## 1. 选择构建类型

| 配置 | 用途 | 主要产物 |
| --- | --- | --- |
| `linux-runtime` | Vulkan 桌面编辑器、独立游戏、项目打包 | `MikanEngine`、`libGame.so`、`libEditor.so`、项目玩法模块 |
| `linux-cpu` | 不创建窗口或 Vulkan 设备的 CPU 单元与契约验证 | CPU 测试程序和 CTest 报告 |

Linux 上 `MIKAN_CPU_ONLY` 默认是 `ON`。要运行游戏，使用 `linux-runtime` preset；手写 CMake 配置时必须传 `-DMIKAN_CPU_ONLY=OFF`。两种配置使用不同构建目录，避免复用 SDL 后端缓存。

## 2. 安装依赖

桌面构建需要 Linux x86_64、支持 C++20 的编译器（建议 GCC 12+）、CMake 3.21+、Ninja、Git、Git LFS、glslangValidator、Vulkan、Assimp、X11 开发包和 SDL 音频后端。Debian/Ubuntu 示例：

```sh
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build git git-lfs glslang-tools \
  libvulkan-dev libassimp-dev libx11-dev libxext-dev libxrandr-dev \
  libxcursor-dev libxi-dev libxfixes-dev libxrender-dev libxkbcommon-dev \
  libasound2-dev
```

初始化固定版本子模块并下载 LFS 资源：

```sh
git submodule update --init --depth 1 -- \
  dependencies/JoltPhysics dependencies/glm dependencies/box2d \
  dependencies/FastNoiseLite dependencies/SDL3 dependencies/SDL3_image \
  dependencies/vulkan dependencies/meshoptimizer dependencies/zlib
git lfs install --local
git lfs pull
```

Linux 不需要初始化 Windows 的 FSR2/DLSS 运行依赖。图片和字体如果仍是 `version https://git-lfs.github.com/spec/v1` 开头的文本指针，应先完成 LFS 下载；文件存在不等于资源有效。

## 3. 构建并启动示例

```sh
cmake --preset linux-runtime \
  -DMIKAN_PROJECT_PATH="$PWD/templates/pixel-hop-twins" \
  -DSDL_AUDIO=ON
cmake --build --preset linux-runtime --parallel 2

out/build/linux-runtime/MikanEngine \
  --project "$PWD/templates/pixel-hop-twins" --no-editor
```

`MIKAN_PROJECT_PATH` 用于选择要编译的玩法源码，运行时的 `--project` 用于选择场景和资源；两者应指向同一项目。CMake 读取 `project.json`，将 `<codeRoot>/<game>/*.cpp` 编译为 `libGame<game>.so`，放在宿主程序旁。切换项目后需要重新配置和构建。

不带 `--no-editor` 可进入项目编辑器；不带 `--project` 启动则进入项目管理器：

```sh
out/build/linux-runtime/MikanEngine
```

在“模板”页选择“Pixel Hop Twins 2D 示例”，填写名称和位置即可复制独立项目。模板会一并复制 C++ 代码、素材、关卡、触控 UI 与 `licenses/`，不依赖原开发项目。新项目需要按上面的方式编译后才能运行其玩法。Linux 修改玩法代码后重新构建并重启；当前没有 Windows F5 编译热重载的同等流程。

如果不使用 preset，等价配置为：

```sh
cmake -S . -B out/build/my-linux-game -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMIKAN_CPU_ONLY=OFF \
  -DMIKAN_PROJECT_PATH="$PWD/templates/pixel-hop-twins" -DSDL_AUDIO=ON
cmake --build out/build/my-linux-game --target MikanEngine --parallel 2
```

SDL 音频支持当前默认编译开启，设备由游戏按需初始化。旧构建缓存可能保留 `SDL_AUDIO=OFF`，所以示例显式开启。头文件、库位于非系统目录时，可使用 `CMAKE_PREFIX_PATH`；Vulkan 与 Assimp 库也可分别通过 `MIKAN_VULKAN_LIBRARY`、`MIKAN_ASSIMP_LIBRARY` 指定。

## 4. 生成独立 Linux 游戏目录

先完成第 3 节构建，再安装到一个新的输出目录。输出目录应位于所选项目和引擎资源目录之外：

```sh
cmake --install out/build/linux-runtime \
  --prefix "$PWD/out/publish/pixel-hop-twins-linux" --component MikanProject

out/publish/pixel-hop-twins-linux/run.sh
```

产物结构大致如下：

```text
pixel-hop-twins-linux/
├── MikanEngine
├── libGame.so
├── libGamepixel-hop-twins.so
├── run.sh
├── engine/                   # 运行所需引擎资源
├── projects/game/            # 场景、素材、关卡、UI、licenses、project.json
├── dependencies/             # 保留的第三方许可
├── LICENSE
└── THIRD_PARTY_NOTICES.md
```

`run.sh` 自动定位包内项目并以独立游戏模式启动，可继续附加运行参数。此组件不安装编辑器，也不安装项目 `codeRoot` 中的玩法源码。需要压缩包时：

```sh
tar -C out/publish -czf out/pixel-hop-twins-linux-x86_64.tar.gz pixel-hop-twins-linux
```

当前 Linux 安装器复制项目目录，排除 `codeRoot` 和 `.git`；并非只复制 `assets[]`。因此开发参考页、提取工具、缓存和历史构建应放在待发布项目之外。`project.json` 仍应维护完整资源和许可清单，供 Android 收集和跨平台检查使用。

2D 项目设置 `"runtimeProfile": "2d"` 时，引擎资源使用 `engine/runtime_2d_assets.list`；额外引擎素材通过 `assets[]` 中的 `engine/` 路径声明。`full` 项目安装完整 `engine/` 资源树。外链资源应先复制到项目内并修改引用，再进行打包。

目录可搬移，但不是静态链接的单文件应用。目标系统仍需兼容的 glibc/C++ 运行库、Assimp、Vulkan loader 和驱动。可用 `ldd out/publish/pixel-hop-twins-linux/libGame.so` 检查依赖；面向较旧发行版时，在兼容的系统镜像中构建。

## 5. 验证运行与资源完整性

先从独立目录启动，确认它可以找到自己的素材与许可证。再进行固定帧渲染检查：

```sh
out/publish/pixel-hop-twins-linux/run.sh \
  --headless --frames 3 --fixed-dt 0.016666667 \
  --screenshot-frame 2 --screenshot-path "$PWD/out/pixel-hop-linux.png"
```

这里的 `--headless` 仍然执行 Vulkan 渲染，需要可用的显示后端或支持无窗口表面的 Vulkan 驱动。没有桌面时，可尝试 Mesa Lavapipe；以下 ICD 路径是常见 Debian/Ubuntu 安装路径，应按实际系统替换：

```sh
sudo apt-get install mesa-vulkan-drivers vulkan-tools
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
out/publish/pixel-hop-twins-linux/run.sh \
  --headless --frames 3 --fixed-dt 0.016666667 \
  --screenshot-frame 2 --screenshot-path "$PWD/out/pixel-hop-offscreen.png"
```

`dummy` 音频适用于自动化启动检查，不证明实际扬声器播放正常。驱动不支持 SDL 无窗口 Vulkan surface 时，改用 X11 会话运行。

玩法逻辑测试可使用完整桌面构建中的 `MikanTestRunner`；它不初始化 SDL Video 或 Vulkan：

```sh
cmake --build out/build/linux-runtime --target MikanTestRunner --parallel 2
out/build/linux-runtime/MikanTestRunner \
  --project "$PWD/templates/pixel-hop-twins" \
  --scene scenes/main.json --game pixel-hop-twins --auto-start-game \
  --frames 120 --fixed-dt 0.016666667 --dump-state "$PWD/out/pixel-hop-state.json"
```

需要确定性回放时使用它的 `--input-replay /path/to/replay.json`，格式与限制见[移植指南](PORTING.md)。此参数属于玩法测试宿主，不是图形宿主的通用录制功能。

## 6. CPU 验证构建

只验证 CPU 核心时，不需要 Vulkan 驱动、X11、Assimp 或音频后端。仍需要编译器、CMake、Ninja、Git、pthread/iconv，以及 SDL/Jolt/Box2D/GLM/Vulkan 头文件子模块。Python 用于可选的数学参考测试。

```sh
git submodule update --init --depth 1 -- \
  dependencies/JoltPhysics dependencies/glm dependencies/box2d \
  dependencies/SDL3 dependencies/vulkan
git lfs install --local
git lfs pull --include='engine/textures/restir/paired-neighbors-3-16.u32.bin' --exclude=''
cmake --preset linux-cpu
cmake --build --preset linux-cpu --parallel 2
ctest --preset linux-cpu --output-junit results.xml
```

测试覆盖任务调度、ECS、UTF-8 路径与资源标识、Box2D、MMD/Jolt、VMD 编码、视口布局及部分渲染数学契约。详细日志位于 `out/build/linux-cpu/Testing/Temporary/LastTest.log`。测试数量随版本变化，以当前 CTest 清单为准。

CPU 配置不会生成 `MikanEngine`。可选的软件 Vulkan 测试另外需要相应依赖，见 [SOFTWARE_VULKAN.md](SOFTWARE_VULKAN.md)、[SOFTWARE_RAY_QUERY.md](SOFTWARE_RAY_QUERY.md) 和 [VOX_TESTING.md](VOX_TESTING.md)。

## 7. 常见问题与运行范围

| 现象 | 检查与处理 |
| --- | --- |
| 找不到 `MikanEngine` 构建目标 | 使用 `linux-runtime` 或显式设置 `MIKAN_CPU_ONLY=OFF`，不要复用 CPU 构建目录 |
| 共享库链接报 relocation / `-fPIC` | 当前 Linux 构建会开启位置无关代码；升级后重新配置，对旧静态依赖必要时使用新的构建目录 |
| 缺少 X11、Vulkan、Assimp 或 glslang | 安装第 2 节开发包，检查 CMake 实际找到的头文件和库 |
| 提示无法定位引擎根或 SPIR-V | 通过 `MikanEngine` 目标构建；它依赖 `CompileShaders`。独立目录按安装流程生成，保留 `engine/shaders/spv/` |
| 项目打开了但玩法不运行 | 核对编译项目与 `--project`、`game`、模块导出名、`libGame<game>.so` 和模块依赖 |
| 贴图白色、字体缺失、资源打开失败 | 检查路径大小写、项目相对路径、LFS 实体文件、独立安装目录内容及加载日志 |
| 有画面没有声音 | 检查 `SDL_AUDIO=ON`、ALSA/实际音频设备、是否误用了 dummy，以及项目 PCM/音效资源 |
| 只在开发目录能运行 | 从安装目录的 `run.sh` 启动，排查绝对路径、外链、遗漏素材和系统动态库 |

独立 `2d` 运行档直接使用 2D swapchain 绘制路径，按声明初始化可选系统。`runtimeFeatures` 可启用 `physics2d` 和引擎的 `audio`；Pixel Hop Twins 自己使用 SDL 音频流，不需要额外开启引擎 AudioManager。编译支持某功能不等于启动时就初始化该功能。

编辑器与 `full` 运行档使用完整渲染路径，其中的大气 LUT 首次生成和缓存仍可能产生启动成本。2D 精简运行档不是将所有 3D 代码从 `libGame.so` 编译移除。Linux KTX2 解码及 DLSS 当前不可用，NRD 在默认 Linux preset 中关闭。不同渲染档、扩展和 GPU 的可用能力应分别验证。

本轮移植已验证 Pixel Hop Twins 的 Linux 独立 2D 可见画面、安装目录启动和两关确定性通关回放；不代表所有 3D 渲染特性都已验证。已有专项结果另见 [KNIGHT_540P.md](KNIGHT_540P.md)、[SKY_RAY_QUERY.md](SKY_RAY_QUERY.md) 和 [SOFTWARE_NRD.md](SOFTWARE_NRD.md)。

## 8. 可选视频输出

安装 FFmpeg 后，可从实际渲染帧导出 H.264 MP4：

```sh
out/publish/pixel-hop-twins-linux/run.sh --headless --frames 130 \
  --video-output "$PWD/out/gameplay.mp4" --video-encoder ffmpeg \
  --video-size 640 360 --video-fps 30 --video-warmup 10
```

上例包含 10 帧预热和 120 帧视频，输出 30 FPS 的 4 秒片段。视频捕获需要 Vulkan 表面，不能使用纯 CPU 验证构建替代。

当前云端会话另有 `/workspace/.mikan-deps/env.sh` 用于设置本机依赖前缀和 Lavapipe 路径；仅在该文件存在的环境中按需加载。它不是仓库通用构建前提。
