#include "Core/PipelineSettings.h"
#include "Core/ProjectManager.h"
#include "Core/Log.h"
#include "Rendering/PostProcessChain.h"

#include "json.hpp"

#include <algorithm>
#include <fstream>
#include <filesystem>

// ============================================================================
// PipelineSettings — 后处理模块开关的设置层（2026-09-26）
//
// 把"哪些效果开着、用哪种抗锯齿"从链 JSON 里分离出来：
//   - 链 JSON 仍描述拓扑（shader/格式/接线/特殊输入源），是资产；
//   - 本设置是用户偏好：{"groups": {"bloom": true, ...}, "aa": "smaa"}，
//     存 engine/pipeline_settings.json，链构建后应用。
// 应用语义：groups 缺省的组不碰（保持 JSON enable 默认）；"aa" 缺省不碰。
// ============================================================================

namespace {

std::string SettingsPath() {
    // 与 engine_settings.json 同目录（引擎根，桌面可写；Android 走默认值）
    return ProjectManager::GetInstance().GetEngineAssetPath("pipeline_settings.json");
}

} // namespace

PipelineSettings& PipelineSettings::GetInstance() {
    static PipelineSettings instance;
    return instance;
}

void PipelineSettings::EnsureLoaded() {
    if (m_Loaded) return;
    m_Loaded = true;
    const std::string path = SettingsPath();
    std::ifstream in(path);
    if (!in.is_open()) return;
    try {
        nlohmann::json j;
        in >> j;
        if (j.contains("groups") && j.at("groups").is_object()) {
            for (auto it = j.at("groups").begin(); it != j.at("groups").end(); ++it) {
                if (it.value().is_boolean()) m_Groups[it.key()] = it.value().get<bool>();
            }
        }
        if (j.contains("aa") && j.at("aa").is_string()) {
            m_AaProfile = j.at("aa").get<std::string>();
        }
        LOGI("[PipelineSettings] loaded: %d groups, aa='%s'",
             (int)m_Groups.size(), m_AaProfile.c_str());
    } catch (const std::exception& ex) {
        LOGW("[PipelineSettings] 解析失败（%s），使用默认: %s", path.c_str(), ex.what());
    }
}

void PipelineSettings::Save() const {
    const std::string path = SettingsPath();
    std::ofstream out(path);
    if (!out.is_open()) {
        LOGW("[PipelineSettings] 无法写入 %s", path.c_str());
        return;
    }
    nlohmann::json j;
    j["groups"] = m_Groups;
    if (!m_AaProfile.empty()) j["aa"] = m_AaProfile;
    out << j.dump(2) << std::endl;
}

void PipelineSettings::SetGroup(const std::string& group, bool enabled) {
    EnsureLoaded();
    m_Groups[group] = enabled;
    Save();
}

void PipelineSettings::SetAaProfile(const std::string& profile) {
    EnsureLoaded();
    m_AaProfile = profile;
    Save();
}

bool PipelineSettings::ApplyToChain(PostProcessChain& chain) const {
    const_cast<PipelineSettings*>(this)->EnsureLoaded();
    bool changed = false;
    for (const auto& [group, enabled] : m_Groups) {
        changed |= chain.SetGroupEnabled(group, enabled);
    }
    if (!m_AaProfile.empty()) {
        changed |= chain.SetAaProfile(m_AaProfile);
    }
    return changed;
}
