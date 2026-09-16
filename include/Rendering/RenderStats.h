#pragma once
// RenderStats.h - 轻量逐帧渲染统计（drawcall / 三角面 / 模型实例）
// 统计机制：本头把 vkCmdDraw / vkCmdDrawIndexed / vkCmdDrawIndexedIndirect
// 包装成同名宏（展开里用括号包住函数名抑制宏递归，最终仍调到真函数）。
// 任何包含本头的渲染器 .cpp，其绘制调用自动计数，无需逐渲染器埋点。
//   - 三角面：索引绘制 = indexCount * instanceCount / 3；非索引绘制 =
//     vertexCount * instanceCount / 3；间接绘制（体素 multi-draw）三角形数在
//     GPU 端，CPU 无法得知，只计 drawcall（每条 indirect 记 drawCount 次）。
//   - 模型实例：由 SceneGeometryPass 在实际提交实例数据时手动累计（剔除后）。
// 读取端：编辑器控制面板每个 UI 帧调用 TakeSnapshot()（读走并清零），
// 读数 = 上一次读取以来的渲染统计（正常节奏下即一帧的量）。
#include "Platform/Export.h"

#include <atomic>
#include <cstdint>

namespace Rendering {

struct MIKAN_API RenderStatsSnapshot {
    uint64_t drawCalls = 0;
    uint64_t triangles = 0;
    uint64_t modelInstances = 0;
    uint64_t gpuInstances = 0;    // Σ 每次绘制的 instanceCount（GPU 实例单元，含逐子网格重复）
    uint64_t modelKinds = 0;      // 去重后的网格模型种数（按提交的模型组计）
    uint64_t indirectDraws = 0;   // 走 vkCmdDrawIndexedIndirect 的绘制条数
};

class MIKAN_API RenderStats {
public:
    static RenderStats& Get();

    // 以下三个由底部宏包装的绘制调用自动累计，不要手动调用。
    void OnDrawIndexed(uint64_t indexCount, uint64_t instanceCount) {
        m_drawCalls.fetch_add(1, std::memory_order_relaxed);
        m_triangles.fetch_add(indexCount * instanceCount / 3, std::memory_order_relaxed);
        m_gpuInstances.fetch_add(instanceCount, std::memory_order_relaxed);
    }
    void OnDraw(uint64_t vertexCount, uint64_t instanceCount) {
        m_drawCalls.fetch_add(1, std::memory_order_relaxed);
        m_triangles.fetch_add(vertexCount * instanceCount / 3, std::memory_order_relaxed);
        m_gpuInstances.fetch_add(instanceCount, std::memory_order_relaxed);
    }
    void OnDrawIndirect(uint64_t drawCount) {
        m_drawCalls.fetch_add(drawCount, std::memory_order_relaxed);
        m_indirectDraws.fetch_add(drawCount, std::memory_order_relaxed);
    }

    // 剔除后实际提交的模型实例数与模型组数（SceneGeometryPass 两个绘制路径各累计一次）
    void AddModelInstances(uint64_t n) { m_modelInstances.fetch_add(n, std::memory_order_relaxed); }
    void AddModelKinds(uint64_t n) { m_modelKinds.fetch_add(n, std::memory_order_relaxed); }

    // 读走当前累计并清零（编辑器 UI 每帧调用）
    RenderStatsSnapshot TakeSnapshot();

private:
    RenderStats() = default;
    std::atomic<uint64_t> m_drawCalls{0};
    std::atomic<uint64_t> m_triangles{0};
    std::atomic<uint64_t> m_modelInstances{0};
    std::atomic<uint64_t> m_gpuInstances{0};
    std::atomic<uint64_t> m_modelKinds{0};
    std::atomic<uint64_t> m_indirectDraws{0};
};

} // namespace Rendering

// ---- 绘制命令计数包装 ----
// 括号包住 (vkCmdDrawIndexed) 等真函数名可防止宏在展开体内再次展开。
#define vkCmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance) \
    (Rendering::RenderStats::Get().OnDrawIndexed(                                                            \
         static_cast<uint64_t>(indexCount), static_cast<uint64_t>(instanceCount)),                           \
     (vkCmdDrawIndexed)((commandBuffer), (indexCount), (instanceCount),                                      \
                        (firstIndex), (vertexOffset), (firstInstance)))

#define vkCmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance)                     \
    (Rendering::RenderStats::Get().OnDraw(                                                                   \
         static_cast<uint64_t>(vertexCount), static_cast<uint64_t>(instanceCount)),                          \
     (vkCmdDraw)((commandBuffer), (vertexCount), (instanceCount), (firstVertex), (firstInstance)))

#define vkCmdDrawIndexedIndirect(commandBuffer, buffer, offset, drawCount, stride)                           \
    (Rendering::RenderStats::Get().OnDrawIndirect(static_cast<uint64_t>(drawCount)),                         \
     (vkCmdDrawIndexedIndirect)((commandBuffer), (buffer), (offset), (drawCount), (stride)))
