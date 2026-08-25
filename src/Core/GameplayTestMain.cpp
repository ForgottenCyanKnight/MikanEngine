#include "Core/GameplayRuntime.h"
#include "Core/PhysicsGlobals.h"
#include "Core/ProjectManager.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <locale>
#include <codecvt>
#include <string>
#include <glm/glm.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

ECS::Entity FindPlayerInSubtree(ECS::Entity entity) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    const bool hasController = coordinator.HasComponent<ECS::PlayerControllerComponent>(entity);
    const bool hasWalkScript = coordinator.HasComponent<ECS::ScriptComponent>(entity) &&
        coordinator.GetComponent<ECS::ScriptComponent>(entity).scriptName == "PlayerWalkScript";
    if ((hasController || hasWalkScript) &&
        coordinator.HasComponent<ECS::TransformComponent>(entity)) {
        return entity;
    }

    auto& scene = ECS::SceneECS::GetInstance();
    for (ECS::Entity child : scene.GetChildren(entity)) {
        const ECS::Entity player = FindPlayerInSubtree(child);
        if (player != ECS::INVALID_ENTITY) return player;
    }
    return ECS::INVALID_ENTITY;
}

ECS::Entity FindPlayerEntity() {
    auto& scene = ECS::SceneECS::GetInstance();
    for (ECS::Entity root : scene.GetRootEntities()) {
        const ECS::Entity player = FindPlayerInSubtree(root);
        if (player != ECS::INVALID_ENTITY) return player;
    }
    return ECS::INVALID_ENTITY;
}

} // namespace

extern "C" MIKAN_API int MikanGameplayTestMain(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::locale::global(std::locale(std::locale::classic(), new std::codecvt_utf8_utf16<wchar_t>));
#endif

    std::string scenePath;
    std::string gameName;
    std::string dumpPath;
    int frames = 120;
    float fixedDelta = 1.0f / 60.0f;
    bool invalidArguments = false;
    bool framesExplicit = false;
    bool scriptedInput = false;
    bool buoyancyTest = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] ? argv[index] : "";
        auto nextValue = [&](const char* option) -> const char* {
            if (index + 1 >= argc || !argv[index + 1]) {
                std::fprintf(stderr, "[GameplayTest] ERROR: %s requires a value\n", option);
                invalidArguments = true;
                return "";
            }
            return argv[++index];
        };
        if (argument.rfind("--scene=", 0) == 0) scenePath = argument.substr(8);
        else if (argument == "--scene") scenePath = nextValue("--scene");
        else if (argument.rfind("--game=", 0) == 0) gameName = argument.substr(7);
        else if (argument == "--game") gameName = nextValue("--game");
        else if (argument.rfind("--dump-state=", 0) == 0) dumpPath = argument.substr(13);
        else if (argument == "--dump-state") dumpPath = nextValue("--dump-state");
        else if (argument == "--frames") {
            framesExplicit = true;
            const char* value = nextValue("--frames");
            char* end = nullptr;
            const long parsed = std::strtol(value, &end, 10);
            if (!end || *end != '\0' || parsed < 1 || parsed > 1000000L) invalidArguments = true;
            else frames = static_cast<int>(parsed);
        } else if (argument == "--fixed-dt") {
            const char* value = nextValue("--fixed-dt");
            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (!end || *end != '\0' || parsed <= 0.0f || parsed > 0.1f) invalidArguments = true;
            else fixedDelta = parsed;
        } else if (argument == "--scripted-input") scriptedInput = true;
        else if (argument == "--buoyancy-test") buoyancyTest = true;
    }

    // A scripted run needs enough time to settle, move, jump, and land. The
    // terrain prototype starts the player well above the island, so 300 frames
    // is not enough to reach the first contact reliably.
    if (scriptedInput && !framesExplicit) frames = 900;
    else if (buoyancyTest && !framesExplicit) frames = 360;

    if (invalidArguments || scenePath.empty() || dumpPath.empty()) {
        std::fprintf(stderr,
            "[GameplayTest] ERROR: required: --scene <path> --dump-state <path>; "
            "--frames must be 1..1000000 and --fixed-dt must be (0,0.1]; "
            "--scripted-input enables deterministic player movement/jump; "
            "--buoyancy-test checks player water contact and buoyancy\n");
        return 64;
    }

    ProjectManager::GetInstance().Initialize(argc, argv);
    scenePath = ProjectManager::GetInstance().ResolveAssetPath(scenePath);
    std::error_code error;
    const std::filesystem::path dumpParent =
        std::filesystem::u8path(dumpPath).parent_path();
    if (!dumpParent.empty()) std::filesystem::create_directories(dumpParent, error);

    Core::GameplayRuntime runtime;
    if (!runtime.Initialize()) return 1;
    if (!runtime.LoadScene(scenePath, gameName)) return 2;

    ECS::Entity player = ECS::INVALID_ENTITY;
    glm::vec3 initialPlayerPosition(0.0f);
    int firstGroundFrame = -1;
    int moveStartFrame = -1;
    int moveEndFrame = -1;
    int jumpFrame = -1;
    float preJumpY = 0.0f;
    float maxYAfterJump = -std::numeric_limits<float>::max();
    float firstGroundY = 0.0f;
    float minimumPlayerY = std::numeric_limits<float>::max();
    int waterContactFrames = 0;
    int firstWaterFrame = -1;
    int groundStableFrames = 0;
    int postJumpGroundStableFrames = 0;
    bool firstGrounded = false;
    bool landedAfterJump = false;
    bool moveStarted = false;
    if (scriptedInput || buoyancyTest) {
        player = FindPlayerEntity();
        if (player == ECS::INVALID_ENTITY) {
            std::fprintf(stderr,
                "[GameplayTest] ERROR: requested player test found no player controller entity\n");
            runtime.Shutdown();
            return 4;
        }

        auto& scene = ECS::SceneECS::GetInstance();
        initialPlayerPosition = scene.GetPosition(player);
        if (scriptedInput) {
            std::fprintf(stderr,
                "[GameplayTest] scripted input: waiting for physics contact before jump\n");
        }
        if (buoyancyTest) {
            minimumPlayerY = initialPlayerPosition.y;
            std::fprintf(stderr,
                "[GameplayTest] buoyancy test: tracking player water contact from y=%.3f\n",
                initialPlayerPosition.y);
        }
    }

    std::fprintf(stderr, "[GameplayTest] running %d frames at fixed_dt=%.8f (no SDL Video, no Vulkan)\n",
        frames, fixedDelta);
    for (int frame = 0; frame < frames; ++frame) {
        if (scriptedInput) {
            glm::vec2 move(0.0f);
            const bool jump = frame == jumpFrame;
            if (moveStarted && frame >= moveStartFrame && frame < moveEndFrame) {
                // Move laterally across the prototype island; this avoids the
                // steep ridge directly in front of the spawn point and gives
                // the jump phase a stable contact surface.
                move.x = 1.0f;
            }
            runtime.SetSyntheticPlayerInput(move, jump);
        } else {
            runtime.ClearSyntheticPlayerInput();
        }

        runtime.Tick(fixedDelta);

        if (buoyancyTest) {
            const glm::vec3 position = ECS::SceneECS::GetInstance().GetPosition(player);
            minimumPlayerY = std::min(minimumPlayerY, position.y);
            if (g_PhysicsSystemPtr && g_PhysicsSystemPtr->IsEntityInWater(player)) {
                ++waterContactFrames;
                if (firstWaterFrame < 0) firstWaterFrame = frame;
            }
        }

        if (scriptedInput) {
            const glm::vec3 position = ECS::SceneECS::GetInstance().GetPosition(player);
            const glm::vec3 velocity = g_PhysicsSystemPtr
                ? g_PhysicsSystemPtr->GetLinearVelocity(player)
                : glm::vec3(0.0f);
            const bool nearRestingVerticalSpeed = std::abs(velocity.y) < 0.35f;

            // Do not use a fixed frame number here: terrain height and spawn
            // height are scene data. A stable near-zero vertical velocity after
            // the initial fall is the gameplay-level ground-contact signal.
            if (!firstGrounded) {
                const bool belowSpawn = position.y < initialPlayerPosition.y - 1.0f;
                if (frame >= 30 && belowSpawn && nearRestingVerticalSpeed) {
                    ++groundStableFrames;
                } else {
                    groundStableFrames = 0;
                }

                if (groundStableFrames >= 8) {
                    firstGrounded = true;
                    firstGroundFrame = frame;
                    firstGroundY = position.y;
                    preJumpY = position.y;
                    // Issue the jump on the next frame so the controller sees
                    // the grounded body state rather than the settling frame.
                    jumpFrame = frame + 1;
                }
            } else if (frame >= jumpFrame) {
                maxYAfterJump = std::max(maxYAfterJump, position.y);

                // Wait until the body has returned close to its first ground
                // height. The apex also has a small vertical velocity, so a
                // height check prevents it from being mistaken for landing.
                const bool nearFirstGround = position.y <= firstGroundY + 1.0f;
                if (frame > jumpFrame + 10 && nearFirstGround &&
                    nearRestingVerticalSpeed && velocity.y <= 0.35f) {
                    ++postJumpGroundStableFrames;
                } else {
                    postJumpGroundStableFrames = 0;
                }

                if (!landedAfterJump && postJumpGroundStableFrames >= 8) {
                    landedAfterJump = true;
                    moveStarted = true;
                    moveStartFrame = frame + 1;
                    moveEndFrame = moveStartFrame + 60;
                }
            }
        }
    }

    bool scriptedInputPass = true;
    bool buoyancyPass = true;
    if (scriptedInput) {
        const glm::vec3 finalPosition = ECS::SceneECS::GetInstance().GetPosition(player);
        const float horizontalDistance = glm::length(glm::vec2(
            finalPosition.x - initialPlayerPosition.x,
            finalPosition.z - initialPlayerPosition.z));
        const bool moved = horizontalDistance > 0.5f;
        const bool jumped = firstGrounded && jumpFrame >= 0 &&
                            maxYAfterJump > preJumpY + 0.25f;
        const bool stayedAboveTerrain = finalPosition.y > -100.0f;
        scriptedInputPass = firstGrounded && jumped && landedAfterJump && moved &&
                             stayedAboveTerrain;
        std::fprintf(stderr,
            "[GameplayTest] input assertions: first_ground_frame=%d first_ground_y=%.3f "
            "jump_frame=%d move=[%d,%d) horizontal_distance=%.3f moved=%s "
            "pre_jump_y=%.3f max_after_jump_y=%.3f jumped=%s landed=%s final_y=%.3f\n",
            firstGroundFrame, firstGroundY, jumpFrame, moveStartFrame, moveEndFrame,
            horizontalDistance, moved ? "true" : "false", preJumpY, maxYAfterJump,
            jumped ? "true" : "false", landedAfterJump ? "true" : "false",
            finalPosition.y);
    }

    if (buoyancyTest) {
        auto& scene = ECS::SceneECS::GetInstance();
        const glm::vec3 finalPosition = scene.GetPosition(player);
        const glm::vec3 finalVelocity = g_PhysicsSystemPtr
            ? g_PhysicsSystemPtr->GetLinearVelocity(player)
            : glm::vec3(0.0f);
        const bool enteredWater = waterContactFrames >= 5;
        // The test volume extends down to -20. A failed buoyancy path will
        // continue falling hundreds of units during a six-second run, so this
        // assertion differentiates a floating body from mere trigger detection.
        const bool remainedInTestVolume = minimumPlayerY > -20.0f &&
                                          finalPosition.y > -20.0f;
        buoyancyPass = enteredWater && remainedInTestVolume;
        std::fprintf(stderr,
            "[GameplayTest] buoyancy assertions: first_water_frame=%d "
            "contact_frames=%d min_y=%.3f final_y=%.3f velocity_y=%.3f "
            "entered=%s remained_in_volume=%s\n",
            firstWaterFrame, waterContactFrames, minimumPlayerY, finalPosition.y,
            finalVelocity.y, enteredWater ? "true" : "false",
            remainedInTestVolume ? "true" : "false");
    }

    if (!Core::GameplayRuntime::DumpState(dumpPath, frames, "gameplay-cpu", 1.0f / fixedDelta)) return 3;
    runtime.Shutdown();
    if (!scriptedInputPass || !buoyancyPass) {
        if (!scriptedInputPass) {
            std::fprintf(stderr, "[GameplayTest] FAIL: scripted input assertions did not pass\n");
        }
        if (!buoyancyPass) {
            std::fprintf(stderr, "[GameplayTest] FAIL: buoyancy assertions did not pass\n");
        }
        return 5;
    }
    std::fprintf(stderr, "[GameplayTest] PASS\n");
    return 0;
}
