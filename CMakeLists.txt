cmake_minimum_required(VERSION 3.16)
project(mikanengine LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    add_compile_options(/utf-8 /FS)
    add_compile_definitions(NOMINMAX)
    # 生成 PDB 调试信息（用于崩溃栈符号化）
    add_compile_options(/Zi)
endif()

# ======================= ImGui (compiled INTO Game.dll, exported for Editor.dll) =======================
set(IMGUI_SOURCES
    dependencies/imgui/imgui.cpp
    dependencies/imgui/imgui_demo.cpp
    dependencies/imgui/imgui_draw.cpp
    dependencies/imgui/imgui_tables.cpp
    dependencies/imgui/imgui_widgets.cpp
    dependencies/imgui/imgui_impl_sdl3.cpp
    dependencies/imgui/imgui_impl_vulkan.cpp
)

# ======================= Jolt Physics (compiled into Game.dll) =======================
file(GLOB_RECURSE JOLT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/JoltPhysics/Jolt/*.cpp)

# ======================= msdfgen core (MSDF 距离场生成，纯算法无依赖) =======================
file(GLOB MSDFGEN_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/msdfgen/core/*.cpp)

# ======================= Box2D 3.0.0 (2D 物理; C 源码, 句柄式 API, 编译进 Game.dll, Android 可编) =======================
file(GLOB_RECURSE BOX2D_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/box2d/src/*.c)
# MSVC 适配: 无 C11 _Atomic 关键字 → 宏置空; stdatomic.h 用 msvc_compat 兼容层; /std:c11 启用 _Static_assert 等
set_source_files_properties(${BOX2D_SOURCES} PROPERTIES COMPILE_OPTIONS "/std:c11;/D_Atomic=")

# ======================= Game.dll - runtime (renderer, physics, ECS, scene) =======================
set(GAME_SOURCES
    # Core
    src/Core/EngineGlobals.cpp
    src/Core/ProjectManager.cpp
    src/Core/EngineMain.cpp
    src/Core/VulkanManager.cpp
    src/Core/Log.cpp
    src/Core/EngineAssets.cpp
    # Rendering
    src/Rendering/SceneRenderer.cpp
    src/Rendering/ShaderManager.cpp
    src/Rendering/ShaderHotReload.cpp
    src/Rendering/VulkanShader.cpp
    src/Rendering/SkyboxRenderer.cpp
    src/Rendering/SceneDebugRenderer.cpp
    src/Rendering/SceneCollector.cpp
    src/Rendering/OcclusionCulling.cpp
    src/Rendering/CameraUniformBuffer.cpp
    src/Rendering/RenderTarget.cpp
    src/Rendering/FullscreenQuad.cpp
    src/Rendering/PostProcessQuad.cpp
    src/Rendering/PostProcessChain.cpp
    src/Rendering/CMAA2.cpp
    src/Rendering/RendererBase.cpp
    src/Rendering/ModelRenderer.cpp
    src/Rendering/WireframeRenderer.cpp
    src/Rendering/VoxRenderer.cpp
    src/Rendering/VoxelMeshMultiDrawIndirect.cpp
    src/Rendering/VoxelTexture3DCache.cpp
    src/Rendering/VoxelTexture3DManager.cpp
    src/Rendering/DdsDecoder.cpp
    src/Rendering/TexturePool.cpp
    src/Rendering/TextureDescriptorCache.cpp
    src/Rendering/PointShadowRenderer.cpp
    src/Rendering/CascadeShadowRenderer.cpp
    # ECS
    src/ECS/SceneECS.cpp
    src/ECS/ComponentRegistry.cpp
    src/ECS/ECSStatics.cpp
    src/ECS/ScriptSystem.cpp
    src/ECS/Systems/RenderSystem.cpp
    src/ECS/PhysicsSystem.cpp
    # Runtime systems
    src/Core/PhysicsManager.cpp
    src/Core/InputController.cpp
    src/Core/InputSystem.cpp
    src/Core/Physics2DManager.cpp
    src/Core/Physics2DSystem.cpp
    src/Core/SaveSystem.cpp
    src/Core/Camera2DSystem.cpp
    src/Core/TilemapSystem.cpp
    src/Core/PaletteManager.cpp
    src/Core/SceneManager.cpp
    src/Core/TmxLoader.cpp
    src/StbVorbis.c
    dependencies/tinyxml2/tinyxml2.cpp
    dependencies/zlib/inflate.c
    dependencies/zlib/inftrees.c
    dependencies/zlib/inffast.c
    dependencies/zlib/adler32.c
    dependencies/zlib/crc32.c
    dependencies/zlib/zutil.c
    src/Rendering/Camera.cpp
    src/Core/AudioManager.cpp
    src/Core/VirtualJoystick.cpp
    src/Rendering/ComputeShader.cpp
    src/Rendering/HiZComputeShader.cpp
    src/Rendering/AtmosphereRenderer.cpp
    src/Rendering/AtmosphereLUT.cpp
    src/Rendering/ModelLoader.cpp
    src/Rendering/ModelBVH.cpp
    src/Rendering/QuadTreeCulling.cpp
    src/Core/SceneSerializer.cpp
    src/Core/SceneSerializer_3D.cpp
    src/Core/SceneSerializer_2D.cpp
    src/Rendering/VoxLoader.cpp
    # 游戏玩法原型(贪吃蛇等示例;SnakeGame 已抽为独立插件 DLL,见 games/snake)
    src/Game/GameManager.cpp
        # 体素世界（从 OpenGL 版迁移的无限世界）
    src/World/World.cpp
    src/World/Chunk.cpp
    src/World/BlockManager.cpp
    src/World/WorldRenderer.cpp
    # 2D 渲染核心与场景图（UI 与 2D 玩法共用）
    src/Rendering/Renderer2D.cpp
    src/Rendering/FontAtlas.cpp
    src/Rendering/TextRenderer.cpp
    src/UI/Canvas2D.cpp
    src/UI/TweenSystem.cpp
    # 世界 ECS 系统
    src/ECS/Systems/WorldSystem.cpp
    src/ECS/Systems/SpriteAnimatorSystem.cpp
    src/ECS/Systems/AudioSourceSystem.cpp
    # ImGui lives in Game.dll so both Game and Editor share one context
    ${IMGUI_SOURCES}
    # Jolt Physics
    ${JOLT_SOURCES}
    # msdfgen core（MSDF 距离场生成）
    ${MSDFGEN_SOURCES}
    # Box2D 2D 物理
    ${BOX2D_SOURCES}
)

add_library(Game SHARED ${GAME_SOURCES})
if(MSVC)
    # 生成 Game.pdb 供崩溃符号化。/DEBUG:FASTLINK 实测对当前规模（314 obj）链接耗时无改善（~2.9s），
    # 且 FASTLINK PDB 事后符号化兼容性更差，故保留 FULL /DEBUG。
    target_link_options(Game PRIVATE /DEBUG)
endif()
set_target_properties(Game PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_compile_definitions(Game PRIVATE
    MIKAN_BUILD_GAME
    JPH_DEBUG_RENDERER=0
    JPH_PROFILE_ENABLED=0
    JPH_DISABLE_TEMP_ALLOCATOR_NAMES=1
    JPH_USE_CUSTOM_ALLOCATOR=0)
target_include_directories(Game PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Rendering
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ECS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Editor
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/box2d/include
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/box2d/src
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/box2d/msvc_compat
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/box2d/extern/simde
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/SDL3
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/SDL3_image
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/vulkan
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/glm
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/assimp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/JoltPhysics
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ktx/include
)
target_link_directories(Game PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib/x64 ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ktx/lib)
target_link_libraries(Game PRIVATE SDL3 SDL3_image vulkan-1 assimp-vc143-mtd)  # ktx 走运行时 LoadLibrary 动态加载
if(WIN32)
    target_link_libraries(Game PRIVATE imm32 version dxguid ole32 winmm)  # winmm/ole32: miniaudio WASAPI 需要
endif()

# ======================= Editor.dll - editor (ImGui windows, assets, previews) =======================
set(EDITOR_SOURCES
    src/Editor/EditorManager.cpp
    src/Editor/AssetsWindow.cpp
    src/Editor/ComponentInspector.cpp
    src/Editor/ControlPanelWindow.cpp
    src/Editor/DockingLayout.cpp
    src/Editor/EntityPresets.cpp
    src/Editor/GameViewWindow.cpp
    src/Editor/HierarchyWindow.cpp
    src/Editor/MainMenuBar.cpp
    src/Editor/MaterialEditor.cpp
    src/Editor/MaterialEditorWindow.cpp
    src/Editor/MRTDebugWindow.cpp
    src/Editor/PreviewGeneratorHelper.cpp
    src/Editor/ProjectManagerWindow.cpp
    src/Editor/UndoManager.cpp
    src/Editor/PropertiesWindow.cpp
    src/Editor/SceneViewWindow.cpp
    src/Editor/TextureCacheManager.cpp
    src/Editor/ToolbarWindow.cpp
    src/Editor/TilemapEditorWindow.cpp
    src/Editor/PreviewGenerator.cpp
    src/Editor/PreviewModelRenderer.cpp
    src/Editor/PreviewVoxRenderer.cpp
    src/Editor/EditorDllApi.cpp
    dependencies/ImGuizmo/ImGuizmo.cpp
)

add_library(Editor SHARED ${EDITOR_SOURCES})
target_compile_definitions(Editor PRIVATE MIKAN_USE_GAME)
target_include_directories(Editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Rendering
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ECS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Editor
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ImGuizmo
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/SDL3
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/vulkan
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ktx/include
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/glm
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/JoltPhysics
)
target_link_directories(Editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib/x64)
target_link_libraries(Editor PRIVATE Game SDL3 SDL3_image vulkan-1)
if(WIN32)
    target_link_libraries(Editor PRIVATE imm32 version dxguid)
endif()

# ======================= EngineMain.exe - thin host =======================
add_executable(EngineMain src/HostMain.cpp)
target_link_libraries(EngineMain PRIVATE Game)
# 让默认构建宿主程序时连带产出 Editor.dll（Editor 依赖 Game，故 Game+Editor+EngineMain 一并构建）。
# 发布阶段：直接删除 Editor.dll，Game.dll 启动时检测不到即以纯游戏模式运行（见 EngineMain.cpp DetectEditorDll）。
add_dependencies(EngineMain Editor)

# ======================= Post-build: copy DLLs & assets =======================
if(WIN32)
    set(DLL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/x64")
    foreach(DLL_NAME SDL3.dll SDL3_image.dll assimp-vc143-mtd.dll ktx.dll)
        if(EXISTS "${DLL_SOURCE_DIR}/${DLL_NAME}")
            add_custom_command(TARGET EngineMain POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DLL_SOURCE_DIR}/${DLL_NAME}"
                    $<TARGET_FILE_DIR:EngineMain>
                COMMENT "Copy ${DLL_NAME}")
        endif()
    endforeach()

    # Compile shaders (GLSL -> SPIR-V)；引擎系统资产在 engine/（职责分离：engine/=引擎，assets/=游戏内容）
    set(GLSLANG_VALIDATOR "${CMAKE_CURRENT_SOURCE_DIR}/tools/glslang/bin/glslangValidator.exe")
    set(GLSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/glsl")
    set(SPV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/spv")
    file(MAKE_DIRECTORY ${SPV_DIR})
    # CONFIGURE_DEPENDS：每次构建自动重扫 glsl 目录——新增/改名 shader 无需手动重新 configure
    # （2026-08-13：曾因缺此导致 bloom_*.frag 不在 build.ninja 规则里，改 shader 不触发重编）
    file(GLOB GLSL_SHADERS CONFIGURE_DEPENDS "${GLSL_DIR}/*.vert" "${GLSL_DIR}/*.frag" "${GLSL_DIR}/*.comp")
    foreach(SHADER ${GLSL_SHADERS})
        get_filename_component(FILENAME ${SHADER} NAME)
        get_filename_component(NAME_WE ${SHADER} NAME_WE)
        string(REGEX MATCH "\.(vert|frag|comp)$" SHADER_EXT ${FILENAME})
        string(REPLACE "." "" SHADER_EXT ${SHADER_EXT})
        set(SPV_FILE "${SPV_DIR}/${NAME_WE}.${SHADER_EXT}.spv")
        add_custom_command(OUTPUT ${SPV_FILE}
            COMMAND ${GLSLANG_VALIDATOR} -V --target-env spirv1.3 ${SHADER} -o ${SPV_FILE}
            DEPENDS ${SHADER}
            COMMENT "Compiling shader: ${FILENAME}")
        list(APPEND SPV_FILES ${SPV_FILE})
    endforeach()
    # 移动端合成 pass 版本（subpassLoad）：fullscreen_subpass.frag.spv（-DMIKAN_SUBPASS_LOAD；桌面用 fullscreen.frag.spv texture 版）
    add_custom_command(OUTPUT "${SPV_DIR}/fullscreen_subpass.frag.spv"
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env spirv1.3 -DMIKAN_SUBPASS_LOAD "${GLSL_DIR}/fullscreen.frag" -o "${SPV_DIR}/fullscreen_subpass.frag.spv"
        DEPENDS "${GLSL_DIR}/fullscreen.frag"
        COMMENT "Compiling shader: fullscreen.frag (mobile subpass variant)")
    list(APPEND SPV_FILES "${SPV_DIR}/fullscreen_subpass.frag.spv")
    # Bloom 核变体（宏切换）：默认高斯 3x3 用 bloom_down2x/up2x.frag.spv，
    # dual 核（-DBLOOM_KERNEL_DUAL）与高斯 5x5（-DBLOOM_KERNEL_GAUSS5）各编译 _dual/_gauss5.frag.spv——
    # JSON shader 字段换名即切核，无需改代码
    add_custom_command(OUTPUT "${SPV_DIR}/bloom_down2x_dual.frag.spv"
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env spirv1.3 -DBLOOM_KERNEL_DUAL "${GLSL_DIR}/bloom_down2x.frag" -o "${SPV_DIR}/bloom_down2x_dual.frag.spv"
        DEPENDS "${GLSL_DIR}/bloom_down2x.frag"
        COMMENT "Compiling shader: bloom_down2x.frag (dual kernel variant)")
    list(APPEND SPV_FILES "${SPV_DIR}/bloom_down2x_dual.frag.spv")
    add_custom_command(OUTPUT "${SPV_DIR}/bloom_up2x_dual.frag.spv"
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env spirv1.3 -DBLOOM_KERNEL_DUAL "${GLSL_DIR}/bloom_up2x.frag" -o "${SPV_DIR}/bloom_up2x_dual.frag.spv"
        DEPENDS "${GLSL_DIR}/bloom_up2x.frag"
        COMMENT "Compiling shader: bloom_up2x.frag (dual kernel variant)")
    list(APPEND SPV_FILES "${SPV_DIR}/bloom_up2x_dual.frag.spv")
    add_custom_command(OUTPUT "${SPV_DIR}/bloom_down2x_gauss5.frag.spv"
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env spirv1.3 -DBLOOM_KERNEL_GAUSS5 "${GLSL_DIR}/bloom_down2x.frag" -o "${SPV_DIR}/bloom_down2x_gauss5.frag.spv"
        DEPENDS "${GLSL_DIR}/bloom_down2x.frag"
        COMMENT "Compiling shader: bloom_down2x.frag (gauss5 kernel variant)")
    list(APPEND SPV_FILES "${SPV_DIR}/bloom_down2x_gauss5.frag.spv")
    add_custom_command(OUTPUT "${SPV_DIR}/bloom_up2x_gauss5.frag.spv"
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env spirv1.3 -DBLOOM_KERNEL_GAUSS5 "${GLSL_DIR}/bloom_up2x.frag" -o "${SPV_DIR}/bloom_up2x_gauss5.frag.spv"
        DEPENDS "${GLSL_DIR}/bloom_up2x.frag"
        COMMENT "Compiling shader: bloom_up2x.frag (gauss5 kernel variant)")
    list(APPEND SPV_FILES "${SPV_DIR}/bloom_up2x_gauss5.frag.spv")
    # Bloom 阈值提取版降采样（ds1 专用）：dual 核 + soft-knee 软阈值（Unity PPv2 Prefilter 公式）
    add_custom_command(OUTPUT "${SPV_DIR}/bloom_down2x_th.frag.spv"
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env spirv1.3 "${GLSL_DIR}/bloom_down2x_th.frag" -o "${SPV_DIR}/bloom_down2x_th.frag.spv"
        DEPENDS "${GLSL_DIR}/bloom_down2x_th.frag"
        COMMENT "Compiling shader: bloom_down2x_th.frag (soft-knee threshold extract)")
    list(APPEND SPV_FILES "${SPV_DIR}/bloom_down2x_th.frag.spv")
    add_custom_target(CompileShaders DEPENDS ${SPV_FILES} COMMENT "Compiling all shaders")
    add_dependencies(EngineMain CompileShaders)
endif()
