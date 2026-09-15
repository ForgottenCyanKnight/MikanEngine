#include "Core/Log.h"
#include "Core/GameplayRuntime.h"
#include "Core/Utf8Path.h"
#include "Core/PhysicsGlobals.h"
#include "Core/ProjectManager.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"
#include "SceneSerializer.h"
#include "Rendering/RenderWorld.h"
#include "Rendering/RenderWorldBuilder.h"
#include "Rendering/RenderWorldFinalizeWorker.h"
#include "Rendering/ModelLoader.h"
#include "Rendering/ModelRenderer.h"
#include "Core/AssetRegistry.h"
#include "World/World.h"
#include "World/WorldTypes.h"

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <codecvt>
#include <sstream>
#include <string>
#include <utility>
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
        LOGE("[GameplayTest] ERROR: input replay %s: %s", path.c_str(), message.c_str());
        return false;
    }

    static bool Fail(const char* context, const std::string& message) {
        LOGE("[GameplayTest] ERROR: input replay %s: %s", context, message.c_str());
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

int RunWorldJobSystemSelftest()
{
    GetWorldConfig().AssetPath = ProjectManager::GetInstance().GetAssetsDir();
    GetWorldConfig().terrainType = FLAT;

    World world(1);
    const std::array<Plane, 6> noFrustum{};
    std::vector<Chunk*> activeChunks;
    bool allInitialMeshesReady = false;
    for (int update = 0; update < 16 && !allInitialMeshesReady; ++update) {
        world.Update(glm::vec3(0.0f), noFrustum);
        world.WaitForAsyncWork();
        activeChunks = world.GetActiveChunks();
        allInitialMeshesReady = !activeChunks.empty();
        for (const Chunk* chunk : activeChunks) {
            allInitialMeshesReady = allInitialMeshesReady &&
                chunk != nullptr && !chunk->needsMeshUpdate;
        }
    }

    std::vector<Chunk::FaceInstance> opaqueFaces;
    std::vector<Chunk::FaceInstance> alphaFaces;
    std::vector<Chunk::FaceInstance> transparentFaces;
    const size_t initialFaceCount = world.CollectVisibleFaces(
        noFrustum, opaqueFaces, alphaFaces, transparentFaces);

    const glm::ivec3 testPosition(0, 101, 0);
    world.PlaceBlock(testPosition, 1);
    world.Update(glm::vec3(0.0f), noFrustum);
    world.WaitForAsyncWork();
    const bool modificationVisible = world.GetBlockAt(
        testPosition.x, testPosition.y, testPosition.z) == 1;

    LOGI(
        "[WorldJobSystem] chunks=%zu initial_faces=%zu opaque=%zu alpha=%zu transparent=%zu ""initial_meshes_ready=%s modification_visible=%s -> %s",
        activeChunks.size(), initialFaceCount, opaqueFaces.size(), alphaFaces.size(),
        transparentFaces.size(), allInitialMeshesReady ? "true" : "false",
        modificationVisible ? "true" : "false",
        allInitialMeshesReady && initialFaceCount > 0 && modificationVisible ? "PASS" : "FAIL");
    return allInitialMeshesReady && initialFaceCount > 0 && modificationVisible ? 0 : 5;
}

int RunSceneSaveSelftest(const std::string& dumpPath)
{
    const std::filesystem::path dumpFile = Utf8Path(dumpPath);
    std::filesystem::path outputDirectory = dumpFile.parent_path();
    if (outputDirectory.empty()) outputDirectory = std::filesystem::current_path();

    std::error_code directoryError;
    std::filesystem::create_directories(outputDirectory, directoryError);
    if (directoryError) {
        LOGE("[SceneSaveSelftest] FAIL: cannot create output directory: %s",
                     outputDirectory.string().c_str());
        return 5;
    }

    const std::filesystem::path scenePath =
        outputDirectory / "scene_save_selftest.scene.json";
    const std::filesystem::path prefabPath =
        outputDirectory / "scene_save_selftest.prefab.json";
    const std::filesystem::path malformedPrefabPath =
        outputDirectory / "scene_save_selftest.malformed-prefab.json";
    const std::filesystem::path directoryTarget =
        outputDirectory / "scene_save_selftest.target-directory";

    auto readText = [](const std::filesystem::path& path, std::string& result) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false;
        std::ostringstream buffer;
        buffer << file.rdbuf();
        if (file.bad()) return false;
        result = buffer.str();
        return true;
    };

    auto pathString = [](const std::filesystem::path& path) {
        return Utf8String(path);
    };

    auto hasCurrentFormatVersion = [](const nlohmann::json& document) {
        return !document.is_discarded() && document.is_object() &&
            document.contains("formatVersion") &&
            document.at("formatVersion").is_number_integer() &&
            document.at("formatVersion").get<int>() == 1;
    };

    ECS::SceneSerializer serializer;
    const std::string initialSceneJson = serializer.SerializeScene();
    std::string initialSceneOnDisk;
    const bool initialSceneSaved = serializer.SaveScene(pathString(scenePath));
    const bool initialSceneRead = readText(scenePath, initialSceneOnDisk);
    nlohmann::json initialDocument(nlohmann::json::value_t::discarded);
    if (initialSceneRead) {
        initialDocument = nlohmann::json::parse(initialSceneOnDisk, nullptr, false);
    }
    const bool initialSceneValid = initialSceneSaved && initialSceneRead &&
        hasCurrentFormatVersion(initialDocument) &&
        initialSceneOnDisk == initialSceneJson;

    auto& scene = ECS::SceneECS::GetInstance();
    ECS::Entity root = scene.CreateCube("AtomicSaveSelftestRoot");
    ECS::Entity child = scene.CreateCube("AtomicSaveSelftestChild");
    const bool entitiesCreated = root != ECS::INVALID_ENTITY &&
        child != ECS::INVALID_ENTITY;
    if (entitiesCreated) scene.SetParent(child, root);

    const std::string updatedSceneJson = serializer.SerializeScene();
    std::string updatedSceneOnDisk;
    const bool replacementSaved = entitiesCreated &&
        serializer.SaveScene(pathString(scenePath));
    const bool replacementRead = readText(scenePath, updatedSceneOnDisk);
    nlohmann::json replacementDocument(nlohmann::json::value_t::discarded);
    if (replacementRead) {
        replacementDocument = nlohmann::json::parse(updatedSceneOnDisk, nullptr, false);
    }
    const bool sceneReplacementValid = replacementSaved && replacementRead &&
        hasCurrentFormatVersion(replacementDocument) &&
        updatedSceneOnDisk == updatedSceneJson;

    bool prefabSaved = false;
    bool prefabShapeValid = false;
    bool prefabReplacementValid = false;
    if (entitiesCreated) {
        prefabSaved = serializer.SavePrefab(root, pathString(prefabPath));
        std::string prefabOnDisk;
        const bool prefabRead = readText(prefabPath, prefabOnDisk);
        nlohmann::json prefabDocument(nlohmann::json::value_t::discarded);
        if (prefabRead) {
            prefabDocument = nlohmann::json::parse(prefabOnDisk, nullptr, false);
        }
        prefabShapeValid = prefabSaved && prefabRead && !prefabDocument.is_discarded() &&
            hasCurrentFormatVersion(prefabDocument) &&
            prefabDocument.contains("entities") &&
            prefabDocument.at("entities").is_array() &&
            prefabDocument.at("entities").size() == 2u;

        scene.SetName(root, "AtomicSaveSelftestRenamed");
        const bool prefabReplaced = serializer.SavePrefab(root, pathString(prefabPath));
        std::string replacedPrefabOnDisk;
        const bool replacedPrefabRead = readText(prefabPath, replacedPrefabOnDisk);
        nlohmann::json replacedPrefabDocument(nlohmann::json::value_t::discarded);
        if (replacedPrefabRead) {
            replacedPrefabDocument = nlohmann::json::parse(
                replacedPrefabOnDisk, nullptr, false);
        }
        prefabReplacementValid = prefabReplaced && replacedPrefabRead &&
            hasCurrentFormatVersion(replacedPrefabDocument) &&
            replacedPrefabDocument.contains("name") &&
            replacedPrefabDocument.at("name").is_string() &&
            replacedPrefabDocument.at("name").get<std::string>() ==
                "AtomicSaveSelftestRenamed";
    }

    auto& coordinator = ECS::Coordinator::GetInstance();
    const bool unsupportedVersionRejected = !serializer.DeserializeScene(
        "{\"formatVersion\": 999, \"entities\": []}");
    const bool scenePreservedAfterReject = entitiesCreated &&
        coordinator.IsAlive(root) && scene.GetName(root) == "AtomicSaveSelftestRenamed";
    const bool malformedSceneRejected = !serializer.DeserializeScene(
        "{\"formatVersion\": 1, \"entities\": [{\"name\": {\"name\": \"bad\"}}]}");
    const bool scenePreservedAfterMalformedScene = entitiesCreated &&
        coordinator.IsAlive(root) && scene.GetName(root) == "AtomicSaveSelftestRenamed";

    bool malformedPrefabWritten = false;
    {
        std::ofstream malformedFile(malformedPrefabPath,
                                     std::ios::binary | std::ios::trunc);
        malformedFile << "{\"formatVersion\": 1, \"name\": \"bad\", "
                         "\"entities\": [{\"name\": {\"name\": \"bad\"}}]}";
        malformedFile.flush();
        malformedPrefabWritten = malformedFile.good();
    }
    const bool malformedPrefabRejected = malformedPrefabWritten &&
        serializer.InstantiatePrefab(pathString(malformedPrefabPath)) == ECS::INVALID_ENTITY;
    const bool scenePreservedAfterMalformedPrefab = entitiesCreated &&
        coordinator.IsAlive(root) && scene.GetName(root) == "AtomicSaveSelftestRenamed";

    std::error_code targetError;
    const bool targetCreated = std::filesystem::create_directory(directoryTarget, targetError);
    const bool failedReplacement = targetCreated &&
        !serializer.SaveScene(pathString(directoryTarget));
    targetError.clear();
    const bool targetPreserved = std::filesystem::is_directory(directoryTarget, targetError) &&
        !targetError;
    std::filesystem::remove(directoryTarget, targetError);
    const bool targetCleanup = !targetError;

    std::error_code scanError;
    size_t temporaryArtifacts = 0;
    const std::string sceneTempPrefix = scenePath.filename().string() + ".tmp.";
    const std::string prefabTempPrefix = prefabPath.filename().string() + ".tmp.";
    const std::string targetTempPrefix = directoryTarget.filename().string() + ".tmp.";
    for (std::filesystem::directory_iterator it(outputDirectory, scanError), end;
         !scanError && it != end; it.increment(scanError)) {
        const std::string name = it->path().filename().string();
        if (name.rfind(sceneTempPrefix, 0) == 0 ||
            name.rfind(prefabTempPrefix, 0) == 0 ||
            name.rfind(targetTempPrefix, 0) == 0) {
            ++temporaryArtifacts;
        }
    }
    const bool temporaryFilesClean = !scanError && temporaryArtifacts == 0u;

    if (child != ECS::INVALID_ENTITY) scene.DestroyEntity(child);
    if (root != ECS::INVALID_ENTITY) scene.DestroyEntity(root);

    const bool passed = initialSceneValid && sceneReplacementValid &&
        prefabShapeValid && prefabReplacementValid && failedReplacement &&
        targetPreserved && targetCleanup && temporaryFilesClean &&
        unsupportedVersionRejected && scenePreservedAfterReject &&
        malformedSceneRejected && scenePreservedAfterMalformedScene &&
        malformedPrefabRejected && scenePreservedAfterMalformedPrefab;
    LOGE(
        "[SceneSaveSelftest] scene_initial=%d scene_replace=%d prefab_shape=%d ""prefab_replace=%d version_guard=%d failed_replace_preserves=%d ""structure_guard=%d temp_clean=%d -> %s",
        initialSceneValid ? 1 : 0, sceneReplacementValid ? 1 : 0,
        prefabShapeValid ? 1 : 0, prefabReplacementValid ? 1 : 0,
        (unsupportedVersionRejected && scenePreservedAfterReject) ? 1 : 0,
        (failedReplacement && targetPreserved && targetCleanup) ? 1 : 0,
        (malformedSceneRejected && scenePreservedAfterMalformedScene &&
         malformedPrefabRejected && scenePreservedAfterMalformedPrefab) ? 1 : 0,
        temporaryFilesClean ? 1 : 0, passed ? "PASS" : "FAIL");
    return passed ? 0 : 5;
}

int RunModelCacheSelftest()
{
    const std::string relativePath = "engine/models/Base Model/cube.glb";
    const std::string absolutePath =
        ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/cube.glb");
    if (absolutePath.empty()) {
        LOGE("[ModelCacheSelftest] FAIL: engine asset root is unavailable");
        return 5;
    }

    auto& assetRegistry = AssetRegistry::GetInstance();
    assetRegistry.Clear();
    ModelLoader::ClearCache();
    const ModelLoadResult first = ModelLoader::LoadModelWithTextures(relativePath);
    const size_t cacheAfterRelative = ModelLoader::GetCacheSize();
    const ModelLoadResult second = ModelLoader::LoadModelWithTextures(absolutePath);
    const size_t cacheAfterAbsolute = ModelLoader::GetCacheSize();
    const bool modelLoaded = !first.meshData.subMeshes.empty() &&
        first.meshData.subMeshes.size() == second.meshData.subMeshes.size();
    const bool deduplicated = cacheAfterRelative == 1u && cacheAfterAbsolute == 1u;
    const auto readyRecord = assetRegistry.Find(relativePath, AssetType::Model);
    const bool registryReady = readyRecord.has_value() &&
        readyRecord->state == AssetState::Ready && readyRecord->refCount == 1 &&
        readyRecord->version == 1 && readyRecord->sizeBytes > 0;
    const ModelLoadResult reloaded = ModelLoader::ReloadModelWithTextures(relativePath);
    const auto reloadedRecord = assetRegistry.Find(relativePath, AssetType::Model);
    const bool reloadTracked = !reloaded.meshData.subMeshes.empty() &&
        reloadedRecord.has_value() && reloadedRecord->state == AssetState::Ready &&
        reloadedRecord->refCount == 1 && reloadedRecord->version == 2;
    ModelLoader::ClearCache();
    const auto releasedRecord = assetRegistry.Find(relativePath, AssetType::Model);
    const bool cacheReleaseTracked = releasedRecord.has_value() &&
        releasedRecord->state == AssetState::Ready && releasedRecord->refCount == 0;

    const std::filesystem::path animationFile =
        std::filesystem::path(ProjectManager::GetInstance().GetEngineRoot()) /
        "projects/third-person-navigation/animations/quaternius/AnimationLibrary_Standard.glb";
    const std::string animationPath = animationFile.string();
    const std::string animationAliasPath =
        (animationFile.parent_path() / ".." / "quaternius" /
         animationFile.filename()).string();
    const auto animationFirst = ModelLoader::LoadAnimationAsset(animationPath);
    const auto animationSecond = ModelLoader::LoadAnimationAsset(animationAliasPath);
    const bool animationAssetValid = animationFirst && animationSecond &&
        !animationFirst->bindBones.empty() && !animationFirst->clips.empty();
    const bool animationAssetShared = animationAssetValid &&
        animationFirst.get() == animationSecond.get();
    ModelLoader::ClearCache();
    const auto animationReloaded = ModelLoader::LoadAnimationAsset(animationPath);
    const bool animationAssetReloaded = animationReloaded && animationAssetValid &&
        animationReloaded.get() != animationFirst.get();
    ModelLoader::ClearCache();

    // Exercise the renderer integration, not only the loader API: two
    // animation-only entities with the same initial state should populate one
    // immutable sampled pose while retaining independent renderer objects.
    ModelLoader::ClearAnimationPoseCache();
    ModelRenderer firstPoseRenderer;
    ModelRenderer secondPoseRenderer;
    const bool poseRenderersLoaded =
        firstPoseRenderer.LoadAnimationOnly(animationPath) &&
        secondPoseRenderer.LoadAnimationOnly(animationAliasPath);
    firstPoseRenderer.UpdateAnimation(0.0f);
    const size_t poseCacheAfterFirst = ModelLoader::GetAnimationPoseCacheSize();
    secondPoseRenderer.UpdateAnimation(0.0f);
    const size_t poseCacheAfterSecond = ModelLoader::GetAnimationPoseCacheSize();
    const auto& firstPoseMatrices = firstPoseRenderer.GetBoneMatrices();
    const auto& secondPoseMatrices = secondPoseRenderer.GetBoneMatrices();
    bool animationPoseMatricesMatch =
        !firstPoseMatrices.empty() && firstPoseMatrices.size() == secondPoseMatrices.size();
    if (animationPoseMatricesMatch) {
        for (size_t i = 0; i < firstPoseMatrices.size() && animationPoseMatricesMatch; ++i) {
            for (int column = 0; column < 4 && animationPoseMatricesMatch; ++column) {
                for (int row = 0; row < 4; ++row) {
                    if (std::fabs(firstPoseMatrices[i][column][row] -
                                  secondPoseMatrices[i][column][row]) > 1.0e-6f) {
                        animationPoseMatricesMatch = false;
                        break;
                    }
                }
            }
        }
    }
    const bool animationPoseShared = poseRenderersLoaded &&
        poseCacheAfterFirst == 1u && poseCacheAfterSecond == 1u &&
        animationPoseMatricesMatch &&
        firstPoseRenderer.GetAnimationPoseIdentity() != nullptr &&
        firstPoseRenderer.GetAnimationPoseIdentity() ==
            secondPoseRenderer.GetAnimationPoseIdentity();
    // The renderer integration loads the model cache as part of constructing
    // the animation-only payload.  Reset both caches before the existing
    // missing-asset assertions so the cases remain isolated.
    ModelLoader::ClearCache();

    const std::string missingPath = "engine/models/Base Model/missing-selftest.glb";
    const ModelLoadResult missingFirst = ModelLoader::LoadModelWithTextures(missingPath);
    const auto failedRecord = assetRegistry.Find(missingPath, AssetType::Model);
    const bool failedWithoutPayload = missingFirst.meshData.subMeshes.empty() &&
        failedRecord.has_value() && failedRecord->state == AssetState::Failed &&
        failedRecord->version == 0 && !failedRecord->error.empty() &&
        ModelLoader::GetCacheSize() == 0;
    const ModelLoadResult missingRetry = ModelLoader::LoadModelWithTextures(missingPath);
    const auto failedRetryRecord = assetRegistry.Find(missingPath, AssetType::Model);
    const bool failedRetryable = missingRetry.meshData.subMeshes.empty() &&
        failedRetryRecord.has_value() && failedRetryRecord->state == AssetState::Failed &&
        failedRetryRecord->version == 0 && ModelLoader::GetCacheSize() == 0;
    const bool passed = modelLoaded && deduplicated && registryReady &&
        reloadTracked && cacheReleaseTracked && animationAssetShared &&
        animationAssetReloaded && animationPoseShared &&
        failedWithoutPayload && failedRetryable;
    LOGE(
        "[ModelCacheSelftest] loaded=%d cache_after_relative=%zu ""cache_after_absolute=%zu deduplicated=%d registry_ready=%d ""reload_tracked=%d cache_release_tracked=%d failed_without_payload=%d ""animation_asset_shared=%d animation_asset_reloaded=%d ""animation_pose_shared=%d failed_retryable=%d -> %s",
        modelLoaded ? 1 : 0, cacheAfterRelative, cacheAfterAbsolute,
        deduplicated ? 1 : 0, registryReady ? 1 : 0, reloadTracked ? 1 : 0,
        cacheReleaseTracked ? 1 : 0, failedWithoutPayload ? 1 : 0,
        animationAssetShared ? 1 : 0, animationAssetReloaded ? 1 : 0,
        animationPoseShared ? 1 : 0, failedRetryable ? 1 : 0,
        passed ? "PASS" : "FAIL");
    assetRegistry.Clear();
    return passed ? 0 : 5;
}

int RunAssetRegistrySelftest()
{
    auto& registry = AssetRegistry::GetInstance();
    registry.Clear();

    const std::string relativePath = "engine/models/Base Model/cube.glb";
    const std::string absolutePath =
        ProjectManager::GetInstance().GetEngineAssetPath("models/Base Model/cube.glb");
    const std::string normalizedRelative = AssetRegistry::NormalizePath(relativePath);
    const std::string normalizedAbsolute = AssetRegistry::NormalizePath(absolutePath);
    const bool normalized = !normalizedRelative.empty() &&
        normalizedRelative == normalizedAbsolute;

    const AssetId modelId = registry.Register(relativePath, AssetType::Model);
    const AssetId sameModelId = registry.Register(absolutePath, AssetType::Model);
    const AssetId textureId = registry.Register(relativePath, AssetType::Texture);
    const bool identity = modelId != 0 && modelId == sameModelId &&
        textureId != 0 && textureId != modelId;

    const bool started = registry.BeginLoad(modelId);
    const auto loading = registry.Find(modelId);
    const bool loadingState = started && loading.has_value() &&
        loading->state == AssetState::Loading;

    const bool firstReady = registry.MarkReady(modelId, 1234);
    const auto ready = registry.Find(modelId);
    const bool readyState = firstReady && ready.has_value() &&
        ready->state == AssetState::Ready && ready->version == 1 &&
        ready->sizeBytes == 1234 && ready->error.empty();

    const bool acquired = registry.Acquire(relativePath, AssetType::Model) == modelId;
    const bool addedRef = registry.AddRef(modelId);
    const auto referenced = registry.Find(modelId);
    const bool references = acquired && addedRef && referenced.has_value() &&
        referenced->refCount == 2;

    const bool reloadStarted = registry.BeginReload(modelId);
    const bool failed = registry.MarkFailed(modelId, "selftest retry");
    const auto failedRecord = registry.Find(modelId);
    const bool failureState = reloadStarted && failed && failedRecord.has_value() &&
        failedRecord->state == AssetState::Failed &&
        failedRecord->error == "selftest retry" && failedRecord->version == 1;

    const bool retryStarted = registry.BeginLoad(modelId);
    const bool secondReady = registry.MarkReady(modelId, 2345);
    const auto retried = registry.Find(modelId);
    const bool retryState = retryStarted && secondReady && retried.has_value() &&
        retried->state == AssetState::Ready && retried->version == 2 &&
        retried->sizeBytes == 2345;

    const bool releasedFirst = registry.Release(modelId);
    const bool releasedSecond = registry.Release(modelId);
    const bool removedModel = registry.Remove(modelId);
    const bool removedTexture = registry.Remove(textureId);
    const bool cleanup = releasedFirst && releasedSecond && removedModel &&
        removedTexture && registry.Size() == 0;

    const bool passed = normalized && identity && loadingState && readyState &&
        references && failureState && retryState && cleanup;
    LOGE(
        "[AssetRegistrySelftest] normalized=%d identity=%d loading=%d ready=%d ""references=%d failure_retry=%d cleanup=%d -> %s",
        normalized ? 1 : 0, identity ? 1 : 0, loadingState ? 1 : 0,
        readyState ? 1 : 0, references ? 1 : 0,
        (failureState && retryState) ? 1 : 0, cleanup ? 1 : 0,
        passed ? "PASS" : "FAIL");
    registry.Clear();
    return passed ? 0 : 5;
}

int RunProjectManifestSelftest(const std::string& dumpPath)
{
    auto& projectManager = ProjectManager::GetInstance();
    const std::string originalRoot = projectManager.GetProjectRoot();
    const std::filesystem::path outputDirectory =
        Utf8Path(dumpPath).parent_path().empty()
            ? std::filesystem::current_path()
            : Utf8Path(dumpPath).parent_path();
    const std::filesystem::path projectDirectory =
        outputDirectory / "project_manifest_selftest_project";
    std::error_code error;
    std::filesystem::remove_all(projectDirectory, error);
    error.clear();
    std::filesystem::create_directories(projectDirectory, error);
    if (error) {
        LOGE("[ProjectManifestSelftest] FAIL: cannot create temp project");
        return 5;
    }
    const std::filesystem::path absoluteProjectDirectory =
        std::filesystem::absolute(projectDirectory, error).lexically_normal();
    if (error) {
        LOGE("[ProjectManifestSelftest] FAIL: cannot resolve temp project");
        return 5;
    }

    const std::filesystem::path manifestPath = absoluteProjectDirectory / "project.json";
    auto writeManifest = [&](const nlohmann::json& document) {
        std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) return false;
        output << document.dump(2) << '\n';
        const bool good = output.good();
        output.close();
        return good && output.good();
    };
    const auto readManifest = [&]() {
        std::ifstream input(manifestPath);
        nlohmann::json document(nlohmann::json::value_t::discarded);
        if (input.is_open()) document = nlohmann::json::parse(input, nullptr, false);
        return document;
    };

    nlohmann::json valid;
    valid["formatVersion"] = 1;
    valid["name"] = "project-manifest-selftest";
    valid["scene"] = "scenes/main.json";
    valid["game"] = "";
    valid["resourceRoot"] = ".";
    valid["codeRoot"] = "games";
    valid["assets"] = nlohmann::json::array({"scenes/main.json"});

    const bool wroteValid = writeManifest(valid);
    const std::string tempProjectPath = Utf8String(absoluteProjectDirectory);
    const std::string expectedProjectRoot =
        GenericUtf8String(absoluteProjectDirectory.lexically_normal()) + "/";
    const bool loadedValid = wroteValid && projectManager.SetProjectRoot(tempProjectPath);
    const bool validVersion = loadedValid &&
        projectManager.GetManifest().formatVersion == 1 &&
        projectManager.GetProjectRoot() == expectedProjectRoot;

    nlohmann::json unsupported = valid;
    unsupported["formatVersion"] = 999;
    const bool wroteUnsupported = writeManifest(unsupported);
    const bool rejectedUnsupported = wroteUnsupported &&
        !projectManager.SetProjectRoot(tempProjectPath);
    const bool preservedAfterReject = projectManager.GetProjectRoot() == expectedProjectRoot &&
        projectManager.GetManifest().formatVersion == 1;

    nlohmann::json legacy = valid;
    legacy.erase("formatVersion");
    const bool wroteLegacy = writeManifest(legacy);
    const bool loadedLegacy = wroteLegacy && projectManager.SetProjectRoot(tempProjectPath);
    const bool legacyVersionDefaulted = loadedLegacy &&
        projectManager.GetManifest().formatVersion == 1;
    const bool savedVersion = loadedLegacy && projectManager.SaveManifest();
    const nlohmann::json saved = savedVersion ? readManifest()
                                              : nlohmann::json(nlohmann::json::value_t::discarded);
    const bool saveWritesVersion = savedVersion && !saved.is_discarded() &&
        saved.contains("formatVersion") && saved.at("formatVersion").is_number_integer() &&
        saved.at("formatVersion").get<int>() == 1;

    size_t manifestTemporaryFiles = 0;
    error.clear();
    for (std::filesystem::directory_iterator it(absoluteProjectDirectory, error), end;
         !error && it != end; it.increment(error)) {
        const std::string filename = Utf8String(it->path().filename());
        if (filename.rfind("project.json.tmp.", 0) == 0) ++manifestTemporaryFiles;
    }
    const bool manifestTemporaryFilesClean = !error && manifestTemporaryFiles == 0;

    bool restored = false;
    if (originalRoot.empty()) {
        projectManager.ClearProjectRoot();
        restored = !projectManager.HasActiveProject();
    } else {
        restored = projectManager.SetProjectRoot(originalRoot);
    }
    error.clear();
    std::filesystem::remove_all(projectDirectory, error);
    const bool cleaned = !error;

    const bool passed = validVersion && rejectedUnsupported && preservedAfterReject &&
        legacyVersionDefaulted && saveWritesVersion && manifestTemporaryFilesClean &&
        restored && cleaned;
    LOGE(
        "[ProjectManifestSelftest] valid=%d reject_unsupported=%d preserve=%d ""legacy_default=%d save_version=%d temp_clean=%d restore=%d cleanup=%d -> %s",
        validVersion ? 1 : 0, rejectedUnsupported ? 1 : 0,
        preservedAfterReject ? 1 : 0, legacyVersionDefaulted ? 1 : 0,
        saveWritesVersion ? 1 : 0, manifestTemporaryFilesClean ? 1 : 0,
        restored ? 1 : 0, cleaned ? 1 : 0,
        passed ? "PASS" : "FAIL");
    return passed ? 0 : 5;
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
    world.modelEntities.reserve(kEntityCount / 4 + 1);
    world.voxEntities.reserve(kEntityCount / 7 + 1);
    world.cullingEntities.reserve(kEntityCount / 4 + kEntityCount / 7 + 2);
    world.cameras.reserve(1);
    world.lights.reserve(1);
    world.terrains.reserve(1);
    world.waters.reserve(1);
    world.skyboxes.reserve(1);
    world.clouds.reserve(1);

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
        }
        if ((index % 7) == 0) {
            entity.hasVoxel = true;
            entity.voxel.voxPath = "stress.vox";
            entity.voxel.loaded = true;
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
    // Exercise the same pure-data stage used after ECS capture.  No ECS
    // registry or SceneECS access is needed to rebuild groups and lists.
    RenderWorldBuilder::Finalize(world);

    std::string validationError;
    const bool valid = world.Validate(&validationError);

    // Compare the pure Finalize stage on the same 10,000-entity snapshot.
    // The worker loop waits after every submission on purpose: this measures
    // the cost of the handoff and worker execution, not an invented frame
    // rate.  Real frames can hide part of that cost behind gameplay work.
    constexpr size_t kFinalizeBenchmarkIterations = 120;
    using BenchmarkClock = std::chrono::steady_clock;
    const auto syncStart = BenchmarkClock::now();
    for (size_t iteration = 0; iteration < kFinalizeBenchmarkIterations; ++iteration) {
        RenderWorldBuilder::Finalize(world);
    }
    const auto syncEnd = BenchmarkClock::now();
    const double syncTotalMilliseconds =
        std::chrono::duration<double, std::milli>(syncEnd - syncStart).count();

    RenderWorldFinalizeWorker finalizeWorker;
    double workerFinalizeTotalMilliseconds = 0.0;
    const auto asyncStart = BenchmarkClock::now();
    bool asyncBenchmarkPassed = true;
    for (size_t iteration = 0; iteration < kFinalizeBenchmarkIterations; ++iteration) {
        if (!finalizeWorker.Submit(world)) {
            asyncBenchmarkPassed = false;
            break;
        }
        try {
            double workerMilliseconds = 0.0;
            finalizeWorker.Wait(&workerMilliseconds);
            workerFinalizeTotalMilliseconds += workerMilliseconds;
        } catch (...) {
            asyncBenchmarkPassed = false;
            break;
        }
    }
    const auto asyncEnd = BenchmarkClock::now();
    const double asyncTotalMilliseconds =
        std::chrono::duration<double, std::milli>(asyncEnd - asyncStart).count();
    const double syncAverageMilliseconds =
        syncTotalMilliseconds / static_cast<double>(kFinalizeBenchmarkIterations);
    const double asyncAverageMilliseconds =
        asyncTotalMilliseconds / static_cast<double>(kFinalizeBenchmarkIterations);
    const double workerFinalizeAverageMilliseconds =
        workerFinalizeTotalMilliseconds / static_cast<double>(kFinalizeBenchmarkIterations);
    const double handoffOverheadPercent = syncTotalMilliseconds > 0.0
        ? ((asyncTotalMilliseconds - syncTotalMilliseconds) / syncTotalMilliseconds) * 100.0
        : 0.0;
    const bool asyncValid = asyncBenchmarkPassed && world.Validate(&validationError);
    LOGI(
        "[RenderWorldBenchmark] entities=%zu iterations=%zu sync_total_ms=%.3f ""async_serial_total_ms=%.3f sync_avg_ms=%.3f async_serial_avg_ms=%.3f ""worker_finalize_avg_ms=%.3f handoff_overhead_pct=%.2f async_valid=%s",
        kEntityCount, kFinalizeBenchmarkIterations,
        syncTotalMilliseconds, asyncTotalMilliseconds,
        syncAverageMilliseconds, asyncAverageMilliseconds,
        workerFinalizeAverageMilliseconds, handoffOverheadPercent,
        asyncValid ? "true" : "false");

    const size_t entityCapacity = world.entities.capacity();
    const size_t childCapacity = world.entities[0].children.capacity();
    const size_t modelEntityCapacity = world.modelEntities.capacity();
    const size_t voxEntityCapacity = world.voxEntities.capacity();
    const size_t cullingEntityCapacity = world.cullingEntities.capacity();
    world.BeginBuild(kEntityCount);
    const bool capacitiesReused =
        world.entities.capacity() == entityCapacity &&
        world.entities[0].children.capacity() == childCapacity &&
        world.modelEntities.capacity() == modelEntityCapacity &&
        world.voxEntities.capacity() == voxEntityCapacity &&
        world.cullingEntities.capacity() == cullingEntityCapacity;
    const bool resetClean =
        world.entities[0].entity == ECS::INVALID_ENTITY &&
        world.entities[0].children.empty() &&
        world.Find(0) == nullptr;

    LOGI(
        "[RenderWorldStress] entities=%zu valid=%s capacities_reused=%s reset_clean=%s ""entity_capacity=%zu child_capacity=%zu%s",
        kEntityCount, valid ? "true" : "false",
        capacitiesReused ? "true" : "false", resetClean ? "true" : "false",
        entityCapacity, childCapacity,
        valid ? "" : (" error=" + validationError).c_str());
    return valid && asyncValid && capacitiesReused && resetClean ? 0 : 5;
}

int RunRenderWorldIncrementalCaptureTest()
{
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& scene = ECS::SceneECS::GetInstance();

    const ECS::Entity parent = scene.CreateEmpty("RenderWorldIncrementalParent");
    const ECS::Entity child = scene.CreateEmpty("RenderWorldIncrementalChild");
    scene.SetParent(child, parent);
    scene.SetPosition(parent, glm::vec3(1.0f, 2.0f, 3.0f));
    scene.SetPosition(child, glm::vec3(2.0f, 0.0f, 0.0f));

    const ECS::Entity renderEntity = scene.CreateEmpty("RenderWorldIncrementalEntity");
    ECS::MeshComponent mesh;
    mesh.type = ECS::MeshType::Model;
    mesh.modelPath = "renderworld-incremental-test.glb";
    coordinator.AddComponent<ECS::MeshComponent>(renderEntity, mesh);
    coordinator.AddComponent<ECS::RenderComponent>(renderEntity, ECS::RenderComponent{});
    ECS::MaterialComponent material;
    material.albedoColor = glm::vec3(0.2f, 0.4f, 0.6f);
    coordinator.AddComponent<ECS::MaterialComponent>(renderEntity, material);

    RenderWorld published;
    RenderWorld staging;
    auto captureAndPublish = [&]() {
        RenderWorldBuilder::Capture(staging);
        RenderWorldBuilder::Finalize(staging);
        RenderWorldBuildStats stats;
        RenderWorldBuilder::CollectStats(staging, stats, 0.0);
        std::string validationError;
        const bool valid = staging.Validate(&validationError);
        std::swap(published, staging);
        return std::pair<RenderWorldBuildStats, std::string>(
            stats, valid ? std::string{} : validationError);
    };

    bool passed = true;
    auto check = [&](const char* name, bool condition) {
        LOGI("[RenderWorldIncremental] %s=%s",
                     name, condition ? "pass" : "FAIL");
        passed = passed && condition;
    };

    // Warm both sides of the double buffer, then require a stable capture to
    // reuse the transform and component payloads.
    (void)captureAndPublish();
    (void)captureAndPublish();
    const auto stable = captureAndPublish();
    check("stable_incremental", stable.first.incrementalCaptureUsed);
    check("stable_transform_reuse", stable.first.reusedTransformCount >= 3u);
    check("stable_component_reuse", stable.first.reusedComponentCount >= 3u);

    // Deliberately bypass a setter: content fingerprints must still detect a
    // direct ECS field write and refresh only the affected render payload.
    material = coordinator.GetComponent<ECS::MaterialComponent>(renderEntity);
    material.albedoColor = glm::vec3(0.8f, 0.1f, 0.3f);
    coordinator.GetComponent<ECS::MaterialComponent>(renderEntity) = material;
    const auto materialChanged = captureAndPublish();
    const RenderWorldEntity* changedEntity = published.Find(renderEntity);
    check("direct_material_change_detected",
          materialChanged.first.recomputedComponentCount >= 1u &&
          changedEntity != nullptr && changedEntity->hasMaterial &&
          changedEntity->material.albedoColor == material.albedoColor);

    // Component removal/addition must not leave a stale payload in the
    // reusable staging record.
    coordinator.RemoveComponent<ECS::MaterialComponent>(renderEntity);
    const auto materialRemoved = captureAndPublish();
    const RenderWorldEntity* removedEntity = published.Find(renderEntity);
    check("component_remove_clears_snapshot",
          materialRemoved.first.incrementalCaptureUsed &&
          removedEntity != nullptr && !removedEntity->hasMaterial);

    ECS::MaterialComponent restoredMaterial;
    restoredMaterial.albedoColor = glm::vec3(0.4f, 0.7f, 0.2f);
    coordinator.AddComponent<ECS::MaterialComponent>(renderEntity, restoredMaterial);
    const auto materialRestored = captureAndPublish();
    const RenderWorldEntity* restoredEntity = published.Find(renderEntity);
    check("component_add_refreshes_snapshot",
          materialRestored.first.recomputedComponentCount >= 1u &&
          restoredEntity != nullptr && restoredEntity->hasMaterial &&
          restoredEntity->material.albedoColor == restoredMaterial.albedoColor);

    // Moving a parent must invalidate both the parent and its descendant.
    scene.SetPosition(parent, glm::vec3(5.0f, 6.0f, 7.0f));
    const auto parentMoved = captureAndPublish();
    const RenderWorldEntity* movedParent = published.Find(parent);
    const RenderWorldEntity* movedChild = published.Find(child);
    const glm::vec3 expectedChildPosition(7.0f, 6.0f, 7.0f);
    const glm::vec3 actualChildPosition = movedChild != nullptr
        ? glm::vec3(movedChild->transform.worldMatrix[3])
        : glm::vec3(0.0f);
    check("parent_change_recomputes_subtree",
          parentMoved.first.recomputedTransformCount >= 2u &&
          movedParent != nullptr && movedChild != nullptr &&
          glm::length(actualChildPosition - expectedChildPosition) < 1e-4f);

    // Entity-set changes intentionally disable cache reuse for that capture,
    // preventing an old raw entity slot from being mistaken for a new one.
    scene.DestroyEntity(renderEntity);
    const ECS::Entity replacement = scene.CreateEmpty("RenderWorldIncrementalReplacement");
    coordinator.AddComponent<ECS::RenderComponent>(replacement, ECS::RenderComponent{});
    const auto structuralChange = captureAndPublish();
    const RenderWorldEntity* replacementEntity = published.Find(replacement);
    check("entity_set_change_invalidates_cache",
          !structuralChange.first.incrementalCaptureUsed &&
          replacementEntity != nullptr && replacementEntity->hasTransform);

    scene.DestroyEntity(child);
    scene.DestroyEntity(parent);
    scene.DestroyEntity(replacement);

    if (!passed) {
        LOGE("[RenderWorldIncremental] FAIL");
        return 5;
    }
    LOGI("[RenderWorldIncremental] PASS");
    return 0;
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
    bool renderWorldIncremental = false;
    bool worldJobSelftest = false;
    bool sceneSaveSelftest = false;
    bool modelCacheSelftest = false;
    bool assetRegistrySelftest = false;
    bool projectManifestSelftest = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] ? argv[index] : "";
        auto nextValue = [&](const char* option) -> const char* {
            if (index + 1 >= argc || !argv[index + 1]) {
                LOGE("[GameplayTest] ERROR: %s requires a value", option);
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
        else if (argument == "--renderworld-incremental") renderWorldIncremental = true;
        else if (argument == "--world-job-selftest") worldJobSelftest = true;
        else if (argument == "--scene-save-selftest") sceneSaveSelftest = true;
        else if (argument == "--model-cache-selftest") modelCacheSelftest = true;
        else if (argument == "--asset-registry-selftest") assetRegistrySelftest = true;
        else if (argument == "--project-manifest-selftest") projectManifestSelftest = true;
    }

    if (invalidArguments) return 64;
    if (renderWorldStress) return RunRenderWorldStressTest();

    InputReplay inputReplay;
    if (!inputReplayPath.empty() && scriptedInput) {
        LOGE("[GameplayTest] ERROR: --input-replay and --scripted-input are mutually exclusive");
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
        LOGE(
            "[GameplayTest] ERROR: required: --scene <path> --dump-state <path>; ""--frames must be 1..1000000 and --fixed-dt must be (0,0.1]; ""--scripted-input enables deterministic player movement/jump; ""--buoyancy-test checks player water contact and buoyancy; ""--auto-start-game invokes the game module's deterministic test start hook; ""--renderworld-stress runs a 10000-entity RenderWorld validation test; ""--renderworld-incremental runs dynamic RenderWorld cache assertions; ""--scene-save-selftest validates atomic scene/prefab saves; ""--model-cache-selftest validates normalized model cache keys; ""--asset-registry-selftest validates asset identity and lifecycle; ""--project-manifest-selftest validates project formatVersion compatibility");
        return 64;
    }

    if (!ProjectManager::GetInstance().Initialize(argc, argv)) {
        LOGE("[GameplayTest] ERROR: failed to initialize engine/project paths");
        return 2;
    }
#ifndef __ANDROID__
    if (!ProjectManager::GetInstance().HasActiveProject()) {
        LOGE(
            "[GameplayTest] ERROR: desktop gameplay tests require --project <project-directory>");
        return 2;
    }
#endif
    if (worldJobSelftest) {
        const int testResult = RunWorldJobSystemSelftest();
        nlohmann::json state;
        state["runtime_layer"] = "gameplay-cpu";
        state["world_job_selftest"] = (testResult == 0);
        std::ofstream dumpFile(Utf8Path(dumpPath));
        if (!dumpFile) {
            LOGE("[WorldJobSystem] FAIL: cannot write dump %s", dumpPath.c_str());
            return 5;
        }
        dumpFile << state.dump(2) << '\n';
        return testResult;
    }
    if (modelCacheSelftest) {
        const int testResult = RunModelCacheSelftest();
        nlohmann::json state;
        state["runtime_layer"] = "gameplay-cpu";
        state["model_cache_selftest"] = (testResult == 0);
        std::ofstream dumpFile(Utf8Path(dumpPath));
        if (!dumpFile) {
            LOGE("[ModelCacheSelftest] FAIL: cannot write dump %s",
                         dumpPath.c_str());
            return 5;
        }
        dumpFile << state.dump(2) << '\n';
        return testResult;
    }
    if (assetRegistrySelftest) {
        const int testResult = RunAssetRegistrySelftest();
        nlohmann::json state;
        state["runtime_layer"] = "gameplay-cpu";
        state["asset_registry_selftest"] = (testResult == 0);
        std::ofstream dumpFile(Utf8Path(dumpPath));
        if (!dumpFile) {
            LOGE("[AssetRegistrySelftest] FAIL: cannot write dump %s",
                         dumpPath.c_str());
            return 5;
        }
        dumpFile << state.dump(2) << '\n';
        return testResult;
    }
    if (projectManifestSelftest) {
        const int testResult = RunProjectManifestSelftest(dumpPath);
        nlohmann::json state;
        state["runtime_layer"] = "gameplay-cpu";
        state["project_manifest_selftest"] = (testResult == 0);
        std::ofstream dumpFile(Utf8Path(dumpPath));
        if (!dumpFile) {
            LOGE("[ProjectManifestSelftest] FAIL: cannot write dump %s",
                         dumpPath.c_str());
            return 5;
        }
        dumpFile << state.dump(2) << '\n';
        return testResult;
    }
    scenePath = ProjectManager::GetInstance().ResolveAssetPath(scenePath);
    if (scenePath.empty()) {
        LOGE("[GameplayTest] ERROR: cannot resolve scene inside the selected project");
        return 2;
    }
    std::error_code error;
    const std::filesystem::path dumpParent =
        Utf8Path(dumpPath).parent_path();
    if (!dumpParent.empty()) std::filesystem::create_directories(dumpParent, error);

    Core::GameplayRuntime runtime;
    if (!runtime.Initialize()) return 1;
    if (!runtime.LoadScene(scenePath, gameName, autoStartGame)) return 2;

    if (renderWorldIncremental) {
        const int testResult = RunRenderWorldIncrementalCaptureTest();
        const bool dumpOk = Core::GameplayRuntime::DumpState(
            dumpPath, 1, "gameplay-cpu", 1.0f / fixedDelta);
        runtime.Shutdown();
        return testResult == 0 && dumpOk ? 0 : 5;
    }

    if (sceneSaveSelftest) {
        const int testResult = RunSceneSaveSelftest(dumpPath);
        const bool dumpOk = Core::GameplayRuntime::DumpState(
            dumpPath, 1, "gameplay-cpu", 1.0f / fixedDelta);
        runtime.Shutdown();
        return testResult == 0 && dumpOk ? 0 : 5;
    }

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
            LOGE(
                "[GameplayTest] ERROR: requested player test found no player controller entity");
            runtime.Shutdown();
            return 4;
        }

        if (player != ECS::INVALID_ENTITY) {
            auto& scene = ECS::SceneECS::GetInstance();
            initialPlayerPosition = scene.GetPosition(player);
        } else if (inputReplay.IsLoaded()) {
            LOGI(
                "[GameplayTest] input replay: no PlayerController entity; treating replay as game-plugin input");
        }
        if (scriptedInput) {
            LOGI(
                "[GameplayTest] scripted input: waiting for physics contact before jump");
        }
        if (buoyancyTest) {
            minimumPlayerY = initialPlayerPosition.y;
            LOGI(
                "[GameplayTest] buoyancy test: tracking player water contact from y=%.3f",
                initialPlayerPosition.y);
        }
    }

    LOGI("[GameplayTest] running %d frames at fixed_dt=%.8f (no SDL Video, no Vulkan)",
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
        LOGI(
            "[GameplayTest] input assertions: first_ground_frame=%d first_ground_y=%.3f ""jump_frame=%d move=[%d,%d) horizontal_distance=%.3f moved=%s ""pre_jump_y=%.3f max_after_jump_y=%.3f jumped=%s landed=%s final_y=%.3f",
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
        LOGI(
            "[GameplayTest] buoyancy assertions: first_water_frame=%d ""contact_frames=%d min_y=%.3f final_y=%.3f velocity_y=%.3f ""entered=%s remained_in_volume=%s",
            firstWaterFrame, waterContactFrames, minimumPlayerY, finalPosition.y,
            finalVelocity.y, enteredWater ? "true" : "false",
            remainedInTestVolume ? "true" : "false");
    }

    if (inputReplay.IsLoaded()) {
        replayPass = replayFramesApplied == frames;
        LOGI(
            "[GameplayTest] input replay: path=%s replay_frames=%d applied_frames=%d ""events=%zu move_frames=%d jump_frames=%d pass=%s",
            inputReplayPath.c_str(), inputReplay.FrameCount(), replayFramesApplied,
            inputReplay.EventCount(), replayMoveFrames, replayJumpFrames,
            replayPass ? "true" : "false");
    }

    if (!Core::GameplayRuntime::DumpState(dumpPath, frames, "gameplay-cpu", 1.0f / fixedDelta)) return 3;
    runtime.Shutdown();
    if (!scriptedInputPass || !buoyancyPass || !replayPass) {
        if (!scriptedInputPass) {
            LOGE("[GameplayTest] FAIL: scripted input assertions did not pass");
        }
        if (!buoyancyPass) {
            LOGE("[GameplayTest] FAIL: buoyancy assertions did not pass");
        }
        if (!replayPass) {
            LOGE("[GameplayTest] FAIL: input replay did not cover the requested frame range");
        }
        return 5;
    }
    LOGI("[GameplayTest] PASS");
    return 0;
}
