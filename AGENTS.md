# AGENTS.md — MikanEngine 工作指引

> **新对话快速入口：先读 `PROJECT_CONTEXT.md`。** 其中包含项目大纲、活跃代码树、关键入口、模块关系和默认扫描边界；只有任务涉及具体模块时再读取对应源码，禁止把 `D:\Engine project\backup`、`out/`、第三方依赖和大资源目录作为常规上下文扫描。

## 项目

- 位置：`D:\Engine project\vulkan engine`（路径含空格，命令中须引号）
- 自研 Vulkan 游戏引擎：C++17 + SDL3 + Vulkan + ImGui + Jolt + Box2D，构建 CMake + Ninja + MSVC，产物 `out\build\x64-Release\`
- **备份目录**：部分项目备份/历史副本位于 `D:\Engine project\backup`。该目录不属于当前源码树；默认不扫描、不编译、不提交、不移动或删除其中内容。需要恢复或引用时，必须使用绝对路径并先确认具体子目录/文件。
- **完整文档：先读 `README.md`**（项目大纲/编译测试/MCP/陷阱四部分）；命令细节见 `编译命令.md`；规划见 `docs/工业化开发计划.md`、`docs/开发路线图_2026H2.md`、`docs/待办问题.md`
- 架构：`Game.dll`(共享运行时) / `Editor.dll`(编辑器,可选) / `EngineMain.exe`(完整 SDL/Vulkan 宿主) / `MikanTestRunner.exe`(CPU-only 玩法测试宿主) / `games/<name>`(游戏插件 DLL，独立编译+热重载，改玩法不碰 Game.dll)

## 构建/测试（用现成工具，勿手拼长命令）

- **构建**：`powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Engine project\vulkan engine\tools\build.ps1" [-Target EngineMain|MikanTestRunner|Editor|Game|CompileShaders] [-KillEngine] [-CleanFirst] [-ConfigureIfMissing]`
  - 退出码：0 成功 / 1 失败 / 2 引擎在运行（加 `-KillEngine` 自动关闭）/ 3 环境错误
  - 改 Editor 代码无需单独 target（EngineMain 连带构建 Editor.dll）
- **游戏插件 DLL（`games/<name>`）独立编译**：`tools\compile_games.ps1`——引擎未运行时直接输出 `out\build\x64-Release\Game<name>.dll`（首次构建/发布形态开箱即用）；引擎运行时输出 `.tmp` 等编辑器热重载重命名（运行中 DLL 被锁定）。**若游戏不运行（如模型不转），先确认 `Game<name>.dll` 存在**（`[GameManager] LoadPlugin: no DLL for '<name>'` = 插件缺失）
- **分层自动测试**：玩法逻辑优先 `MikanTestRunner.exe --frames N --fixed-dt 0.016666667 --dump-state <path> --scene <绝对路径> --game x`；它不创建窗口、不初始化 SDL Video/Vulkan，覆盖场景/插件/脚本/物理/动画。渲染回归使用 `EngineMain.exe --headless --frames N ...`，覆盖 SDL Video/Vulkan/FrameRender/Present
  - 两层 dump 共用结构并带 `runtime_layer`：玩法=`gameplay-cpu`，渲染=`render-vulkan`；退出码 0 正常跑完 / 2 初始化或场景/插件加载失败 / 3 dump 失败
  - 旧 `EngineMain --headless-no-render` 仍可用，但会初始化 SDL/Vulkan，不再作为玩法层首选
  - `MikanTestRunner` 使用 `Game.dll` 中共享运行时代码，但通过 `RuntimeCapabilities` 禁止设备型渲染/音频/输入路径；它是 CPU-only 执行路径，不是单独复制一套 ECS/物理
- **场景校验**：`tools\validate_scene.ps1 <scene.json> [-CheckAssets]`（组件键/字段白名单 + 引用检查；schema 在 `tools\scene_schema.json`）
- **MCP**（若客户端已接入 `mikanengine`）：`build` / `validate_scene` / `run_gameplay_test` / `run_render_test` / `run_test`(兼容) / `read_dump` / `assert_state` / `engine_status` / `stop_engine`；玩法默认走 CPU-only runner，渲染显式走 EngineMain；每次运行使用独立 `out/build/x64-Release/mcp_runs/<run-id>/`，禁止用旧 `crash_log` 判断新测试

## 硬性约定

- **🚫 备份纪律（2026-08-26 起，防 AI 对话乱放备份）**：源码树内**禁止创建/遗留任何 `*.bak*` 文件**（含 `.bak-codex-*`、`.stripz.bak`、`*_20260824_timestamp.bak` 等一切变体）。备份只有两条路：**git**（项目已 git 化，改前 `git status` 看基线、大改动先提交 checkpoint 或 `git stash`）或**外部目录 `D:\Engine project\backup\`**（带日期子目录，恢复用绝对路径）。**发现源码树里的 `.bak*` 顺手移到 `D:\Engine project\backup\`，不要留在原地**（2026-08-26 一次清理移走 116 个，历史教训见下）。不要用 `Copy-Item <file> <file>.bak` 这种"原地备份"写法，统一临时目录 + 验证通过即删。规则适用一切文件类型（.cpp/.h/.json/.glsl/.ps1/.java/.kt），不止源码。
- **插件若直接调用 SDL API（如 `SDL_GetKeyboardState`），`compile_games.ps1` 已链接 SDL3.lib/SDL3_image.lib**（2026-08 起）；游戏逻辑尽量走 InputSystem 动作映射，跨 DLL 更稳
- **编码红线（强化）：源文件读写必须全部用 .NET 显式编码**——`[System.IO.File]::ReadAllLines/ReadAllText` + `WriteAllLines/WriteAllText` 都传 `[System.Text.UTF8Encoding]::new($false)`；**禁止 `Get-Content`/`Set-Content` 以隐式编码（ANSI）读写含中文的 .cpp/.h**（历史事故三次：PropertiesWindow 全文件损坏、EngineGlobals 吞代码——即使 WriteAllText 用了 UTF-8，ReadAllText 用 ANSI 也会破坏）
- **无限体素世界原型**：场景放空实体 + 插件挂 `WorldComponent`（参考 `games/voxelproto`）；物品栏选中格写入 `g_HandBlockId`（EngineGlobals，MIKAN_API 导出），EngineMain 放置用 `g_HandBlockId`——物品栏 ↔ 方块放置联动靠此全局
- **修改/删除/覆写源码文件前，备份只允许两种方式，禁止第三条**：① git（首选）——改动前先 `git status` 确认基线，大改动先提交 checkpoint 或 `git stash`；② 外部备份目录 `D:\Engine project\backup\`（带日期子目录）。**禁止在源码树内创建/遗留任何 `*.bak*` 文件**（含 `.bak-codex-*` / `.stripz.bak` / `*_timestamp.bak` 等变体）——历史教训：AI 对话习惯性 `Copy-Item <file> <file>.bak` 留在原目录，源码树一度堆了 116 个 bak 污染排查（2026-08-26 已全部移到 `D:\Engine project\backup\_backup_20260826_bak_sweep\`）；另两项历史事故：① 删 `src/Game/BreakoutGame.cpp` 未备份（靠上下文重建 OnRenderUI）② 覆写 `PropertiesWindow.cpp` 未备份（编码事故致 4-8 月定制编辑逻辑无法恢复，只能重写反射驱动版）——这两次的教训是"要备份"，不是"要 .bak 文件"
- **编码红线：绝不用 `Get-Content`/`Set-Content`/`WriteAllText` 以隐式编码读写含中文的 .cpp/.h**（历史事故：`PropertiesWindow.cpp` 被 ANSI 读 + UTF-8 写导致乱码+换行丢失+注释吞代码）；读写源文件一律用专用工具（edit_file/write_file），确需脚本处理时必须先确认源文件 BOM/编码并显式传编码
- **新增组件需 3 处接入（属性面板已反射驱动，无需手写）**：
  ① `SceneECS.cpp` 组件类型注册表 `RegisterComponent<X>()`（漏了 → GetComponent 崩溃）
  ② `ComponentRegistry.cpp` 反射注册 `RegisterComponent<X>("显示名", "分类", ..., fields表, "serializeKey")`（有 fields+serializeKey 时**序列化与属性面板自动生效**）
  ③ `SceneSerializer.cpp` 的 `DeserializeEntity` 已改为**遍历注册表自动分派**（含手写回退表）——新增组件无需改它；只有需要特殊反序列化逻辑的组件才要加手写回退（kHandwritten）
- **编码红线：绝不用 `Get-Content`/`Set-Content` 读写含中文的 .cpp/.h**（历史事故：`PropertiesWindow.cpp` 被 ANSI 读 + UTF-8 写导致全文件中文字符串/换行损坏 + 注释吞代码，4-8 月定制编辑逻辑无法恢复，只能重写为反射驱动版）；读写一律 `[System.IO.File]::ReadAllText/WriteAllText` + 显式 UTF-8，先确认源文件 BOM/编码再动
- **场景模式判定 `g_SceneIs2D` 由 `SceneRenderer::UpdateSceneMode()` 每帧无条件更新（FrameRender 开头）**：不得把判定放进 3D 渲染路径内部（历史 bug：判定曾内联在 PrepareFrame，2D 场景不经过它，2D→3D 切换后 g_SceneIs2D 卡 true，3D 场景无画面）；「X 只在某渲染路径里执行」是切换类 bug 的常见根因
- **Shader 热更新（2026-08 已实现）**：`ShaderHotReload`（`src/Rendering/ShaderHotReload.cpp`）经 FrameRender 轮询（0.5s 限频，在 vkWaitForFences 后、命令录制前——GPU 空闲窗口）检测 `engine/shaders/glsl/` 与 `engine/shaders/spv/` 文件变化；glsl 变化若有 glslangValidator/glslc 则自动重编（找不到打日志提示，手动跑 `build.ps1 -Target CompileShaders` 后靠 spv 扫描触发）；spv 变化 → `VulkanPipeline::ReloadAllPipelines()` 重建全部已登记管线（VulkanPipeline::Create 成功自动登记、Cleanup 注销；Reload 先建新后毁旧，失败保留旧管线）。**接口不变约定：热更新只允许改 shader 内部计算逻辑，不得改 UBO/采样器/push constant 等接口布局**（descriptor set layout 不重建，改了会崩）；compute pipeline（voxel_culling/voxel_hiz）尚未纳入热更新
- **脚本组件系统（Unity 式 C++ 玩法挂载，2026-08 已实现）**：玩法逻辑不再硬编码 FindByName——脚本类（继承 `ECS::IScriptBehaviour`，实现 OnStart/OnUpdate/OnDestroy/GetParamFields）经 `REGISTER_SCRIPT(Class, "Name")` 注册（游戏插件 DLL 加载时注册），场景 JSON 实体挂 `"script": {"scriptName": "...", "params": {...}}` 即绑定挂载关系。`ScriptSystem` 反序列化时创建实例、回填 params（按脚本参数字段表，`SCRIPT_FIELD` 声明，复用 ComponentRegistry 反射）、每帧 OnUpdate（主循环播放态块，先于插件 OnUpdate）、场景卸载 OnDestroy。**属性面板已支持脚本编辑（Unity 式）**：选中实体 → "脚本"块显示脚本类下拉（已注册脚本列表，切换即 RebindScript 重建实例）+ 参数反射面板（写回实时生效，保存场景时序列化刷新 paramsJson）。**脚本宿主 = 游戏插件 DLL**（保持独立编译/热重载）；**改脚本后跑 `compile_games.ps1`**。参考样例：`games/baka3d/Baka3D.cpp` 的 RotateScript + `assets/baka3d.json` 的 script 组件。注意：场景加载早于插件加载，`InstantiateAll` 幂等，引擎在 Activate 游戏模块后补齐实例
- **预制体（Unity 式可复用实体模板，2026-08 已实现）**：`SceneSerializer::SavePrefab(root, path)` 保存实体子树（含全部子实体/组件）为 `assets/prefabs/<name>.prefab.json`（实体 id 归一化 0..N、hierarchy 内部引用重映射）；`InstantiatePrefab(path, parent)` 实例化到场景（返回新根实体，自动补齐脚本实例）。编辑器入口：属性面板"保存为预制体"按钮（选中实体）+ 资产窗口双击 `*.prefab.json` 实例化（自动选中新根）。headless 自测：`--prefab-selftest`（场景加载后保存 Baka + 构造父子树验证，退出码 0/1）
- **骨骼动画（glTF 兼容，2026-08 已实现）**：`ModelLoader` 解析 `aiAnimation`（assimp 归一化，glTF/FBX 通用）为 `MeshData::animations`（AnimationClip/BoneChannel/BoneKeyframe）。**两个关键修复**：① **glTF 动画时间轴**——assimp 把 glTF 的秒放大 1000 存成"毫秒"tick，检测换算后时长 >10s 时按毫秒→秒修正（CesiumMan 83s→2s）；② **关键帧时间轴优先 rotation 通道**——assimp 的 position/rotation/scale 三通道各有独立时间轴，骨骼动画通常 rotation 主导、position 可能单帧，若统一用 position 时间轴会被单帧（time=0）覆盖导致动画不动（Fox 的 Survey 等动画曾因此全静止）。`SampleAnimation` 做关键帧插值（pos lerp / rot slerp / scale lerp）+ 骨骼层级 globalTransform 重算。**蒙皮方案**：GPU 蒙皮主路径 = **uniform texel buffer**（骨骼矩阵每骨骼 4 个 vec4 列，`boneTex` texelFetch 组装 mat4）——**绕开 NVIDIA 驱动对 vertex stage SSBO 读取的兼容性问题（数据正确但 GPU 读错致不可见）与 UBO 数组的 device lost**；`--cpu-skinning` 强制 CPU 蒙皮 fallback（逐顶点加权写 host-visible 顶点缓冲）。实测：GPU 蒙皮 CPU 侧 0.005ms/帧 vs CPU 蒙皮 0.17ms/帧（3273 顶点，34×）。`AnimatorComponent`（挂实体，serializeKey "animator"）：clipIndex/speed/loop/playing + time 运行时回写，SceneRenderer 每帧同步到对应 ModelRenderer（按 mesh.modelPath）。测试素材：CesiumMan.glb（1 动画）、Fox.glb（3 动画 Survey/Walk/Run）、场景 `assets/cesium_man.json`
- **2D 严格由全局 ECS 场景树管理，禁止重建/复活独立 Node 子树**：UI = Canvas 实体(canvas2d) + 子级 UI 实体(sprite2d/textComp/button/slice9)；渲染走 `Canvas2D::RenderECSNodes`、交互走 `UpdateCanvasNodeRecursive`。旧 `Node/SpriteNode/RectNode/ButtonNode` 树已删除（2026-08），头文件注释与 `Clear()` 均为空壳——不要因看到旧文档/旧代码片段而复活
- **Renderer2D 顶点缓冲是单实例共享的**：编辑器场景视图与游戏视图同帧渲染必须 pass 隔离（场景视图用 `UseSecondaryBuffer(true)`，见 `RenderSceneToTarget`），且每个离屏 pass 渲染前必须 `ResetFrame()`——否则同帧互相覆盖顶点数据、跨帧累积 `Vertex buffer overflow`（历史教训：场景视口丢 UI 层 + 98286 quads 溢出）
- **排查「全局标志恒为默认值/功能永不执行」时，先检查赋值语句是否被编码损坏的注释吞掉**（历史教训：`EditorDllApi.cpp` 193 行 `g_ShowSceneView` 赋值被 GBK 乱码注释合并进 `//` 行，场景视图永不渲染；**含历史乱码注释的 .cpp（如 EditorDllApi.cpp）改动前先检查 `//` 行里是否藏了代码**，改文件后用 UTF-8 保存）
- **隐藏/显示 UI 用 `SceneECS::SetVisible(entity, bool)`，禁止移出屏幕（-5000,5000）土办法**：2D 组件（Sprite2D/Text/Button/Slice9）与 3D（Render）统一支持可见性，Canvas2D 渲染前检查、父隐藏连带子树；visible 是运行时字段不序列化（snake/breakout 的 UpdateMenuVisibility 是正确样例）
- **原型/玩法文字用场景树实现，不硬编码绘制**：UI 文字 = Canvas 实体（canvas2d）+ 子实体 textComp（isUI=true，屏幕坐标左上原点 y 向下）；动态文字在插件里更新 `TextComponent.text`，**不要用 `TextRenderer::DrawStringMsdf` 直接画**（baka3d 的 InfoText 是正确样例）
- **场景 JSON 约定**（写 3D 场景前必读 `assets/baka3d.json`——已验证的完整 3D 样例）：
  - `rotation` 四元数顺序为 **[w,x,y,z]**（identity = `[1,0,0,0]`；`[0,0,0,1]` 是绕 Z 转 180°，会把相机倒转）
  - **3D 模型实体必须带 `material` 组件**（`albedoPath` + `useAlbedoTexture:true`），只给 `mesh`+`render` 会 fallback 成无纹理纯色
  - **组件遗漏引擎不报错**（静默忽略/fallback）——写完场景用 `tools\validate_scene.ps1` 校验；权威格式对照 = 引擎 Ctrl+S 保存的 `out\build\x64-Release\auto_save.json`（引擎序列化产物，含完整组件字段）
  - 历史教训（baka3d）：相机倒转 = rotation 顺序写错；模型无纹理 = 缺 material 组件；两者均靠对比 auto_save.json 定位
- **删除/移动源码文件前必须先确认有备份**（git 已跟踪则直接删后用 `git restore` 找回；未跟踪或需留历史副本时先移到 `D:\Engine project\backup\` 或系统临时目录，或直接用 move_file 工具而非裸 Remove-Item）；创建新文件前确认父目录存在（历史教训：删 `src/Game/BreakoutGame.cpp` 未备份，靠会话上下文 + obj 字符串重建了 OnRenderUI）
- 新文件按分类放 `src/` 与 `include/` 同名镜像目录（Core/Rendering/ECS/Editor/UI/World/Game/Platform）；公共头放 `include/`（games/ 插件只可见 include/），实现头跟随 .cpp；**不要放 src/ 或 include/ 根目录**（根只留 HostMain.cpp / StbVorbis.c）
- 改 `tools\*.ps1` 必须保留 **UTF-8 BOM**（Windows PowerShell 5.1 无 BOM 时含中文脚本解析错乱）
- 改组件字段后重新生成 schema：`EngineMain.exe --dump-schema tools\scene_schema.json`
- 场景 JSON 的未知组件键会被引擎**静默忽略**——写完场景务必用 `validate_scene` 校验
- 引擎运行中锁定 Game.dll/Editor.dll：构建前 `-KillEngine` 或 `stop_engine`
- 崩溃排查：默认在 exe 工作目录的 `log/crash_log.txt`；MCP 运行在对应 `mcp_runs/<run-id>/crash_log.txt`（异常码/地址/模块偏移）
- 完整陷阱清单：`README.md` §4、`编译命令.md` 末尾表格
