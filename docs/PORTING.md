# 游戏移植指南

本文面向通过 C++ 玩法模块移植 2D 游戏的开发者。先用固定输入验证逻辑，再检查资源、画面、声音和设备交互，便于定位差异。构建环境与包体生成分别见 [Linux 构建](LINUX.md)和 [Android 构建](ANDROID.md)，引擎概览见[主 README](../README.md)。可运行的完整范例见 [Pixel Hop Twins 模板](../templates/pixel-hop-twins/README.md)。

## 固定来源与还原目标

移植前记录原作仓库、提交哈希、代码许可，以及图片、字体、音频和关卡各自的授权。代码许可与素材许可可能分别声明，保存原始文本和来源记录，并将需要随包保留的文件显式列入 `assets`。研究项目可以保留在本地私有目录，移植工作本身不需要发布源码或产物。

Pixel Hop Twins 模板固定原作版本为 `9835295888c7cb8afa795ca2a31707a65167c2ea`，分别保留 MIT、素材 CC0、字体与 Skia BSD 许可。[模板来源与许可](../templates/pixel-hop-twins/licenses/UPSTREAM.md)

先约定验收范围。玩法还原需要比较坐标、速度、碰撞、分数、状态转换和关卡数据。逐像素还原还需要固定参考浏览器、字体、逻辑分辨率、颜色与透明度运算、采样、抗锯齿、截图帧和时钟。设备分辨率与浏览器版本变化后，应重新定义比较基准。

建议保存原版固定版本、参考运行方式、输入序列和关键帧，覆盖开始、移动、受伤、死亡、暂停、过关与重启。建立基线后再做移动端操作布局，保留玩法画面的比较条件。

## 使用完整项目模板

在项目管理器中选择“Pixel Hop Twins 2D 示例”可创建完整项目。也可以复制 `templates/pixel-hop-twins` 后修改项目名称和玩法。其目录职责如下。

```text
pixel-hop-twins/
├── README.md
├── project.json
├── scenes/main.json
├── ui/mobile.ui.json
├── levels/level1.json、level2.json
├── games/pixel-hop-twins/
│   ├── GamePixelHopExports.cpp
│   ├── PixelHopGame.h、PixelHopGame.cpp
│   ├── NativeModel.h、NativeModel.cpp
│   ├── NativeRender.h、NativeRender.cpp
│   └── NativeAudio.h、NativeAudio.cpp
├── assets/
│   ├── sprites.json
│   ├── fonts.json、fonts.png、fonts.masks
│   └── audio.json、audio.f32
└── licenses/
    ├── MIT_LICENSE.txt、SKIA_LICENSE.txt、FONT_LICENSE.txt
    ├── CC0-1.0.txt、CC0_DECLARATION.md
    └── UPSTREAM.md、UPSTREAM_README.md
```

模板的清单可直接用于桌面与 Android 构建。

```json
{
  "formatVersion": 1,
  "name": "Pixel Hop Twins",
  "game": "pixel-hop-twins",
  "scene": "scenes/main.json",
  "resourceRoot": ".",
  "codeRoot": "games",
  "runtimeProfile": "2d",
  "assets": [
    "scenes/main.json",
    "ui/mobile.ui.json",
    "levels/level1.json",
    "levels/level2.json",
    "assets/sprites.json",
    "assets/fonts.json",
    "assets/fonts.png",
    "assets/audio.json",
    "assets/audio.f32",
    "assets/fonts.masks",
    "licenses/MIT_LICENSE.txt",
    "licenses/SKIA_LICENSE.txt",
    "licenses/FONT_LICENSE.txt",
    "licenses/CC0-1.0.txt",
    "licenses/CC0_DECLARATION.md",
    "licenses/UPSTREAM.md",
    "licenses/UPSTREAM_README.md"
  ]
}
```

`name` 是显示名称。`game` 是模块标识，应与场景中的 `game`、模块 `GetName()`、注册名和桌面导出名一致。`codeRoot` 从项目根目录解析，当前 Linux 项目构建收集 `<codeRoot>/<game>/*.cpp`，新增子目录中的源码需要相应调整构建规则。

模板采用 `resourceRoot: "."`，所有资源引用使用项目根目录内的相对路径。当前桌面 `ProjectManager::ResolveAssetPath()` 从项目根目录解析，`resourceRoot` 用于资源浏览范围。Android 打包脚本从 `<项目根>/<resourceRoot>` 查找场景和项目资源。沿用模板布局可保持两端一致，自定义资源根时需分别验证解析和打包结果。避免依赖工作目录、开发机绝对路径及 `../` 引用。引擎资源使用 `engine/` 前缀。[项目路径实现](../src/Core/ProjectManager.cpp) · [Android 项目入口](../android/app/build.gradle)

`assets` 应包含玩法实际读取的文件和许可。Android 会继续收集部分场景、UI、贴图和模型引用，自定义 JSON 中的任意字段没有通用依赖推导，字体遮罩、PCM 等数据应显式声明。当前 Linux 安装会复制项目目录并排除 `codeRoot` 和 `.git`，整理发布目录时还要检查开发参考、工具、缓存和私有资料。[Linux 项目安装规则](../cmake/LinuxProject.cmake) · [Android 资源收集规则](../android/mikan_project_assets.gradle)

## 原生模块与平台边界

模板由 `PixelHopGame` 实现 `IGameModule`，`NativeModel` 管理玩法状态，`NativeRender` 绘制画面，`NativeAudio` 管理音效。玩法逻辑使用 C++，资源转换脚本属于开发工具，运行包通过 Vulkan 和 SDL 执行，无需 HTML 页面或 JavaScript 运行时。

桌面构建生成 `libGamepixel-hop-twins.so` 并使用 C ABI 导出加载模块，Android 将玩法编入应用并静态注册。可直接沿用模板的导出方式。

```cpp
#include "PixelHopGame.h"

#if defined(__ANDROID__) || defined(MIKAN_OPENHARMONY)
#include "Game/GameManager.h"
namespace {
const bool registered = [] {
    Game::GameManager::GetInstance().Register(
        "pixel-hop-twins", [] { return &Game::PixelHopGame::GetInstance(); });
    return true;
}();
}
#else
#include "Game/PluginExport.h"
extern "C" MIKAN_GAME_MODULE_EXPORT const char* GetGameModuleName() {
    return "pixel-hop-twins";
}
extern "C" MIKAN_GAME_MODULE_EXPORT Game::IGameModule* CreateGameModule() {
    return &Game::PixelHopGame::GetInstance();
}
#endif
```

`OnSceneLoaded()` 在场景加载后重新绑定实体与资源。场景重载或停止后清理项目持有的实体、音频和输入状态，避免继续使用旧实体句柄。`OnUpdate()` 受引擎播放和暂停状态控制，`OnAlwaysUpdate()` 始终执行，`OnRenderUI()` 接收屏幕尺寸并绘制自定义内容。测试启动钩子 `OnGameplayTestStart()` 仅在测试器传入 `--auto-start-game` 时调用。[模块接口](../include/Game/IGameModule.h)

公共玩法代码避免 Win32 专属调用、盘符路径和平台脚本假设。通过项目路径解析与 SDL 文件接口读取资源，移动端的资产可能位于 APK 内。平台分支集中在文件、窗口、输入和音频等适配边界，保持模型可独立回放。

## 输入、时间与碰撞

- 输入区分按住、按下和释放。键盘使用 `InputSystem::IsDown/IsPressed/IsReleased`，触控使用 `Canvas2D::IsActionHeld/WasActionPressed`。同一帧内完成的短按也要保留边沿，多次固定步更新只消费一次按下事件。别名按键、同时左右、多指移动加跳跃、失焦和触摸取消都应验证。[输入接口](../include/Core/InputSystem.h) · [模板输入适配](../templates/pixel-hop-twins/games/pixel-hop-twins/PixelHopGame.cpp)
- 固定步长、更新顺序和原作数值类型会影响行为。Pixel Hop Twins 使用项目内的固定步模型承接帧时间，保留跳跃缓冲、离地宽限、提前松键和碰撞顺序。引擎回调的 `float deltaTime` 与模型需要的精度应明确转换，严格比较时还要关注表达式求值顺序与浮点收缩。
- 区分模拟时间、墙钟时间和音频时间。暂停通常冻结模拟，原作使用墙钟的装饰动画可能继续变化。截图与回放时固定这些时钟，避免把相位变化误判成渲染错误。
- 保留世界单位、AABB 边界、单向平台、落地修正、敌人与收集物的检测时机。先确认行为，再评估引擎碰撞设施是否符合语义。将已验证的通用缺陷修入核心，特定游戏规则留在模型中。

## 画面、UI 与音频

确定一套逻辑坐标，再映射到实际视口。Pixel Hop Twins 保持原作 640×400，通过等比缩放与留边适应设备。DPR、窗口像素数、安全区域和游戏世界坐标分别处理，输入命中采用绘制使用的同一变换。

移动端 UI 使用 `.ui.json`，由场景的 `uiDocuments` 引用。`stretchToViewport`、`scaleWithViewport`、`respectSafeArea` 控制画布布局，节点 `id` 与 `action` 用于绑定行为。触控按钮可通过 `Canvas2D::GetEntityLayout()` 获取布局后绘制，确保视觉与命中区域一致。`OnRenderUI()` 中覆盖全屏的内容会遮住先前绘制的 UI，模板在游戏画面之后绘制触控按钮。[UI 文档接口](../include/UI/UiDocument.h) · [移动端 UI 示例](../templates/pixel-hop-twins/ui/mobile.ui.json)

普通精灵通过 `Renderer2D::LoadTexture()` 注册纹理，再使用对应描述符绘制。像素图应明确选择最近邻采样，并核对 UV、透明度、颜色空间与层次。Signal Climb 的 Android 白色砖块排查说明，包内存在文件还不足以证明贴图已进入绘制路径，缺失纹理名会回退为白色。检查资源字节、加载返回值、注册名称和实际截图。[Renderer2D 接口](../include/Rendering/Renderer2D.h)

Pixel Hop Twins 为固定参考画面的逐像素还原，在项目内采用 CPU 整数像素合成，再将合并后的不透明矩形交给 `Renderer2D` 通过 Vulkan 提交。字体遮罩、几何覆盖率和混合运算包含来源明确的兼容实现。这是示例项目的渲染策略，引擎现有通用 API 提供精灵、矩形、三角形和 UI，完整 Canvas 语义仍需要继续抽象与验证。常规移植可优先使用精灵、Tilemap 和 UI，严格像素兼容会增加字体数据、CPU 开销、算法维护与回归测试成本。[模板渲染实现](../templates/pixel-hop-twins/games/pixel-hop-twins/NativeRender.cpp)

音效还原包括波形或采样、包络、音符延迟、重叠、静音时机与暂停语义。模板将原作合成音效预处理为 PCM，通过项目的 `NativeAudio` 使用 SDL 播放，构建需启用 SDL 音频。项目自行管理音频设备时，要处理启动、停止、后台与资源释放，避免与引擎音频管理器重复持有播放责任。[模板音频实现](../templates/pixel-hop-twins/games/pixel-hop-twins/NativeAudio.cpp)

## 数据格式与按需运行

关卡、场景和 UI 使用带版本的 JSON，便于编辑、审查和差异比较。大块像素遮罩、PCM、纹理和编译后着色器适合二进制资产，描述文件保留尺寸、格式、采样率和偏移信息。Pixel Hop Twins 将字体遮罩从大段 JSON 数组移到 `fonts.masks`，减少解析开销。只有出现启动、内存或读取瓶颈时，再增加缓存或烘焙流程，并保留可编辑源与明确的失效规则。

`runtimeProfile: "2d"` 在独立游戏模式中选择精简 2D 启动和渲染路径，跳过默认的 3D 世界、大气等准备。桌面使用 `--no-editor`，Android 由打包配置选择对应运行档。编辑器仍需要自身工具设施，该设置也未将所有 3D 代码从链接库中剔除。[运行档选择](../src/Core/EngineMain.cpp)

当前 `runtimeFeatures` 仅支持 `physics2d` 与 `audio`，并且仅用于 `2d` 运行档。需要引擎 Box2D 或音频管理器时显式添加对应功能，例如 `"runtimeFeatures": ["physics2d", "audio"]`。模板自行处理碰撞和音频，因此清单中没有此字段。功能选择与资源按需加载分别设计，现有字段未提供任意子系统依赖图或自动懒加载能力。[清单解析](../src/Core/ProjectManager.cpp)

独立 2D 引擎必需资源由共享的 `engine/runtime_2d_assets.list` 控制，包含基础字体、语言文件、启动图和 UI SPIR-V。玩法资源由项目清单声明。不要为一个玩法恢复整个 `engine` 目录，也不要通过增加游戏专属核心分支补齐包体遗漏。[2D 引擎资源清单](../engine/runtime_2d_assets.list)

## 最小验证流程

按 [Linux 指南](LINUX.md)准备依赖后，在仓库根目录构建范例与测试器。移植新项目时替换项目路径、场景和模块名。

```sh
cmake -S . -B out/build/port-check -DCMAKE_BUILD_TYPE=Release \
  -DMIKAN_CPU_ONLY=OFF -DMIKAN_PROJECT_PATH="$PWD/templates/pixel-hop-twins" -DSDL_AUDIO=ON
cmake --build out/build/port-check --target MikanEngine MikanTestRunner --parallel 2
out/build/port-check/MikanTestRunner \
  --project "$PWD/templates/pixel-hop-twins" --scene scenes/main.json \
  --game pixel-hop-twins --auto-start-game --frames 120 \
  --fixed-dt 0.016666666666666666 --dump-state "$PWD/out/pixel-hop-state.json"
out/build/port-check/MikanEngine --project "$PWD/templates/pixel-hop-twins" \
  --no-editor --headless --frames 5 --fixed-dt 0.016666666666666666 \
  --screenshot-frame 3 --screenshot-path "$PWD/out/pixel-hop.png"
```

状态导出用于检查模型，截图命令仍需要可用 Vulkan 环境。`--input-replay` 当前支持移动与跳跃回放，暂停、静音、重启和触控需另外验证。比较时记录初始状态、模拟步长、输入序列、帧编号与时钟。只启动若干帧适合作为冒烟检查，还需覆盖完整玩法流程。[测试器参数](../src/Core/GameplayTestMain.cpp) · [截图参数](../src/Core/EngineCommandLine.cpp)

回放文件使用下面的 JSON 格式，可保存为 `out/pixel-hop-replay.json`。帧区间为左闭右开，事件按顺序排列且不能重叠；每段输入从 `defaultInput` 继承省略字段，事件之间回到默认输入。移动分量范围为 `[-1, 1]`，跳跃值表示按住状态。这是短回放样例，完整通关需要对应关卡的输入序列。

```json
{
  "schemaVersion": 1,
  "frames": 120,
  "defaultInput": {"move": [0, 0], "jump": false},
  "events": [
    {"startFrame": 0, "endFrame": 30, "move": [1, 0]},
    {"startFrame": 30, "endFrame": 45, "move": [1, 0], "jump": true},
    {"startFrame": 45, "endFrame": 90, "move": [1, 0]}
  ]
}
```

```sh
out/build/port-check/MikanTestRunner \
  --project "$PWD/templates/pixel-hop-twins" --scene scenes/main.json \
  --game pixel-hop-twins --auto-start-game --fixed-dt 0.016666667 \
  --input-replay "$PWD/out/pixel-hop-replay.json" --dump-state "$PWD/out/pixel-hop-replay-state.json"
```

未显式传入 `--frames` 时，测试器使用回放文件的总帧数。回放最多 1,000,000 帧、4,096 段事件，不能与 `--scripted-input` 同时使用。

- 核对全部关卡与状态转换，比较位置、速度、碰撞结果、分数和生命值，重复死亡、暂停、继续与重启。
- 按逻辑分辨率比较画面，分别检查 HUD、场景、精灵、特效与叠层。记录差异像素数、最大误差和容差，逐像素目标使用原始像素比较。
- 按 [Android 指南](ANDROID.md)构建 APK，检查项目资产、许可和引擎必需资源。安装到目标设备测试触控并发、短按、安全区域、后台恢复、静音和声音，构建成功只能验证构建链路。
- 从独立安装目录启动 Linux 包，确认资源路径不依赖源码目录。检查两端产物中的开发工具、私有参考和未使用资源，保留必要许可。

每次移植将发现归到有证据的层次。输入边沿、资源加载和通用 UI 缺陷适合修入引擎，玩法规则与固定参考渲染兼容保留在项目中。通用能力经第二个用例验证后再提炼接口，可减少核心长期维护的特例。
