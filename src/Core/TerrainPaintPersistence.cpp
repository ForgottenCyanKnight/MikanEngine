// TerrainPaintPersistence.cpp - 地形笔刷产物持久化（显式保存时导出四张图）
#include "Core/TerrainPaintPersistence.h"

#include "Core/EngineConfig.h"
#include "Core/Log.h"
#include "Core/ProjectManager.h"
#include "Core/RenderGlobals.h"
#include "Core/Utf8Path.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/SceneECS.h"
#include "Rendering/SceneRenderer.h"
#include "Rendering/TerrainRenderer.h"

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace TerrainPaintPersistence {

namespace {

// 场景路径解析：项目相对路径相对项目根，绝对路径直通。
fs::path ResolveSceneFile(const std::string& sceneFilePath) {
    fs::path p = Utf8Path(sceneFilePath);
    if (p.is_absolute()) {
        return p;
    }
    // 场景路径相对项目根；ProjectManager 没有直接暴露 join，这里手动拼。
    const std::string projectRoot = ProjectManager::GetInstance().GetProjectRoot();
    if (projectRoot.empty()) {
        return fs::absolute(p);
    }
    return Utf8Path(projectRoot) / p;
}

std::string ToProjectRelative(const fs::path& absolutePath) {
    const std::string projectRoot = ProjectManager::GetInstance().GetProjectRoot();
    if (projectRoot.empty()) {
        return GenericUtf8String(absolutePath);
    }
    std::error_code ec;
    fs::path rel = fs::relative(absolutePath, Utf8Path(projectRoot), ec);
    if (ec || rel.empty()) {
        return GenericUtf8String(absolutePath);
    }
    return GenericUtf8String(rel);
}

void CollectTerrainEntities(const std::vector<ECS::Entity>& roots,
                            std::vector<ECS::Entity>& out) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    auto& sceneECS = ECS::SceneECS::GetInstance();

    std::vector<ECS::Entity> stack = roots;
    while (!stack.empty()) {
        const ECS::Entity entity = stack.back();
        stack.pop_back();
        if (entity == ECS::INVALID_ENTITY) {
            continue;
        }
        if (coordinator.HasComponent<ECS::TerrainComponent>(entity)) {
            out.push_back(entity);
        }
        for (const ECS::Entity child : sceneECS.GetChildren(entity)) {
            stack.push_back(child);
        }
    }
}

} // namespace

uint32_t SaveTerrainPaintData(const std::string& sceneFilePath, std::string* errorMessage) {
    if (errorMessage) {
        errorMessage->clear();
    }

    const fs::path sceneFile = ResolveSceneFile(sceneFilePath);
    if (sceneFile.empty() || sceneFile.stem().empty()) {
        if (errorMessage) *errorMessage = "invalid scene file path";
        return 0;
    }

    std::vector<ECS::Entity> roots = ECS::SceneECS::GetInstance().GetRootEntities();
    std::vector<ECS::Entity> terrainEntities;
    CollectTerrainEntities(roots, terrainEntities);
    if (terrainEntities.empty()) {
        return 0;
    }

    TerrainRenderer& terrainRenderer = g_SceneRenderer.GetTerrainRenderer();
    auto& coordinator = ECS::Coordinator::GetInstance();

    const fs::path outputDir = sceneFile.parent_path() / "terrain_paint";
    const std::string baseName = Utf8String(sceneFile.stem());

    std::error_code ec;
    fs::create_directories(outputDir, ec);

    uint32_t savedCount = 0;
    bool reportedError = false;

    for (const ECS::Entity entity : terrainEntities) {
        ECS::TerrainComponent& terrain =
            coordinator.GetComponent<ECS::TerrainComponent>(entity);
        const std::string stem =
            baseName + "_" + std::to_string(static_cast<unsigned long long>(entity));

        // 四张图各自判断脏标记：没改过的不重复写盘。
        const fs::path heightAbs = outputDir / (stem + "_height.png");
        const int heightResult = terrainRenderer.ExportSculptedHeightmap(
            entity, GenericUtf8String(heightAbs));
        if (heightResult == 1) {
            terrain.sculptedHeightmapPath = ToProjectRelative(heightAbs);
            ++savedCount;
        } else if (heightResult < 0 && !reportedError) {
            if (errorMessage) {
                *errorMessage = "heightmap export failed for entity " +
                                std::to_string(static_cast<unsigned long long>(entity));
            }
            reportedError = true;
        }

        const fs::path controlAbs = outputDir / (stem + "_control.png");
        const int controlResult = terrainRenderer.ExportPaintedControlMap(
            entity, GenericUtf8String(controlAbs));
        if (controlResult == 1) {
            terrain.paintedControlMapPath = ToProjectRelative(controlAbs);
            ++savedCount;
        } else if (controlResult < 0 && !reportedError) {
            if (errorMessage) {
                *errorMessage = "control map export failed for entity " +
                                std::to_string(static_cast<unsigned long long>(entity));
            }
            reportedError = true;
        }

        const fs::path grassAbs = outputDir / (stem + "_grass.png");
        const int grassResult = terrainRenderer.ExportPaintedGrassMap(
            entity, GenericUtf8String(grassAbs));
        if (grassResult == 1) {
            terrain.paintedGrassPath = ToProjectRelative(grassAbs);
            ++savedCount;
        } else if (grassResult < 0 && !reportedError) {
            if (errorMessage) {
                *errorMessage = "grass map export failed for entity " +
                                std::to_string(static_cast<unsigned long long>(entity));
            }
            reportedError = true;
        }

        const fs::path waterAbs = outputDir / (stem + "_water.png");
        const int waterResult = terrainRenderer.ExportPaintedWaterMap(
            entity, GenericUtf8String(waterAbs));
        if (waterResult == 1) {
            terrain.paintedWaterPath = ToProjectRelative(waterAbs);
            ++savedCount;
        } else if (waterResult < 0 && !reportedError) {
            if (errorMessage) {
                *errorMessage = "water map export failed for entity " +
                                std::to_string(static_cast<unsigned long long>(entity));
            }
            reportedError = true;
        }
    }

    if (savedCount > 0) {
        LOGI("[TerrainPaint] saved %u painted map(s) across %zu terrain entity(s)",
             savedCount, terrainEntities.size());
    }
    return savedCount;
}

} // namespace TerrainPaintPersistence
