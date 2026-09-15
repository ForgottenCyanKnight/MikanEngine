static int RunEngineSceneStartup(
    bool skipProjectManager,
    const std::string& sceneArg,
    const std::string& gameArg,
    bool prefabSelftest,
    int w,
    int h,
    const std::function<void(float, const char*)>& renderStartupLoading)
{
    // 所有系统初始化完成后加载场景。
    // 桌面端只有选定项目后才进入运行时；--scene 和 project.json.scene
    // 都相对于当前项目的 resourceRoot 解析。Android 由同步脚本选择项目
    // 并写入 APK 专用启动场景配置，因为 Android 没有桌面端项目管理器流程。
    const bool hasSelectedProject = ProjectManager::GetInstance().HasActiveProject();
#ifdef __ANDROID__
    const bool shouldLoadScene = hasSelectedProject || skipProjectManager;
#else
    const bool shouldLoadScene = hasSelectedProject;
    if (!sceneArg.empty() && !hasSelectedProject) {
        LOGI("[Startup] --scene requires an explicitly selected project");
        return 2;
    }
#endif
    if (shouldLoadScene) {
        bool loaded = false;
        bool sceneLoadedForRuntime = false;
        std::string loadedScenePath;
        renderStartupLoading(0.12f, "Loading scene...");
        if (!sceneArg.empty()) {
            const std::string scenePath =
                ProjectManager::GetInstance().ResolveAssetPath(sceneArg);
            if (scenePath.empty()) {
                LOGE("[Startup] Cannot resolve scene '%s' inside the selected project",
                        sceneArg.c_str());
                return 2;
            }
            ECS::SceneSerializer sceneLoader;
            if (sceneLoader.LoadScene(scenePath)) {
                LOGI("Scene loaded: %s", scenePath.c_str());
                loaded = true;
                sceneLoadedForRuntime = true;
                loadedScenePath = scenePath;
            } else {
                LOGE("[Startup] Failed to load scene '%s'", scenePath.c_str());
                return 2;
            }
        }
        if (!loaded) {
#ifdef __ANDROID__
            // Android 无命令行参数机制，由同步脚本把选定项目的 manifest.scene
            // 写入 mikan_android_scene.txt；对应玩法已静态编入 Android so。
            // SceneSerializer::LoadScene 的 Android 分支走 SDL_IOFromFile（APK assets 安全）。
            {
                const std::string androidScenePath = ReadAndroidStartupScenePath();
                ECS::SceneSerializer sceneLoader;
                if (sceneLoader.LoadScene(androidScenePath)) {
                    LOGI("Android project scene loaded: %s", androidScenePath.c_str());
                    LOGI("Android project scene loaded: %s", androidScenePath.c_str());
                    loaded = true;
                    sceneLoadedForRuntime = true;
                    loadedScenePath = androidScenePath;
                } else {
                    LOGE("Android project scene '%s' load failed", androidScenePath.c_str());
                    LOGE("Android project scene '%s' load FAILED", androidScenePath.c_str());
                }
            }
            if (!loaded) {
                LOGW("[Startup] Android project scene is unavailable; run sync_assets.ps1 for a selected project");
                return 2;
            }
#else
            // 桌面端只使用当前项目 manifest 指定的场景，不创建或寻找默认场景。
            std::string projectScenePath;
            if (ProjectManager::GetInstance().IsManifestProject()) {
                const ProjectManifest& manifest =
                    ProjectManager::GetInstance().GetManifest();
                if (!manifest.scene.empty()) {
                    projectScenePath =
                        ProjectManager::GetInstance().ResolveAssetPath(manifest.scene);
                }
            }

            if (projectScenePath.empty()) {
                LOGI(
                        "[Startup] Selected project has no scene (set project.json.scene)");
                return 2;
            }

            ECS::SceneSerializer sceneLoader;
            if (!sceneLoader.LoadScene(projectScenePath)) {
                LOGE("[Startup] Failed to load selected project scene '%s'",
                        projectScenePath.c_str());
                return 2;
            }
            LOGI("Project scene loaded: %s", projectScenePath.c_str());
            loaded = true;
            sceneLoadedForRuntime = true;
            loadedScenePath = projectScenePath;
#endif
        }

        if (!loaded || !sceneLoadedForRuntime) {
            LOGI("[Startup] Scene loading did not produce a runnable scene");
            return 2;
        }

        // 启动时真正加载的场景即"当前场景"：--scene 覆盖了 manifest.scene 时，
        // 工具栏保存/重载与自动快照必须跟随实际加载的文件，而不是清单里的那个。
        ProjectManager::GetInstance().SetActiveScenePath(loadedScenePath);

        Physics2DSystem::GetInstance().ClearBodies(); // 场景重建后清理旧 2D 刚体
        Camera2DSystem::GetInstance().SetSceneContext(loadedScenePath);
        Camera2DSystem::GetInstance().Reset();
        Camera2DSystem::GetInstance().Rebind();
        ThirdPersonCameraSystem::GetInstance().SetSceneContext(loadedScenePath);
        ThirdPersonCameraSystem::GetInstance().Reset();

        // 瓦片地图: 遍历场景加载所有带 TilemapComponent 的实体(TMX/自产解析 + 图集纹理 + Box2D 碰撞体)
        TilemapSystem::GetInstance().LoadAllFromScene();
        renderStartupLoading(0.78f, "Preparing gameplay...");

        // 激活游戏模块: 优先 --game <name> 参数;否则按场景文件顶层 "game" 键，
        // 再回退到当前项目 manifest 的 game 字段。
        std::string effectiveGame = gameArg;
        if (effectiveGame.empty()) {
            effectiveGame = ECS::SceneECS::GetInstance().GetSceneGameModule();
        }
        if (effectiveGame.empty() && ProjectManager::GetInstance().HasManifest()) {
            effectiveGame = ProjectManager::GetInstance().GetManifest().game;
        }
        bool gameReadyForRuntime = effectiveGame.empty();
        if (!effectiveGame.empty()) {
            if (auto* gm = Game::GameManager::GetInstance().Activate(effectiveGame)) {
                gm->OnSceneLoaded();
                gameReadyForRuntime = true;
            } else {
                LOGE("[Startup] game module activation failed: %s", effectiveGame.c_str());
            }
            // 补齐脚本实例：插件 DLL 在 Activate 时才加载并注册脚本工厂，而场景反序列化
            // （InstantiateAll）可能早于它——此处幂等补齐缺失实例（已创建的不动）。
            ECS::ScriptSystem::GetInstance().InstantiateAll(false);

            // 预制体自测：保存 CesiumMan 子树 -> 实例化 -> 断言（--prefab-selftest）
            if (prefabSelftest) {
                return Core::RunPrefabSelftest();
            }
        }
        const bool runtimeReady = sceneLoadedForRuntime && gameReadyForRuntime;
        const std::string readinessStatus = runtimeReady
            ? (effectiveGame.empty() ? "scene-ready" : "scene-and-game-ready")
            : (sceneLoadedForRuntime ? "scene-ready-game-activation-failed" : "scene-load-failed");
        Core::ScreenshotCapture::GetInstance().SetSceneReady(runtimeReady, readinessStatus);
        LOGI("[Startup] READY=%s scene=%s game=%s window=%dx%d",
               runtimeReady ? "true" : "false",
               sceneLoadedForRuntime ? "ready" : "failed",
               effectiveGame.empty() ? "none" : (gameReadyForRuntime ? "ready" : "failed"),
               w, h);
        renderStartupLoading(0.94f, "Starting prototype...");
    } else {
        g_ProjectSelectionPending = true;
        LOGI("No project selected: showing project manager (pending selection)");
        Core::ScreenshotCapture::GetInstance().SetSceneReady(false, "project-selection-pending");
    }

    return 0;
}

