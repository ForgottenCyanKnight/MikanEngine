cmake_minimum_required(VERSION 3.22.1)
project("mikanengine")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Android 平台强制使用 Release 构建优化
# 如果未指定构建类型，默认为 Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 设置 Release 优化标志（无论构建类型如何）
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

# ======================= ImGui (compiled INTO mikanengine.so) =======================
set(IMGUI_SOURCES
    ${PROJECT_ROOT}/dependencies/imgui/imgui.cpp
    ${PROJECT_ROOT}/dependencies/imgui/imgui_demo.cpp
    ${PROJECT_ROOT}/dependencies/imgui/imgui_draw.cpp
    ${PROJECT_ROOT}/dependencies/imgui/imgui_tables.cpp
    ${PROJECT_ROOT}/dependencies/imgui/imgui_widgets.cpp
    ${PROJECT_ROOT}/dependencies/imgui/imgui_impl_sdl3.cpp
    ${PROJECT_ROOT}/dependencies/imgui/imgui_impl_vulkan.cpp
    ${PROJECT_ROOT}/dependencies/ImGuizmo/ImGuizmo.cpp
)

# ======================= Jolt Physics (与桌面版一致，自动扫描全部源文件) =======================
file(GLOB_RECURSE JOLT_SOURCES ${PROJECT_ROOT}/dependencies/JoltPhysics/Jolt/*.cpp)

# ======================= msdfgen core (MSDF 距离场生成，纯算法无依赖) =======================
file(GLOB MSDFGEN_SOURCES ${PROJECT_ROOT}/dependencies/msdfgen/core/*.cpp)

# ======================= Box2D 3.0.0 (2D 物理; C 源码, 句柄式 API, Android 可编) =======================
file(GLOB_RECURSE BOX2D_SOURCES ${PROJECT_ROOT}/dependencies/box2d/src/*.c)
# MSVC 适配: 无 C11 _Atomic 关键字 → 宏置空; stdatomic.h 用 msvc_compat 兼容层; /std:c11 启用 _Static_assert 等
if(MSVC)
    set_source_files_properties(${BOX2D_SOURCES} PROPERTIES COMPILE_OPTIONS "/std:c11;/D_Atomic=")
endif()

find_package(SDL3 REQUIRED CONFIG)
find_package(SDL3_image REQUIRED CONFIG)

# ======================= mikanengine - runtime (renderer, physics, ECS, scene) =======================
# 源码清单与桌面版 CMakeLists.txt 的 GAME_SOURCES 完全对齐
add_library(mikanengine SHARED
    # Core
    ${PROJECT_ROOT}/src/Core/EngineGlobals.cpp
    ${PROJECT_ROOT}/src/Core/ProjectManager.cpp
    ${PROJECT_ROOT}/src/Core/EngineMain.cpp
    ${PROJECT_ROOT}/src/Core/GameplayRuntime.cpp
    ${PROJECT_ROOT}/src/Core/GameplayTestMain.cpp
    ${PROJECT_ROOT}/src/Core/RuntimeCapabilities.cpp
    ${PROJECT_ROOT}/src/Core/VulkanManager.cpp
    ${PROJECT_ROOT}/src/Core/Log.cpp
    ${PROJECT_ROOT}/src/Core/EngineAssets.cpp
    ${PROJECT_ROOT}/src/Core/ScreenshotCapture.cpp
    ${PROJECT_ROOT}/src/Core/RenderDocCapture.cpp
    # Rendering
    ${PROJECT_ROOT}/src/Rendering/SceneRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/ShaderManager.cpp
    ${PROJECT_ROOT}/src/Rendering/ShaderHotReload.cpp
    ${PROJECT_ROOT}/src/Rendering/VulkanShader.cpp
    ${PROJECT_ROOT}/src/Rendering/SkyboxRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/SceneDebugRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/SceneCollector.cpp
    ${PROJECT_ROOT}/src/Rendering/OcclusionCulling.cpp
    ${PROJECT_ROOT}/src/Rendering/CameraUniformBuffer.cpp
    ${PROJECT_ROOT}/src/Rendering/RenderTarget.cpp
    ${PROJECT_ROOT}/src/Rendering/FullscreenQuad.cpp
    ${PROJECT_ROOT}/src/Rendering/PostProcessQuad.cpp
    ${PROJECT_ROOT}/src/Rendering/PostProcessChain.cpp
    ${PROJECT_ROOT}/src/Rendering/CloudNoise3D.cpp
    ${PROJECT_ROOT}/src/Rendering/CMAA2.cpp
    ${PROJECT_ROOT}/src/Rendering/RendererBase.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/TerrainRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/WaterRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/WireframeRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/VoxRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/VoxelMeshMultiDrawIndirect.cpp
    ${PROJECT_ROOT}/src/Rendering/VoxelTexture3DCache.cpp
    ${PROJECT_ROOT}/src/Rendering/VoxelTexture3DManager.cpp
    ${PROJECT_ROOT}/src/Rendering/DdsDecoder.cpp
    ${PROJECT_ROOT}/src/Rendering/HeightmapLoader.cpp
    ${PROJECT_ROOT}/src/Rendering/TexturePool.cpp
    ${PROJECT_ROOT}/src/Rendering/TextureDescriptorCache.cpp
    ${PROJECT_ROOT}/src/Rendering/PointShadowRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/CascadeShadowRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/ParticleSystem.cpp
    ${PROJECT_ROOT}/src/Rendering/ParticleRenderer.cpp
    # ECS
    ${PROJECT_ROOT}/src/ECS/SceneECS.cpp
    ${PROJECT_ROOT}/src/ECS/ComponentRegistry.cpp
    ${PROJECT_ROOT}/src/ECS/ECSStatics.cpp
    ${PROJECT_ROOT}/src/ECS/ScriptSystem.cpp
    ${PROJECT_ROOT}/src/ECS/ScriptContext.cpp
    ${PROJECT_ROOT}/src/Animation/VmdMotion.cpp
    ${PROJECT_ROOT}/src/ECS/Systems/VmdSystem.cpp
    ${PROJECT_ROOT}/src/ECS/Systems/RenderSystem.cpp
    ${PROJECT_ROOT}/src/ECS/PhysicsSystem.cpp
    # Runtime systems
    ${PROJECT_ROOT}/src/Core/PhysicsManager.cpp
    ${PROJECT_ROOT}/src/Core/InputController.cpp
    ${PROJECT_ROOT}/src/Core/InputSystem.cpp
    ${PROJECT_ROOT}/src/Core/Physics2DManager.cpp
    ${PROJECT_ROOT}/src/Core/Physics2DSystem.cpp
    ${PROJECT_ROOT}/src/Core/SaveSystem.cpp
    ${PROJECT_ROOT}/src/Core/Camera2DSystem.cpp
    ${PROJECT_ROOT}/src/Core/ThirdPersonCameraSystem.cpp
    ${PROJECT_ROOT}/src/Core/PlayerControllerSystem.cpp
    ${PROJECT_ROOT}/src/Core/TilemapSystem.cpp
    ${PROJECT_ROOT}/src/Core/PaletteManager.cpp
    ${PROJECT_ROOT}/src/Core/SceneManager.cpp
    ${PROJECT_ROOT}/src/Core/TmxLoader.cpp
    ${PROJECT_ROOT}/src/StbVorbis.c
    ${PROJECT_ROOT}/dependencies/tinyxml2/tinyxml2.cpp
    ${PROJECT_ROOT}/dependencies/zlib/inflate.c
    ${PROJECT_ROOT}/dependencies/zlib/inftrees.c
    ${PROJECT_ROOT}/dependencies/zlib/inffast.c
    ${PROJECT_ROOT}/dependencies/zlib/adler32.c
    ${PROJECT_ROOT}/dependencies/zlib/crc32.c
    ${PROJECT_ROOT}/dependencies/zlib/zutil.c
    ${PROJECT_ROOT}/src/Rendering/Camera.cpp
    ${PROJECT_ROOT}/src/Core/AudioManager.cpp
    ${PROJECT_ROOT}/src/Core/VirtualJoystick.cpp
    ${PROJECT_ROOT}/src/Rendering/ComputeShader.cpp
    ${PROJECT_ROOT}/src/Rendering/HiZComputeShader.cpp
    ${PROJECT_ROOT}/src/Rendering/AtmosphereRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/AtmosphereLUT.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelLoader.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelBVH.cpp
    ${PROJECT_ROOT}/src/Rendering/QuadTreeCulling.cpp
    ${PROJECT_ROOT}/src/Core/SceneSerializer.cpp
    ${PROJECT_ROOT}/src/Core/SceneSerializer_3D.cpp
    ${PROJECT_ROOT}/src/Core/SceneSerializer_2D.cpp
    ${PROJECT_ROOT}/src/Rendering/VoxLoader.cpp
    # 游戏玩法原型(贪吃蛇等示例)
    ${PROJECT_ROOT}/src/Game/GameManager.cpp
    # third-person 原型玩法与场景脚本（Android 端静态编入 so，替代桌面 DLL 插件）
    ${PROJECT_ROOT}/projects/third-person-navigation/games/cesiumwalk/CesiumWalk.cpp
    # 体素世界（从 OpenGL 版迁移的无限世界）
    ${PROJECT_ROOT}/src/World/World.cpp
    ${PROJECT_ROOT}/src/World/Chunk.cpp
    ${PROJECT_ROOT}/src/World/BlockManager.cpp
    ${PROJECT_ROOT}/src/World/WorldRenderer.cpp
    # 2D 渲染核心与场景图（UI 与 2D 玩法共用）
    ${PROJECT_ROOT}/src/Rendering/Renderer2D.cpp
    ${PROJECT_ROOT}/src/Rendering/FontAtlas.cpp
    ${PROJECT_ROOT}/src/Rendering/TextRenderer.cpp
    ${PROJECT_ROOT}/src/UI/Canvas2D.cpp
    ${PROJECT_ROOT}/src/UI/RuntimeSettingsOverlay.cpp
    ${PROJECT_ROOT}/src/UI/TweenSystem.cpp
    # 世界 ECS 系统
    ${PROJECT_ROOT}/src/ECS/Systems/WorldSystem.cpp
    ${PROJECT_ROOT}/src/ECS/Systems/SpriteAnimatorSystem.cpp
    ${PROJECT_ROOT}/src/ECS/Systems/AudioSourceSystem.cpp
    # ImGui lives in mikanengine.so so both Game and Editor share one context
    ${IMGUI_SOURCES}
    # Jolt Physics
    ${JOLT_SOURCES}
    # msdfgen core（MSDF 距离场生成）
    ${MSDFGEN_SOURCES}
    # Box2D 2D 物理
    ${BOX2D_SOURCES}
)

# JoltPhysics definitions
target_compile_definitions(mikanengine PRIVATE
    JPH_DEBUG_RENDERER=0
    JPH_PROFILE_ENABLED=0
    JPH_DISABLE_TEMP_ALLOCATOR_NAMES=1
    JPH_USE_CUSTOM_ALLOCATOR=0
)

# Android 平台定义 - 禁用桌面端特性
target_compile_definitions(mikanengine PRIVATE
    ANDROID_BUILD=1
    DISABLE_RAY_TRACING=1
)

target_include_directories(mikanengine PRIVATE
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/include/Core
    ${PROJECT_ROOT}/include/Rendering
    ${PROJECT_ROOT}/include/ECS
    ${PROJECT_ROOT}/include/Editor
    ${PROJECT_ROOT}/src
    # libassimp.so is Assimp 6.0.4; keep its generated public headers ahead
    # of the legacy flat header copies under dependencies/assimp.
    ${PROJECT_ROOT}/dependencies/assimp/include
    ${PROJECT_ROOT}/dependencies/box2d/include
    ${PROJECT_ROOT}/dependencies/box2d/src
    ${PROJECT_ROOT}/dependencies/box2d/msvc_compat
    ${PROJECT_ROOT}/dependencies/box2d/extern/simde
    ${PROJECT_ROOT}/dependencies
    ${PROJECT_ROOT}/dependencies/imgui
    ${PROJECT_ROOT}/dependencies/ImGuizmo
    ${PROJECT_ROOT}/dependencies/SDL3
    ${PROJECT_ROOT}/dependencies/SDL3_image
    ${PROJECT_ROOT}/dependencies/vulkan
    ${PROJECT_ROOT}/dependencies/glm
    ${PROJECT_ROOT}/dependencies/JoltPhysics
    ${PROJECT_ROOT}/tools/ktx/include
)

target_link_libraries(mikanengine PRIVATE
    SDL3::SDL3
    SDL3_image::SDL3_image
    android
    log
    vulkan
    ${PROJECT_ROOT}/android/app/libs/${ANDROID_ABI}/libassimp.so
)

# Android 15+ 16KB 页大小兼容：ELF LOAD 段按 16KB 对齐（AGP 打包检查要求）
target_link_options(mikanengine PRIVATE "-Wl,-z,max-page-size=16384")
