static int RunEngineLoop(SDL_Window* window,
                         ImGui_ImplVulkanH_Window* wd,
                         int w, int h,
                         bool headless,
                         bool headlessNoRender,
                         int headlessFrames,
                         float fixedDeltaSeconds,
                         bool editorActive)
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    uint64_t engineCpuStageProfileFrames = 0;
    double engineCpuStageEventsMs = 0.0;
    double engineCpuStageCameraMs = 0.0;
    double engineCpuStagePhysicsMs = 0.0;
    double engineCpuStageWorldMs = 0.0;
    double engineCpuStageLogicMs = 0.0;
    double engineCpuStageGameplayCameraMs = 0.0;
    double engineCpuStageRenderWorldBuildMs = 0.0;
    double engineCpuStageAnimationMs = 0.0;
    // 主循环
    bool done = false;
    int frameCount = 0;
    while (!done)
    {
        const bool engineCpuProfileEnabled = IsEngineCpuProfileEnabled();
        const auto engineFrameStart = engineCpuProfileEnabled
            ? EngineCpuProfileClock::now()
            : EngineCpuProfileClock::time_point{};
        auto currentTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(currentTime - g_LastTime).count();
        g_LastTime = currentTime;
        if (headless) deltaTime = fixedDeltaSeconds;
        // 限制单帧最大步长（100ms = ~10fps 下限）：
        // 调试断点、窗口最小化、驱动卡顿都会产生大 deltaTime，直接传给 Jolt 可能积分出爆炸的力/位移。
        // 超过上限时丢帧（逻辑少推进而不是一次性补巨大步长），保持物理稳定。
        if (deltaTime > 0.1f) deltaTime = 0.1f;

        // 平滑帧率(游戏画面 FPS 显示用)
        g_FPS = g_FPS * 0.9f + (1.0f / (deltaTime > 0.0001f ? deltaTime : 0.0001f)) * 0.1f;

#ifdef __ANDROID__
        // 低频运行时诊断：用于真机确认主循环、帧率和 native 内存状态。
        // 不记录每帧，避免 logcat 刷屏影响性能和诊断结果。
        if ((frameCount % 120) == 0 && frameCount > 0) {
            long residentKb = 0;
            std::ifstream statusFile("/proc/self/status");
            std::string statusLine;
            while (std::getline(statusFile, statusLine)) {
                if (statusLine.rfind("VmRSS:", 0) == 0) {
                    std::sscanf(statusLine.c_str(), "VmRSS: %ld kB", &residentKb);
                    break;
                }
            }
            LOGI("[Runtime] frame=%d fps=%.1f dt=%.2fms native_rss=%ldKB",
                 frameCount, g_FPS, deltaTime * 1000.0f, residentKb);
        }
#endif

        const auto engineCpuEventsStart = engineCpuProfileEnabled
            ? EngineCpuProfileClock::now()
            : EngineCpuProfileClock::time_point{};
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            // 调试：打印所有事件类型
            #ifdef __ANDROID__
            if (event.type >= SDL_EVENT_FIRST && event.type <= SDL_EVENT_LAST) {
                LOGD("[SDL Event] Type: %u, Timestamp: %llu", event.type, event.common.timestamp);
                if (event.type == SDL_EVENT_WINDOW_RESIZED || 
                    event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                    event.type == SDL_EVENT_WINDOW_SHOWN ||
                    event.type == SDL_EVENT_WINDOW_HIDDEN ||
                    event.type == SDL_EVENT_WINDOW_MINIMIZED ||
                    event.type == SDL_EVENT_WINDOW_RESTORED) {
                    LOGI("[SDL Window Event] WindowID: %u, Data1: %d, Data2: %d", 
                           event.window.windowID, event.window.data1, event.window.data2);
                }
            }
            #endif
            
            // Dear ImGui 1.92 保留了基础 Tab 遍历，即使关闭 NavEnableKeyboard 也会
            // 产生焦点蓝框。只过滤发给 ImGui 的 Tab，下面的引擎/游戏输入仍能收到它。
            const bool isTabEvent =
                (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
                event.key.key == SDLK_TAB;
            if (editorActive && !isTabEvent)
                ImGui_ImplSDL3_ProcessEvent(&event);
            // 运行时设置页是项目清单的可选能力，不属于引擎默认 UI。
            // 默认项目不消费这组输入；第三人称原型通过 project.json 显式开启。
            auto* currentGame = Game::GameManager::GetInstance().GetCurrent();
            const bool runtimeSettingsEnabled =
                (g_RunMode == RunMode::Game) && currentGame != nullptr &&
                ProjectManager::GetInstance().GetManifest().runtimeSettingsOverlay;
            if (!runtimeSettingsEnabled &&
                UI::RuntimeSettingsOverlay::GetInstance().IsOpen()) {
                UI::RuntimeSettingsOverlay::GetInstance().SetOpen(false);
            }
            const bool settingsConsumed =
                runtimeSettingsEnabled &&
                UI::RuntimeSettingsOverlay::GetInstance().ProcessEvent(event);
            if (!settingsConsumed && !g_ProjectSelectionPending)
                g_InputController.ProcessInput(event, g_Camera, deltaTime);
            // 键盘事件转发给当前游戏模块(引擎不感知具体游戏)
            if (!settingsConsumed && event.type == SDL_EVENT_KEY_DOWN) {
                // F 键: 切换游戏画面 FPS 显示(全局,菜单提示中有说明)
                if (event.key.key == SDLK_F) {
                    g_ShowFPS = !g_ShowFPS;
                    printf("FPS display %s\n", g_ShowFPS ? "on" : "off");
                }
                // T 键: 切换 2D 碰撞体线框调试显示(物理排错)
                if (event.key.key == SDLK_T) {
                    g_ShowPhysics2DDebug = !g_ShowPhysics2DDebug;
                    printf("Physics2D debug %s\n", g_ShowPhysics2DDebug ? "on" : "off");
                }
                // F12：抓取下一帧最终 Swapchain 画面，供 AI/人工视觉检查。
                if (event.key.key == SDLK_F12) {
                    Core::ScreenshotCapture::GetInstance().Request();
                }
                if (auto* gm = Game::GameManager::GetInstance().GetCurrent()) {
                    gm->OnKey(event.key.key);
                }
            }
            if (event.type == SDL_EVENT_QUIT)
            {
                done = true;
                g_IsPaused = true; // 立即暂停渲染
                break; // 立即跳出事件循环
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))
            {
                done = true;
                g_IsPaused = true; // 立即暂停渲染
                break; // 立即跳出事件循环
            }
            #ifndef __ANDROID__
            if (event.type == SDL_EVENT_WINDOW_RESIZED && event.window.windowID == SDL_GetWindowID(window))
            {
                g_SwapChainRebuild = true;
            }
            #endif
            #ifdef __ANDROID__
            // 处理 Android 特有的事件
            if (event.type == SDL_EVENT_WINDOW_MINIMIZED || event.type == SDL_EVENT_WINDOW_HIDDEN)
            {
                g_IsPaused = true; // 窗口最小化或隐藏时暂停渲染
                LOGI("[Android] Window minimized/hidden - pausing render");
            }
            if (event.type == SDL_EVENT_WINDOW_SHOWN || event.type == SDL_EVENT_WINDOW_RESTORED)
            {
                g_IsPaused = false; // 窗口显示或恢复时恢复渲染
                LOGI("[Android] Window shown/restored - resuming render");
                
                // Android 从后台恢复时，即使没有 RESIZED 事件，也需要重建 surface
                // 因为 surface 可能已经被系统销毁并重新创建
                g_SwapChainRebuild = true;
                LOGI("[Android] Marking swapchain for rebuild on resume");
            }
            // Android 上 Surface 重建时也需要重建 swapchain
            if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
            {
                g_SwapChainRebuild = true;
                LOGI("[Android] Surface changed - rebuilding swapchain (size: %dx%d)", event.window.data1, event.window.data2);
                
                // Surface 改变后立即更新 SDL 对应的 Vulkan surface；同时由
                // CreateAndroidVulkanSurface 保护 Java SurfaceView 的短暂空窗。
                if (g_MainWindowData.Surface != VK_NULL_HANDLE) {
                    LOGI("[Android] Destroying old Vulkan surface");
                    vkDestroySurfaceKHR(g_Instance, g_MainWindowData.Surface, g_Allocator);
                    g_MainWindowData.Surface = VK_NULL_HANDLE;
                }
                
                LOGI("[Android] Creating new Vulkan surface");
                VkSurfaceKHR newSurface;
                if (!CreateAndroidVulkanSurface(window, g_Instance, g_Allocator, &newSurface)) {
                    LOGE("[Android] Failed to create new Vulkan surface!");
                } else {
                    g_MainWindowData.Surface = newSurface;
                    LOGI("[Android] New Vulkan surface created successfully");
                }
            }
            #endif
        }
        const double engineCpuEventsMs = engineCpuProfileEnabled
            ? std::chrono::duration<double, std::milli>(
                EngineCpuProfileClock::now() - engineCpuEventsStart).count()
            : 0.0;

        // 检查是否需要退出
        if (done)
        {
            break; // 立即跳出主循环
        }

        const auto engineCpuCameraStart = engineCpuProfileEnabled
            ? EngineCpuProfileClock::now()
            : EngineCpuProfileClock::time_point{};
        // 项目管理器阶段不再接收游戏输入，也不推进旧项目的相机。
        if (!g_ProjectSelectionPending) {
            // 处理游戏模式/编辑器模式切换
            g_InputController.ProcessModeToggle();

            if (g_RunMode == RunMode::Game) {
                g_InputController.UpdateSceneCamera(deltaTime);
            } else {
                g_InputController.Update(g_Camera, deltaTime);
                g_Camera.Update(deltaTime);
            }
        }
        const double engineCpuCameraMs = engineCpuProfileEnabled
            ? std::chrono::duration<double, std::milli>(
                EngineCpuProfileClock::now() - engineCpuCameraStart).count()
            : 0.0;
        
        // 更新物理系统
        const auto engineCpuPhysicsStart = engineCpuProfileEnabled
            ? EngineCpuProfileClock::now()
            : EngineCpuProfileClock::time_point{};
        bool gameRunning = !g_ProjectSelectionPending;
        bool gamePaused = false;
#ifdef _WIN32
        if (!g_ProjectSelectionPending && editorActive && s_editorIsGameRunning) gameRunning = s_editorIsGameRunning();
        if (!g_ProjectSelectionPending && editorActive && s_editorIsGamePaused) gamePaused = s_editorIsGamePaused();
#endif

        // 播放/暂停/停止状态接入游戏模块: 检测状态边沿, 区分"停止"与"暂停"
        {
            // s_wasGameRunning 跟踪工具栏的原始运行位，而不是
            // gameRunning && !gamePaused。否则从暂停状态点击停止时，
            // 上一帧会被误记为 false，停止回调和场景恢复都会被跳过。
            static bool s_wasGameRunning = false;
            static bool s_wasPaused = false;
            auto* gm = Game::GameManager::GetInstance().GetCurrent();
            if (!s_wasGameRunning && gameRunning) {
                if (editorActive) {
                    CaptureEditorPlaySnapshot();
                }
                if (gm) gm->OnGameStart();            // 播放: 进入运行态
            } else if (s_wasGameRunning && !gameRunning) {
                if (gm) gm->OnGameStop();             // 停止: 先通知游戏清理运行时状态
                if (editorActive) {
                    RestoreEditorPlaySnapshot();      // 停止: 恢复播放前场景
                    s_editorPlaySnapshot.clear();
                    s_editorPlaySnapshotValid = false;
                }
            } else if (s_wasGameRunning && !s_wasPaused && gamePaused) {
                if (gm) gm->OnGamePause();            // 暂停: 只冻结时间,不重置
            } else if (s_wasGameRunning && s_wasPaused && !gamePaused) {
                if (gm) gm->OnGameResume();           // 恢复
            }
            s_wasGameRunning = gameRunning;
            s_wasPaused = gamePaused;
        }

        // 自动快照：只在"可编辑"状态推进计时——编辑器模式 + 项目已加载 + 未播放。
        // 快照写引擎根 out/autosave/，绝不覆盖项目场景文件（见 AutosaveService.h）。
        // 放在播放/停止状态边沿处理之后，停止时的场景恢复已完成，不会快照到运行态。
        {
            const bool editableScene =
                editorActive && !g_ProjectSelectionPending &&
                g_RunMode == RunMode::Editor && !gameRunning;
            AutosaveService::GetInstance().Tick(deltaTime, editableScene);
        }

        if (!g_ProjectSelectionPending && !g_IsPaused && gameRunning && !gamePaused) {
            // 玩家控制器只写入动态刚体速度/朝向，再由下面的 Jolt Step
            // 统一处理地形接触和 Transform 回写。
            coordinator.GetSystem<ECS::PlayerControllerSystem>()->Update(deltaTime);
            // 获取相机位置用于清理远距离刚体
            glm::vec3 cameraPos = g_Camera.Position;
            g_PhysicsSystemPtr->Update(deltaTime, cameraPos);
            // 2D 物理(Box2D): 仅运行态(工具栏播放)步进, 暂停/停止冻结
            Physics2DSystem::GetInstance().Update(deltaTime);
        }
        const double engineCpuPhysicsMs = engineCpuProfileEnabled
            ? std::chrono::duration<double, std::milli>(
                EngineCpuProfileClock::now() - engineCpuPhysicsStart).count()
            : 0.0;
        
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }
        
        // Android 平台：每次渲染前检查 surface 是否有效
        #ifdef __ANDROID__
        if (g_MainWindowData.Surface == VK_NULL_HANDLE || g_MainWindowData.Swapchain == VK_NULL_HANDLE) {
            LOGI("[Android] Surface or Swapchain is null, rebuilding...");
            g_SwapChainRebuild = true;
        }
        #endif

        if (g_SwapChainRebuild)
        {
            int new_width, new_height;
            SDL_GetWindowSize(window, &new_width, &new_height);
            
            // 如果尺寸为 0，跳过重建
            if (new_width == 0 || new_height == 0)
            {
                LOGI("[Android] Window size is 0, skipping rebuild");
                g_SwapChainRebuild = false;
                SDL_Delay(10);
                continue;
            }

            LOGI("[Android] Starting swapchain rebuild process...");
            
            // Android 平台：总是先销毁并重新创建 surface
            // 因为从后台恢复时，即使 surface 句柄有效，底层 surface 也可能已经被系统销毁
            #ifdef __ANDROID__
            if (g_MainWindowData.Surface != VK_NULL_HANDLE) {
                LOGI("[Android] Destroying old Vulkan surface before rebuild");
                vkDestroySurfaceKHR(g_Instance, g_MainWindowData.Surface, g_Allocator);
                g_MainWindowData.Surface = VK_NULL_HANDLE;
            }
            
            // 创建新的 surface
            LOGI("[Android] Creating new Vulkan surface");
            VkSurfaceKHR newSurface;
            if (!CreateAndroidVulkanSurface(window, g_Instance, g_Allocator, &newSurface)) {
                LOGE("[Android] Failed to create Vulkan surface!");
                SDL_Delay(100);
                continue;
            }
            g_MainWindowData.Surface = newSurface;
            LOGI("[Android] New surface created successfully, handle: %p", (void*)newSurface);
            g_IsPaused = false;
            #else
            // 其他平台：仅在 surface 为空时创建
            if (g_MainWindowData.Surface == VK_NULL_HANDLE) {
                LOGI("[Platform] Surface is null, creating new surface...");
                VkSurfaceKHR newSurface;
                if (!SDL_Vulkan_CreateSurface(window, g_Instance, g_Allocator, &newSurface)) {
                    LOGE("[Platform] Failed to create Vulkan surface!");
                    g_SwapChainRebuild = false;
                    SDL_Delay(100);
                    continue;
                }
                g_MainWindowData.Surface = newSurface;
                LOGI("[Platform] New surface created successfully");
            }
            #endif
            
            // RecreateSwapChain 已经处理了渲染目标的重建和描述符集更新
            ::RecreateSwapChain(new_width, new_height);
            
            g_SwapChainRebuild = false;
            LOGI("[Android] Swapchain rebuild completed");
        }

        // 编辑器帧：全部 UI（ImGui/窗口/Gizmo）由 Editor.dll 提供。
        // 游戏模式（RunMode::Game）同样要走编辑器帧：Editor.dll 在该分支下只绘制
        // "控制面板"浮窗（含模式/垂直同步/全屏开关），它是切换回编辑器模式的入口，
        // 必须保留；其余编辑器窗口由 EditorDllApi 按运行模式自行跳过。
        ImDrawData* draw_data = nullptr;
        double editorUiMs = 0.0;
        bool editorUiActive = false;
#ifdef _WIN32
        editorUiActive = editorActive;
        const auto editorUiStart = engineCpuProfileEnabled
            ? EngineCpuProfileClock::now()
            : EngineCpuProfileClock::time_point{};
        if (editorUiActive && s_editorRenderFrame) {
            // 每帧同步渲染目标描述符（resize 重建后 descriptor 会变化）
            // 用"显示附件"（合成 subpass 输出）——SceneView/GameView 面板显示 fullscreen.frag 后处理结果
            if (s_editorSetSceneViewDesc) s_editorSetSceneViewDesc(g_SceneRenderTarget.GetDisplayDescriptorSet());
            if (s_editorSetGameViewDesc) s_editorSetGameViewDesc(g_GameRenderTarget.GetDisplayDescriptorSet());
            s_editorRenderFrame();
        }
        if (engineCpuProfileEnabled && editorUiActive && s_editorRenderFrame) {
            editorUiMs = std::chrono::duration<double, std::milli>(
                EngineCpuProfileClock::now() - editorUiStart).count();
        }
        if (editorUiActive) {
            draw_data = ImGui::GetDrawData();
        }
#endif

        // Profiling-only view override. The editor's active dock tab normally
        // decides which offscreen target is rendered; automated CPU/GPU
        // comparisons need a deterministic way to select one without mouse
        // input. It is gated by MIKAN_CPU_PROFILE and has no effect normally.
        if (engineCpuProfileEnabled) {
            const char* profileView = std::getenv("MIKAN_PROFILE_VIEW");
            if (profileView && std::strcmp(profileView, "scene") == 0) {
                g_ShowSceneView = true;
                g_ShowGameView = false;
            } else if (profileView && std::strcmp(profileView, "game") == 0) {
                g_ShowSceneView = false;
                g_ShowGameView = true;
            } else if (profileView && std::strcmp(profileView, "both") == 0) {
                g_ShowSceneView = true;
                g_ShowGameView = true;
            }
        }

        // 只有编辑器模式依赖 draw_data 判断最小化；独立游戏模式始终渲染
        const bool is_minimized = (editorUiActive &&
            (!draw_data || draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f));
        if (!is_minimized && !g_IsPaused)
        {
            // 项目管理器仍需要提交交换链帧；这里只跳过旧项目的逻辑更新，
            // 由 FrameRender 内部的管理器 pass 清屏并绘制项目管理器 UI。
            if (g_ProjectSelectionPending) {
                if (!headlessNoRender) {
                    Core::RenderDocCapture::GetInstance().BeforeFramePresent(frameCount + 1);
                    const auto engineRenderStart = engineCpuProfileEnabled
                        ? EngineCpuProfileClock::now()
                        : EngineCpuProfileClock::time_point{};
                    ::FrameRender(wd, draw_data, glm::mat4(1.0f), glm::mat4(1.0f));
                    const auto engineAfterRender = engineCpuProfileEnabled
                        ? EngineCpuProfileClock::now()
                        : EngineCpuProfileClock::time_point{};
                    ::FramePresent(wd);
                    const auto engineAfterPresent = engineCpuProfileEnabled
                        ? EngineCpuProfileClock::now()
                        : EngineCpuProfileClock::time_point{};
                    if (engineCpuProfileEnabled) {
                        RecordEngineCpuProfile(engineFrameStart, engineRenderStart,
                                               engineAfterRender, engineAfterPresent,
                                               editorUiMs);
                    }
                }
                ++frameCount;
                if (headless && headlessFrames > 0 && frameCount >= headlessFrames) {
                    done = true;
                }
                continue;
            }

            glm::mat4 view = g_Camera.GetViewMatrix();
            glm::mat4 proj = glm::perspective(glm::radians(EngineConfig::FOV), (float)wd->Width / (float)wd->Height, EngineConfig::NEAR_PLANE, EngineConfig::FAR_PLANE);
            proj[1][1] *= -1;

            ImVec4 clear_color = ImVec4(0.1f, 0.1f, 0.1f, 1.00f);
            wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
            wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
            wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
            wd->ClearValue.color.float32[3] = clear_color.w;
            
            // Android 平台：渲染前检查 surface 状态
            #ifdef __ANDROID__
            if (wd->Surface == VK_NULL_HANDLE || wd->Swapchain == VK_NULL_HANDLE) {
                LOGI("[Android] Render: Surface/Swapchain invalid, triggering rebuild");
                g_SwapChainRebuild = true;
                continue; // 跳过本次渲染，下一帧会重建
            }
            #endif
            
            const auto engineCpuWorldStart = engineCpuProfileEnabled
                ? EngineCpuProfileClock::now()
                : EngineCpuProfileClock::time_point{};
            // 更新体素世界（chunk 生成/网格重建，从 OpenGL 版迁移）
            if (g_EnableVoxelWorld && g_WorldSystemPtr) {
                // 世界生成中心 + 视锥：优先游戏主相机（与 model 渲染的剔除基准一致），fallback 到编辑器相机
                glm::vec3 worldCamPos = g_Camera.Position;
                glm::mat4 worldView = view, worldProj = proj;
                {
                    glm::mat4 wv, wp;
                    glm::vec3 wcp;
                    float aspect = (float)g_GameRenderTarget.GetWidth() / (float)g_GameRenderTarget.GetHeight();
                    if (g_SceneRenderer.GetMainCameraMatrices(aspect, wv, wp, wcp)) {
                        worldCamPos = wcp;
                        worldView = wv;
                        worldProj = wp;
                    }
                }
                auto worldFrustum = AABBUtils::ExtractFrustumPlanes(worldProj * worldView, -10.0f);
                g_WorldSystemPtr->Update(deltaTime, worldCamPos, worldFrustum);

                // 方块交互（从 OpenGL 版迁移）：游戏模式下左键破坏 / 右键放置
                if (g_RunMode == RunMode::Game && g_World != nullptr) {
                    // 活动相机（游戏模式优先场景主相机）
                    glm::vec3 camPos = g_Camera.Position;
                    glm::vec3 camFront = g_Camera.Front;
                    {
                        glm::mat4 wv, wp;
                        glm::vec3 wcp;
                        float aspect = (float)g_GameRenderTarget.GetWidth() / (float)g_GameRenderTarget.GetHeight();
                        if (g_SceneRenderer.GetMainCameraMatrices(aspect, wv, wp, wcp)) {
                            camPos = wcp;
                            camFront = -glm::vec3(wv[0][2], wv[1][2], wv[2][2]);
                        }
                    }
                    World::HitResult hit = g_World->RayCast(camPos, camFront, 10.0f);
                    const int handBlockId = g_HandBlockId; // 手持方块类型（物品栏选中格写入；默认 3=草方块）
                    g_InputController.HandleBlockInteraction(*g_World, handBlockId, hit, deltaTime);
                }
            }
            const double engineCpuWorldMs = engineCpuProfileEnabled
                ? std::chrono::duration<double, std::milli>(
                    EngineCpuProfileClock::now() - engineCpuWorldStart).count()
                : 0.0;
            
            // ===== 2D Canvas 交互更新（无按钮时仅命中测试，无副作用）=====
            const auto engineCpuLogicStart = engineCpuProfileEnabled
                ? EngineCpuProfileClock::now()
                : EngineCpuProfileClock::time_point{};
            {
                // 引擎标准输入: 快照本帧键盘/鼠标状态(游戏经 InputSystem 查询动作)
                Input::InputSystem::GetInstance().Update();
                // Cinemachine 风格 2D 智能相机: 计算 Camera2DComponent.center(跟随/阻尼/前视/边界/震动)
                Camera2DSystem::GetInstance().SetViewport((uint32_t)w, (uint32_t)h);
                Camera2DSystem::GetInstance().Update(deltaTime);
                // 2D 世界层相机:场景树 Camera2DComponent 驱动(无组件默认)
                UI::Canvas2D::GetInstance().SyncCameraFromScene();
                float mx = 0.0f, my = 0.0f;
                Uint32 mouseState = SDL_GetMouseState(&mx, &my);
                UI::Canvas2D::GetInstance().Update(glm::vec2(mx, my),
                    (mouseState & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) != 0);
                UI::TweenSystem::GetInstance().Update(deltaTime);
                // 2D 精灵帧动画(序2): 推进帧号写回 Sprite2D.uv0/uv1
                SpriteAnimatorSystem::GetInstance().Update(deltaTime);
                // 音频源组件驱动（场景树 AudioSourceComponent → AudioManager）
                ECS::AudioSourceSystem::GetInstance().Update(deltaTime);
                // 游戏模块: 始终执行回调(开始后即需响应的逻辑, 如玩家控制/相机)
                if (auto* gm = Game::GameManager::GetInstance().GetCurrent()) {
                    gm->OnAlwaysUpdate(deltaTime);
                }
                // 游戏模块: 逻辑更新仅在运行态(播放且未暂停)时调用
                if (gameRunning && !gamePaused) {
                    // 脚本组件（Unity 式玩法挂载）先于游戏模块更新
                    ECS::ScriptSystem::GetInstance().Update(deltaTime);
                    if (auto* gm = Game::GameManager::GetInstance().GetCurrent()) {
                        gm->OnUpdate(deltaTime);
                    }
                    // 粒子属于游戏逻辑阶段：每帧只模拟一次，多个视口渲染只读取
                    // 同一份实例快照，避免编辑器 SceneView/GameView 各自推进生命周期。
                    ParticleSystem::GetInstance().Update(deltaTime);
                }
            }
            const double engineCpuLogicMs = engineCpuProfileEnabled
                ? std::chrono::duration<double, std::milli>(
                    EngineCpuProfileClock::now() - engineCpuLogicStart).count()
                : 0.0;

            // 基础 3D 第三人称轨道相机: 保持原有的 UI/脚本更新时序，
            // 在玩法更新后写回主相机 Transform。玩家脚本读取的是上一帧已
            // 完成的相机姿态，但方向仍然完全来自相机前向，不使用世界固定轴。
            // 编辑器播放时 g_RunMode 仍可能保持 Editor，但 gameRunning 已经表示真正的播放态；
            // 这里必须按实际运行态更新，否则 GameView 中鼠标/滚轮输入永远不会到达第三人称系统。
#ifndef __ANDROID__
            const bool gameplayRunning =
                (g_RunMode == RunMode::Game && !gamePaused) ||
                (editorActive && s_editorIsGameRunning && gameRunning && !gamePaused);
#else
            const bool gameplayRunning = (g_RunMode == RunMode::Game && !gamePaused);
#endif
            const auto engineCpuGameplayCameraStart = engineCpuProfileEnabled
                ? EngineCpuProfileClock::now()
                : EngineCpuProfileClock::time_point{};
            if (gameplayRunning) {
                ThirdPersonCameraSystem::GetInstance().Update(deltaTime);
                // VMD 相机覆盖第三人称轨道相机对同一 Transform 的写入；
                // PMX VMD 也在这里应用，确保相机/模型都在本帧 FrameRender 前完成。
                ECS::VmdSystem::GetInstance().Update(deltaTime);
            }
            const double engineCpuGameplayCameraMs = engineCpuProfileEnabled
                ? std::chrono::duration<double, std::milli>(
                    EngineCpuProfileClock::now() - engineCpuGameplayCameraStart).count()
                : 0.0;

            // Capture the post-gameplay state once and finalize its derived
            // render lists on the persistent worker. BeginRenderFrame waits
            // for completion before shadows, SceneView, GameView, UI and
            // particles read the published snapshot.
            const auto engineCpuRenderWorldBuildStart = engineCpuProfileEnabled
                ? EngineCpuProfileClock::now()
                : EngineCpuProfileClock::time_point{};
            g_SceneRenderer.BeginRenderWorldBuild();
            const double engineCpuRenderWorldBuildMs = engineCpuProfileEnabled
                ? std::chrono::duration<double, std::milli>(
                    EngineCpuProfileClock::now() - engineCpuRenderWorldBuildStart).count()
                : 0.0;

            // Normal skeletal animation consumes the just-published snapshot.
            // VMD model poses were applied immediately above; the renderer skips
            // those per-entity keys so the external VMD pose remains authoritative.
            const auto engineCpuAnimationStart = engineCpuProfileEnabled
                ? EngineCpuProfileClock::now()
                : EngineCpuProfileClock::time_point{};
            if (gameRunning && !gamePaused) {
                g_SceneRenderer.UpdateModelAnimations(deltaTime);
            }
            const double engineCpuAnimationMs = engineCpuProfileEnabled
                ? std::chrono::duration<double, std::milli>(
                    EngineCpuProfileClock::now() - engineCpuAnimationStart).count()
                : 0.0;

            if (engineCpuProfileEnabled) {
                ++engineCpuStageProfileFrames;
                engineCpuStageEventsMs += engineCpuEventsMs;
                engineCpuStageCameraMs += engineCpuCameraMs;
                engineCpuStagePhysicsMs += engineCpuPhysicsMs;
                engineCpuStageWorldMs += engineCpuWorldMs;
                engineCpuStageLogicMs += engineCpuLogicMs;
                engineCpuStageGameplayCameraMs += engineCpuGameplayCameraMs;
                engineCpuStageRenderWorldBuildMs += engineCpuRenderWorldBuildMs;
                engineCpuStageAnimationMs += engineCpuAnimationMs;
                if ((engineCpuStageProfileFrames % 60u) == 0u) {
                    const double invFrames = 1.0 /
                        static_cast<double>(engineCpuStageProfileFrames);
                    printf("[EngineMain][CPU][Stages] frames=%llu "
                           "events_ms=%.3f camera_ms=%.3f physics_ms=%.3f "
                           "world_ms=%.3f logic_ms=%.3f gameplay_camera_ms=%.3f "
                           "renderworld_build_ms=%.3f animation_ms=%.3f\n",
                           static_cast<unsigned long long>(engineCpuStageProfileFrames),
                           engineCpuStageEventsMs * invFrames,
                           engineCpuStageCameraMs * invFrames,
                           engineCpuStagePhysicsMs * invFrames,
                           engineCpuStageWorldMs * invFrames,
                           engineCpuStageLogicMs * invFrames,
                           engineCpuStageGameplayCameraMs * invFrames,
                           engineCpuStageRenderWorldBuildMs * invFrames,
                           engineCpuStageAnimationMs * invFrames);
                }
            }

            if (!headlessNoRender) {
                Core::RenderDocCapture::GetInstance().BeforeFramePresent(frameCount + 1);
                const auto engineRenderStart = engineCpuProfileEnabled
                    ? EngineCpuProfileClock::now()
                    : EngineCpuProfileClock::time_point{};
                ::FrameRender(wd, draw_data, view, proj, deltaTime);
                const auto engineAfterRender = engineCpuProfileEnabled
                    ? EngineCpuProfileClock::now()
                    : EngineCpuProfileClock::time_point{};
                ::FramePresent(wd);
                const auto engineAfterPresent = engineCpuProfileEnabled
                    ? EngineCpuProfileClock::now()
                    : EngineCpuProfileClock::time_point{};
                if (engineCpuProfileEnabled) {
                    RecordEngineCpuProfile(engineFrameStart, engineRenderStart,
                                           engineAfterRender, engineAfterPresent,
                                           editorUiMs);
                }
            }

            // 所有运行模式都维护帧计数；headless 还用它判断自动退出。
            frameCount++;

            // ===== headless: 固定逻辑帧数后自动退出 =====
            if (headless) {
                if (headlessFrames > 0 && frameCount >= headlessFrames) {
                    printf("[Headless] Reached frame limit (%d), exiting\n", headlessFrames);
                    done = true;
                }
            }
        }
    }

    return frameCount;
}
