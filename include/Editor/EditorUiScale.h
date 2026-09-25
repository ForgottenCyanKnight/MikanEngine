#pragma once

// ============================================================================
// EditorUiScale — 编辑器 UI 统一缩放模型（2026-09-25）
//
// 旧模型的问题（跨设备显示不一致的根因）：字体大小按"屏幕对角线像素 + DPI
// clamp"算出 fontScale（内含一次 DPI），再被 style.FontScaleDpi 乘第二次 DPI，
// 有效字号近似随系统缩放平方增长；而控件间距用 fontScale/13 缩放（不同倍率），
// 资源窗口还有 cellSize=100 之类与缩放完全脱节的硬编码像素。
//
// 新模型：字体按固定基准字号 kBaseFontSize 加载（FontSizeBase），设备缩放全部
// 由 style.FontScaleDpi 承担（ImGui 1.92 动态字体按需重栅格化）；style 间距用
// 同一倍率 ScaleAllSizes 缩放；DPI 变化（窗口跨显示器/系统缩放调整）由
// EditorManager::UpdateUiScale 每帧从基准样式整体重建。所有需要"随 UI 缩放的
// 像素值"都通过 GetUiScale() 换算，禁止再写死像素。
// ============================================================================
namespace EditorUi {

// 基准字号（FontSizeBase，逻辑 100% 缩放下的像素）。= 13px × 1.12 编辑器放大系数。
inline constexpr float kBaseFontSize = 13.0f * 1.12f;

// 当前 UI 缩放（1.0 = 系统 100%）。InitImGui 之前返回 1.0。
float GetUiScale();

} // namespace EditorUi
