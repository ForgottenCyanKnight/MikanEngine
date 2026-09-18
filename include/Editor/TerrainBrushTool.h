#pragma once

#include "ECS/Types.h"

#include <glm/glm.hpp>

namespace Editor {

// 水位笔刷的满刻度水深（米），与 TerrainRenderer.h 的 kTerrainWaterMaxDepth
// 同值同语义（两侧无公共头，修改时必须同步）。
inline constexpr float kTerrainWaterMaxDepthMeters = 8.0f;

// 地形笔刷模式。顺序与属性面板下拉一致。
enum class TerrainBrushMode {
    Raise = 0,
    Lower = 1,
    // 材质涂抹：往控制图（图层权重图）写权重，改变地表用哪一层材质。
    Material = 2,
    // 草地散布：往草密度图（R8）写密度，下一帧按密度重建实例化草叶。
    Grass = 3,
    // 水位涂抹：往水位图（R8）写水深，地形着色器按水位图出不透明水面 mask。
    Water = 4,
};

// 编辑器侧地形笔刷状态。
//
// 刻意不塞进 ECS::TerrainComponent：编辑会话状态（正在编辑哪个地形、笔刷多大）
// 既不需要序列化，也不需要跨 DLL 共享；加进组件会改变已有结构体布局，
// 按本项目的 ABI 约定会迫使全量重编。这里用编辑器单例持有，
// 属性面板写入、场景视图读取，两侧共享同一份状态。
class TerrainBrushTool {
public:
    static TerrainBrushTool& GetInstance() {
        static TerrainBrushTool instance;
        return instance;
    }

    // 当前处于编辑模式的地形实体；INVALID_ENTITY 表示没有地形在编辑。
    ECS::Entity GetEditingEntity() const { return m_editingEntity; }
    void SetEditingEntity(ECS::Entity entity) { m_editingEntity = entity; }
    bool IsEditing(ECS::Entity entity) const {
        return entity != ECS::INVALID_ENTITY && m_editingEntity == entity;
    }
    void StopEditing() { m_editingEntity = ECS::INVALID_ENTITY; }
    // 实体被删除后清理悬空引用，避免属性面板/场景视图继续引用失效实体。
    void ClearEditingIfNotAlive(ECS::Entity aliveCheck) {
        if (m_editingEntity != aliveCheck) {
            m_editingEntity = ECS::INVALID_ENTITY;
        }
    }

    TerrainBrushMode GetMode() const { return m_mode; }
    void SetMode(TerrainBrushMode mode) { m_mode = mode; }
    bool IsMaterialMode() const { return m_mode == TerrainBrushMode::Material; }
    bool IsGrassMode() const { return m_mode == TerrainBrushMode::Grass; }
    bool IsWaterMode() const { return m_mode == TerrainBrushMode::Water; }
    const char* GetModeName() const {
        switch (m_mode) {
        case TerrainBrushMode::Raise: return "升高地形";
        case TerrainBrushMode::Lower: return "降低地形";
        case TerrainBrushMode::Grass: return "草地散布";
        case TerrainBrushMode::Water: return "水位涂抹";
        default: return "材质涂抹";
        }
    }

    float GetRadius() const { return m_radius; }
    void SetRadius(float radius) { m_radius = glm::clamp(radius, 0.25f, 4096.0f); }
    // 滚轮步进：小半径时步长小，便于精修；大半径时按比例放大，避免刷到手酸。
    void AddRadiusStep(int steps) {
        const float step = glm::max(0.5f, m_radius * 0.15f);
        SetRadius(m_radius + step * static_cast<float>(steps));
    }

    // 强度 = 满权重处每秒改变的归一化高度（占 heightScale 的比例）。
    float GetStrength() const { return m_strength; }
    void SetStrength(float strength) { m_strength = glm::clamp(strength, 0.01f, 8.0f); }
    void AddStrengthStep(int steps) {
        SetStrength(m_strength + 0.1f * static_cast<float>(steps));
    }

    // 本帧写进高度图的归一化增量。按帧时长缩放，保证不同帧率下手感一致；
    // 正为升高、负为降低。
    float ComputeNormalizedDelta(float deltaTime) const {
        const float scaled = m_strength * glm::clamp(deltaTime, 0.0f, 0.1f);
        return m_mode == TerrainBrushMode::Raise ? scaled : -scaled;
    }

    // ===== 材质涂抹（往控制图的图层权重上写）=====
    // 目标图层 0..3，对应 TerrainComponent 的 layer0Path..layer3Path。
    int GetMaterialLayer() const { return m_materialLayer; }
    void SetMaterialLayer(int layer) { m_materialLayer = glm::clamp(layer, 0, 3); }
    // 滚轮/按钮步进按 4 取模循环，越过 3 回到 0，省得还要反向点。
    void AddMaterialLayerStep(int steps) {
        const int count = 4;
        m_materialLayer = ((m_materialLayer + steps) % count + count) % count;
    }

    // 材质模式下"笔刷强度"就是过渡边界的软硬：
    //   1 = 硬边：平顶核心几乎占满半径，只有外缘极窄一条带里做渐变；
    //   0 = 最软：从笔刷中心到边缘全程渐变。
    // 与高度模式的强度分开存：共用一份的话，在材质里调软了边界，切回升高/降低
    // 会连带把雕刻速度也一起改掉。
    float GetMaterialHardness() const { return m_materialHardness; }
    void SetMaterialHardness(float hardness) {
        m_materialHardness = glm::clamp(hardness, 0.0f, 1.0f);
    }
    void AddMaterialHardnessStep(int steps) {
        SetMaterialHardness(m_materialHardness + 0.08f * static_cast<float>(steps));
    }

    // 本次调用写进控制图的混合量。按帧时长缩放，速率固定：
    // 涂多浓由"按住多久"决定，软硬只由上面的硬度决定，两根旋钮不互相干扰。
    float ComputeMaterialAmount(float deltaTime) const {
        return glm::clamp(glm::clamp(deltaTime, 0.0f, 0.1f) * 10.0f, 0.0f, 1.0f);
    }

    // ===== 草地散布（往草密度图上写）=====
    // 草地模式下"笔刷强度"就是目标草密度（0 = 除草，1 = 最密的草丛），
    // 过渡软硬复用材质硬度（GetMaterialHardness）——语义相同，不必再立一根旋钮。
    float GetGrassDensity() const { return m_grassDensity; }
    void SetGrassDensity(float density) { m_grassDensity = glm::clamp(density, 0.0f, 1.0f); }

    // 与材质涂抹同一混合速率：按住越久越接近目标密度。
    float ComputeGrassAmount(float deltaTime) const {
        return glm::clamp(glm::clamp(deltaTime, 0.0f, 0.1f) * 10.0f, 0.0f, 1.0f);
    }

    // ===== 水位涂抹（往水位图上写）=====
    // 水位模式下"笔刷强度"就是目标水深（米，0 = 抹掉水，满刻度 =
    // kTerrainWaterMaxDepthMeters），过渡软硬复用材质硬度——语义相同。
    // 渲染侧（PaintTerrainWaterWorld）涂水深时会同步把地形挖低同等米数，
    // 水面贴在原地面高度；抹掉水不回填湖底。
    float GetWaterDepthMeters() const { return m_waterDepthMeters; }
    void SetWaterDepthMeters(float meters) {
        m_waterDepthMeters = glm::clamp(meters, 0.0f, kTerrainWaterMaxDepthMeters);
    }
    // 归一化目标深度（传给 PaintTerrainWaterWorld 的 0..1 值）。
    float GetWaterDepthNormalized() const {
        return m_waterDepthMeters / kTerrainWaterMaxDepthMeters;
    }

    // 与材质涂抹同一混合速率：按住越久越接近目标水深。
    float ComputeWaterAmount(float deltaTime) const {
        return glm::clamp(glm::clamp(deltaTime, 0.0f, 0.1f) * 10.0f, 0.0f, 1.0f);
    }

private:
    TerrainBrushTool() = default;

    ECS::Entity m_editingEntity = ECS::INVALID_ENTITY;
    TerrainBrushMode m_mode = TerrainBrushMode::Raise;
    float m_radius = 24.0f;
    float m_strength = 0.6f;
    int m_materialLayer = 1;
    float m_materialHardness = 0.7f;
    float m_grassDensity = 0.8f;
    float m_waterDepthMeters = 2.0f;
};

} // namespace Editor
