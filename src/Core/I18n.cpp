#include "Core/I18n.h"
#include "Core/ProjectManager.h"
#include "Core/Log.h"

#include "json.hpp"

#include <algorithm>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

// 引擎资产根的 i18n 目录（桌面端可写；Android 走 APK 只读资产，设置持久化暂缓）。
std::string I18nPath(const char* name) {
    return ProjectManager::GetInstance().GetEngineAssetPath(
        (std::string("i18n/") + name).c_str());
}

} // namespace

I18n& I18n::GetInstance() {
    static I18n instance;
    return instance;
}

std::string I18n::I18nDir() const {
    std::string p = ProjectManager::GetInstance().GetEngineAssetPath("i18n");
    // GetEngineAssetPath 可能带尾分隔符，统一去掉供拼接
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    return p;
}

void I18n::EnsureLoaded() {
    if (m_SettingLoaded) return;
    m_SettingLoaded = true;

    // 读当前语言设置
    std::ifstream settingsIn(I18nPath("language.json"));
    if (settingsIn.is_open()) {
        try {
            nlohmann::json j;
            settingsIn >> j;
            if (j.contains("language") && j.at("language").is_string()) {
                m_Language = j.at("language").get<std::string>();
            }
        } catch (const std::exception& ex) {
            LOGW("[I18n] language.json 解析失败，使用默认语言: %s", ex.what());
        }
    }

    LoadLanguagePack(m_Language);
}

void I18n::SaveSettings() {
    std::ofstream out(I18nPath("language.json"));
    if (!out.is_open()) {
        LOGW("[I18n] 无法写入 language.json（路径 %s）", I18nPath("language.json").c_str());
        return;
    }
    nlohmann::json j;
    j["language"] = m_Language;
    out << j.dump(2) << std::endl;
}

bool I18n::LoadLanguagePack(const std::string& language) {
    m_Language = language;
    m_Table.clear();

    // 中文是 key 本身，恒等返回
    if (language == "zh-CN" || language.empty()) {
        LOGI("[I18n] 语言: zh-CN（默认）");
        return true;
    }

    const std::string path = I18nPath((language + ".json").c_str());
    std::ifstream in(path);
    if (!in.is_open()) {
        LOGW("[I18n] 语言包不存在: %s，回退中文", path.c_str());
        m_Language = "zh-CN";
        return false;
    }

    try {
        nlohmann::json j;
        in >> j;
        if (!j.is_object()) throw std::runtime_error("language pack must be a JSON object");
        m_Table.reserve(j.size());
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string()) {
                m_Table.emplace_back(it.key(), it.value().get<std::string>());
            }
        }
        LOGI("[I18n] 语言: %s（%zu 条映射）", language.c_str(), m_Table.size());
        return true;
    } catch (const std::exception& ex) {
        LOGE("[I18n] 语言包解析失败 %s: %s，回退中文", path.c_str(), ex.what());
        m_Table.clear();
        m_Language = "zh-CN";
        return false;
    }
}

const char* I18n::Tr(const char* zh) const {
    if (zh == nullptr || m_Table.empty()) return zh;
    for (const auto& entry : m_Table) {
        if (entry.first == zh) return entry.second.c_str();
    }
    return zh; // 缺 key：原样返回中文
}

void I18n::SetLanguage(const std::string& language) {
    EnsureLoaded();
    if (language == m_Language) return;
    LoadLanguagePack(language);
    SaveSettings();
}

const std::vector<std::string>& I18n::GetAvailableLanguages() {
    EnsureLoaded();
    if (m_LanguagesScanned) return m_AvailableLanguages;
    m_LanguagesScanned = true;

    m_AvailableLanguages.clear();
    m_AvailableLanguages.push_back("zh-CN");

    std::error_code ec;
    const std::string dir = I18nDir();
    if (fs::exists(dir, ec) && fs::is_directory(dir, ec)) {
        std::vector<std::string> found;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();
            if (name == "language.json") continue;
            if (name.size() > 5 && name.substr(name.size() - 5) == ".json") {
                name = name.substr(0, name.size() - 5);
                if (name != "zh-CN") found.push_back(name);
            }
        }
        std::sort(found.begin(), found.end());
        m_AvailableLanguages.insert(m_AvailableLanguages.end(), found.begin(), found.end());
    }
    return m_AvailableLanguages;
}

std::string I18n::LanguageDisplayName(const std::string& language) {
    if (language == "zh-CN") return "中文 (简体)";
    if (language == "en-US") return "English";
    return language;
}

std::string I18n::WindowTitle(const char* zh, const char* stableId) {
    GetInstance().EnsureLoaded();
    return std::string(GetInstance().Tr(zh)) + "###" + stableId;
}
