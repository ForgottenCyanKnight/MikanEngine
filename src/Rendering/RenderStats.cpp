// RenderStats.cpp - 渲染统计单例实现（计数逻辑都在头文件内联，这里只出符号）
#include "Rendering/RenderStats.h"

namespace Rendering {

RenderStats& RenderStats::Get() {
    static RenderStats instance;
    return instance;
}

RenderStatsSnapshot RenderStats::TakeSnapshot() {
    RenderStatsSnapshot s;
    s.drawCalls = m_drawCalls.exchange(0, std::memory_order_relaxed);
    s.triangles = m_triangles.exchange(0, std::memory_order_relaxed);
    s.modelInstances = m_modelInstances.exchange(0, std::memory_order_relaxed);
    s.gpuInstances = m_gpuInstances.exchange(0, std::memory_order_relaxed);
    s.modelKinds = m_modelKinds.exchange(0, std::memory_order_relaxed);
    s.indirectDraws = m_indirectDraws.exchange(0, std::memory_order_relaxed);
    return s;
}

} // namespace Rendering
