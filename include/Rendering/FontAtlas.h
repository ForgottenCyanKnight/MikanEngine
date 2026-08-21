#pragma once
// FontAtlas.h - 运行时字体图集（stb_truetype 烘焙，每字号一张 RGBA8 图集）
// 设计：支持多个字号 —— 每个字号一个独立图集 slot（纹理 + 字形缓存），
// 字形按需惰性烘焙到对应 slot 的 shelf-packing 图集，图集满时清空重建。
// 图集纹理描述符注册到 Renderer2D，字符四边形直接走现有 2D 合批管线。

#include "Platform/Export.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <unordered_map>

class MIKAN_API FontAtlas {
public:
    static FontAtlas& GetInstance();

    // ===== 生命周期 =====
    // 加载 TTF 并预创建默认字号 slot（也可不调用：GetGlyph 遇到新字号自动创建）
    // ttfPath: 字体文件路径; fontSize: 像素高度; atlasWidth/Height: 图集尺寸
    bool Init(const std::string& ttfPath, float fontSize,
              uint32_t atlasWidth = 1024, uint32_t atlasHeight = 1024);
    void Cleanup();
    bool IsInitialized() const { return m_FontLoaded; }

    // ===== 字形信息 =====
    struct GlyphInfo {
        bool valid = false;         // 字形是否存在
        glm::vec2 uv0{0, 0};       // 图集 UV 左下
        glm::vec2 uv1{0, 0};       // 图集 UV 右上
        glm::vec2 size{0, 0};      // 字形像素尺寸
        glm::vec2 bearing{0, 0};   // 左下偏移（相对于 baseline 原点）
        float advance = 0.0f;      // 水平推进量（像素）
    };

    // 获取字形（未命中时按对应字号惰性烘焙到该字号的图集）
    const GlyphInfo& GetGlyph(uint32_t codepoint, float fontSize);

    // ===== SDF（标准距离场，单基础字号图集，任意目标字号缩放） =====
    // baseFontSize: 烘焙基础字号（建议 48px）；渲染时按目标字号缩放 quad
    bool InitSdf(float baseFontSize, uint32_t atlasWidth = 1024, uint32_t atlasHeight = 1024);
    bool IsSdfReady() const { return m_SdfInited; }
    const GlyphInfo& GetGlyphSdf(uint32_t codepoint);
    VkDescriptorSet GetSdfDescriptor() const { return m_SdfInited ? m_SdfSlot.descriptorSet : VK_NULL_HANDLE; }
    float GetSdfBaseFontSize() const { return m_SdfInited ? m_SdfSlot.baseFontSize : 0.0f; }
    float GetSdfLineHeight() const { return m_SdfInited ? (m_SdfSlot.ascent - m_SdfSlot.descent + m_SdfSlot.lineGap) * m_SdfSlot.scale : 0.0f; }
    size_t GetSdfGlyphCount() const { return m_SdfInited ? m_SdfSlot.glyphs.size() : 0; }

    // ===== MSDF（多通道距离场，角点更锐利；单基础字号图集） =====
    bool InitMsdf(float baseFontSize, uint32_t atlasWidth = 1024, uint32_t atlasHeight = 1024);
    bool IsMsdfReady() const { return m_MsdfInited; }
    const GlyphInfo& GetGlyphMsdf(uint32_t codepoint);
    VkDescriptorSet GetMsdfDescriptor() const { return m_MsdfInited ? m_MsdfSlot.descriptorSet : VK_NULL_HANDLE; }
    float GetMsdfBaseFontSize() const { return m_MsdfInited ? m_MsdfSlot.baseFontSize : 0.0f; }
    float GetMsdfLineHeight() const { return m_MsdfInited ? (m_MsdfSlot.ascent - m_MsdfSlot.descent + m_MsdfSlot.lineGap) * m_MsdfSlot.scale : 0.0f; }
    size_t GetMsdfGlyphCount() const { return m_MsdfInited ? m_MsdfSlot.glyphs.size() : 0; }

    // ===== 字体度量（按字号） =====
    // 注意：此方法会惰性创建该字号的 slot（与 GetGlyph 一致），需在 Renderer2D::Init 之后调用
    VkDescriptorSet GetAtlasDescriptor(float fontSize);
    float GetScale(float fontSize) const;
    float GetAscent(float fontSize) const;        // baseline 以上（像素，正值）
    float GetDescent(float fontSize) const;       // baseline 以下（像素，负值）
    float GetLineHeight(float fontSize) const;    // 行高 = ascent - descent + lineGap
    size_t GetGlyphCount(float fontSize) const;   // 该字号已烘焙字形数

    // ===== 图集状态 =====
    size_t GetSlotCount() const { return m_Slots.size(); }
    // 清空指定字号的图集（强制重建）
    void ClearAtlas(float fontSize);

private:
    FontAtlas() = default;
    FontAtlas(const FontAtlas&) = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;

    // ===== 单个字号的图集 slot =====
    struct Shelf {
        uint32_t y = 0;       // shelf 顶部 y 坐标
        uint32_t height = 0;  // shelf 高度
        uint32_t x = 0;       // 当前写入 x 位置
    };

    // 缓存字形：info + CPU 位图（驱逐/重打包时避免重新栅格化）+ LRU 时间戳
    struct CachedGlyph {
        GlyphInfo info;
        std::vector<unsigned char> bitmap;  // 单通道 alpha/SDF 值，正向（顶部在数组开头）
        uint32_t bitmapW = 0;               // 位图实际宽度（SDF 含 padding；位图 = 字形宽）
        uint32_t bitmapH = 0;               // 位图实际高度（SDF 含 padding；位图 = 字形高）
        uint64_t lastUsedFrame = 0;         // 最近一次被绘制/烘焙的帧号（LRU 淘汰依据）
        bool pinned = false;                // 常用字符钉住，永不驱逐（ASCII 32-126）
    };

    struct AtlasSlot {
        float scale = 0.0f;
        int ascent = 0, descent = 0, lineGap = 0;
        uint32_t atlasWidth = 0, atlasHeight = 0;
        uint64_t frameCounter = 0;          // 单调递增，用于 LRU 时间戳
        bool isSdf = false;                 // true=SDF 距离场图集（用于任意字号缩放）
        bool isMsdf = false;                // true=MSDF 多通道距离场图集（角点更锐利）
        int padding = 0;                    // SDF/MSDF 边缘填充像素
        float baseFontSize = 0.0f;          // SDF/MSDF 基础字号（渲染时按目标字号缩放）
        std::vector<unsigned char> atlasPixels;  // RGBA8, CPU 端
        // Vulkan 资源
        VkImage atlasImage = VK_NULL_HANDLE;
        VkDeviceMemory atlasMemory = VK_NULL_HANDLE;
        VkImageView atlasView = VK_NULL_HANDLE;
        VkSampler atlasSampler = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        // 字形缓存 + packing
        std::unordered_map<uint32_t, CachedGlyph> glyphs;
        std::vector<Shelf> shelves;
    };

    // 内部方法
    bool LoadFontData(const std::string& ttfPath);
    const AtlasSlot* FindSlot(float fontSize) const;
    AtlasSlot* GetOrCreateSlot(float fontSize, uint32_t atlasWidth = 1024, uint32_t atlasHeight = 1024);
    bool CreateAtlasResources(AtlasSlot& slot);
    void DestroyAtlasResources(AtlasSlot& slot);
    bool BakeGlyph(AtlasSlot& slot, uint32_t codepoint);
    bool BakeGlyphSdf(AtlasSlot& slot, uint32_t codepoint);   // stbtt_GetGlyphSDF 路径
    bool BakeGlyphMsdf(AtlasSlot& slot, uint32_t codepoint);  // msdfgen 路径（三通道）
    void UploadAtlasRegion(AtlasSlot& slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           const unsigned char* alphaData, bool sdfMode = false, const unsigned char* rgbData = nullptr);
    void UploadAtlasFull(AtlasSlot& slot);   // 整张图集重传（重打包后）
    bool AllocShelfSlot(AtlasSlot& slot, uint32_t w, uint32_t h, uint32_t& outX, uint32_t& outY);
    bool Repack(AtlasSlot& slot);            // 按当前 glyphs 重新打包整个图集（用缓存的位图）
    bool EvictAndRepack(AtlasSlot& slot, uint32_t needW, uint32_t needH, uint32_t& outX, uint32_t& outY);  // LRU 驱逐+重打包
    void ClearAtlasSlot(AtlasSlot& slot);

    // ASCII 常用字符范围（预烘焙 + pinned，永不驱逐）
    static constexpr uint32_t ASCII_MIN = 32;
    static constexpr uint32_t ASCII_MAX = 126;

    // TTF 数据（所有字号共享同一份字体解析）
    std::vector<unsigned char> m_FontData;
    void* m_FontInfo = nullptr;     // stbtt_fontinfo*
    bool m_FontLoaded = false;

    // 按字号分组的图集 slot
    std::unordered_map<int, AtlasSlot> m_Slots;

    // SDF 图集（单基础字号实例）
    AtlasSlot m_SdfSlot;
    bool m_SdfInited = false;

    // MSDF 图集（单基础字号实例）
    AtlasSlot m_MsdfSlot;
    bool m_MsdfInited = false;
};
