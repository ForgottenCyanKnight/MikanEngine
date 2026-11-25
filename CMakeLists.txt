cmake_minimum_required(VERSION 3.16)
project(mikanengine LANGUAGES CXX C)

# This installed MSVC emits Chinese /showIncludes even with VSLANG=1033.
# Match its UTF-8 bytes explicitly; a mismatched prefix loses header deps.
if(MSVC)
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "注意: 包含文件:")
endif()

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Keep CPU validation as the low-dependency Linux default. Pass
# -DMIKAN_CPU_ONLY=OFF to build the SDL/Vulkan desktop runtime.
set(_mikan_cpu_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_mikan_cpu_default ON)
endif()
option(MIKAN_CPU_ONLY "Build portable CPU validation targets without the renderer" ${_mikan_cpu_default})
if(MIKAN_CPU_ONLY)
    include(cmake/CpuValidation.cmake)
    return()
endif()

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


# Stable PC dependencies live outside the engine build/clean graph.
set(JOLT_COMPILE_DEFINITIONS JPH_DEBUG_RENDERER=0 JPH_PROFILE_ENABLED=0 JPH_DISABLE_TEMP_ALLOCATOR_NAMES=1 JPH_USE_CUSTOM_ALLOCATOR=0)
if(WIN32)
    include(cmake/PCDependencies.cmake)

    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    add_subdirectory(dependencies/SDL3 EXCLUDE_FROM_ALL)
    # Keep SDL3.dll next to the hosts and SDL3.lib next to Game.lib for plugins.
    set_target_properties(SDL3-shared PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
else()
# ======================= Third-party static libraries =======================
# Keep the large third-party source sets out of Game.dll's object list.  This
# preserves the existing compile definitions and include layout, while making
# their object files reusable across incremental Game/Editor builds.
#
# ImGui intentionally remains part of Game below on Windows, where Editor.dll
# shares the context. Linux builds the no-editor runtime and still uses ImGui's
# SDL/Vulkan backend for runtime overlays.

# ======================= SDL and platform dependencies =======================
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_VIDEO ON CACHE BOOL "" FORCE)
set(SDL_VULKAN ON CACHE BOOL "" FORCE)
set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK ON CACHE BOOL "" FORCE)
set(SDL_HAPTIC ON CACHE BOOL "" FORCE)
set(SDL_X11 ON CACHE BOOL "" FORCE)
set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
set(SDL_X11_XDBE OFF CACHE BOOL "" FORCE)
set(SDL_X11_XSCRNSAVER OFF CACHE BOOL "" FORCE)
set(SDL_X11_XSHAPE OFF CACHE BOOL "" FORCE)
set(SDL_X11_XSYNC OFF CACHE BOOL "" FORCE)
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
set(SDL_FRIBIDI OFF CACHE BOOL "" FORCE)
set(SDL_LIBTHAI OFF CACHE BOOL "" FORCE)
find_package(Threads REQUIRED)
add_subdirectory(dependencies/SDL3 EXCLUDE_FROM_ALL)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_VENDORED ON CACHE BOOL "" FORCE)
set(SDLIMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JXL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(dependencies/SDL3_image EXCLUDE_FROM_ALL)

find_library(MIKAN_VULKAN_LIBRARY NAMES vulkan libvulkan.so.1 REQUIRED)
add_library(MikanVulkanLoader UNKNOWN IMPORTED)
set_target_properties(MikanVulkanLoader PROPERTIES
    IMPORTED_LOCATION "${MIKAN_VULKAN_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/vulkan/include")

# This source checkout carries Assimp's public headers. Linux links the host's
# Assimp shared library (install libassimp-dev on a development machine).
find_library(MIKAN_ASSIMP_LIBRARY NAMES assimp libassimp.so.5 REQUIRED)
endif()

# ======================= Jolt Physics =======================
file(GLOB_RECURSE JOLT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/JoltPhysics/Jolt/*.cpp)
set(JOLT_COMPILE_DEFINITIONS
    JPH_DEBUG_RENDERER=0
    JPH_PROFILE_ENABLED=0
    JPH_DISABLE_TEMP_ALLOCATOR_NAMES=1
    JPH_USE_CUSTOM_ALLOCATOR=0)
add_library(JoltPhysics STATIC ${JOLT_SOURCES})
target_compile_definitions(JoltPhysics PRIVATE ${JOLT_COMPILE_DEFINITIONS})
target_include_directories(JoltPhysics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/JoltPhysics)

# ======================= msdfgen core (MSDF 距离场生成，纯算法无依赖) =======================
file(GLOB MSDFGEN_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/msdfgen/core/*.cpp)
add_library(msdfgen STATIC ${MSDFGEN_SOURCES})
target_include_directories(msdfgen PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies)

# ======================= Box2D 3.0.0 (2D 物理; C 源码, 句柄式 API) =======================
file(GLOB_RECURSE BOX2D_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/box2d/src/*.c)
# MSVC 适配: 上游 v3.0.0 的 src/allocate.c 直接 include <stdatomic.h>，MSVC 需显式
# 开启 C11 atomics（旧 /D_Atomic= 置空宏只对 vendored msvc_compat 桩有效，对系统头会 C1189）
add_library(Box2D STATIC ${BOX2D_SOURCES})
set_target_properties(Box2D PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(Box2D PRIVATE /experimental:c11atomics)
endif()
target_include_directories(Box2D PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/box2d/include)
target_include_directories(Box2D PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/box2d/src
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/box2d/extern/simde)


endif()

# ======================= Game.dll - runtime (renderer, physics, ECS, scene) =======================
# .inl 是被 #include 进 .cpp 的实现分片。Ninja 只信 /showIncludes 依赖，而 MSVC 对
# 这些 .inl 的依赖跟踪不可靠（2026-09-25 事故：改 EngineRuntimeLoop.inl 后
# EngineMain.cpp.obj 不重编，主循环路由代码没进产物，"改了没生效"）。
# 用 OBJECT_DEPENDS 建立显式依赖，保证改 .inl 必触发对应 .cpp 重编。
set_source_files_properties(src/Core/EngineMain.cpp PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/EngineRuntimeLoop.inl;${CMAKE_CURRENT_SOURCE_DIR}/src/Core/EngineSceneStartup.inl")
set_source_files_properties(src/Core/PhysicsManager.cpp PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PhysicsManagerBodies.inl;${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PhysicsManagerCollisionShapes.inl;${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PhysicsManagerLifecycle.inl;${CMAKE_CURRENT_SOURCE_DIR}/src/Core/PhysicsManagerQueries.inl")
set_source_files_properties(src/Rendering/RenderTarget.cpp PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Rendering/RenderTargetAttachmentResources.inl;${CMAKE_CURRENT_SOURCE_DIR}/src/Rendering/RenderTargetCompositeAndFinal.inl;${CMAKE_CURRENT_SOURCE_DIR}/src/Rendering/RenderTargetDisplayResources.inl;${CMAKE_CURRENT_SOURCE_DIR}/src/Rendering/RenderTargetLifecycle.inl;${CMAKE_CURRENT_SOURCE_DIR}/src/Rendering/RenderTargetRecording.inl;${CMAKE_CURRENT_SOURCE_DIR}/src/Rendering/RenderTargetRenderPasses.inl")
set_source_files_properties(src/Rendering/SceneReflectionProbe.cpp PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Rendering/RenderTargetLifecycle.inl;${CMAKE_CURRENT_SOURCE_DIR}/src/Rendering/RenderTargetRecording.inl;${CMAKE_CURRENT_SOURCE_DIR}/src/Rendering/RenderTargetRenderPasses.inl")
set(GAME_SOURCES
    # Core
    src/Core/I18n.cpp
    src/Core/PipelineSettings.cpp
    src/Core/EngineGlobals.cpp
    src/Core/JobSystem.cpp
    src/Core/EngineCommandLine.cpp
    src/Core/EngineDiagnostics.cpp
    src/Core/EngineShutdown.cpp
    src/Core/ProjectManager.cpp
    src/Core/AutosaveService.cpp
    src/Core/EngineMain.cpp
    src/Core/RenderDocCapture.cpp
    src/Core/ScreenshotCapture.cpp
    src/Core/PipelineCapture.cpp
    src/Core/VulkanGpuProfiler.cpp
    src/Core/GameplayRuntime.cpp
    src/Core/GameplayTestMain.cpp
    src/Core/RuntimeCapabilities.cpp
    src/Core/VulkanManager.cpp
    src/Core/VulkanPresentation.cpp
    src/Core/VulkanStartupPasses.cpp
    src/Core/VulkanDeviceSetup.cpp
    src/Core/VulkanRayTracingDevice.cpp
    src/Rendering/RayTracing/AccelerationStructure.cpp
    src/Rendering/RayTracing/VoxRayTracingGeometry.cpp
    src/Rendering/RayTracing/RayTracingScene.cpp
    src/Rendering/SceneRayTracing.cpp
    src/Rendering/RayTracing/RayTracingViewport.cpp
    src/Rendering/RayTracing/RayTracingViewportResources.cpp
    src/Rendering/RayTracing/RayTracingViewportEnvironment.cpp
    src/Rendering/RayTracing/ModelRayTracingGeometry.cpp
    src/Rendering/RayTracing/RayTracingSceneModels.cpp
    src/Rendering/RayTracing/RayTracingViewportModels.cpp
    src/Rendering/RayTracing/RayTracingViewportDenoising.cpp
    src/Core/VulkanRayTracingPipeline.cpp
    src/Core/VulkanWindowLifecycle.cpp
    src/Core/VulkanDiagnostics.cpp
    src/Core/VulkanCompositeResources.cpp
    src/Core/VulkanPostProcessHistory.cpp
    src/Core/VulkanPostProcessChains.cpp
    src/Core/VulkanRenderHelpers.cpp
    src/Core/VulkanLightingCulling.cpp
    src/Core/VulkanFramePipeline.cpp
    src/Core/VulkanFrameLoop.cpp
    src/Core/VulkanRuntimeResources.cpp
    src/Core/VulkanCompositeLifecycle.cpp
    src/Core/VulkanSwapchainLifecycle.cpp
    src/Core/VulkanShutdown.cpp
    src/Core/Log.cpp
    src/Core/EngineAssets.cpp
    src/Core/AssetRegistry.cpp
    src/Core/AssetHotReload.cpp
    src/Core/ConsoleCommands.cpp
    src/Rendering/GltfLights.cpp
    src/Rendering/RenderStats.cpp
    # Rendering
    src/Rendering/SceneRenderer.cpp
    src/Rendering/SceneReflectionProbe.cpp
    src/Rendering/SceneFramePreparation.cpp
    src/Rendering/SceneGeometryPass.cpp
    src/Rendering/SceneEnvironmentPass.cpp
    src/Rendering/SceneDebugPass.cpp
    src/Rendering/SceneShadowPass.cpp
    src/Rendering/ShaderManager.cpp
    src/Rendering/ShaderHotReload.cpp
    src/Rendering/VulkanShader.cpp
    src/Rendering/SkyboxRenderer.cpp
    src/Rendering/SceneDebugRenderer.cpp
    src/Rendering/SceneCollector.cpp
    src/Rendering/RenderWorldBuilder.cpp
    src/Rendering/RenderWorldPartition.cpp
    src/Rendering/RenderWorldExtraction.cpp
    src/Rendering/RenderWorldValidation.cpp
    src/Rendering/RenderWorldFinalizeWorker.cpp
    src/Rendering/OcclusionCulling.cpp
    src/Rendering/CameraUniformBuffer.cpp
    src/Rendering/RenderTarget.cpp
    src/Rendering/FullscreenQuad.cpp
    src/Rendering/InfiniteGridRenderer.cpp
    src/Rendering/PostProcessQuad.cpp
    src/Rendering/PostProcessChain.cpp
    src/Rendering/Denoising/DiffuseDenoiser.cpp
    src/Rendering/CloudNoise3D.cpp
    src/Rendering/CMAA2.cpp
    src/Rendering/RendererBase.cpp
    src/Rendering/ModelRenderer.cpp
    src/Rendering/PmxRenderer.cpp
    src/Rendering/ModelRendererLifecycle.cpp
    src/Rendering/ModelRendererBuffers.cpp
    src/Rendering/ModelRendererGeometry.cpp
    src/Rendering/ModelRendererLoading.cpp
    src/Rendering/ModelRendererAnimation.cpp
    src/Rendering/ModelRendererSkinning.cpp
    src/Rendering/ModelRendererMaterials.cpp
    src/Rendering/ModelRendererQueries.cpp
    src/Rendering/ModelIndirectRenderer.cpp
    src/Rendering/ModelRendererBatching.cpp
    src/Rendering/ModelRendererInstanceBuffer.cpp
    src/Rendering/ModelRendererInstanced.cpp
    src/Rendering/ModelRendererInstancedVariants.cpp
    src/Rendering/ModelRendererDepth.cpp
    src/Rendering/ModelRendererShadow.cpp
    src/Rendering/ModelRendererPipeline.cpp
    src/Rendering/ModelRendererDescriptors.cpp
    src/Rendering/ModelRendererSorting.cpp
    src/Rendering/WireframeRenderer.cpp
    src/Rendering/VoxRenderer.cpp
    src/Rendering/VoxelDDARegistry.cpp
    src/Rendering/VoxelMeshMultiDrawIndirect.cpp
    src/Rendering/VoxelTexture3DCache.cpp
    src/Rendering/VoxelTexture3DManager.cpp
    src/Rendering/DdsDecoder.cpp
    src/Rendering/HeightmapLoader.cpp
    src/Rendering/TerrainRenderer.cpp
    src/Rendering/TerrainGrassRenderer.cpp
    src/Rendering/TerrainBrush.cpp
    src/Rendering/WaterRenderer.cpp
    src/Rendering/WaterTargetRT.cpp
    src/Rendering/ParticleSystem.cpp
    src/Rendering/ParticleRenderer.cpp
    src/Rendering/CpuClothSimulation.cpp
    src/Rendering/CpuClothRenderer.cpp
    src/Rendering/GpuSphSimulation.cpp
    src/Rendering/GpuSphContainerRenderer.cpp
    src/Rendering/TexturePool.cpp
    src/Rendering/TextureDescriptorCache.cpp
    src/Rendering/PointShadowRenderer.cpp
    src/Rendering/CascadeShadowRenderer.cpp
    # ECS
    src/ECS/SceneECS.cpp
    src/ECS/ComponentRegistry.cpp
    src/ECS/ECSStatics.cpp
    src/ECS/ScriptSystem.cpp
    src/ECS/ScriptContext.cpp
    src/ECS/Systems/RenderSystem.cpp
    src/ECS/PhysicsSystem.cpp
    # Runtime systems
    src/Core/PhysicsManager.cpp
    src/Core/InputController.cpp
    src/Core/VirtualJoystick.cpp
    src/Core/InputSystem.cpp
    src/Core/Physics2DManager.cpp
    src/Core/Physics2DSystem.cpp
    src/Core/SaveSystem.cpp
    src/Core/Camera2DSystem.cpp
    src/Core/ThirdPersonCameraSystem.cpp
    src/Core/PlayerControllerSystem.cpp
    src/Core/TilemapSystem.cpp
    src/Animation/VmdMotion.cpp
    src/Animation/MMD/Base/MMDTypes.cpp
    src/Animation/MMD/Model/MMDIkSolver.cpp
    src/Animation/MMD/Model/MMDModel.cpp
    src/Animation/MMD/Model/MMDMorph.cpp
    src/Animation/MMD/Model/MMDNode.cpp
    src/Animation/MMD/Model/PMXFile.cpp
    src/Animation/MMD/Model/VMDAnimation.cpp
    src/Animation/MMD/Model/VMDFile.cpp
    src/Animation/MMD/Physics/MMDPhysics.cpp
    src/Animation/MmdRuntime.cpp
    src/ECS/Systems/VmdSystem.cpp
    src/Core/PaletteManager.cpp
    src/Core/SceneManager.cpp
    src/Core/TmxLoader.cpp
    src/StbVorbis.c
    dependencies/tinyxml2/tinyxml2.cpp
    dependencies/zlib/inflate.c
    dependencies/zlib/compress.c
    dependencies/zlib/uncompr.c
    dependencies/zlib/deflate.c
    dependencies/zlib/trees.c
    dependencies/zlib/inftrees.c
    dependencies/zlib/inffast.c
    dependencies/zlib/adler32.c
    dependencies/zlib/crc32.c
    dependencies/zlib/zutil.c
    src/Rendering/Camera.cpp
    src/Core/AudioManager.cpp
    src/Rendering/ComputeShader.cpp
    src/Rendering/HiZComputeShader.cpp
    src/Rendering/AtmosphereRenderer.cpp
    src/Rendering/AtmosphereLUT.cpp
    src/Rendering/ModelLoader.cpp
    src/Rendering/ModelBVH.cpp
    src/Rendering/QuadTreeCulling.cpp
    src/Core/SceneSerializer.cpp
    src/Core/TerrainPaintPersistence.cpp
    src/Core/SceneSerializer_3D.cpp
    src/Core/SceneSerializer_2D.cpp
    src/Rendering/VoxLoader.cpp
    src/Rendering/CxwLoader.cpp
    src/Rendering/RayTracing/VoxLod.cpp
    src/Rendering/RayTracing/VoxLodGeometry.cpp
    # 项目插件不编入运行时核心；Windows 端由 tools/compile_games.ps1 按项目单独编译。
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
    src/UI/UiDocument.cpp
    src/UI/RuntimeSettingsOverlay.cpp
    src/UI/VoxelHotbar.cpp
    src/UI/TweenSystem.cpp
    # 世界 ECS 系统
    src/ECS/Systems/WorldSystem.cpp
    src/ECS/Systems/SpriteAnimatorSystem.cpp
    src/ECS/Systems/AudioSourceSystem.cpp
    # ImGui lives in Game.dll so both Game and Editor share one context
    $<$<NOT:$<BOOL:${WIN32}>>:${IMGUI_SOURCES}>
)

add_library(Game SHARED ${GAME_SOURCES})
add_library(MikanMeshoptimizer STATIC dependencies/meshoptimizer/src/allocator.cpp
    dependencies/meshoptimizer/src/indexgenerator.cpp dependencies/meshoptimizer/src/remesher.cpp
    dependencies/meshoptimizer/src/simplifier.cpp dependencies/meshoptimizer/src/vfetchoptimizer.cpp)
target_include_directories(MikanMeshoptimizer PUBLIC dependencies/meshoptimizer/src)
target_link_libraries(Game PRIVATE MikanMeshoptimizer)
add_executable(MikanCxwInspect tools/standalone/CxwInspect.cpp)
target_link_libraries(MikanCxwInspect PRIVATE Game)
target_include_directories(MikanCxwInspect PRIVATE include dependencies dependencies/glm)
target_compile_definitions(MikanCxwInspect PRIVATE MIKAN_USE_GAME)
add_executable(MikanVoxComponentStats tools/standalone/VoxComponentStats.cpp)
target_link_libraries(MikanVoxComponentStats PRIVATE Game)
target_include_directories(MikanVoxComponentStats PRIVATE $<TARGET_PROPERTY:Game,INCLUDE_DIRECTORIES>)
target_compile_definitions(MikanVoxComponentStats PRIVATE MIKAN_USE_GAME NOMINMAX)
add_executable(MikanVoxMeshCompare tools/standalone/VoxMeshCompare.cpp)
target_link_libraries(MikanVoxMeshCompare PRIVATE Game)
target_include_directories(MikanVoxMeshCompare PRIVATE $<TARGET_PROPERTY:Game,INCLUDE_DIRECTORIES>)
target_compile_definitions(MikanVoxMeshCompare PRIVATE MIKAN_USE_GAME NOMINMAX)
add_executable(MikanVoxDestructionBench tools/standalone/VoxDestructionBench.cpp)
target_link_libraries(MikanVoxDestructionBench PRIVATE Game)
target_include_directories(MikanVoxDestructionBench PRIVATE $<TARGET_PROPERTY:Game,INCLUDE_DIRECTORIES>)
target_compile_definitions(MikanVoxDestructionBench PRIVATE MIKAN_USE_GAME NOMINMAX)
add_executable(MikanVoxLodBake tools/standalone/VoxLodBake.cpp)
target_link_libraries(MikanVoxLodBake PRIVATE Game)
target_include_directories(MikanVoxLodBake PRIVATE $<TARGET_PROPERTY:Game,INCLUDE_DIRECTORIES>)
target_compile_definitions(MikanVoxLodBake PRIVATE MIKAN_USE_GAME NOMINMAX)
include(cmake/Fsr2.cmake)
include(cmake/Nrd.cmake)
include(cmake/DlssRayReconstruction.cmake)
include(cmake/DlssFrameGeneration.cmake)
if(WIN32)
    string(JOIN "\n" nvidia_runtime_files ${MIKAN_NVIDIA_RUNTIME_FILES})
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/nvidia-runtime-files.txt"
        CONTENT "${nvidia_runtime_files}\n")
endif()
if(MSVC)
    # 生成 Game.pdb 供崩溃符号化。/DEBUG:FASTLINK 实测对当前规模（314 obj）链接耗时无改善（~2.9s），
    # 且 FASTLINK PDB 事后符号化兼容性更差，故保留 FULL /DEBUG。
    target_link_options(Game PRIVATE /DEBUG "/MAP")
endif()
set_target_properties(Game PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_compile_definitions(Game PRIVATE
    MIKAN_BUILD_GAME
    ${JOLT_COMPILE_DEFINITIONS})
target_include_directories(Game PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Rendering
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ECS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Editor
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/box2d/include
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/box2d/src
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/box2d/extern/simde
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/FastNoiseLite/Cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/SDL3/include
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/SDL3_image/include
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/vulkan/include
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/glm
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/glm/glm
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/assimp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/JoltPhysics
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ktx/include
)
if(WIN32)
    target_link_directories(Game PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib/x64 ${CMAKE_CURRENT_SOURCE_DIR}/tools/ktx/lib)
endif()
if(WIN32)
    if(NOT MIKAN_RUNTIME_IMGUI)
        set(MIKAN_RUNTIME_IMGUI ImGui)
    endif()
    target_link_libraries(Game PRIVATE ${MIKAN_RUNTIME_IMGUI})
    target_compile_definitions(Game PRIVATE "IMGUI_API=__declspec(dllexport)" "IMGUI_IMPL_API=__declspec(dllexport)")
    target_link_options(Game PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:${MIKAN_RUNTIME_IMGUI}>")
endif()

target_link_libraries(Game PRIVATE JoltPhysics msdfgen Box2D)
if(WIN32)
    target_link_libraries(Game PRIVATE SDL3::SDL3-shared SDL3_image vulkan-1 assimp-vc143-mtd)  # ktx 走运行时 LoadLibrary 动态加载
    target_sources(Game PRIVATE src/Platform/Windows/SystemFont.cpp)
    target_link_libraries(Game PRIVATE imm32 version dxguid ole32 winmm dwrite dbghelp psapi)  # winmm/ole32: miniaudio WASAPI 需要; dbghelp/psapi: 崩溃栈符号化
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(Game PRIVATE SDL3::SDL3-static SDL3_image::SDL3_image-static
        MikanVulkanLoader "${MIKAN_ASSIMP_LIBRARY}" ${CMAKE_DL_LIBS} Threads::Threads m)
else()
    target_link_libraries(Game PRIVATE SDL3 SDL3_image vulkan-1 assimp-vc143-mtd)
endif()

# ======================= Editor.dll - editor (ImGui windows, assets, previews) =======================
if(WIN32)
set(EDITOR_SOURCES
    src/Editor/EditorManager.cpp
    src/Editor/AssetsWindow.cpp
    src/Editor/LogWindow.cpp
    src/Editor/AssetPathPicker.cpp
    src/Editor/ComponentInspector.cpp
    src/Editor/CommandConsoleWindow.cpp
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
    src/Editor/ScenePicking.cpp
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
if(WIN32)
    target_compile_definitions(Editor PRIVATE "IMGUI_API=__declspec(dllimport)" "IMGUI_IMPL_API=__declspec(dllimport)")
endif()
target_include_directories(Editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Rendering
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ECS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Editor
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/FastNoiseLite/Cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ImGuizmo
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/SDL3/include
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/SDL3_image/include
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/vulkan/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ktx/include
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/glm
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/glm/glm
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/JoltPhysics
)
target_link_directories(Editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib/x64)
target_link_libraries(Editor PRIVATE Game SDL3::SDL3-shared SDL3_image vulkan-1)
target_link_libraries(Editor PRIVATE imm32 version dxguid comdlg32 shell32 ole32)
endif()

# ======================= MikanEngine.exe - thin host =======================
add_executable(MikanEngine src/HostMain.cpp)
target_link_libraries(MikanEngine PRIVATE Game)
if(WIN32)
    # Embed the tracked Windows icon. Keep the resource optional so a stripped
    # source distribution without resources/windows can still build.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/mikan_engine.rc")
        target_sources(MikanEngine PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/mikan_engine.rc)
    endif()
    target_link_libraries(MikanEngine PRIVATE shell32)
endif()
# 让默认构建宿主程序时连带产出 Editor.dll（Editor 依赖 Game，故 Game+Editor+MikanEngine 一并构建）。
# 发布阶段：直接删除 Editor.dll，Game.dll 启动时检测不到即以纯游戏模式运行（见 EngineMain.cpp DetectEditorDll）。
if(WIN32)
    add_dependencies(MikanEngine Editor)
endif()

# ======================= MikanTestRunner.exe - gameplay-only host =======================
# Links Game.dll for ECS/physics/plugin code, but its entry never initializes SDL Video or Vulkan.
add_executable(MikanTestRunner src/Core/GameplayTestHost.cpp)

# Headless vox reconstruction: existing runtime geometry/AS modules, no raster pipeline.
target_sources(Game PRIVATE
    src/Rendering/RayTracing/RayTracingValidationDevice.cpp
    src/Rendering/RayTracing/RayTracingValidation.cpp)
add_executable(MikanVoxRTValidate src/Core/VoxRayTracingValidationHost.cpp)
target_include_directories(MikanVoxRTValidate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(MikanVoxRTValidate PRIVATE Game)
target_link_libraries(MikanTestRunner PRIVATE Game)
target_include_directories(MikanTestRunner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(WIN32)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/mikan_engine.rc")
        target_sources(MikanTestRunner PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/mikan_engine.rc)
    endif()
    target_link_libraries(MikanTestRunner PRIVATE shell32)
endif()

# ======================= Post-build: copy DLLs & assets =======================
if(WIN32)
    set(DLL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/x64")
    set(KTX_DLL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tools/ktx/bin/ktx.dll")
    foreach(DLL_NAME SDL3_image.dll assimp-vc143-mtd.dll ktx.dll)
        set(DLL_SOURCE_PATH "${DLL_SOURCE_DIR}/${DLL_NAME}")
        if("${DLL_NAME}" STREQUAL "ktx.dll")
            set(DLL_SOURCE_PATH "${KTX_DLL_SOURCE}")
        endif()
        if(EXISTS "${DLL_SOURCE_PATH}")
            add_custom_command(TARGET MikanEngine POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DLL_SOURCE_PATH}"
                    $<TARGET_FILE_DIR:MikanEngine>
                COMMENT "Copy ${DLL_NAME}")
            add_custom_command(TARGET MikanTestRunner POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DLL_SOURCE_PATH}"
                    $<TARGET_FILE_DIR:MikanTestRunner>
                    COMMENT "Copy ${DLL_NAME} for gameplay test runner")
        endif()
    endforeach()

    # Compile shaders (GLSL -> SPIR-V)；引擎系统资产在 engine/（职责分离：engine/=引擎，assets/=游戏内容）
    set(GLSLANG_VALIDATOR "${CMAKE_CURRENT_SOURCE_DIR}/tools/glslang/bin/glslangValidator.exe")
else()
    find_program(GLSLANG_VALIDATOR NAMES glslangValidator REQUIRED)
endif()
if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(cmake/CompileShaders.cmake)
    mikan_compile_shaders("${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/glsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/spv" "${GLSLANG_VALIDATOR}")
    add_dependencies(MikanEngine CompileShaders)
    add_dependencies(MikanVoxRTValidate CompileShaders)
endif()

# CPU-only MMD/Jolt contract tests compile the same runtime sources.
add_executable(MikanMmdTests tests/MmdRuntimeTests.cpp src/Animation/VmdMotion.cpp
    src/Animation/MMD/Base/MMDTypes.cpp
    src/Animation/MMD/Model/MMDIkSolver.cpp
    src/Animation/MMD/Model/MMDModel.cpp
    src/Animation/MMD/Model/MMDMorph.cpp
    src/Animation/MMD/Model/MMDNode.cpp
    src/Animation/MMD/Model/PMXFile.cpp
    src/Animation/MMD/Model/VMDAnimation.cpp
    src/Animation/MMD/Model/VMDFile.cpp
    src/Animation/MMD/Physics/MMDPhysics.cpp
    src/Animation/MmdRuntime.cpp
)
target_include_directories(MikanMmdTests PRIVATE include dependencies dependencies/glm
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/glm/glm dependencies/JoltPhysics)
target_compile_definitions(MikanMmdTests PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS ${JOLT_COMPILE_DEFINITIONS})
target_link_libraries(MikanMmdTests PRIVATE JoltPhysics)
enable_testing()
add_executable(MikanHiZTests tests/HiZGpuTests.cpp)
target_include_directories(MikanHiZTests PRIVATE include dependencies dependencies/glm
    ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/glm/glm dependencies/imgui dependencies/imgui/backends
    dependencies/vulkan/include)
target_compile_definitions(MikanHiZTests PRIVATE MIKAN_USE_GAME NOMINMAX)
target_link_directories(MikanHiZTests PRIVATE lib/x64)
if(WIN32)
    target_link_libraries(MikanHiZTests PRIVATE Game vulkan-1)
else()
    target_link_libraries(MikanHiZTests PRIVATE Game MikanVulkanLoader)
endif()
if(TARGET CompileShaders)
    add_dependencies(MikanHiZTests CompileShaders)
endif()
add_test(NAME grass_hiz_gpu COMMAND MikanHiZTests)
set_tests_properties(grass_hiz_gpu PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" SKIP_RETURN_CODE 77 TIMEOUT 90)
add_test(NAME mmd_interpolation COMMAND MikanMmdTests --interpolation)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/projects/mmd-jolt/models/miku/miku.pmx")
add_test(NAME mmd_jolt COMMAND MikanMmdTests
    "${CMAKE_CURRENT_SOURCE_DIR}/projects/mmd-jolt/models/miku/miku.pmx"
    "${CMAKE_CURRENT_SOURCE_DIR}/projects/mmd-jolt/animations/dance.vmd"
    "${CMAKE_CURRENT_SOURCE_DIR}/projects/mmd-jolt/animations/face.vmd")

endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/projects/mmd-dance-prototype/models/miku/miku.pmx")
add_test(NAME mmd_dance_prototype COMMAND MikanMmdTests
    "${CMAKE_CURRENT_SOURCE_DIR}/projects/mmd-dance-prototype/models/miku/miku.pmx"
    "${CMAKE_CURRENT_SOURCE_DIR}/projects/mmd-dance-prototype/animations/dance.vmd"
    "${CMAKE_CURRENT_SOURCE_DIR}/projects/mmd-dance-prototype/animations/face.vmd"
    "${CMAKE_CURRENT_SOURCE_DIR}/projects/mmd-dance-prototype/animations/camera.vmd")
endif()
option(MIKAN_MMD_ASAN "Enable AddressSanitizer for MMD contract tests" OFF)
if(MSVC AND MIKAN_MMD_ASAN)
    target_compile_definitions(MikanMmdTests PRIVATE _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
    target_compile_options(MikanMmdTests PRIVATE /fsanitize=address)
    target_link_options(MikanMmdTests PRIVATE /INCREMENTAL:NO)
endif()
