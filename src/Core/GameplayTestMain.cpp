#include "Core/GameplayRuntime.h"
#include "Core/Utf8Path.h"
#include "Core/PhysicsGlobals.h"
#include "Core/ProjectManager.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"
#include "Rendering/RenderWorld.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <codecvt>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "json.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

struct ReplayInput {
    glm::vec2 move{0.0f};
    bool jump = false;
};

struct ReplayEvent {
    int startFrame = 0;
    int endFrame = 0;
    ReplayInput input;
};

class InputReplay {
public:
    bool Load(const std::string& path) {
        m_events.clear();
        m_loaded = false;
        m_frames = 0;
        m_defaultInput = ReplayInput{};

        try {
            std::ifstream file(Utf8Path(path));
            if (!file) return Fail(path, "cannot open replay file");

            nlohmann::json document;
            file >> document;
            if (!document.is_object()) return Fail(path, "root must be an object");
            if (!document.contains("schemaVersion") ||
                !IsInteger(document.at("schemaVersion")) ||
                document.at("schemaVersion").get<int>() != 1) {
                return Fail(path, "schemaVersion must be 1");
            }
            if (!document.contains("frames") || !IsInteger(document.at("frames"))) {
                return Fail(path, "frames must be an integer");
            }
            m_frames = document.at("frames").get<int>();
            if (m_frames < 1 || m_frames > 1000000) {
                return Fail(path, "frames must be in 1..1000000");
            }

            if (document.contains("defaultInput") &&
                !ParseInput(document.at("defaultInput"), "defaultInput", false, m_defaultInput)) {
                return false;
            }

            if (!document.contains("events") || !document.at("events").is_array()) {
                return Fail(path, "events must be an array");
            }
            const auto& events = document.at("events");
            if (events.size() > 4096) return Fail(path, "events cannot exceed 4096 entries");

            int previousEnd = 0;
            for (size_t index = 0; index < events.size(); ++index) {
                const auto& event = events.at(index);
                const std::string context = "events[" + std::to_string(index) + "]";
                if (!event.is_object()) return Fail(path, context + " must be an object");
                if (!event.contains("startFrame") || !IsInteger(event.at("startFrame")) ||
                    !event.contains("endFrame") || !IsInteger(event.at("endFrame"))) {
                    return Fail(path, context + " requires integer startFrame/endFrame");
                }

                const int startFrame = event.at("startFrame").get<int>();
                const int endFrame = event.at("endFrame").get<int>();
                if (startFrame < 0 || endFrame <= startFrame || endFrame > m_frames) {
                    return Fail(path, context + " frame range is invalid");
                }
                if (startFrame < previousEnd) {
                    return Fail(path, context + " overlaps or is out of order");
                }

                ReplayInput input = m_defaultInput;
                if (!ParseInput(event, context.c_str(), true, input)) return false;
                m_events.push_back(ReplayEvent{startFrame, endFrame, input});
                previousEnd = endFrame;
            }
        } catch (const std::exception& error) {
            return Fail(path, std::string("parse failed: ") + error.what());
        }

        m_path = path;
        m_loaded = true;
        return true;
    }

    bool IsLoaded() const { return m_loaded; }
    int FrameCount() const { return m_frames; }
    size_t EventCount() const { return m_events.size(); }

    ReplayInput Sample(int frame) const {
        ReplayInput result = m_defaultInput;
        if (!m_loaded || frame < 0) return result;

        const auto it = std::upper_bound(
            m_events.begin(), m_events.end(), frame,
            [](int value, const ReplayEvent& event) { return value < event.startFrame; });
        if (it != m_events.begin()) {
            const ReplayEvent& candidate = *(it - 1);
            if (frame >= candidate.startFrame && frame < candidate.endFrame) {
                result = candidate.input;
            }
        }
        return result;
    }

private:
    static bool IsInteger(const nlohmann::json& value) {
        return value.is_number_integer() || value.is_number_unsigned();
    }

    static bool ParseInput(const nlohmann::json& value,
                           const char* context,
                           bool requireInput,
                           ReplayInput& output) {
        if (!value.is_object()) return Fail(context, "must be an object");
        const bool hasMove = value.contains("move");
        const bool hasJump = value.contains("jump");
        if (requireInput && !hasMove && !hasJump) {
            return Fail(context, "must contain move or jump");
        }

        if (hasMove) {
            const auto& move = value.at("move");
            if (!move.is_array() || move.size() != 2) {
                return Fail(context, "move must contain exactly two numbers");
            }
            for (size_t axis = 0; axis < 2; ++axis) {
                if (!move.at(axis).is_number()) return Fail(context, "move values must be numbers");
                const double component = move.at(axis).get<double>();
                if (!std::isfinite(component) || component < -1.0 || component > 1.0) {
                    return Fail(context, "move values must be in [-1, 1]");
                }
                output.move[axis] = static_cast<float>(component);
            }
        }
        if (hasJump) {
            if (!value.at("jump").is_boolean()) return Fail(context, "jump must be boolean");
            output.jump = value.at("jump").get<bool>();
        }
        return true;
    }

    static bool Fail(const std::string& path, const std::string& message) {
        std::fprintf(stderr, "[GameplayTest] ERROR: input replay %s: %s\n", path.c_str(), message.c_str());
        return false;
    }

    static bool Fail(const char* context, const std::string& message) {
        std::fprintf(stderr, "[GameplayTest] ERROR: input replay %s: %s\n", context, message.c_str());
        return false;
    }

    bool m_loaded = false;
    int m_frames = 0;
    std::string m_path;
    ReplayInput m_defaultInput;
    std::vector<ReplayEvent> m_events;
};

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

int RunRenderWorldStressTest()
{
    constexpr size_t kEntityCount = 10000;
    RenderWorld world;
    world.BeginBuild(kEntityCount);
    world.frameNumber = 1;
    world.entitySetVersion = 1;

    world.rootEntities.reserve(1);
    world.rootEntities.push_back(0);
    world.hierarchyEntities.reserve(kEntityCount);
    world.modelGroups.reserve(1);
    world.voxGroups.reserve(1);
    world.cameras.reserve(1);
    world.lights.reserve(1);
    world.terrains.reserve(1);
    world.waters.reserve(1);
    world.skyboxes.reserve(1);
    world.clouds.reserve(1);

    RenderModelGroup modelGroup;
    modelGroup.rendererKey = "stress.glb";
    modelGroup.modelPath = "stress.glb";
    modelGroup.entities.reserve(kEntityCount / 4 + 1);

    RenderVoxGroup voxGroup;
    voxGroup.voxPath = "stress.vox";
    voxGroup.entities.reserve(kEntityCount / 7 + 1);

    for (size_t index = 0; index < kEntityCount; ++index) {
        const ECS::Entity entityId = static_cast<ECS::Entity>(index);
        RenderWorldEntity& entity = world.entities[index];
        entity.entity = entityId;
        entity.parent = index == 0
            ? ECS::INVALID_ENTITY
            : static_cast<ECS::Entity>(index - 1);
        entity.visible = true;
        entity.hasTransform = true;
        entity.transform.position = glm::vec3(static_cast<float>(index), 0.0f, 0.0f);
        entity.transform.worldMatrix = glm::translate(
            glm::mat4(1.0f), entity.transform.position);
        world.hierarchyEntities.push_back(entityId);
        if (index > 0) {
            world.entities[index - 1].children.push_back(entityId);
        }

        if ((index % 4) == 0) {
            entity.hasMesh = true;
            entity.mesh.type = RenderMeshType::Model;
            entity.mesh.modelPath = "stress.glb";
            entity.hasRenderFlags = true;
            entity.render.visible = true;
            modelGroup.entities.push_back(entityId);
        }
        if ((index % 7) == 0) {
            entity.hasVoxel = true;
            entity.voxel.voxPath = "stress.vox";
            entity.voxel.loaded = true;
            voxGroup.entities.push_back(entityId);
        }

        if (index == 0) {
            entity.hasCamera = true;
            entity.camera.entity = entityId;
            world.cameras.push_back(entity.camera);
        } else if (index == 1) {
            entity.hasLight = true;
            entity.light.entity = entityId;
            world.lights.push_back(entity.light);
        } else if (index == 2) {
            entity.hasTerrain = true;
            entity.terrain.entity = entityId;
            world.terrains.push_back(entity.terrain);
        } else if (index == 3) {
            entity.hasWater = true;
            entity.water.entity = entityId;
            world.waters.push_back(entity.water);
        } else if (index == 4) {
            entity.hasSkybox = true;
            entity.skybox.entity = entityId;
            world.skyboxes.push_back(entity.skybox);
        } else if (index == 5) {
            entity.hasCloud = true;
            entity.cloud.entity = entityId;
            world.clouds.push_back(entity.cloud);
        }
    }
    world.modelGroups.push_back(std::move(modelGroup));
    world.voxGroups.push_back(std::move(voxGroup));
    world.RebuildIndex();

    std::string validationError;
    const bool valid = world.Validate(&validationError);
    const size_t entityCapacity = world.entities.capacity();
    const size_t childCapacity = world.entities[0].children.capacity();
    world.BeginBuild(kEntityCount);
    const bool capacitiesReused =
        world.entities.capacity() == entityCapacity &&
        world.entities[0].children.capacity() == childCapacity;
    const bool resetClean =
        world.entities[0].entity == ECS::INVALID_ENTITY &&
        world.entities[0].children.empty() &&
        world.Find(0) == nullptr;

    std::fprintf(stderr,
        "[RenderWorldStress] entities=%zu valid=%s capacities_reused=%s reset_clean=%s "
        "entity_capacity=%zu child_capacity=%zu%s\n",
        kEntityCount, valid ? "true" : "false",
        capacitiesReused ? "true" : "false", resetClean ? "true" : "false",
        entityCapacity, childCapacity,
        valid ? "" : (" error=" + validationError).c_str());
    return valid && capacitiesReused && resetClean ? 0 : 5;
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
    std::string inputReplayPath;
    int frames = 120;
    float fixedDelta = 1.0f / 60.0f;
    bool invalidArguments = false;
    bool framesExplicit = false;
    bool scriptedInput = false;
    bool buoyancyTest = false;
    bool autoStartGame = false;
    bool renderWorldStress = false;

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
        } else if (argument.rfind("--input-replay=", 0) == 0) inputReplayPath = argument.substr(15);
        else if (argument == "--input-replay") inputReplayPath = nextValue("--input-replay");
        else if (argument == "--scripted-input") scriptedInput = true;
        else if (argument == "--buoyancy-test") buoyancyTest = true;
        else if (argument == "--auto-start-game") autoStartGame = true;
        else if (argument == "--renderworld-stress") renderWorldStress = true;
    }

    if (invalidArguments) return 64;
    if (renderWorldStress) return RunRenderWorldStressTest();

    InputReplay inputReplay;
    if (!inputReplayPath.empty() && scriptedInput) {
        std::fprintf(stderr, "[GameplayTest] ERROR: --input-replay and --scripted-input are mutually exclusive\n");
        return 64;
    }
    if (!inputReplayPath.empty() && !inputReplay.Load(inputReplayPath)) return 64;

    // A scripted run needs enough time to settle, move, jump, and land. The
    // terrain prototype starts the player well above the island, so 300 frames
    // is not enough to reach the first contact reliably.
    if (inputReplay.IsLoaded() && !framesExplicit) frames = inputReplay.FrameCount();
    else if (scriptedInput && !framesExplicit) frames = 900;
    else if (buoyancyTest && !framesExplicit) frames = 360;

    if (invalidArguments || scenePath.empty() || dumpPath.empty()) {
        std::fprintf(stderr,
            "[GameplayTest] ERROR: required: --scene <path> --dump-state <path>; "
            "--frames must be 1..1000000 and --fixed-dt must be (0,0.1]; "
            "--scripted-input enables deterministic player movement/jump; "
            "--buoyancy-test checks player water contact and buoyancy; "
            "--auto-start-game invokes the game module's deterministic test start hook; "
            "--renderworld-stress runs a 10000-entity RenderWorld validation test\n");
        return 64;
    }

    if (!ProjectManager::GetInstance().Initialize(argc, argv)) {
        std::fprintf(stderr, "[GameplayTest] ERROR: failed to initialize engine/project paths\n");
        return 2;
    }
#ifndef __ANDROID__
    if (!ProjectManager::GetInstance().HasActiveProject()) {
        std::fprintf(stderr,
            "[GameplayTest] ERROR: desktop gameplay tests require --project <project-directory>\n");
        return 2;
    }
#endif
    scenePath = ProjectManager::GetInstance().ResolveAssetPath(scenePath);
    if (scenePath.empty()) {
        std::fprintf(stderr, "[GameplayTest] ERROR: cannot resolve scene inside the selected project\n");
        return 2;
    }
    std::error_code error;
    const std::filesystem::path dumpParent =
        Utf8Path(dumpPath).parent_path();
    if (!dumpParent.empty()) std::filesystem::create_directories(dumpParent, error);

    Core::GameplayRuntime runtime;
    if (!runtime.Initialize()) return 1;
    if (!runtime.LoadScene(scenePath, gameName, autoStartGame)) return 2;

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
    int replayFramesApplied = 0;
    int replayMoveFrames = 0;
    int replayJumpFrames = 0;
    if (scriptedInput || buoyancyTest || inputReplay.IsLoaded()) {
        player = FindPlayerEntity();
        if (player == ECS::INVALID_ENTITY && (scriptedInput || buoyancyTest)) {
            std::fprintf(stderr,
                "[GameplayTest] ERROR: requested player test found no player controller entity\n");
            runtime.Shutdown();
            return 4;
        }

        if (player != ECS::INVALID_ENTITY) {
            auto& scene = ECS::SceneECS::GetInstance();
            initialPlayerPosition = scene.GetPosition(player);
        } else if (inputReplay.IsLoaded()) {
            std::fprintf(stderr,
                "[GameplayTest] input replay: no PlayerController entity; treating replay as game-plugin input\n");
        }
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
        if (inputReplay.IsLoaded()) {
            const ReplayInput input = inputReplay.Sample(frame);
            runtime.SetSyntheticPlayerInput(input.move, input.jump);
            ++replayFramesApplied;
            if (glm::length(input.move) > 0.001f) ++replayMoveFrames;
            if (input.jump) ++replayJumpFrames;
        } else if (scriptedInput) {
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
    bool replayPass = true;
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

    if (inputReplay.IsLoaded()) {
        replayPass = replayFramesApplied == frames;
        std::fprintf(stderr,
            "[GameplayTest] input replay: path=%s replay_frames=%d applied_frames=%d "
            "events=%zu move_frames=%d jump_frames=%d pass=%s\n",
            inputReplayPath.c_str(), inputReplay.FrameCount(), replayFramesApplied,
            inputReplay.EventCount(), replayMoveFrames, replayJumpFrames,
            replayPass ? "true" : "false");
    }

    if (!Core::GameplayRuntime::DumpState(dumpPath, frames, "gameplay-cpu", 1.0f / fixedDelta)) return 3;
    runtime.Shutdown();
    if (!scriptedInputPass || !buoyancyPass || !replayPass) {
        if (!scriptedInputPass) {
            std::fprintf(stderr, "[GameplayTest] FAIL: scripted input assertions did not pass\n");
        }
        if (!buoyancyPass) {
            std::fprintf(stderr, "[GameplayTest] FAIL: buoyancy assertions did not pass\n");
        }
        if (!replayPass) {
            std::fprintf(stderr, "[GameplayTest] FAIL: input replay did not cover the requested frame range\n");
        }
        return 5;
    }
    std::fprintf(stderr, "[GameplayTest] PASS\n");
    return 0;
}
