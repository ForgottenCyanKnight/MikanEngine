// FontAtlas.cpp - 多字号运行时字体图集实现（位图 + SDF + MSDF）
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"
#include "msdfgen/msdfgen.h"
#include "Rendering/FontAtlas.h"
#include "Rendering/Renderer2D.h"
#include "Core/EngineConfig.h"
#include "Core/VulkanContext.h"
#include "Rendering/RendererBase.h"  // RendererUtils::FindMemoryType
#include <cstring>
#include <cstdio>
#include <cmath>
#include <utility>
#include <algorithm>

// ===== FontAtlas 实现 =====

FontAtlas& FontAtlas::GetInstance() {
    static FontAtlas instance;
    return instance;
}

bool FontAtlas::Init(const std::string& ttfPath, float fontSize,
                     uint32_t atlasWidth, uint32_t atlasHeight) {
    if (m_FontLoaded) {
        // 字体已加载：确保默认字号 slot 存在
        GetOrCreateSlot(fontSize, atlasWidth, atlasHeight);
        return true;
    }

    // 1. 加载 TTF 文件
    if (!LoadFontData(ttfPath)) {
        fprintf(stderr, "[FontAtlas] Failed to load font: %s\n", ttfPath.c_str());
        return false;
    }

    // 2. 初始化 stb_truetype（所有字号共享同一份解析）
    auto* font = new stbtt_fontinfo();
    if (!stbtt_InitFont(font, m_FontData.data(),
                        stbtt_GetFontOffsetForIndex(m_FontData.data(), 0))) {
        fprintf(stderr, "[FontAtlas] Failed to init stb_truetype\n");
        delete font;
        return false;
    }
    m_FontInfo = font;
    m_FontLoaded = true;

    // 3. 预创建默认字号 slot（含 ASCII 32-126 预烘焙）
    AtlasSlot* slot = GetOrCreateSlot(fontSize, atlasWidth, atlasHeight);
    if (!slot) {
        fprintf(stderr, "[FontAtlas] Failed to create default slot\n");
        return false;
    }

    fprintf(stderr, "[FontAtlas] Loaded: default size=%.1fpx, glyphs=%zu\n",
            fontSize, slot->glyphs.size());
    return true;
}

void FontAtlas::Cleanup() {
    m_FontLoaded = false;
    if (m_FontInfo) {
        delete static_cast<stbtt_fontinfo*>(m_FontInfo);
        m_FontInfo = nullptr;
    }
    m_FontData.clear();

    for (auto& [key, slot] : m_Slots) {
        DestroyAtlasResources(slot);
    }
    m_Slots.clear();

    // SDF 图集
    if (m_SdfInited) {
        DestroyAtlasResources(m_SdfSlot);
        m_SdfInited = false;
    }
    // MSDF 图集
    if (m_MsdfInited) {
        DestroyAtlasResources(m_MsdfSlot);
        m_MsdfInited = false;
    }
}

bool FontAtlas::LoadFontData(const std::string& ttfPath) {
    std::string fullPath = EngineConfig::ResolvePlatformPath(ttfPath);
    std::vector<char> raw = RendererUtils::ReadFile(fullPath);
    if (raw.empty()) {
        // 回退：按文件名从引擎资源目录加载
        size_t slash = ttfPath.find_last_of("/\\");
        std::string fname = (slash != std::string::npos) ? ttfPath.substr(slash + 1) : ttfPath;
        std::string altFull = EngineConfig::GetFontPath(fname.c_str());
        raw = RendererUtils::ReadFile(altFull);
    }
    if (raw.empty()) return false;

    m_FontData.assign(raw.begin(), raw.end());
    return true;
}

// ===== slot 管理 =====

const FontAtlas::AtlasSlot* FontAtlas::FindSlot(float fontSize) const {
    int key = (int)std::lround(fontSize);
    auto it = m_Slots.find(key);
    return (it != m_Slots.end()) ? &it->second : nullptr;
}

FontAtlas::AtlasSlot* FontAtlas::GetOrCreateSlot(float fontSize, uint32_t atlasWidth, uint32_t atlasHeight) {
    if (!m_FontLoaded || !m_FontInfo) return nullptr;

    AtlasSlot* existing = const_cast<AtlasSlot*>(FindSlot(fontSize));
    if (existing) return existing;

    int key = (int)std::lround(fontSize);
    AtlasSlot& slot = m_Slots[key];
    // 位图图集尺寸按字号自适应：大字号字形槽大（~字号×字号），1024² 装不下 ASCII 95 个
    // 经验：字号 ≤48px 用 1024²（~3600 槽位），>48px 用 2048²（~4 倍容量）
    uint32_t w = std::min(atlasWidth, 2048u);
    uint32_t h = std::min(atlasHeight, 2048u);
    if (fontSize > 48.0f) {
        w = 2048u;
        h = 2048u;
    }
    slot.atlasWidth = w;
    slot.atlasHeight = h;

    auto* font = static_cast<stbtt_fontinfo*>(m_FontInfo);
    slot.scale = stbtt_ScaleForPixelHeight(font, (float)key);
    stbtt_GetFontVMetrics(font, &slot.ascent, &slot.descent, &slot.lineGap);

    if (!CreateAtlasResources(slot)) {
        fprintf(stderr, "[FontAtlas] Failed to create atlas resources for size %d\n", key);
        DestroyAtlasResources(slot);
        m_Slots.erase(key);
        return nullptr;
    }

    // 预烘焙 ASCII 32-126
    slot.glyphs.reserve(256);
    for (uint32_t cp = 32; cp <= 126; ++cp) {
        BakeGlyph(slot, cp);
    }

    fprintf(stderr, "[FontAtlas] Slot created: size=%dpx atlas=%ux%u glyphs=%zu\n",
            key, slot.atlasWidth, slot.atlasHeight, slot.glyphs.size());
    return &slot;
}

// ===== 字形 =====

const FontAtlas::GlyphInfo& FontAtlas::GetGlyph(uint32_t codepoint, float fontSize) {
    static GlyphInfo s_Invalid;
    s_Invalid = {};

    AtlasSlot* slot = GetOrCreateSlot(fontSize);
    if (!slot) return s_Invalid;

    // 命中：刷新 LRU 时间戳
    auto it = slot->glyphs.find(codepoint);
    if (it != slot->glyphs.end()) {
        it->second.lastUsedFrame = ++slot->frameCounter;
        return it->second.info;
    }
    // 惰性烘焙
    if (!BakeGlyph(*slot, codepoint)) {
        // 烘焙失败 → 回退到 '?'，再回退到空格
        auto fit = slot->glyphs.find((uint32_t)'?');
        if (fit != slot->glyphs.end()) { fit->second.lastUsedFrame = ++slot->frameCounter; return fit->second.info; }
        fit = slot->glyphs.find((uint32_t)' ');
        if (fit != slot->glyphs.end()) { fit->second.lastUsedFrame = ++slot->frameCounter; return fit->second.info; }
        return s_Invalid;
    }
    auto inserted = slot->glyphs.find(codepoint);
    if (inserted != slot->glyphs.end()) {
        inserted->second.lastUsedFrame = ++slot->frameCounter;
        return inserted->second.info;
    }
    return s_Invalid;
}

bool FontAtlas::BakeGlyph(AtlasSlot& slot, uint32_t codepoint) {
    if (slot.isMsdf) return BakeGlyphMsdf(slot, codepoint);
    if (slot.isSdf) return BakeGlyphSdf(slot, codepoint);
    if (!m_FontInfo) return false;
    auto* font = static_cast<stbtt_fontinfo*>(m_FontInfo);

    // 新字形（烘焙即视为使用）
    CachedGlyph cached;
    cached.lastUsedFrame = ++slot.frameCounter;
    cached.pinned = (codepoint >= ASCII_MIN && codepoint <= ASCII_MAX);

    int glyphIndex = stbtt_FindGlyphIndex(font, (int)codepoint);
    if (glyphIndex == 0 && codepoint != 0) {
        // 缺失字形：记录空 glyph 避免重复查找
        slot.glyphs[codepoint] = std::move(cached);
        return false;
    }

    // 获取字形度量
    int advance, lsb;
    stbtt_GetGlyphHMetrics(font, glyphIndex, &advance, &lsb);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(font, glyphIndex, slot.scale, slot.scale, &x0, &y0, &x1, &y1);

    int gw = x1 - x0;
    int gh = y1 - y0;

    if (gw <= 0 || gh <= 0) {
        // 空格等无位图字形：仅记录度量
        cached.info.valid = true;
        cached.info.size = glm::vec2(0.0f, 0.0f);
        cached.info.bearing = glm::vec2((float)x0, (float)y0);
        cached.info.advance = advance * slot.scale;
        slot.glyphs[codepoint] = std::move(cached);
        return true;
    }

    // 在图集中分配位置；图集满 → LRU 驱逐 + 重打包
    uint32_t allocX, allocY;
    if (!AllocShelfSlot(slot, (uint32_t)gw + 1, (uint32_t)gh + 1, allocX, allocY)) {
        if (!EvictAndRepack(slot, (uint32_t)gw + 1, (uint32_t)gh + 1, allocX, allocY)) {
            fprintf(stderr, "[FontAtlas] Glyph %u (%d x %d) too large / not evictable\n",
                    codepoint, gw, gh);
            return false;
        }
    }

    // 栅格化字形（单通道 alpha）
    std::vector<unsigned char> glyphBitmap(gw * gh, 0);
    stbtt_MakeGlyphBitmap(font, glyphBitmap.data(), gw, gh, gw,
                          slot.scale, slot.scale, glyphIndex);

    // 关键：stb_truetype 位图行序与图像坐标系相反 —— 位图第 0 行对应字形底部
    // （stbtt__rasterize 中 y_scanline = j + iy0，j=0 即 bbox 底部，TrueType y 轴向上）。
    // 直接写入图集会导致字形上下颠倒，必须垂直翻转后再写入。
    // 翻转后的正向位图同时缓存到 CachedGlyph.bitmap，供驱逐后 Repack 直接复用。
    cached.bitmap.assign((size_t)gw * gh, 0);
    cached.bitmapW = (uint32_t)gw;
    cached.bitmapH = (uint32_t)gh;
    for (int row = 0; row < gh; ++row) {
        memcpy(cached.bitmap.data() + (gh - 1 - row) * gw, glyphBitmap.data() + row * gw, (size_t)gw);
    }

    // 写入 CPU 图集（RGBA = 白色 + glyphAlpha）
    for (int row = 0; row < gh; ++row) {
        unsigned char* dst = slot.atlasPixels.data() + ((allocY + row) * slot.atlasWidth + allocX) * 4;
        const unsigned char* src = cached.bitmap.data() + row * gw;
        for (int col = 0; col < gw; ++col) {
            dst[col * 4 + 0] = 255;
            dst[col * 4 + 1] = 255;
            dst[col * 4 + 2] = 255;
            dst[col * 4 + 3] = src[col];
        }
    }

    // 上传到 GPU（同样使用翻转后的数据，保证 GPU 图集与 CPU 端一致）
    UploadAtlasRegion(slot, allocX, allocY, (uint32_t)gw, (uint32_t)gh, cached.bitmap.data());

    // 记录 GlyphInfo
    cached.info.valid = true;
    cached.info.uv0 = glm::vec2((float)allocX / slot.atlasWidth,
                                (float)allocY / slot.atlasHeight);
    cached.info.uv1 = glm::vec2((float)(allocX + gw) / slot.atlasWidth,
                                (float)(allocY + gh) / slot.atlasHeight);
    cached.info.size = glm::vec2((float)gw, (float)gh);
    cached.info.bearing = glm::vec2((float)x0, (float)y0);
    cached.info.advance = advance * slot.scale;

    slot.glyphs[codepoint] = std::move(cached);
    return true;
}

// ===== SDF（标准距离场） =====

bool FontAtlas::InitSdf(float baseFontSize, uint32_t atlasWidth, uint32_t atlasHeight) {
    if (!m_FontLoaded || !m_FontInfo) return false;
    if (m_SdfInited) return true;

    auto* font = static_cast<stbtt_fontinfo*>(m_FontInfo);
    AtlasSlot& slot = m_SdfSlot;
    slot.isSdf = true;
    slot.padding = 4;                            // 边缘填充（防止 UV 采样越界出血）
    slot.baseFontSize = baseFontSize;
    slot.atlasWidth = std::min(atlasWidth, 2048u);
    slot.atlasHeight = std::min(atlasHeight, 2048u);
    slot.scale = stbtt_ScaleForPixelHeight(font, baseFontSize);
    stbtt_GetFontVMetrics(font, &slot.ascent, &slot.descent, &slot.lineGap);

    if (!CreateAtlasResources(slot)) {
        fprintf(stderr, "[FontAtlas] Failed to create SDF atlas resources\n");
        DestroyAtlasResources(slot);
        return false;
    }

    // 预烘焙 ASCII 32-126（pinned）
    slot.glyphs.reserve(256);
    for (uint32_t cp = ASCII_MIN; cp <= ASCII_MAX; ++cp) {
        BakeGlyphSdf(slot, cp);
    }

    m_SdfInited = true;
    fprintf(stderr, "[FontAtlas] SDF slot ready: base=%u px, padding=%d, atlas=%ux%u, glyphs=%zu\n",
            (unsigned)std::lround(baseFontSize), slot.padding,
            slot.atlasWidth, slot.atlasHeight, slot.glyphs.size());
    return true;
}

const FontAtlas::GlyphInfo& FontAtlas::GetGlyphSdf(uint32_t codepoint) {
    static GlyphInfo s_Invalid;
    s_Invalid = {};
    if (!m_SdfInited) return s_Invalid;

    AtlasSlot& slot = m_SdfSlot;
    auto it = slot.glyphs.find(codepoint);
    if (it != slot.glyphs.end()) {
        it->second.lastUsedFrame = ++slot.frameCounter;
        return it->second.info;
    }
    // 惰性烘焙
    if (!BakeGlyphSdf(slot, codepoint)) {
        // 回退到 '?'，再回退到空格
        auto fit = slot.glyphs.find((uint32_t)'?');
        if (fit != slot.glyphs.end()) { fit->second.lastUsedFrame = ++slot.frameCounter; return fit->second.info; }
        fit = slot.glyphs.find((uint32_t)' ');
        if (fit != slot.glyphs.end()) { fit->second.lastUsedFrame = ++slot.frameCounter; return fit->second.info; }
        return s_Invalid;
    }
    auto inserted = slot.glyphs.find(codepoint);
    if (inserted != slot.glyphs.end()) {
        inserted->second.lastUsedFrame = ++slot.frameCounter;
        return inserted->second.info;
    }
    return s_Invalid;
}

bool FontAtlas::BakeGlyphSdf(AtlasSlot& slot, uint32_t codepoint) {
    if (!m_FontInfo) return false;
    auto* font = static_cast<stbtt_fontinfo*>(m_FontInfo);
    const int padding = slot.padding;

    CachedGlyph cached;
    cached.lastUsedFrame = ++slot.frameCounter;
    cached.pinned = (codepoint >= ASCII_MIN && codepoint <= ASCII_MAX);

    int glyphIndex = stbtt_FindGlyphIndex(font, (int)codepoint);
    if (glyphIndex == 0 && codepoint != 0) {
        // 缺失字形：记录空 glyph 避免重复查找
        slot.glyphs[codepoint] = std::move(cached);
        return false;
    }

    int advance, lsb;
    stbtt_GetGlyphHMetrics(font, glyphIndex, &advance, &lsb);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(font, glyphIndex, slot.scale, slot.scale, &x0, &y0, &x1, &y1);
    const int gw = x1 - x0;
    const int gh = y1 - y0;

    if (gw <= 0 || gh <= 0) {
        // 空格等无位图字形：仅记录度量
        cached.info.valid = true;
        cached.info.size = glm::vec2(0.0f, 0.0f);
        cached.info.bearing = glm::vec2((float)x0, (float)y0);
        cached.info.advance = advance * slot.scale;
        slot.glyphs[codepoint] = std::move(cached);
        return true;
    }

    // 图集 slot 分配（SDF 位图含 padding）
    const uint32_t sdfW = (uint32_t)gw + padding * 2;
    const uint32_t sdfH = (uint32_t)gh + padding * 2;
    uint32_t allocX, allocY;
    if (!AllocShelfSlot(slot, sdfW + 1, sdfH + 1, allocX, allocY)) {
        if (!EvictAndRepack(slot, sdfW + 1, sdfH + 1, allocX, allocY)) {
            fprintf(stderr, "[FontAtlas] SDF glyph %u (%ux%u) too large / not evictable\n",
                    codepoint, gw, gh);
            return false;
        }
    }

    // 生成 SDF 位图（stbtt 内部 malloc，用完 stbtt_FreeSDF 释放）
    int sdfWOut = 0, sdfHOut = 0, xoff = 0, yoff = 0;
    unsigned char* sdf = stbtt_GetGlyphSDF(font, slot.scale, glyphIndex, padding, 128, 48.0f,
                                           &sdfWOut, &sdfHOut, &xoff, &yoff);
    if (!sdf || sdfWOut != (int)sdfW || sdfHOut != (int)sdfH) {
        if (sdf) stbtt_FreeSDF(sdf, nullptr);
        return false;
    }

    // 位图行序与 MakeGlyphBitmap 一致：第 0 行 = 字形底部 → 翻转后写入（缓存正向 SDF 位图）
    cached.bitmap.assign((size_t)sdfW * sdfH, 0);
    cached.bitmapW = sdfW;
    cached.bitmapH = sdfH;
    for (int row = 0; row < (int)sdfH; ++row) {
        memcpy(cached.bitmap.data() + (sdfH - 1 - row) * sdfW, sdf + row * sdfW, sdfW);
    }
    stbtt_FreeSDF(sdf, nullptr);

    // 写入 CPU 图集（RGBA = SDF 值填入 RGB + alpha=255；shader 采样 .r）
    for (uint32_t row = 0; row < sdfH; ++row) {
        unsigned char* dst = slot.atlasPixels.data() + ((allocY + row) * slot.atlasWidth + allocX) * 4;
        const unsigned char* src = cached.bitmap.data() + row * sdfW;
        for (uint32_t col = 0; col < sdfW; ++col) {
            dst[col * 4 + 0] = src[col];
            dst[col * 4 + 1] = src[col];
            dst[col * 4 + 2] = src[col];
            dst[col * 4 + 3] = 255;
        }
    }

    // 上传 GPU（SDF 模式：RGB 存距离值）
    UploadAtlasRegion(slot, allocX, allocY, sdfW, sdfH, cached.bitmap.data(), true);

    // GlyphInfo：UV 取位图内有效字形区（排除 padding）；度量用原始 bbox（基础字号单位）
    cached.info.valid = true;
    cached.info.uv0 = glm::vec2((float)(allocX + padding) / slot.atlasWidth,
                                (float)(allocY + padding) / slot.atlasHeight);
    cached.info.uv1 = glm::vec2((float)(allocX + padding + gw) / slot.atlasWidth,
                                (float)(allocY + padding + gh) / slot.atlasHeight);
    cached.info.size = glm::vec2((float)gw, (float)gh);
    cached.info.bearing = glm::vec2((float)x0, (float)y0);
    cached.info.advance = advance * slot.scale;

    slot.glyphs[codepoint] = std::move(cached);
    return true;
}

// ===== MSDF（多通道距离场） =====

namespace {
    // stb_truetype 轮廓 → msdfgen::Shape
    // 顶点语义（stbtt_vertex.type）：
    //   STBTT_vmove   : 轮廓起点（新轮廓）
    //   STBTT_vline   : 直线段，终点 (x,y)，起点 = 上一顶点
    //   STBTT_vcurve  : 二次贝塞尔，终点 (x,y)，控制点 (cx,cy)
    //   STBTT_vcubic  : 三次贝塞尔，终点 (x,y)，控制点 (cx,cy) 与 (cx1,cy1)
    // stb 轮廓为 TrueType 字体单位（y 向上），msdfgen 默认 Y_UPWARD 直接匹配。
    bool BuildMsdfShape(const stbtt_vertex* verts, int numVerts, msdfgen::Shape& shape) {
        if (numVerts <= 0) return false;
        int start = 0;
        for (int i = 0; i <= numVerts; ++i) {
            bool isEnd = (i == numVerts) || (verts[i].type == STBTT_vmove);
            if (isEnd) {
                int n = i - start;
                if (n >= 2) {
                    msdfgen::Contour& contour = shape.addContour();
                    for (int k = 0; k < n; ++k) {
                        const stbtt_vertex& a = verts[start + k];
                        const stbtt_vertex& b = verts[start + (k + 1) % n];
                        const msdfgen::Point2 pa((double)a.x, (double)a.y);
                        const msdfgen::Point2 pb((double)b.x, (double)b.y);
                        switch (b.type) {
                        case STBTT_vline:
                            contour.addEdge(msdfgen::EdgeHolder(pa, pb));
                            break;
                        case STBTT_vcurve:
                            contour.addEdge(msdfgen::EdgeHolder(
                                pa, msdfgen::Point2((double)b.cx, (double)b.cy), pb));
                            break;
                        case STBTT_vcubic:
                            contour.addEdge(msdfgen::EdgeHolder(
                                pa,
                                msdfgen::Point2((double)b.cx, (double)b.cy),
                                msdfgen::Point2((double)b.cx1, (double)b.cy1),
                                pb));
                            break;
                        default:
                            break;  // vmove 起点本身不产生边
                        }
                    }
                }
                start = i + 1;
            }
        }
        if (shape.contours.empty()) return false;
        shape.normalize();
        return shape.validate();
    }
}

bool FontAtlas::InitMsdf(float baseFontSize, uint32_t atlasWidth, uint32_t atlasHeight) {
    if (!m_FontLoaded || !m_FontInfo) return false;
    if (m_MsdfInited) return true;

    auto* font = static_cast<stbtt_fontinfo*>(m_FontInfo);
    AtlasSlot& slot = m_MsdfSlot;
    slot.isMsdf = true;
    slot.padding = 8;                            // MSDF 需要更宽的过渡带
    slot.baseFontSize = baseFontSize;
    slot.atlasWidth = std::min(atlasWidth, 2048u);
    slot.atlasHeight = std::min(atlasHeight, 2048u);
    slot.scale = stbtt_ScaleForPixelHeight(font, baseFontSize);
    stbtt_GetFontVMetrics(font, &slot.ascent, &slot.descent, &slot.lineGap);

    if (!CreateAtlasResources(slot)) {
        fprintf(stderr, "[FontAtlas] Failed to create MSDF atlas resources\n");
        DestroyAtlasResources(slot);
        return false;
    }

    slot.glyphs.reserve(256);
    for (uint32_t cp = ASCII_MIN; cp <= ASCII_MAX; ++cp) {
        BakeGlyphMsdf(slot, cp);
    }

    m_MsdfInited = true;
    fprintf(stderr, "[FontAtlas] MSDF slot ready: base=%u px, padding=%d, atlas=%ux%u, glyphs=%zu\n",
            (unsigned)std::lround(baseFontSize), slot.padding,
            slot.atlasWidth, slot.atlasHeight, slot.glyphs.size());
    return true;
}

const FontAtlas::GlyphInfo& FontAtlas::GetGlyphMsdf(uint32_t codepoint) {
    static GlyphInfo s_Invalid;
    s_Invalid = {};
    if (!m_MsdfInited) return s_Invalid;

    AtlasSlot& slot = m_MsdfSlot;
    auto it = slot.glyphs.find(codepoint);
    if (it != slot.glyphs.end()) {
        it->second.lastUsedFrame = ++slot.frameCounter;
        return it->second.info;
    }
    if (!BakeGlyphMsdf(slot, codepoint)) {
        auto fit = slot.glyphs.find((uint32_t)'?');
        if (fit != slot.glyphs.end()) { fit->second.lastUsedFrame = ++slot.frameCounter; return fit->second.info; }
        fit = slot.glyphs.find((uint32_t)' ');
        if (fit != slot.glyphs.end()) { fit->second.lastUsedFrame = ++slot.frameCounter; return fit->second.info; }
        return s_Invalid;
    }
    auto inserted = slot.glyphs.find(codepoint);
    if (inserted != slot.glyphs.end()) {
        inserted->second.lastUsedFrame = ++slot.frameCounter;
        return inserted->second.info;
    }
    return s_Invalid;
}

bool FontAtlas::BakeGlyphMsdf(AtlasSlot& slot, uint32_t codepoint) {
    if (!m_FontInfo) return false;
    auto* font = static_cast<stbtt_fontinfo*>(m_FontInfo);
    const int padding = slot.padding;

    CachedGlyph cached;
    cached.lastUsedFrame = ++slot.frameCounter;
    cached.pinned = (codepoint >= ASCII_MIN && codepoint <= ASCII_MAX);

    int glyphIndex = stbtt_FindGlyphIndex(font, (int)codepoint);
    if (glyphIndex == 0 && codepoint != 0) {
        slot.glyphs[codepoint] = std::move(cached);
        return false;
    }

    int advance, lsb;
    stbtt_GetGlyphHMetrics(font, glyphIndex, &advance, &lsb);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(font, glyphIndex, slot.scale, slot.scale, &x0, &y0, &x1, &y1);
    const int gw = x1 - x0;
    const int gh = y1 - y0;

    if (gw <= 0 || gh <= 0) {
        cached.info.valid = true;
        cached.info.size = glm::vec2(0.0f, 0.0f);
        cached.info.bearing = glm::vec2((float)x0, (float)y0);
        cached.info.advance = advance * slot.scale;
        slot.glyphs[codepoint] = std::move(cached);
        return true;
    }

    // 图集 slot 分配（MSDF 位图含 padding）
    const uint32_t msdfW = (uint32_t)gw + padding * 2;
    const uint32_t msdfH = (uint32_t)gh + padding * 2;
    uint32_t allocX, allocY;
    if (!AllocShelfSlot(slot, msdfW + 1, msdfH + 1, allocX, allocY)) {
        if (!EvictAndRepack(slot, msdfW + 1, msdfH + 1, allocX, allocY)) {
            fprintf(stderr, "[FontAtlas] MSDF glyph %u (%ux%u) too large / not evictable\n",
                    codepoint, gw, gh);
            return false;
        }
    }

    // stb 轮廓 → msdfgen Shape → MSDF 三通道位图
    stbtt_vertex* verts = nullptr;
    int numVerts = stbtt_GetGlyphShape(font, glyphIndex, &verts);
    if (numVerts <= 0) return false;

    msdfgen::Shape shape;
    bool shapeOk = BuildMsdfShape(verts, numVerts, shape);
    STBTT_free(verts, nullptr);
    if (!shapeOk) return false;

    // 边着色（MSDF 必须）：保证每个锐角附近至少两个通道携带正确距离
    msdfgen::edgeColoringSimple(shape, 3.0, (unsigned long long)codepoint);

    // 投影：形状单位(字体单位, y 向上) → 像素。字形 bbox 左下 (gx0,gy0) 映射到位图 (padding, padding)
    // 注意：必须用 stbtt_GetGlyphBox（字体单位、y 向上，与轮廓一致），
    // 不能用 GetGlyphBitmapBox（像素、y 向下）——否则坐标单位/方向错乱导致字形颠倒。
    int gx0, gy0, gx1, gy1;
    stbtt_GetGlyphBox(font, glyphIndex, &gx0, &gy0, &gx1, &gy1);
    const double scale = slot.scale;
    const msdfgen::Vector2 projScale(scale, scale);
    const msdfgen::Vector2 projTranslate(padding / scale - gx0, padding / scale - gy0);

    msdfgen::Bitmap<float, 3> msdfBitmap((int)msdfW, (int)msdfH);
    msdfgen::MSDFGeneratorConfig genConfig;
    genConfig.overlapSupport = true;
    msdfgen::generateMSDF(msdfBitmap, shape,
        msdfgen::SDFTransformation(
            msdfgen::Projection(projScale, projTranslate),
            msdfgen::DistanceMapping(msdfgen::Range(2.0 / scale))),  // 2px 过渡带（形状单位）；padding=8 ≥ 2×2 足够
        genConfig);

    // MSDF 像素值 float 0..1（msdfgen 内部 DistancePixelConversion 已完成距离→值映射，0.5=边缘）
    // → 0-255 直接线性缩放（边缘 0.5 → 128）；不是 px*128+128（那是距离 d∈[-1,1] 的公式）
    cached.bitmap.assign((size_t)msdfW * msdfH * 3, 0);
    cached.bitmapW = msdfW;
    cached.bitmapH = msdfH;
    for (int row = 0; row < (int)msdfH; ++row) {
        for (int col = 0; col < (int)msdfW; ++col) {
            float* px = msdfBitmap(col, row);  // 不翻转（与 SDF 图集一致的倒立存储）
            size_t idx = ((size_t)row * msdfW + col) * 3;
            cached.bitmap[idx + 0] = (unsigned char)std::clamp((int)(px[0] * 255.0f), 0, 255);
            cached.bitmap[idx + 1] = (unsigned char)std::clamp((int)(px[1] * 255.0f), 0, 255);
            cached.bitmap[idx + 2] = (unsigned char)std::clamp((int)(px[2] * 255.0f), 0, 255);
        }
    }

    // 写入 CPU 图集（RGBA = MSDF 三通道 + alpha 255；shader 用 median(r,g,b)）
    for (uint32_t row = 0; row < msdfH; ++row) {
        unsigned char* dst = slot.atlasPixels.data() + ((allocY + row) * slot.atlasWidth + allocX) * 4;
        const unsigned char* src = cached.bitmap.data() + row * msdfW * 3;
        for (uint32_t col = 0; col < msdfW; ++col) {
            dst[col * 4 + 0] = src[col * 3 + 0];
            dst[col * 4 + 1] = src[col * 3 + 1];
            dst[col * 4 + 2] = src[col * 3 + 2];
            dst[col * 4 + 3] = 255;
        }
    }

    // 上传 GPU（三通道模式）
    UploadAtlasRegion(slot, allocX, allocY, msdfW, msdfH, nullptr, false, cached.bitmap.data());

    // GlyphInfo：全槽 UV（含 padding，过渡带完整进入 quad）+ 槽尺寸/槽偏移
    // 这样 quad 边缘像素能采样到字形边缘外的过渡带，抗锯齿充分不截断。
    // 槽左下角 = bbox 左下 (x0,y0) 再偏移 (-padding, -padding)。
    cached.info.valid = true;
    cached.info.uv0 = glm::vec2((float)allocX / slot.atlasWidth,
                                (float)allocY / slot.atlasHeight);
    cached.info.uv1 = glm::vec2((float)(allocX + msdfW) / slot.atlasWidth,
                                (float)(allocY + msdfH) / slot.atlasHeight);
    cached.info.size = glm::vec2((float)msdfW, (float)msdfH);
    cached.info.bearing = glm::vec2((float)(x0 - padding), (float)(y0 - padding));
    cached.info.advance = advance * slot.scale;

    slot.glyphs[codepoint] = std::move(cached);
    return true;
}

// ===== Vulkan 资源 =====

bool FontAtlas::CreateAtlasResources(AtlasSlot& slot) {
    // --- CPU 端图集清零 ---
    const VkDeviceSize pixelCount = (VkDeviceSize)slot.atlasWidth * slot.atlasHeight;
    slot.atlasPixels.assign(pixelCount * 4, 0);  // RGBA8 = 全透明

    // --- VkImage ---
    VkImageCreateInfo imageCI{};
    imageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCI.imageType = VK_IMAGE_TYPE_2D;
    imageCI.extent = { slot.atlasWidth, slot.atlasHeight, 1 };
    imageCI.mipLevels = 1;
    imageCI.arrayLayers = 1;
    imageCI.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCI.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(g_Device, &imageCI, g_Allocator, &slot.atlasImage) != VK_SUCCESS) {
        fprintf(stderr, "[FontAtlas] Failed to create atlas image\n");
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(g_Device, slot.atlasImage, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(
        memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &slot.atlasMemory) != VK_SUCCESS) {
        fprintf(stderr, "[FontAtlas] Failed to allocate atlas memory\n");
        return false;
    }
    vkBindImageMemory(g_Device, slot.atlasImage, slot.atlasMemory, 0);

    // --- VkImageView ---
    VkImageViewCreateInfo viewCI{};
    viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image = slot.atlasImage;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.levelCount = 1;
    viewCI.subresourceRange.layerCount = 1;
    if (vkCreateImageView(g_Device, &viewCI, g_Allocator, &slot.atlasView) != VK_SUCCESS) {
        fprintf(stderr, "[FontAtlas] Failed to create atlas view\n");
        return false;
    }

    // --- VkSampler（线性插值 + clamp，适合字体） ---
    VkSamplerCreateInfo samplerCI{};
    samplerCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerCI.magFilter = VK_FILTER_LINEAR;
    samplerCI.minFilter = VK_FILTER_LINEAR;
    samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCI.anisotropyEnable = VK_FALSE;
    samplerCI.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerCI.unnormalizedCoordinates = VK_FALSE;
    samplerCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerCI.minLod = 0.0f;
    samplerCI.maxLod = 0.0f;
    if (vkCreateSampler(g_Device, &samplerCI, g_Allocator, &slot.atlasSampler) != VK_SUCCESS) {
        fprintf(stderr, "[FontAtlas] Failed to create atlas sampler\n");
        return false;
    }

    // --- VkDescriptorPool + VkDescriptorSet（使用 Renderer2D 的布局） ---
    VkDescriptorSetLayout layout = Renderer2D::GetInstance().GetTextureLayout();
    if (layout == VK_NULL_HANDLE) {
        fprintf(stderr, "[FontAtlas] Renderer2D texture layout not available\n");
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    poolCI.maxSets = 1;
    if (vkCreateDescriptorPool(g_Device, &poolCI, g_Allocator, &slot.descriptorPool) != VK_SUCCESS) {
        fprintf(stderr, "[FontAtlas] Failed to create atlas descriptor pool\n");
        return false;
    }

    VkDescriptorSetAllocateInfo setAI{};
    setAI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAI.descriptorPool = slot.descriptorPool;
    setAI.descriptorSetCount = 1;
    setAI.pSetLayouts = &layout;
    if (vkAllocateDescriptorSets(g_Device, &setAI, &slot.descriptorSet) != VK_SUCCESS) {
        fprintf(stderr, "[FontAtlas] Failed to allocate atlas descriptor set\n");
        return false;
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = slot.atlasView;
    imageInfo.sampler = slot.atlasSampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = slot.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(g_Device, 1, &write, 0, nullptr);

    // --- 初始 layout 转换（UNDEFINED → SHADER_READ_ONLY_OPTIMAL） ---
    {
        VkCommandBufferAllocateInfo cmdAI{};
        cmdAI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAI.commandPool = g_CommandPool;
        cmdAI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAI.commandBufferCount = 1;
        VkCommandBuffer cmd;
        if (vkAllocateCommandBuffers(g_Device, &cmdAI, &cmd) != VK_SUCCESS) return false;

        VkCommandBufferBeginInfo beginBI{};
        beginBI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginBI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginBI);

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = slot.atlasImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(cmd);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(g_Queue);
        vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &cmd);
    }

    return true;
}

void FontAtlas::DestroyAtlasResources(AtlasSlot& slot) {
    if (slot.descriptorPool) { vkDestroyDescriptorPool(g_Device, slot.descriptorPool, g_Allocator); slot.descriptorPool = VK_NULL_HANDLE; }
    slot.descriptorSet = VK_NULL_HANDLE;
    if (slot.atlasSampler) { vkDestroySampler(g_Device, slot.atlasSampler, g_Allocator); slot.atlasSampler = VK_NULL_HANDLE; }
    if (slot.atlasView) { vkDestroyImageView(g_Device, slot.atlasView, g_Allocator); slot.atlasView = VK_NULL_HANDLE; }
    if (slot.atlasImage) { vkDestroyImage(g_Device, slot.atlasImage, g_Allocator); slot.atlasImage = VK_NULL_HANDLE; }
    if (slot.atlasMemory) { vkFreeMemory(g_Device, slot.atlasMemory, g_Allocator); slot.atlasMemory = VK_NULL_HANDLE; }
    slot.atlasPixels.clear();
}

void FontAtlas::UploadAtlasRegion(AtlasSlot& slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                  const unsigned char* alphaData, bool sdfMode, const unsigned char* rgbData) {
    // 使用 staging buffer 上传指定区域
    VkDeviceSize regionSize = (VkDeviceSize)w * h * 4;  // RGBA
    VkBuffer staging;
    VkDeviceMemory stagingMem;

    VkBufferCreateInfo bufCI{};
    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size = regionSize;
    bufCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &bufCI, g_Allocator, &staging) != VK_SUCCESS) return;

    VkMemoryRequirements bufReq;
    vkGetBufferMemoryRequirements(g_Device, staging, &bufReq);
    VkMemoryAllocateInfo bufAI{};
    bufAI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bufAI.allocationSize = bufReq.size;
    bufAI.memoryTypeIndex = RendererUtils::FindMemoryType(
        bufReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_Device, &bufAI, g_Allocator, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(g_Device, staging, g_Allocator);
        return;
    }
    vkBindBufferMemory(g_Device, staging, stagingMem, 0);

    // 写入 RGBA 数据
    //   位图模式：白色 + alpha（shader 采样 .a）
    //   SDF 模式：距离值填入 RGB + alpha=255（shader 采样 .r）
    //   MSDF 模式：三通道距离值 + alpha=255（shader 采样 median(r,g,b)）
    void* mapped;
    vkMapMemory(g_Device, stagingMem, 0, regionSize, 0, &mapped);
    unsigned char* dst = static_cast<unsigned char*>(mapped);
    for (uint32_t row = 0; row < h; ++row) {
        for (uint32_t col = 0; col < w; ++col) {
            size_t idx = ((size_t)row * w + col) * 4;
            if (rgbData) {
                const unsigned char* src = rgbData + ((size_t)row * w + col) * 3;
                dst[idx + 0] = src[0];
                dst[idx + 1] = src[1];
                dst[idx + 2] = src[2];
                dst[idx + 3] = 255;
            } else if (sdfMode) {
                unsigned char v = alphaData[row * w + col];
                dst[idx + 0] = v;
                dst[idx + 1] = v;
                dst[idx + 2] = v;
                dst[idx + 3] = 255;
            } else {
                unsigned char v = alphaData[row * w + col];
                dst[idx + 0] = 255;
                dst[idx + 1] = 255;
                dst[idx + 2] = 255;
                dst[idx + 3] = v;
            }
        }
    }
    vkUnmapMemory(g_Device, stagingMem);

    // 单次命令：layout 转换 + 拷贝
    VkCommandBufferAllocateInfo cmdAI{};
    cmdAI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAI.commandPool = g_CommandPool;
    cmdAI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAI.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(g_Device, &cmdAI, &cmd);

    VkCommandBufferBeginInfo beginBI{};
    beginBI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginBI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginBI);

    // 转换到 TRANSFER_DST
    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = slot.atlasImage;
    toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toDst.subresourceRange.levelCount = 1;
    toDst.subresourceRange.layerCount = 1;
    toDst.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toDst);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = { (int32_t)x, (int32_t)y, 0 };
    region.imageExtent = { w, h, 1 };
    vkCmdCopyBufferToImage(cmd, staging, slot.atlasImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // 转回 SHADER_READ_ONLY
    VkImageMemoryBarrier toRO{};
    toRO.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRO.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRO.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRO.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRO.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRO.image = slot.atlasImage;
    toRO.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toRO.subresourceRange.levelCount = 1;
    toRO.subresourceRange.layerCount = 1;
    toRO.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRO.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toRO);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_Queue);
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &cmd);

    vkDestroyBuffer(g_Device, staging, g_Allocator);
    vkFreeMemory(g_Device, stagingMem, g_Allocator);
}

// ===== Shelf Packing =====

bool FontAtlas::AllocShelfSlot(AtlasSlot& slot, uint32_t w, uint32_t h, uint32_t& outX, uint32_t& outY) {
    if (w > slot.atlasWidth || h > slot.atlasHeight) return false;

    // 尝试放入现有 shelf
    for (auto& shelf : slot.shelves) {
        if (h <= shelf.height && (shelf.x + w) <= slot.atlasWidth) {
            outX = shelf.x;
            outY = shelf.y;
            shelf.x += w;
            return true;
        }
    }

    // 需要新建 shelf
    uint32_t nextY = 0;
    if (!slot.shelves.empty()) {
        const auto& last = slot.shelves.back();
        nextY = last.y + last.height;
    }
    if (nextY + h > slot.atlasHeight) {
        return false;  // 没有垂直空间
    }

    Shelf newShelf;
    newShelf.y = nextY;
    newShelf.height = h;
    newShelf.x = w;
    slot.shelves.push_back(newShelf);

    outX = 0;
    outY = nextY;
    return true;
}

// ===== LRU 驱逐 + 重打包 =====

bool FontAtlas::Repack(AtlasSlot& slot) {
    // 清空 packing 状态 + CPU 图集（字形缓存与位图保留）
    slot.shelves.clear();
    slot.atlasPixels.assign(slot.atlasPixels.size(), 0);

    // 收集所有有位图的字形（空格等无位图字形不占图集区域）
    std::vector<uint32_t> cps;
    for (auto& [cp, g] : slot.glyphs) {
        if (!g.bitmap.empty()) cps.push_back(cp);
    }

    for (uint32_t cp : cps) {
        CachedGlyph& g = slot.glyphs[cp];
        uint32_t bw = g.bitmapW;
        uint32_t bh = g.bitmapH;
        if (bw == 0 || bh == 0) continue;

        uint32_t x, y;
        if (!AllocShelfSlot(slot, bw + 1, bh + 1, x, y)) {
            fprintf(stderr, "[FontAtlas] Repack failed (glyph %u %ux%u)\n", cp, bw, bh);
            return false;
        }
        // 写入 CPU 图集（位图已是正向；RGBA 格式按 slot 类型）
        //   位图：白+alpha；SDF：距离值三份+alpha255；MSDF：三通道距离值+alpha255
        for (uint32_t row = 0; row < bh; ++row) {
            unsigned char* dst = slot.atlasPixels.data() + ((y + row) * slot.atlasWidth + x) * 4;
            const unsigned char* src = g.bitmap.data() + row * bw * (slot.isMsdf ? 3u : 1u);
            for (uint32_t col = 0; col < bw; ++col) {
                if (slot.isMsdf) {
                    dst[col * 4 + 0] = src[col * 3 + 0];
                    dst[col * 4 + 1] = src[col * 3 + 1];
                    dst[col * 4 + 2] = src[col * 3 + 2];
                    dst[col * 4 + 3] = 255;
                } else if (slot.isSdf) {
                    unsigned char v = src[col];
                    dst[col * 4 + 0] = v;
                    dst[col * 4 + 1] = v;
                    dst[col * 4 + 2] = v;
                    dst[col * 4 + 3] = 255;
                } else {
                    unsigned char v = src[col];
                    dst[col * 4 + 0] = 255;
                    dst[col * 4 + 1] = 255;
                    dst[col * 4 + 2] = 255;
                    dst[col * 4 + 3] = v;
                }
            }
        }
        // 更新 UV：SDF 排除 padding（有效字形区）；MSDF 用全槽（含 padding，与 BakeGlyphMsdf 一致）
        const uint32_t pad = (slot.isSdf || slot.isMsdf) ? (uint32_t)slot.padding : 0u;
        if (slot.isMsdf) {
            g.info.uv0 = glm::vec2((float)x / slot.atlasWidth, (float)y / slot.atlasHeight);
            g.info.uv1 = glm::vec2((float)(x + bw) / slot.atlasWidth, (float)(y + bh) / slot.atlasHeight);
        } else {
            const uint32_t gw = (uint32_t)g.info.size.x;
            const uint32_t gh = (uint32_t)g.info.size.y;
            g.info.uv0 = glm::vec2((float)(x + pad) / slot.atlasWidth, (float)(y + pad) / slot.atlasHeight);
            g.info.uv1 = glm::vec2((float)(x + pad + gw) / slot.atlasWidth, (float)(y + pad + gh) / slot.atlasHeight);
        }
    }

    // 全量上传 GPU 图集
    UploadAtlasFull(slot);
    return true;
}

bool FontAtlas::EvictAndRepack(AtlasSlot& slot, uint32_t needW, uint32_t needH, uint32_t& outX, uint32_t& outY) {
    if (needW > slot.atlasWidth || needH > slot.atlasHeight) return false;

    // 收集可驱逐字形：优先非 pinned；若全部 pinned（图集满且无低优先级字形），
    // 退化为按 LRU 驱逐最旧的（保证图集满时不卡死，被驱逐字形下次显示时重新烘焙）
    std::vector<uint32_t> nonPinned;
    for (auto& [cp, g] : slot.glyphs) {
        if (!g.pinned) nonPinned.push_back(cp);
    }
    if (nonPinned.empty()) {
        for (auto& [cp, g] : slot.glyphs) nonPinned.push_back(cp);
        fprintf(stderr, "[FontAtlas] Atlas full with only pinned glyphs, evicting LRU (will re-bake on demand)\n");
    }

    // 驱逐循环：每次驱逐最久未用的 25%（至少 1 个），重打包后试放新字形
    while (!nonPinned.empty()) {
        std::sort(nonPinned.begin(), nonPinned.end(), [&slot](uint32_t a, uint32_t b) {
            return slot.glyphs[a].lastUsedFrame < slot.glyphs[b].lastUsedFrame;
        });
        size_t evictNow = std::max<size_t>(1, nonPinned.size() / 4);
        for (size_t i = 0; i < evictNow; ++i) {
            slot.glyphs.erase(nonPinned[i]);
        }
        nonPinned.erase(nonPinned.begin(), nonPinned.begin() + evictNow);

        if (!Repack(slot)) return false;

        // 试放新字形（失败则恢复 shelves 状态，供下一轮驱逐/后续分配使用）
        auto shelvesBackup = slot.shelves;
        if (AllocShelfSlot(slot, needW, needH, outX, outY)) {
            fprintf(stderr, "[FontAtlas] LRU evicted %zu glyph(s), repacked, %zu remain\n",
                    evictNow, slot.glyphs.size());
            return true;
        }
        slot.shelves = std::move(shelvesBackup);
    }
    return false;
}

void FontAtlas::UploadAtlasFull(AtlasSlot& slot) {
    const VkDeviceSize fullSize = (VkDeviceSize)slot.atlasWidth * slot.atlasHeight * 4;
    VkBuffer staging;
    VkDeviceMemory stagingMem;

    VkBufferCreateInfo bufCI{};
    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size = fullSize;
    bufCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &bufCI, g_Allocator, &staging) != VK_SUCCESS) return;

    VkMemoryRequirements bufReq;
    vkGetBufferMemoryRequirements(g_Device, staging, &bufReq);
    VkMemoryAllocateInfo bufAI{};
    bufAI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bufAI.allocationSize = bufReq.size;
    bufAI.memoryTypeIndex = RendererUtils::FindMemoryType(
        bufReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_Device, &bufAI, g_Allocator, &stagingMem) != VK_SUCCESS) {
        vkDestroyBuffer(g_Device, staging, g_Allocator);
        return;
    }
    vkBindBufferMemory(g_Device, staging, stagingMem, 0);

    void* mapped;
    vkMapMemory(g_Device, stagingMem, 0, fullSize, 0, &mapped);
    memcpy(mapped, slot.atlasPixels.data(), (size_t)fullSize);
    vkUnmapMemory(g_Device, stagingMem);

    // 单次命令：TRANSFER_DST 转换 + 全量拷贝 + 回 SHADER_READ_ONLY
    VkCommandBufferAllocateInfo cmdAI{};
    cmdAI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAI.commandPool = g_CommandPool;
    cmdAI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAI.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(g_Device, &cmdAI, &cmd);

    VkCommandBufferBeginInfo beginBI{};
    beginBI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginBI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginBI);

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;

    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = slot.atlasImage;
    toDst.subresourceRange = range;
    toDst.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toDst);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { slot.atlasWidth, slot.atlasHeight, 1 };
    vkCmdCopyBufferToImage(cmd, staging, slot.atlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toRO{};
    toRO.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRO.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRO.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRO.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRO.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRO.image = slot.atlasImage;
    toRO.subresourceRange = range;
    toRO.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRO.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toRO);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_Queue);
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &cmd);

    vkDestroyBuffer(g_Device, staging, g_Allocator);
    vkFreeMemory(g_Device, stagingMem, g_Allocator);
}

void FontAtlas::ClearAtlasSlot(AtlasSlot& slot) {
    // 清空 CPU 图集
    slot.atlasPixels.assign(slot.atlasPixels.size(), 0);

    // 清空 packing 状态
    slot.shelves.clear();

    // 清空字形缓存
    slot.glyphs.clear();

    // 上传空白图集到 GPU（clear 到透明）
    VkCommandBufferAllocateInfo cmdAI{};
    cmdAI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAI.commandPool = g_CommandPool;
    cmdAI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAI.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(g_Device, &cmdAI, &cmd);

    VkCommandBufferBeginInfo beginBI{};
    beginBI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginBI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginBI);

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    VkClearColorValue clearValue = { 0.0f, 0.0f, 0.0f, 0.0f };

    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = slot.atlasImage;
    toDst.subresourceRange = range;
    toDst.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toDst);

    vkCmdClearColorImage(cmd, slot.atlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clearValue, 1, &range);

    VkImageMemoryBarrier toRO{};
    toRO.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRO.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRO.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRO.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRO.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRO.image = slot.atlasImage;
    toRO.subresourceRange = range;
    toRO.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRO.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toRO);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_Queue);
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &cmd);

    fprintf(stderr, "[FontAtlas] Atlas slot cleared and rebuilt\n");
}

void FontAtlas::ClearAtlas(float fontSize) {
    AtlasSlot* slot = const_cast<AtlasSlot*>(FindSlot(fontSize));
    if (slot) ClearAtlasSlot(*slot);
}

// ===== 字体度量 =====

float FontAtlas::GetScale(float fontSize) const {
    const AtlasSlot* slot = FindSlot(fontSize);
    return slot ? slot->scale : 0.0f;
}

float FontAtlas::GetAscent(float fontSize) const {
    const AtlasSlot* slot = FindSlot(fontSize);
    return slot ? slot->ascent * slot->scale : 0.0f;
}

float FontAtlas::GetDescent(float fontSize) const {
    const AtlasSlot* slot = FindSlot(fontSize);
    return slot ? slot->descent * slot->scale : 0.0f;
}

float FontAtlas::GetLineHeight(float fontSize) const {
    const AtlasSlot* slot = FindSlot(fontSize);
    return slot ? (slot->ascent - slot->descent + slot->lineGap) * slot->scale : 0.0f;
}

size_t FontAtlas::GetGlyphCount(float fontSize) const {
    const AtlasSlot* slot = FindSlot(fontSize);
    return slot ? slot->glyphs.size() : 0;
}

VkDescriptorSet FontAtlas::GetAtlasDescriptor(float fontSize) {
    AtlasSlot* slot = GetOrCreateSlot(fontSize);
    return slot ? slot->descriptorSet : VK_NULL_HANDLE;
}
