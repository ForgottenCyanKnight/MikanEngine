#include "Rendering/RenderWorldBuilder.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

void AddModelEntity(const RenderWorldEntity& snapshot,
                    std::unordered_map<std::string, size_t>& groupIndices,
                    std::vector<RenderModelGroup>& groups)
{
    const bool hasVisibleRender = snapshot.visible && snapshot.hasTransform;
    if (!hasVisibleRender || !snapshot.hasMesh || !snapshot.hasRenderFlags ||
        (snapshot.mesh.type != RenderMeshType::Model &&
         snapshot.mesh.type != RenderMeshType::Plane) ||
        snapshot.mesh.modelPath.empty()) {
        return;
    }

    const bool perEntityAnimation = snapshot.hasAnimator || snapshot.hasVmdPlayer;
    const std::string key = snapshot.mesh.modelPath +
        (perEntityAnimation ? "#entity:" + std::to_string(snapshot.entity) : "");
    auto [it, inserted] = groupIndices.emplace(key, groups.size());
    if (inserted) {
        RenderModelGroup group;
        group.rendererKey = key;
        group.modelPath = snapshot.mesh.modelPath;
        groups.push_back(std::move(group));
    }
    groups[it->second].entities.push_back(snapshot.entity);
}

void AddVoxEntity(const RenderWorldEntity& snapshot,
                  std::unordered_map<std::string, size_t>& groupIndices,
                  std::vector<RenderVoxGroup>& groups)
{
    const bool hasVisibleRender = snapshot.visible && snapshot.hasTransform;
    if (!hasVisibleRender || !snapshot.hasVoxel || snapshot.voxel.voxPath.empty()) {
        return;
    }

    auto [it, inserted] = groupIndices.emplace(snapshot.voxel.voxPath, groups.size());
    if (inserted) {
        RenderVoxGroup group;
        group.voxPath = snapshot.voxel.voxPath;
        groups.push_back(std::move(group));
    }
    groups[it->second].entities.push_back(snapshot.entity);
}

template <typename Group>
size_t CountGroupedEntities(const std::vector<Group>& groups)
{
    size_t count = 0;
    for (const Group& group : groups) {
        count += group.entities.size();
    }
    return count;
}

void MergeModelGroups(const std::vector<RenderWorldFinalizePartition>& partitions,
                      std::vector<RenderModelGroup>& output)
{
    std::unordered_map<std::string, size_t> groupIndices;
    size_t expectedGroupCount = 0;
    for (const auto& partition : partitions) {
        expectedGroupCount += partition.modelGroups.size();
    }
    groupIndices.reserve(expectedGroupCount);
    output.clear();
    output.reserve(expectedGroupCount);

    for (const auto& partition : partitions) {
        for (const RenderModelGroup& group : partition.modelGroups) {
            auto [it, inserted] = groupIndices.emplace(group.rendererKey, output.size());
            if (inserted) {
                RenderModelGroup merged;
                merged.rendererKey = group.rendererKey;
                merged.modelPath = group.modelPath;
                output.push_back(std::move(merged));
            }
            auto& destination = output[it->second].entities;
            destination.insert(destination.end(), group.entities.begin(), group.entities.end());
        }
    }
}

void MergeVoxGroups(const std::vector<RenderWorldFinalizePartition>& partitions,
                    std::vector<RenderVoxGroup>& output)
{
    std::unordered_map<std::string, size_t> groupIndices;
    size_t expectedGroupCount = 0;
    for (const auto& partition : partitions) {
        expectedGroupCount += partition.voxGroups.size();
    }
    groupIndices.reserve(expectedGroupCount);
    output.clear();
    output.reserve(expectedGroupCount);

    for (const auto& partition : partitions) {
        for (const RenderVoxGroup& group : partition.voxGroups) {
            auto [it, inserted] = groupIndices.emplace(group.voxPath, output.size());
            if (inserted) {
                RenderVoxGroup merged;
                merged.voxPath = group.voxPath;
                output.push_back(std::move(merged));
            }
            auto& destination = output[it->second].entities;
            destination.insert(destination.end(), group.entities.begin(), group.entities.end());
        }
    }
}

} // namespace

void RenderWorldBuilder::FinalizeRange(const RenderWorld& source,
                                       std::size_t begin,
                                       std::size_t end,
                                       RenderWorldFinalizePartition& out)
{
    out.Reset();
    const std::size_t safeBegin = std::min(begin, source.entities.size());
    const std::size_t safeEnd = std::min(std::max(safeBegin, end), source.entities.size());
    const std::size_t rangeSize = safeEnd - safeBegin;
    out.modelGroups.reserve(rangeSize);
    out.voxGroups.reserve(rangeSize);

    std::unordered_map<std::string, size_t> modelGroupIndices;
    std::unordered_map<std::string, size_t> voxGroupIndices;
    modelGroupIndices.reserve(rangeSize);
    voxGroupIndices.reserve(rangeSize);

    for (std::size_t index = safeBegin; index < safeEnd; ++index) {
        const RenderWorldEntity& snapshot = source.entities[index];
        AddModelEntity(snapshot, modelGroupIndices, out.modelGroups);
        AddVoxEntity(snapshot, voxGroupIndices, out.voxGroups);
    }
}

void RenderWorldBuilder::MergeFinalizedPartitions(
    RenderWorld& out,
    const std::vector<RenderWorldFinalizePartition>& partitions)
{
    MergeModelGroups(partitions, out.modelGroups);
    MergeVoxGroups(partitions, out.voxGroups);

    out.modelEntities.clear();
    out.modelEntities.reserve(CountGroupedEntities(out.modelGroups));
    for (const RenderModelGroup& group : out.modelGroups) {
        out.modelEntities.insert(out.modelEntities.end(), group.entities.begin(), group.entities.end());
    }

    out.voxEntities.clear();
    out.voxEntities.reserve(CountGroupedEntities(out.voxGroups));
    for (const RenderVoxGroup& group : out.voxGroups) {
        out.voxEntities.insert(out.voxEntities.end(), group.entities.begin(), group.entities.end());
    }

    out.cullingEntities.clear();
    out.cullingEntities.reserve(out.modelEntities.size() + out.voxEntities.size());
    out.cullingEntities.insert(out.cullingEntities.end(), out.modelEntities.begin(), out.modelEntities.end());
    out.cullingEntities.insert(out.cullingEntities.end(), out.voxEntities.begin(), out.voxEntities.end());
    out.RebuildIndex();
}
