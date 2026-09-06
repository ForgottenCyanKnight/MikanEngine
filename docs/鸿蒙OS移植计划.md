# MikanEngine → 鸿蒙 OS 移植计划

> 文档日期：2026-09-01 · 状态：**可行性已分析，待 P0 验证**
> 前置阅读：`README.md`（项目大纲）、`docs/安卓构建与真机测试指南.md`（Android 移植先例，本计划大量复用其模式）
> 结论摘要：**技术可行，单人 3~5 周**。最大变量 = SDL3 对鸿蒙的后端支持状态、目标设备 Vulkan 扩展可用性。

---

## 1. 背景与目标

将 MikanEngine（C++17 + Vulkan + SDL3 + ImGui）移植到鸿蒙 OS。
参考先例：引擎已完整走通 Android arm64 移植（`#ifdef ANDROID` 遍布 20+ 文件、真机指南、触屏适配），鸿蒙移植路径与 Android 高度同构。

目标形态：
- **目标 A（OpenHarmony）**：原生 Vulkan 应用，贴近现有 Android 开发习惯 → 技术验证首选
- **目标 B（HarmonyOS NEXT）**：官方商用形态，Stage 模型 + NAPI 壳 + ArkTS 入口（P3 阶段）

## 2. 技术栈 × 鸿蒙支持矩阵（2026-08-26 已核实）

| 引擎依赖 | 可行性 | 说明 |
|---|---|---|
| SDL3（窗口/输入/事件/surface） | ⚠️ 最大变量 | 引擎使用的预编译 SDL3 3.4.2（`dependencies/SDL3/`，无源码）`SDL_platform_defines.h` 中**无 HARMONYOS/OHOS 平台宏** → 该包无鸿蒙后端。路径见 §4 |
| Vulkan | ✅ 可用需验证 | 引擎扩展需求保守：`VK_EXT_descriptor_indexing`、`VK_KHR_buffer_device_address`、`VK_KHR_swapchain`（+ debug utils）。OpenHarmony 4.x 起提供 Vulkan；NEXT（Rosen + 麒麟 Maleoon）已由 Unity/Unreal/Cocos 官方适配验证。**buffer_device_address 需真机确认**（不支持仅影响 BLAS，已有懒构建可跳过） |
| ImGui | ✅ | 官方 desktop Vulkan 后端（`ImGui_ImplVulkanH_*`），surface 到手即可用 |
| Jolt / Box2D / assimp / msdfgen | ✅ 直接编译 | 纯 C++ 无平台依赖 |
| miniaudio | ⚠️ 需后端 | `AudioManager.cpp` `ma_engine_init` 默认后端枚举；官方无鸿蒙后端 → 写 OHAudio custom backend |
| CMake 构建 | ✅ | DevEco Studio 原生支持 CMake + ohos-clang toolchain（用法同 Android NDK）；在 CMakeLists 仿写 `OHOS` 分支 |
| 文件系统/资产 | ✅ 可复用 | Android 分支已解决 asset 直读 + 沙盒路径；鸿蒙路径语义（`/data/storage/...` + 原生资源）同构 |
| 输入/触屏 | ✅ 可复用 | Android touch/虚拟摇杆/按键适配直接生效 |
| 游戏插件化 / hot reload | ✅ | 插件 DLL → 鸿蒙 `.so` 加载机制；编辑器模式可禁用（`--no-editor`），游戏模式优先 |

## 3. 关键事实（锚定决策）

1. **引擎对 SDL 的使用已全面 SDL3 化**（47 个文件引用；`SDL_IOFromFile` 34 处 / `SDL_ReadIO` 39 / `SDL_CloseIO` 62 / `SDL_iostream` / `SDL_Event` SDL3 风格）→ **不可降级 SDL2**（SDL2 用 `SDL_RWops`，事件枚举不同）。同时意味着 SDL3 的 IO 抽象在鸿蒙上自动屏蔽路径差异——是加分项。
2. **SDL 平台差异已收敛**：`EngineMain.cpp:803` 单点 `SDL_Init(SDL_INIT_VIDEO|GAMEPAD|AUDIO)`；`EngineMain.cpp:821` `SDL_WINDOW_VULKAN|HIGH_PIXEL_DENSITY`；surface 由 SDL 创建 + `CreateAndroidVulkanSurface`（EngineMain.cpp:267）的 `#ifdef ANDROID` 分支。
3. **引擎实际从 SDL 拿的核心只有 5 个面**：Init / 建窗口 / 事件循环 / `SDL_Vulkan_CreateSurface`（5 处）/ `SDL_GetBasePath`（12 处）。
4. **鸿蒙原生提供画面路径**：`OHNativeWindow` + `vkCreateOhosSurfaceKHR`（OpenHarmony Vulkan surface 扩展）——不依赖 SDL 也能出画面。
5. **测试基础设施可平移**：分层测试（MikanTestRunner CPU 层 + `EngineMain --headless` 渲染层）+ `--dump-state` + `--frames N` 渲染回归可原样在真机跑，是移植验收的度量尺。

## 4. 三条路线（推荐 A 为主、B 保底）

### A. 给 SDL3 补 ohos backend（首选，引擎零改动）
- 将 SDL2 官方 HarmonyOS video backend（SDL 2.28+，`__HARMONYOS__` + OHNativeWindow）移植到 SDL3。
- SDL3 backend API 与 SDL2 高度相似；SDL 官方也在推进该平台。
- 依赖：把预编译包换成 SDL3 源码构建（纳入 `dependencies/SDL3/` 或独立 build）。
- 工作量 ~1~2 周 · 引擎层一行不改。

### B. 引擎侧薄后端（SDL3 短期不支持的保底）
写 `PlatformOHOS` 薄层，仅实现 5 个面：
1. 窗口：`OHNativeWindow`（native window handle）
2. 表面：`vkCreateOhosSurfaceKHR` → 接入现有 swapchain 管理（ImGui_ImplVulkanH）
3. 事件：OHOS 触摸/按键事件 → 引擎事件映射（复用 Android 触屏适配）
4. 文件路径：`SDL_GetBasePath` 替代（OHOS 沙盒/资源路径）
5. 音频：miniaudio custom backend → OHAudio（独立解耦，可后置）
挂 `#ifdef OHOS`，与现有 `#ifdef ANDROID` 模式平级。工作量 ~1 周 + 触屏复用；引擎改动仅条件编译，不动结构。

### C. 完整平台抽象层（不推荐现在做）
IPlatform 接口（Window/Event/Input/FileSystem/Audio）+ 各平台实现。商业引擎形态，但项目已有完整工作代码，为单一新平台重写隔离层不划算——**抽象层这笔债应在"第三个平台出现时"才还**。

## 5. 阶段路线图（3~5 周）

### P0 验证（1 周）——先钉死"鸿蒙上能否出画面"
- [ ] 核查 SDL3 官方鸿蒙支持现状（官方 wiki supported platforms / 仓库 issue / 预编译包渠道）→ 决定走 A 还是 B
- [ ] DevEco Studio 建最小 native（C++）工程
- [ ] Vulkan 初始化：`vkCreateInstance` → `OHNativeWindow` surface → `vkCreateOhosSurfaceKHR` → swapchain → Present 一帧
- [ ] ImGui Vulkan 后端画一帧测试界面
- [ ] 真机枚举 `vkEnumerateDeviceExtensionProperties`：确认 `VK_KHR_buffer_device_address` / `VK_EXT_descriptor_indexing` 是否存在
- [ ] OHAudio 出声（单独验证，解耦音频）

**验收**：目标设备出现 Vulkan 渲染画面 + ImGui 叠加 + 音频有声。产出 P0 报告（设备信息 / 扩展清单 / SDL3 决策 A 或 B）。

### P1 平台层（1~2 周）
- [ ] 路线 A：SDL3 ohos backend 移植并本地构建；或路线 B：`PlatformOHOS` 薄层（5 面）+ `#ifdef OHOS`
- [ ] CMakeLists 加 `OHOS` 分支（仿 Android 分支；ohos-clang toolchain）
- [ ] 引擎入口/生命周期（Stage 模型下 native 附加）接入；`--no-editor` 游戏模式优先
- [ ] 资源路径：复用 Android 资产解析（asset 直读 + 沙盒路径）

**验收**：`EngineMain --headless --frames N --dump-state <path>`（鸿蒙渲染链）在真机 exit=0 且 dump 与基线一致。

### P2 全量（1~2 周）
- [ ] 触屏输入 / 虚拟摇杆 / 按键适配落地（复用 Android 实现）
- [ ] 音频后端落地（miniaudio custom backend 或 OHAudio 直桥）
- [ ] 回归：跑现有渲染层场景（snake / baka3d / terrain 等）真机验证
- [ ] 性能基线：GPU timings、后处理半分辩率等移动带宽决策（复用 Android 那套判断）

**验收**：一个完整游戏（如 baka3d 或 terrain-prototype）在鸿蒙真机可玩，帧率/发热可接受。

### P3 打磨（按需）
- [ ] HarmonyOS NEXT 加壳：ArkTS 入口 + NAPI bridge + Stage 模型生命周期
- [ ] 发布流程（对标 `publish.ps1 -Project`：鸿蒙 .so + 资源 + 图标）
- [ ] 文档：`docs/鸿蒙构建与真机测试指南.md`（对标安卓指南）

## 6. 风险清单（按概率×影响排序）

| # | 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|---|
| 1 | SDL3 无鸿蒙后端 | 高（未知） | 高 | 路线 B 已覆盖，非阻断；P0 第一项先核查 |
| 2 | 目标设备 Vulkan 扩展缺失（descriptor_indexing / buffer_device_address） | 中 | 中 | BLAS/特定管线条件化跳过（已有懒构建基础设施）；P0 真机枚举 |
| 3 | 麒麟 Maleoon 驱动行为差异 | 中（未验证） | 中 | 大引擎已验证 Vulkan 可用；P2 尽早接真机 |
| 4 | miniaudio 无鸿蒙后端 | 确定 | 低 | OHAudio custom backend（数天），与渲染解耦 |
| 5 | NEXT vs OpenHarmony 形态选择错误 | 中 | 中 | 先 OpenHarmony 验证，后 NEXT 加壳；P0 明确目标 |
| 6 | SDL3 预编译包缺适配 → 需源码构建 | 中 | 低 | 路线 A 本身就含源码构建；SDL3 CMake 构建已成熟 |

## 7. 下一步行动（决策前置清单）

1. **查 SDL3 鸿蒙支持状态**（决定路线 A/B）——官方 wiki / GitHub issue / 社区预编译包
2. **锁定目标设备**（一台 OpenHarmony 或 NEXT 真机；确认 Vulkan 驱动版本）
3. **P0 最小工程**（DevEco native + Vulkan + ImGui + 音频）—— 1 周封顶
4. 产出 P0 报告后按 §5 进入 P1

## 附录：关键 API 与接入点速查

| 项 | 内容 |
|---|---|
| 鸿蒙窗口 | `OHNativeWindow`（NAPI 获取 native window handle）|
| 鸿蒙 Vulkan surface | `vkCreateOhosSurfaceKHR`（VK_KHR_ohos_surface 扩展，OpenHarmony 提供）|
| 鸿蒙音频 | `OHAudio`（Native API）→ miniaudio custom backend |
| 引擎接入点（路线 B） | `EngineMain.cpp:803`（SDL_Init）、`:821`（窗口 flags）、`:267`（CreateAndroidVulkanSurface 平行分支）、`AudioManager.cpp:48`（ma_engine_init）、`ProjectManager.cpp`（GetBasePath 替代）|
| 引擎扩展需求 | `VK_EXT_descriptor_indexing` / `VK_KHR_buffer_device_address` / `VK_KHR_swapchain`（VulkanManager.cpp 扩展枚举处核对）|
| 渲染回归度量 | `EngineMain --headless --frames N --fixed-dt 0.016666667 --dump-state <path>` exit=0 + dump 一致 |
| 参考先例 | `docs/安卓构建与真机测试指南.md`、`_backup_*` 前的 Android 移植提交 |