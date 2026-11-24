# Native runtime sources shared by Android and HarmonyOS.
# ======================= ImGui (compiled into the native runtime) =======================
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

set(MIKAN_MOBILE_RUNTIME_SOURCES
    # Core
    ${PROJECT_ROOT}/src/Core/EngineGlobals.cpp
    ${PROJECT_ROOT}/src/Core/I18n.cpp
    ${PROJECT_ROOT}/src/Core/PipelineSettings.cpp
    ${PROJECT_ROOT}/src/Core/JobSystem.cpp
    ${PROJECT_ROOT}/src/Core/EngineCommandLine.cpp
    ${PROJECT_ROOT}/src/Core/EngineDiagnostics.cpp
    ${PROJECT_ROOT}/src/Core/EngineShutdown.cpp
    ${PROJECT_ROOT}/src/Core/AutosaveService.cpp
    ${PROJECT_ROOT}/src/Core/ProjectManager.cpp
    ${PROJECT_ROOT}/src/Core/EngineMain.cpp
    ${PROJECT_ROOT}/src/Core/DlssFrameGeneration.cpp
    ${PROJECT_ROOT}/src/Core/GameplayRuntime.cpp
    ${PROJECT_ROOT}/src/Core/GameplayTestMain.cpp
    ${PROJECT_ROOT}/src/Core/RuntimeCapabilities.cpp
    ${PROJECT_ROOT}/src/Core/VulkanManager.cpp
    ${PROJECT_ROOT}/src/Core/VulkanGpuProfiler.cpp
    ${PROJECT_ROOT}/src/Core/VulkanPresentation.cpp
    ${PROJECT_ROOT}/src/Core/VulkanStartupPasses.cpp
    ${PROJECT_ROOT}/src/Core/VulkanDeviceSetup.cpp
    ${PROJECT_ROOT}/src/Core/VulkanWindowLifecycle.cpp
    ${PROJECT_ROOT}/src/Core/VulkanDiagnostics.cpp
    ${PROJECT_ROOT}/src/Core/VulkanCompositeResources.cpp
    ${PROJECT_ROOT}/src/Core/VulkanPostProcessHistory.cpp
    ${PROJECT_ROOT}/src/Core/VulkanPostProcessChains.cpp
    ${PROJECT_ROOT}/src/Core/VulkanRenderHelpers.cpp
    ${PROJECT_ROOT}/src/Core/VulkanLightingCulling.cpp
    ${PROJECT_ROOT}/src/Core/VulkanFramePipeline.cpp
    ${PROJECT_ROOT}/src/Core/VulkanFrameLoop.cpp
    ${PROJECT_ROOT}/src/Core/VulkanRuntimeResources.cpp
    ${PROJECT_ROOT}/src/Core/VulkanCompositeLifecycle.cpp
    ${PROJECT_ROOT}/src/Core/VulkanSwapchainLifecycle.cpp
    ${PROJECT_ROOT}/src/Core/VulkanShutdown.cpp
    ${PROJECT_ROOT}/src/Core/Log.cpp
    ${PROJECT_ROOT}/src/Core/EngineAssets.cpp
    ${PROJECT_ROOT}/src/Core/AssetRegistry.cpp
    ${PROJECT_ROOT}/src/Core/AssetHotReload.cpp
    ${PROJECT_ROOT}/src/Core/ConsoleCommands.cpp
    ${PROJECT_ROOT}/src/Core/ScreenshotCapture.cpp
    ${PROJECT_ROOT}/src/Core/PipelineCapture.cpp
    ${PROJECT_ROOT}/src/Core/RenderDocCapture.cpp
    # Rendering
    ${PROJECT_ROOT}/src/Rendering/SceneRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/GltfLights.cpp
    ${PROJECT_ROOT}/src/Rendering/RenderStats.cpp
    ${PROJECT_ROOT}/src/Rendering/SceneReflectionProbe.cpp
    ${PROJECT_ROOT}/src/Rendering/SceneFramePreparation.cpp
    ${PROJECT_ROOT}/src/Rendering/SceneGeometryPass.cpp
    ${PROJECT_ROOT}/src/Rendering/SceneEnvironmentPass.cpp
    ${PROJECT_ROOT}/src/Rendering/SceneDebugPass.cpp
    ${PROJECT_ROOT}/src/Rendering/SceneShadowPass.cpp
    ${PROJECT_ROOT}/src/Rendering/RenderWorldBuilder.cpp
    ${PROJECT_ROOT}/src/Rendering/RenderWorldPartition.cpp
    ${PROJECT_ROOT}/src/Rendering/RenderWorldExtraction.cpp
    ${PROJECT_ROOT}/src/Rendering/RenderWorldValidation.cpp
    ${PROJECT_ROOT}/src/Rendering/RenderWorldFinalizeWorker.cpp
    ${PROJECT_ROOT}/src/Core/VulkanRayTracingDevice.cpp
    ${PROJECT_ROOT}/src/Rendering/RayTracing/AccelerationStructure.cpp
    ${PROJECT_ROOT}/src/Rendering/RayTracing/VoxRayTracingGeometry.cpp
    ${PROJECT_ROOT}/src/Rendering/RayTracing/VoxLod.cpp
    ${PROJECT_ROOT}/src/Rendering/RayTracing/VoxLodGeometry.cpp
    ${PROJECT_ROOT}/src/Rendering/RayTracing/RayTracingScene.cpp
    ${PROJECT_ROOT}/src/Rendering/SceneRayTracing.cpp
    ${PROJECT_ROOT}/src/Rendering/RayTracing/RayTracingViewport.cpp
    ${PROJECT_ROOT}/src/Rendering/RayTracing/RayTracingViewportResources.cpp
    ${PROJECT_ROOT}/src/Rendering/RayTracing/RayTracingViewportEnvironment.cpp
    ${PROJECT_ROOT}/src/Rendering/RayTracing/ModelRayTracingGeometry.cpp
    ${PROJECT_ROOT}/src/Rendering/RayTracing/RayTracingSceneModels.cpp
    ${PROJECT_ROOT}/src/Rendering/RayTracing/RayTracingViewportModels.cpp
    ${PROJECT_ROOT}/src/Rendering/RayTracing/RayTracingViewportDenoising.cpp
    ${PROJECT_ROOT}/src/Core/VulkanRayTracingPipeline.cpp
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
    ${PROJECT_ROOT}/src/Rendering/InfiniteGridRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/PostProcessQuad.cpp
    ${PROJECT_ROOT}/src/Rendering/PostProcessChain.cpp
    ${PROJECT_ROOT}/src/Rendering/Denoising/DiffuseDenoiser.cpp
    ${PROJECT_ROOT}/src/Rendering/Denoising/DlssRayReconstruction.cpp
    ${PROJECT_ROOT}/src/Rendering/CloudNoise3D.cpp
    ${PROJECT_ROOT}/src/Rendering/CMAA2.cpp
    ${PROJECT_ROOT}/src/Rendering/RendererBase.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererLifecycle.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererBuffers.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererGeometry.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererLoading.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererAnimation.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererSkinning.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererMaterials.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererQueries.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererBatching.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererInstanceBuffer.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererInstanced.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererInstancedVariants.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererDepth.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererShadow.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererPipeline.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererDescriptors.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelRendererSorting.cpp
    ${PROJECT_ROOT}/src/Rendering/ModelIndirectRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/PmxRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/TerrainRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/TerrainGrassRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/TerrainBrush.cpp
    ${PROJECT_ROOT}/src/Rendering/WaterRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/WaterTargetRT.cpp
    ${PROJECT_ROOT}/src/Rendering/WireframeRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/VoxRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/VoxelDDARegistry.cpp
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
    ${PROJECT_ROOT}/src/Rendering/CpuClothSimulation.cpp
    ${PROJECT_ROOT}/src/Rendering/CpuClothRenderer.cpp
    ${PROJECT_ROOT}/src/Rendering/GpuSphSimulation.cpp
    ${PROJECT_ROOT}/src/Rendering/GpuSphContainerRenderer.cpp
    # ECS
    ${PROJECT_ROOT}/src/ECS/SceneECS.cpp
    ${PROJECT_ROOT}/src/ECS/ComponentRegistry.cpp
    ${PROJECT_ROOT}/src/ECS/ECSStatics.cpp
    ${PROJECT_ROOT}/src/ECS/ScriptSystem.cpp
    ${PROJECT_ROOT}/src/ECS/ScriptContext.cpp
    ${PROJECT_ROOT}/src/Animation/VmdMotion.cpp
    ${PROJECT_ROOT}/src/Animation/MMD/Base/MMDTypes.cpp
    ${PROJECT_ROOT}/src/Animation/MMD/Model/MMDIkSolver.cpp
    ${PROJECT_ROOT}/src/Animation/MMD/Model/MMDModel.cpp
    ${PROJECT_ROOT}/src/Animation/MMD/Model/MMDMorph.cpp
    ${PROJECT_ROOT}/src/Animation/MMD/Model/MMDNode.cpp
    ${PROJECT_ROOT}/src/Animation/MMD/Model/PMXFile.cpp
    ${PROJECT_ROOT}/src/Animation/MMD/Model/VMDAnimation.cpp
    ${PROJECT_ROOT}/src/Animation/MMD/Model/VMDFile.cpp
    ${PROJECT_ROOT}/src/Animation/MMD/Physics/MMDPhysics.cpp
    ${PROJECT_ROOT}/src/Animation/MmdRuntime.cpp
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
    ${PROJECT_ROOT}/dependencies/zlib/compress.c
    ${PROJECT_ROOT}/dependencies/zlib/uncompr.c
    ${PROJECT_ROOT}/dependencies/zlib/deflate.c
    ${PROJECT_ROOT}/dependencies/zlib/trees.c
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
    ${PROJECT_ROOT}/src/Core/TerrainPaintPersistence.cpp
    ${PROJECT_ROOT}/src/Core/SceneSerializer_3D.cpp
    ${PROJECT_ROOT}/src/Core/SceneSerializer_2D.cpp
    ${PROJECT_ROOT}/src/Rendering/VoxLoader.cpp
    ${PROJECT_ROOT}/src/Rendering/CxwLoader.cpp
    ${PROJECT_ROOT}/dependencies/meshoptimizer/src/allocator.cpp
    ${PROJECT_ROOT}/dependencies/meshoptimizer/src/indexgenerator.cpp
    ${PROJECT_ROOT}/dependencies/meshoptimizer/src/remesher.cpp
    ${PROJECT_ROOT}/dependencies/meshoptimizer/src/simplifier.cpp
    ${PROJECT_ROOT}/dependencies/meshoptimizer/src/vfetchoptimizer.cpp
    # 游戏玩法原型(贪吃蛇等示例)
    ${PROJECT_ROOT}/src/Game/GameManager.cpp
    # third-person 原型玩法与场景脚本（Android 端静态编入 so，替代桌面 DLL 插件）
    # Optional local project gameplay is added below when available.
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
    ${PROJECT_ROOT}/src/UI/UiDocument.cpp
    ${PROJECT_ROOT}/src/UI/RuntimeSettingsOverlay.cpp
    ${PROJECT_ROOT}/src/UI/VoxelHotbar.cpp
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
