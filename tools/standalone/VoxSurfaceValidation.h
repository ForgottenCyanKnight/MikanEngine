#pragma once
// Offline validation of the SAME loader, material packing and BakeVoxSurfaces
// used by VoxRenderer. No renderer object, Vulkan calls or GPU allocation.
#include "Core/Log.h"
#include "Core/Utf8Path.h"
#include "Rendering/VoxFaceMaterial.h"
#include "Rendering/RayTracing/VoxRayTracingGeometry.h"
#include "Rendering/RayTracing/VoxSurfaceBake.h"
#include "json.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

namespace VoxValidation {
using Cell = std::array<int, 3>; // raw VOX x,y,z
using Face = std::array<int, 4>; // direction,x,y,z of owning voxel
using Attribute = std::pair<uint32_t, uint32_t>;
constexpr Cell normals[6] = {{0,1,0},{0,-1,0},{-1,0,0},{1,0,0},{0,0,1},{0,0,-1}};
inline void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct Source {
    std::vector<VoxRtQuad> quads;
    std::vector<uint32_t> materials;
    VoxelMeshData mesh{};
    const auto& GetQuads() const { return quads; }
    const auto& GetQuadMaterials() const { return materials; }
    const auto& GetMeshData() const { return mesh; }
};
struct Baked {
    std::vector<VoxRtQuad> quads;
    std::vector<uint32_t> materials, attributes;
    std::vector<VoxRayTracingRange> ranges;
    VoxContactBackfillStats contacts;
};
inline Attribute DecodeAttribute(const Baked& baked, size_t q, uint32_t u, uint32_t v) {
    const auto& a = baked.attributes;
    const auto descriptor = a.at(a.at(1) + q);
    uint32_t id = descriptor & 0x7fffffffu;
    if (descriptor & 0x80000000u) {
        const uint32_t byte = id + v * ((baked.quads[q].geometry >> 24) + 1) + u;
        id = (a.at(a.at(2) + byte / 4) >> ((byte % 4) * 8)) & 255u;
    }
    return {a.at(a.at(0) + id * 2), a.at(a.at(0) + id * 2 + 1)};
}
inline nlohmann::json StatsJson(const VoxSurfaceReductionStats& stats) {
    return {{"exposed_unit_faces", stats.exposedFaceCount}, {"greedy_quads", stats.greedyQuadCount},
        {"final_quads", stats.finalQuadCount}, {"triangles", stats.TriangleCount()},
        {"greedy_reduction_percent", stats.GreedyReductionFraction() * 100},
        {"final_reduction_percent", stats.ReductionFraction() * 100}, {"merge_factor", stats.MergeFactor()}};
}

struct ModelResult {
    VoxSurfaceReductionStats stats;
    nlohmann::json details;
    Baked greedySurface, finalSurface;
    Cell minimum{}, maximum{};
};
inline ModelResult Analyze(const VoxFormat::VoxData& data, const VoxFormat::Model& model) {
    const auto start = std::chrono::steady_clock::now();
    std::map<Cell, uint8_t> cells;
    Cell minimum{256,256,256}, maximum{0,0,0};
    for (const auto& voxel : model.voxels) {
        Require(voxel.colorIndex != 0, "zero-palette voxel is not a solid cell");
        const Cell p{voxel.x, voxel.y, voxel.z};
        Require(cells.emplace(p, voxel.colorIndex).second, "duplicate voxel coordinate");
        for (int axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], p[axis]);
            maximum[axis] = std::max(maximum[axis], p[axis]);
        }
    }
    if (cells.empty()) return {{}, {{"voxel_count", 0}, {"stats", StatsJson({})}, {"coverage", "pass"}}};
    auto neighbor = [](Cell p, int d) {
        for (int axis = 0; axis < 3; ++axis) p[axis] += normals[d][axis];
        return p;
    };
    auto attribute = [&](uint8_t index) -> Attribute {
        const auto c = data.palette[index];
        return {(uint32_t(c.r) << 8) | (uint32_t(c.g) << 16) | (uint32_t(c.b) << 24),
                PackVoxFaceMaterial(data, index)};
    };
    auto ordinary = [&](Cell p) {
        const auto it = cells.find(p);
        if (it == cells.end()) return false;
        const auto type = data.hasMaterials ? data.materials[it->second].type : 0;
        return type != 2 && type != 3;
    };

    // Independent six-neighbor oracle. Keys are raw coordinates, so the audit
    // also catches direction/axis swapping and occupied-bounds normalization.
    std::map<Face, Attribute> oracle;
    for (const auto& [p, index] : cells)
        for (int d = 0; d < 6; ++d)
            if (!cells.contains(neighbor(p, d))) oracle.emplace(Face{d,p[0],p[1],p[2]}, attribute(index));

    // Unit-face adapter follows VoxRenderer's packed input contract. The actual
    // rectangle reconstruction and backfill run in the production bake function.
    Source source;
    for (int d = 0; d < 6; ++d) {
        auto& group = source.mesh.faceGroups[d];
        group.faceDirection = d;
        group.firstIndex = source.quads.size() * 6;
        for (const auto& voxel : model.voxels) {
            const Cell p{voxel.x, voxel.y, voxel.z};
            if (cells.contains(neighbor(p, d))) continue;
            const uint32_t x = p[0] - minimum[0], y = p[2] - minimum[2], z = p[1] - minimum[1];
            const auto [color, material] = attribute(voxel.colorIndex);
            source.quads.push_back({x | (y << 8) | (z << 16), color});
            source.materials.push_back(material);
        }
        group.indexCount = source.quads.size() * 6 - group.firstIndex;
    }
    Require(source.quads.size() == oracle.size(), "exposed face count differs from oracle");
    Baked uniform, greedy, candidate;
    BakeVoxSurfaces(source, false, uniform.quads, uniform.materials, uniform.ranges, uniform.attributes);
    BakeVoxSurfaces(source, true, greedy.quads, greedy.materials, greedy.ranges, greedy.attributes);
    const VoxContactQuery contact = [&](int d, int plane, int u, int v) {
        const auto pair = VoxContactRawPair(d, plane, u, v, minimum[0], minimum[1], minimum[2]);
        return ordinary(pair[0]) && ordinary(pair[1]);
    };
    const VoxContactQuery eligible = [&](int d, int plane, int u, int v) {
        return ordinary(VoxContactRawPair(d, plane, u, v, minimum[0], minimum[1], minimum[2])[0]);
    };
    BakeVoxSurfaces(source, true, candidate.quads, candidate.materials, candidate.ranges,
                    candidate.attributes, contact, &candidate.contacts, eligible, true);

    auto audit = [&](const Baked& baked, bool allowHidden) {
        std::set<Face> seen, hidden;
        for (const auto& range : baked.ranges) {
            const int d = int(range.direction);
            Require(d >= 0 && d < 6, "invalid baked face direction");
            for (size_t q = range.firstQuad; q < size_t(range.firstQuad) + range.quadCount; ++q) {
                const auto quad = baked.quads.at(q);
                const Cell origin{int(quad.geometry & 255), int((quad.geometry >> 8) & 255), int((quad.geometry >> 16) & 255)};
                const int width = int(quad.geometry >> 24) + 1, height = int(quad.appearance & 255) + 1;
                for (int v = 0; v < height; ++v) for (int u = 0; u < width; ++u) {
                    Cell local = origin;
                    if (d < 2) { local[0] += u; local[1] += v; }
                    else if (d < 4) { local[2] += u; local[1] += v; }
                    else { local[0] += u; local[2] += v; }
                    const Cell raw{local[0]+minimum[0], local[2]+minimum[1], local[1]+minimum[2]};
                    const Face key{d,raw[0],raw[1],raw[2]};
                    const auto expected = oracle.find(key);
                    if (expected != oracle.end()) {
                        Require(seen.insert(key).second, "exterior face covered more than once");
                        Require(DecodeAttribute(baked,q,u,v) == expected->second, "surface color/material changed");
                    } else {
                        Require(allowHidden && ordinary(raw) && ordinary(neighbor(raw,d)), "quad crosses a true hole or ineligible contact");
                        Require(hidden.insert(key).second, "hidden face covered more than once");
                    }
                }
            }
        }
        Require(seen.size() == oracle.size(), "baked surface omitted exterior faces");
        return hidden.size();
    };
    audit(uniform, false);
    audit(greedy, false);
    const size_t hidden = audit(candidate, true);
    Require(hidden == candidate.contacts.emittedHidden, "hidden contact counter differs from audit");
    Require(candidate.contacts.sourceReal == oracle.size() && candidate.contacts.emittedReal == oracle.size(),
            "backfill real coverage counters differ from oracle");
    auto emitters = [](const Baked& baked) {
        std::vector<std::array<uint32_t,4>> result;
        for (const auto& range : baked.ranges)
            for (size_t q = range.firstQuad; q < size_t(range.firstQuad)+range.quadCount; ++q)
                if (baked.materials.at(q) & 2u)
                    result.push_back({range.direction,baked.quads[q].geometry,baked.quads[q].appearance,baked.materials[q]});
        std::sort(result.begin(),result.end());
        return result;
    };
    Require(emitters(greedy) == emitters(candidate), "backfill changed source-emitter geometry/material");
    const bool selectCandidate = candidate.quads.size() < greedy.quads.size();
    const auto& final = selectCandidate ? candidate : greedy;
    VoxSurfaceReductionStats stats{oracle.size(), greedy.quads.size(), final.quads.size()};
    Require(stats.finalQuadCount <= stats.greedyQuadCount && stats.greedyQuadCount <= stats.exposedFaceCount,
            "reduction counters out of bounds");
    const double elapsed = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    return {stats, {{"voxel_count",cells.size()}, {"occupied_dimensions",Cell{maximum[0]-minimum[0]+1,maximum[1]-minimum[1]+1,maximum[2]-minimum[2]+1}},
        {"stats",StatsJson(stats)}, {"same_color_greedy_quads",uniform.quads.size()},
        {"backfill_candidate_quads",candidate.quads.size()}, {"backfill_selected",selectCandidate},
        {"candidate_hidden_unit_faces",hidden}, {"selected_hidden_unit_faces",selectCandidate?hidden:0},
        {"surface_attribute_bytes",final.attributes.size()*4}, {"coverage","pass"},
        {"colors_and_materials","pass"}, {"emitter_preservation","pass"}, {"analysis_ms",elapsed}}, greedy, final, minimum, maximum};
}

} // namespace VoxValidation
