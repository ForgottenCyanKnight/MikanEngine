#pragma once
#include "Platform/Export.h"

#include <string>
#include <vector>
#include <utility>

// ============================================================================
// I18n — 引擎多语言支持（2026-09-26，第一阶段：中英映射 + 编辑器局部替换）
//
// 设计：以"中文原文"为 key 查表。存量 UI 的改造成本最低（字符串字面量外面包
// 一层 Tr(...)），缺 key / 中文语言 / 未加载时原样返回原文，绝不产生空串。
//
// 文件布局（engine 资产根的 i18n/ 目录）：
//   i18n/language.json   当前语言设置 {"language": "en-US"}（惰性读写）
//   i18n/en-US.json      语言包：扁平 { "中文原文": "译文" }
//   i18n/zh-CN.json      无需存在——中文是 key 本身，恒等返回
//
// 范围约定（第一阶段）：只翻译菜单项、按钮、弹窗文案、窗口内标签。
// ImGui 窗口标题暂不翻译——它们是 dock 标识符（FindWindowByName / imgui.ini /
// DockBuilderDockWindow 依赖精确匹配），运行时切换会让布局失联，后续阶段统一处理。
// ============================================================================
class MIKAN_API I18n {
public:
    static I18n& GetInstance();

    // 翻译：当前语言包命中返回译文，否则原样返回 zh。返回指针随语言包生命周期
    // 有效（UI 每帧重新调用，不持有）。
    const char* Tr(const char* zh) const;

    // 切换语言（如 "zh-CN" / "en-US"）：加载语言包并持久化设置。下一帧生效。
    void SetLanguage(const std::string& language);
    const std::string& GetLanguage() const { return m_Language; }

    // 可用语言列表（扫描 i18n/ 目录的语言包；恒含 "zh-CN"，中文排最前）。
    const std::vector<std::string>& GetAvailableLanguages();

    // 语言代码 → 显示名（组合框用；zh-CN=中文，其余取语言包同名条目或原码）。
    static std::string LanguageDisplayName(const std::string& language);

    // 窗口标题：显示名走翻译，追加 "###稳定ID" —— ImGui 以 ### 之后的部分作为
    // 窗口/dock/ini 标识，因此显示名可随语言切换而 dock 布局与持久化保持稳定。
    // 所有 Begin / FindWindowByName / DockBuilderDockWindow / ImHashStr 的窗口名
    // 引用都必须经由本函数（或直接使用同一 stableId），禁止再写裸中文字面量。
    static std::string WindowTitle(const char* zh, const char* stableId);

private:
    I18n() = default;
    void EnsureLoaded();
    void SaveSettings();
    bool LoadLanguagePack(const std::string& language);
    std::string I18nDir() const;

    std::string m_Language = "zh-CN";
    bool m_SettingLoaded = false;
    std::vector<std::pair<std::string, std::string>> m_Table; // 原文 → 译文
    std::vector<std::string> m_AvailableLanguages;
    bool m_LanguagesScanned = false;
};

// 便捷入口：Tr("播放")
inline const char* Tr(const char* zh) { return I18n::GetInstance().Tr(zh); }
