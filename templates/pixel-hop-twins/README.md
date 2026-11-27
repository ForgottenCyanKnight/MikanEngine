# Pixel Hop Twins

这是 stm1978 的 Pixel Hop Twins 原生移植模板，使用 MikanEngine 的 C++ 玩法模块、Vulkan 渲染和 SDL 音频。模板包含原作的两个关卡、角色、收集物、敌人、Prisma 能力和游戏状态，逻辑分辨率为 640×400。创建项目后可直接运行并继续修改。

桌面端使用 A/D 或左右方向键移动，空格、W 或上方向键跳跃，P 暂停，M 静音，Enter 重试、继续或重新开始。移动端提供适配安全区域的触控按钮。

- `games/pixel-hop-twins/` 保存原生玩法、渲染和音频代码。
- `assets/` 保存精灵矩阵、字体图集与二进制遮罩、音效 PCM 与描述文件。
- `levels/` 保存两个原作 JSON 关卡，`scenes/` 和 `ui/` 保存引擎场景与移动端界面。
- `licenses/` 保存原作 MIT、素材 CC0 声明、字体和 Skia 许可，以及固定版本的来源记录。
- `project.json` 声明运行入口和打包文件，资源与许可均按项目根目录解析。

依照引擎主 README 配置依赖后，从引擎仓库根目录构建模板。

完整流程与常见问题见引擎仓库内的 `docs/LINUX.md`、`docs/ANDROID.md` 和 `docs/PORTING.md`。模板复制到外部项目后，也从引擎仓库打开这些指南。

```sh
cmake -S . -B out/build/pixel-hop-twins -DCMAKE_BUILD_TYPE=Release -DMIKAN_CPU_ONLY=OFF -DMIKAN_PROJECT_PATH="$PWD/templates/pixel-hop-twins" -DSDL_AUDIO=ON
cmake --build out/build/pixel-hop-twins --target MikanEngine --parallel 2
cmake --install out/build/pixel-hop-twins --prefix "$PWD/out/Pixel-Hop-Twins" --component MikanProject
```

生成的 `out/Pixel-Hop-Twins/run.sh` 启动独立游戏。构建从模板创建的新项目时，将 `MIKAN_PROJECT_PATH` 指向新项目目录。`SDL_AUDIO=ON` 用于编译原生音效所需的 SDL 音频支持。

Android 构建使用同一份项目清单。配置好 SDK、NDK 和 Gradle 后，从仓库根目录运行。

```sh
gradle -p android :app:assembleDebug -PmikanProjectPath="$PWD/templates/pixel-hop-twins"
```

来源版本为 `9835295888c7cb8afa795ca2a31707a65167c2ea`。完整许可与作者的 CC0 声明随项目资源一起打包，具体归属见 `licenses/UPSTREAM.md`。
