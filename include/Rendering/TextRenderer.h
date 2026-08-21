#pragma once
// TextRenderer.h - 文本渲染辅助（基于 FontAtlas + Renderer2D）
// 设计：将 UTF-8 字符串展开为字形四边形，直接调用 Renderer2D::DrawQuad 合批。
// 支持多字号：FontAtlas 按字号维护独立图集 slot，DrawString 按 fontSize 选择。
// 调用者需在 Renderer2D::BeginFrame/Flush 之间使用。
// ===== 坐标约定 =====
// x, y 为画布坐标(左下原点, y 向下, 顶部=0; 屏幕上方 = y 较小), 与 Renderer2D/Canvas2D 一致。

#include "Platform/Export.h"
#include <glm/glm.hpp>
#include <string>

class MIKAN_API TextRenderer {
public:
    static TextRenderer& GetInstance();

    // ===== 初始化（需在 Renderer2D::Init 之后调用） =====
    // ttfPath: TTF 字体文件路径; defaultFontSize: 默认字号（像素高度）
    bool Init(const std::string& ttfPath, float defaultFontSize);
    void Cleanup();
    bool IsReady() const;

    // ===== 文本渲染 =====
    // 返回：文本像素宽度（供对齐计算）
    // x, y: 左下角起始坐标（y 为 baseline 位置）
    // fontSize: 字号（像素高度；新字号首次使用时惰性创建图集 slot 并烘焙 ASCII）
    // color: 文本颜色（仅 RGB 生效；alpha 取字形图集）
    // layer: Renderer2D 渲染层
    float DrawString(const std::string& text, float x, float y, float fontSize,
                     const glm::vec4& color = glm::vec4(1.0f),
                     int layer = 0);

    // ===== SDF 文本渲染（标准距离场，任意字号锐利） =====
    // 与 DrawString 同参；内部用 SDF 图集（FontAtlas::InitSdf 需已调用），
    // 字形按 (fontSize / 基础字号) 缩放，走 SHADER_MODE_SDF 管线
    float DrawStringSdf(const std::string& text, float x, float y, float fontSize,
                        const glm::vec4& color = glm::vec4(1.0f),
                        int layer = 0);

    // ===== MSDF 文本渲染（多通道距离场，角点更锐利） =====
    // 同 DrawStringSdf，但用 MSDF 图集（FontAtlas::InitMsdf 需已调用）；
    // shader 用 median(r,g,b) 重建距离，同走 SHADER_MODE_SDF 管线
    float DrawStringMsdf(const std::string& text, float x, float y, float fontSize,
                         const glm::vec4& color = glm::vec4(1.0f),
                         int layer = 0);

    // 便捷：计算字符串宽度（不绘制）
    float MeasureString(const std::string& text, float fontSize);

    // ===== 字体度量 =====
    float GetFontSize() const;          // 默认字号
    float GetAscent(float fontSize) const;
    float GetDescent(float fontSize) const;
    float GetLineHeight(float fontSize) const;      // 位图字号行高
    float GetSdfLineHeight(float fontSize) const;   // SDF 目标字号行高（基础字号行高 × 缩放）

private:
    TextRenderer() = default;
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    bool m_Ready = false;
    float m_DefaultFontSize = 24.0f;
};
