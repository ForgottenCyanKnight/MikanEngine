// TextRenderer.cpp - 文本渲染实现（多字号 + SDF）
#include "Rendering/TextRenderer.h"
#include "Rendering/FontAtlas.h"
#include "Rendering/Renderer2D.h"
#include <cstdio>

namespace {
    // 返回 *p 位置的 UTF-8 码点并推进 p；无效字节返回 0xFFFD
    uint32_t DecodeUtf8(const char*& p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) { ++p; return c; }
        else if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            uint32_t cp = ((c & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F);
            p += 2; return cp;
        }
        else if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            uint32_t cp = ((c & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) | ((unsigned char)p[2] & 0x3F);
            p += 3; return cp;
        }
        else if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            uint32_t cp = ((c & 0x07) << 18) | (((unsigned char)p[1] & 0x3F) << 12) | (((unsigned char)p[2] & 0x3F) << 6) | ((unsigned char)p[3] & 0x3F);
            p += 4; return cp;
        }
        ++p;
        return 0xFFFD;
    }
}

TextRenderer& TextRenderer::GetInstance() {
    static TextRenderer instance;
    return instance;
}

bool TextRenderer::Init(const std::string& ttfPath, float defaultFontSize) {
    if (m_Ready) return true;

    m_DefaultFontSize = defaultFontSize;
    if (!FontAtlas::GetInstance().Init(ttfPath, defaultFontSize)) {
        fprintf(stderr, "[TextRenderer] FontAtlas init failed\n");
        return false;
    }

    // 自动初始化 SDF 图集（基础字号 48px；失败不影响位图渲染，SDF 文本不可用）
    if (!FontAtlas::GetInstance().InitSdf(48.0f)) {
        fprintf(stderr, "[TextRenderer] WARNING: SDF atlas init failed, SDF text unavailable\n");
    }
    // 自动初始化 MSDF 图集（基础字号 48px；失败不影响位图/SDF 渲染）
    if (!FontAtlas::GetInstance().InitMsdf(48.0f)) {
        fprintf(stderr, "[TextRenderer] WARNING: MSDF atlas init failed, MSDF text unavailable\n");
    }

    m_Ready = true;
    fprintf(stderr, "[TextRenderer] Ready: %s @ %.1fpx\n", ttfPath.c_str(), defaultFontSize);
    return true;
}

void TextRenderer::Cleanup() {
    m_Ready = false;
    FontAtlas::GetInstance().Cleanup();
}

bool TextRenderer::IsReady() const {
    return m_Ready && FontAtlas::GetInstance().IsInitialized();
}

float TextRenderer::DrawString(const std::string& text, float x, float y, float fontSize,
                                const glm::vec4& color, int layer) {
    if (!IsReady()) return 0.0f;

    FontAtlas& atlas = FontAtlas::GetInstance();
    VkDescriptorSet atlasDesc = atlas.GetAtlasDescriptor(fontSize);
    if (atlasDesc == VK_NULL_HANDLE) return 0.0f;

    float startX = x;

    const char* p = text.c_str();
    while (*p) {
        // UTF-8 解码
        uint32_t cp;
        {
            unsigned char c = (unsigned char)*p;
            if (c < 0x80) {
                cp = c;
                ++p;
            } else if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
                cp = ((c & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F);
                p += 2;
            } else if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
                cp = ((c & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) | ((unsigned char)p[2] & 0x3F);
                p += 3;
            } else if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
                cp = ((c & 0x07) << 18) | (((unsigned char)p[1] & 0x3F) << 12) | (((unsigned char)p[2] & 0x3F) << 6) | ((unsigned char)p[3] & 0x3F);
                p += 4;
            } else {
                cp = 0xFFFD;  // 替换字符
                ++p;
            }
        }

        // 换行处理
        if (cp == '\n') {
            y -= atlas.GetLineHeight(fontSize);
            x = startX;
            continue;
        }
        // 回车忽略
        if (cp == '\r') continue;

        // 查找字形（按字号，未命中惰性烘焙）
        const FontAtlas::GlyphInfo& glyph = atlas.GetGlyph(cp, fontSize);
        if (!glyph.valid || glyph.size.x <= 0.0f || glyph.size.y <= 0.0f) {
            // 无可见位图的字符（空格等）：仅推进光标
            x += glyph.advance;
            continue;
        }

        // 构建四边形（baseline 原点为 x, y）
        float x0 = x + glyph.bearing.x;
        float y0 = y + glyph.bearing.y;                      // baseline + bearing.y（通常为负）
        float x1 = x0 + glyph.size.x;
        float y1 = y0 + glyph.size.y;

        Quad2D quad;
        quad.p0 = glm::vec2(x0, y0);   // 左下
        quad.p1 = glm::vec2(x1, y0);   // 右下
        quad.p2 = glm::vec2(x1, y1);   // 右上
        quad.p3 = glm::vec2(x0, y1);   // 左上
        quad.uv0 = glyph.uv0;
        quad.uv1 = glyph.uv1;
        quad.color = color;
        quad.texture = atlasDesc;
        quad.layer = layer;

        Renderer2D::GetInstance().DrawQuad(quad);

        x += glyph.advance;
    }

    return x - startX;
}

float TextRenderer::DrawStringSdf(const std::string& text, float x, float y, float fontSize,
                                   const glm::vec4& color, int layer) {
    if (!IsReady()) return 0.0f;

    FontAtlas& atlas = FontAtlas::GetInstance();
    if (!atlas.IsSdfReady()) {
        fprintf(stderr, "[TextRenderer] DrawStringSdf: SDF atlas not initialized\n");
        return 0.0f;
    }
    VkDescriptorSet atlasDesc = atlas.GetSdfDescriptor();
    if (atlasDesc == VK_NULL_HANDLE) return 0.0f;

    // SDF 图集按基础字号烘焙，渲染时按目标字号缩放（字形几何乘 scale）
    const float baseSize = atlas.GetSdfBaseFontSize();
    const float scale = (baseSize > 0.0f) ? (fontSize / baseSize) : 1.0f;
    // 过渡带屏幕像素：stb SDF 值 = 128 + d*48 → 边缘到满值 128/48 ≈ 2.67px（半宽），按字号缩放
    const float screenPxRange = 2.6667f * scale;

    float startX = x;
    const char* p = text.c_str();
    while (*p) {
        uint32_t cp = DecodeUtf8(p);

        // 换行处理
        if (cp == '\n') {
            y -= atlas.GetSdfLineHeight() * scale;
            x = startX;
            continue;
        }
        if (cp == '\r') continue;

        // SDF 字形（基础字号单位度量）
        const FontAtlas::GlyphInfo& glyph = atlas.GetGlyphSdf(cp);
        if (!glyph.valid || glyph.size.x <= 0.0f || glyph.size.y <= 0.0f) {
            x += glyph.advance * scale;
            continue;
        }

        float x0 = x + glyph.bearing.x * scale;
        float y0 = y + glyph.bearing.y * scale;
        float x1 = x0 + glyph.size.x * scale;
        float y1 = y0 + glyph.size.y * scale;

        Quad2D quad;
        quad.p0 = glm::vec2(x0, y0);   // 左下
        quad.p1 = glm::vec2(x1, y0);   // 右下
        quad.p2 = glm::vec2(x1, y1);   // 右上
        quad.p3 = glm::vec2(x0, y1);   // 左上
        quad.uv0 = glyph.uv0;
        quad.uv1 = glyph.uv1;
        quad.color = color;
        quad.texture = atlasDesc;
        quad.layer = layer;
        quad.shaderMode = SHADER_MODE_SDF;   // 关键：走 SDF 管线（ui2d_sdf.frag）
        quad.screenPxRange = screenPxRange;

        Renderer2D::GetInstance().DrawQuad(quad);
        x += glyph.advance * scale;
    }
    return x - startX;
}

float TextRenderer::DrawStringMsdf(const std::string& text, float x, float y, float fontSize,
                                    const glm::vec4& color, int layer) {
    if (!IsReady()) return 0.0f;

    FontAtlas& atlas = FontAtlas::GetInstance();
    if (!atlas.IsMsdfReady()) {
        fprintf(stderr, "[TextRenderer] DrawStringMsdf: MSDF atlas not initialized\n");
        return 0.0f;
    }
    VkDescriptorSet atlasDesc = atlas.GetMsdfDescriptor();
    if (atlasDesc == VK_NULL_HANDLE) return 0.0f;

    // MSDF 图集按基础字号烘焙，渲染时按目标字号缩放（字形几何乘 scale）
    const float baseSize = atlas.GetMsdfBaseFontSize();
    const float scale = (baseSize > 0.0f) ? (fontSize / baseSize) : 1.0f;
    // 过渡带屏幕像素：MSDF Range=2px（半宽），按字号缩放
    const float screenPxRange = 2.0f * scale;

    float startX = x;
    const char* p = text.c_str();
    while (*p) {
        uint32_t cp = DecodeUtf8(p);

        if (cp == '\n') {
            y -= atlas.GetMsdfLineHeight() * scale;
            x = startX;
            continue;
        }
        if (cp == '\r') continue;

        const FontAtlas::GlyphInfo& glyph = atlas.GetGlyphMsdf(cp);
        if (!glyph.valid || glyph.size.x <= 0.0f || glyph.size.y <= 0.0f) {
            x += glyph.advance * scale;
            continue;
        }

        float x0 = x + glyph.bearing.x * scale;
        float y0 = y + glyph.bearing.y * scale;
        float x1 = x0 + glyph.size.x * scale;
        float y1 = y0 + glyph.size.y * scale;

        Quad2D quad;
        quad.p0 = glm::vec2(x0, y0);
        quad.p1 = glm::vec2(x1, y0);
        quad.p2 = glm::vec2(x1, y1);
        quad.p3 = glm::vec2(x0, y1);
        quad.uv0 = glyph.uv0;
        quad.uv1 = glyph.uv1;
        quad.color = color;
        quad.texture = atlasDesc;
        quad.layer = layer;
        quad.shaderMode = SHADER_MODE_SDF;   // 复用 SDF 管线（shader 已改 median 兼容）
        quad.screenPxRange = screenPxRange;

        Renderer2D::GetInstance().DrawQuad(quad);
        x += glyph.advance * scale;
    }
    return x - startX;
}

float TextRenderer::MeasureString(const std::string& text, float fontSize) {
    if (!IsReady()) return 0.0f;

    FontAtlas& atlas = FontAtlas::GetInstance();
    float width = 0.0f;
    float maxWidth = 0.0f;

    const char* p = text.c_str();
    while (*p) {
        uint32_t cp;
        {
            unsigned char c = (unsigned char)*p;
            if (c < 0x80) { cp = c; ++p; }
            else if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { cp = ((c & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F); p += 2; }
            else if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) { cp = ((c & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) | ((unsigned char)p[2] & 0x3F); p += 3; }
            else if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) { cp = ((c & 0x07) << 18) | (((unsigned char)p[1] & 0x3F) << 12) | (((unsigned char)p[2] & 0x3F) << 6) | ((unsigned char)p[3] & 0x3F); p += 4; }
            else { cp = 0xFFFD; ++p; }
        }

        if (cp == '\n') {
            if (width > maxWidth) maxWidth = width;
            width = 0.0f;
            continue;
        }
        if (cp == '\r') continue;

        const FontAtlas::GlyphInfo& glyph = atlas.GetGlyph(cp, fontSize);
        width += glyph.advance;
    }
    return (width > maxWidth) ? width : maxWidth;
}

float TextRenderer::GetFontSize() const {
    return m_DefaultFontSize;
}

float TextRenderer::GetAscent(float fontSize) const {
    return FontAtlas::GetInstance().GetAscent(fontSize);
}

float TextRenderer::GetDescent(float fontSize) const {
    return FontAtlas::GetInstance().GetDescent(fontSize);
}

float TextRenderer::GetLineHeight(float fontSize) const {
    return FontAtlas::GetInstance().GetLineHeight(fontSize);
}

float TextRenderer::GetSdfLineHeight(float fontSize) const {
    FontAtlas& atlas = FontAtlas::GetInstance();
    const float base = atlas.GetSdfBaseFontSize();
    if (base <= 0.0f) return 0.0f;
    return atlas.GetSdfLineHeight() * (fontSize / base);
}
