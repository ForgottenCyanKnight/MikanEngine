#pragma once
#include "Platform/Export.h"

#include <map>
#include <string>

class PostProcessChain;

// ============================================================================
// PipelineSettings — 后处理模块开关的设置层（2026-09-26）
//
// 链 JSON 描述拓扑（资产），本设置描述用户偏好（模块开/关、AA 算法选择），
// 存 engine/pipeline_settings.json：
//   { "groups": { "bloom": true, "taa": true }, "aa": "smaa" }
//
// 应用时机：链构建后（RebuildSelectedPostProcessChain / LoadAndBuild 后）由
// VulkanPostProcessChains 调 ApplyToChain——groups 逐组 SetGroupEnabled、
// aa 走 SetAaProfile；缺省的组不碰（保持 JSON enable 默认）。
// 控制面板的开关改动 → SetGroup/SetAaProfile（保存）+ 对三条链即时应用
// + g_PostProcessRebuildRequested 请求下一安全帧重建。
// ============================================================================
class MIKAN_API PipelineSettings {
public:
    static PipelineSettings& GetInstance();

    void EnsureLoaded();
    void Save() const;

    void SetGroup(const std::string& group, bool enabled);
    const std::map<std::string, bool>& GetGroups() const { return m_Groups; }
    void SetAaProfile(const std::string& profile);
    const std::string& GetAaProfile() const { return m_AaProfile; }

    // 把保存的偏好应用到一个链；有实际改动返回 true。
    bool ApplyToChain(PostProcessChain& chain) const;

private:
    PipelineSettings() = default;
    std::map<std::string, bool> m_Groups;
    std::string m_AaProfile;
    bool m_Loaded = false;
};
