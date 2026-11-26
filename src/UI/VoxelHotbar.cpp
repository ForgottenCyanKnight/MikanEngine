#include "UI/VoxelHotbar.h"

#include "Core/RenderGlobals.h"
#include "Rendering/Renderer2D.h"
#include "Rendering/TextRenderer.h"
#include "World/BlockManager.h"
#include "World/WorldGlobals.h"
#include "World/WorldRenderer.h"
#include "../../engine/shaders/glsl/gbuffer/voxel/voxel_world_tint.glsl"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>
#include <vector>

namespace UI::VoxelHotbar {
namespace {

constexpr std::array kDefaultBlocks{
    BlockManager::GRASSBLOCK, BlockManager::DIRT, BlockManager::STONE,
    BlockManager::SAND, BlockManager::WOOD, BlockManager::PLANK,
    BlockManager::BRICK, BlockManager::GLASS, BlockManager::LEAVES};
constexpr int kSlotCount = static_cast<int>(kDefaultBlocks.size());
constexpr int kAtlasTiles = 16;
constexpr int kLayer = 90;
constexpr float kSlotSize = 50.0f;
constexpr float kGap = 5.0f;
constexpr float kPadding = 7.0f;
constexpr float kBottomMargin = 18.0f;
constexpr float kBarWidth = kSlotCount * kSlotSize + (kSlotCount - 1) * kGap + kPadding * 2.0f;
constexpr float kBarHeight = kSlotSize + kPadding * 2.0f;
constexpr float kPageButtonWidth = 32.0f;
float scrollRemainder = 0.0f;

const std::vector<int>& SelectableBlocks()
{
    static const auto blocks = [] {
        std::vector<int> result(kDefaultBlocks.begin(), kDefaultBlocks.end());
        const auto& manager = BlockManager::GetInstance();
        for (int id = BlockManager::GRASS + 1; id <= UINT8_MAX; ++id) {
            const auto& block = manager.GetBlockType(id);
            if (!block.name.empty() && block.isSolid && !block.isTransparent &&
                block.blockType == BlockManager::BLOCK_TYPE_CUBE) {
                result.push_back(id);
            }
        }
        return result;
    }();
    return blocks;
}

std::span<const int> CurrentPage(const std::vector<int>& blocks)
{
    const auto selected = std::find(blocks.begin(), blocks.end(), g_HandBlockId);
    const size_t start = selected == blocks.end() ? 0 : (selected - blocks.begin()) / kSlotCount * kSlotCount;
    return std::span(blocks).subspan(start, std::min<size_t>(kSlotCount, blocks.size() - start));
}

bool ChangePage(int direction)
{
    const auto& blocks = SelectableBlocks();
    const int pageCount = (static_cast<int>(blocks.size()) + kSlotCount - 1) / kSlotCount;
    if (pageCount <= 1) return false;

    const int page = static_cast<int>(CurrentPage(blocks).data() - blocks.data()) / kSlotCount;
    const int next = (page + direction + pageCount) % pageCount;
    g_HandBlockId = blocks[next * kSlotCount];
    scrollRemainder = 0.0f;
    return true;
}

struct Layout {
    glm::vec2 viewSize{0.0f};
    glm::vec2 origin{0.0f};
    float scale = 0.0f;
    bool hasPages = false;

    glm::vec2 SlotOrigin(int index) const
    {
        return origin + scale * glm::vec2(kPadding + index * (kSlotSize + kGap), kPadding);
    }

    glm::vec2 PageButtonOrigin(int direction) const
    {
        return origin + scale * glm::vec2(direction < 0 ? -kPageButtonWidth : kBarWidth, kPadding);
    }

    glm::vec2 PanelOrigin() const
    {
        return origin - glm::vec2(hasPages ? kPageButtonWidth * scale : 0.0f, 0.0f);
    }

    glm::vec2 PanelSize() const
    {
        return scale * glm::vec2(kBarWidth + (hasPages ? 2.0f * kPageButtonWidth : 0.0f), kBarHeight);
    }
};

Layout lastLayout;

Layout MakeLayout(uint32_t width, uint32_t height, bool touchMode, float bottomInset, bool hasPages)
{
    const glm::vec2 viewSize(width, height);
    const float fullWidth = kBarWidth + (hasPages ? 2.0f * kPageButtonWidth : 0.0f);
    const float desiredScale = touchMode ? viewSize.y / 480.0f
                                        : std::clamp(viewSize.y / 720.0f, 0.7f, 1.5f);
    const float scale = std::min({
        desiredScale,
        (viewSize.x - 16.0f) / fullWidth,
        (viewSize.y - bottomInset) / (kBarHeight + kBottomMargin)});
    return {viewSize, {(viewSize.x - kBarWidth * scale) * 0.5f,
                       viewSize.y - bottomInset - (kBarHeight + kBottomMargin) * scale}, scale, hasPages};
}

glm::vec4 Color(float r, float g, float b, float a = 1.0f)
{
    return {r, g, b, a * GetUIOpacity()};
}

bool Contains(glm::vec2 point, glm::vec2 origin, glm::vec2 size)
{
    const glm::vec2 end = origin + size;
    return point.x >= origin.x && point.x < end.x && point.y >= origin.y && point.y < end.y;
}

void DrawPageButton(Renderer2D& renderer, const Layout& layout, int direction)
{
    const float scale = layout.scale;
    const glm::vec2 center = layout.PageButtonOrigin(direction) +
        scale * glm::vec2(kPageButtonWidth * 0.5f, kSlotSize * 0.5f);
    const glm::vec2 tip = center + scale * glm::vec2(4.0f * direction, 0.0f);
    const float tail = -4.0f * direction;
    const glm::vec4 color = Color(0.83f, 0.87f, 0.83f);
    renderer.DrawLine(center + scale * glm::vec2(tail, -7.0f), tip, 2.0f * scale, color, kLayer + 6);
    renderer.DrawLine(tip, center + scale * glm::vec2(tail, 7.0f), 2.0f * scale, color, kLayer + 6);
}

void DrawAtlasFace(Renderer2D& renderer, VkDescriptorSet atlas, glm::vec2 inset, int tile,
                   const std::array<glm::vec2, 4>& corners, glm::vec3 tint)
{
    if (tile < 0 || tile >= kAtlasTiles * kAtlasTiles) return;

    constexpr float tileSize = 1.0f / kAtlasTiles;
    const glm::vec2 tileOrigin = tileSize * glm::vec2(tile % kAtlasTiles, tile / kAtlasTiles);
    const glm::vec2 first = tileOrigin + inset;
    const glm::vec2 last = tileOrigin + glm::vec2(tileSize) - inset;

    Quad2D face{};
    face.p0 = corners[0];
    face.p1 = corners[1];
    face.p2 = corners[2];
    face.p3 = corners[3];
    // Quad2D uses uv1.y at p0/p1. The world atlas stores its top row first.
    face.uv0 = {first.x, last.y};
    face.uv1 = {last.x, first.y};
    face.color = Color(tint.r, tint.g, tint.b);
    face.texture = atlas;
    face.layer = kLayer + 6;
    renderer.DrawQuad(face);
}

void DrawBlockPreview(Renderer2D& renderer, VkDescriptorSet atlas, glm::vec2 inset,
                      int blockId, glm::vec2 slot, float scale)
{
    const auto& block = BlockManager::GetInstance().GetBlockType(blockId);
    constexpr glm::vec3 grassTint(MIKAN_VOXEL_WORLD_GRASS_TINT);
    const glm::vec3 sideTint = blockId == BlockManager::LEAVES
        ? grassTint * MIKAN_VOXEL_WORLD_LEAF_TINT_SCALE : glm::vec3(1.0f);
    const glm::vec3 topTint = blockId == BlockManager::GRASSBLOCK ? grassTint : sideTint;

    const glm::vec2 back = slot + scale * glm::vec2(25.0f, 7.0f);
    const glm::vec2 left = slot + scale * glm::vec2(8.0f, 16.0f);
    const glm::vec2 right = slot + scale * glm::vec2(42.0f, 16.0f);
    const glm::vec2 front = slot + scale * glm::vec2(25.0f, 25.0f);
    const glm::vec2 height(0.0f, 20.0f * scale);

    DrawAtlasFace(renderer, atlas, inset, block.topTexIndex, {back, right, front, left}, topTint);
    DrawAtlasFace(renderer, atlas, inset, block.frontTexIndex,
                  {left, front, front + height, left + height}, sideTint * 0.89f);
    DrawAtlasFace(renderer, atlas, inset, block.rightTexIndex,
                  {front, right, right + height, front + height}, sideTint * 0.74f);
}

} // namespace

void Draw(Renderer2D& renderer, uint32_t width, uint32_t height, bool touchMode, float bottomInset)
{
    if (!g_World || !g_WorldRenderer || !g_WorldRenderer->IsInitialized()) {
        lastLayout = {};
        scrollRemainder = 0.0f;
        return;
    }

    const auto& blocks = SelectableBlocks();
    const auto page = CurrentPage(blocks);
    const int occupiedSlots = static_cast<int>(page.size());
    lastLayout = MakeLayout(width, height, touchMode, bottomInset, blocks.size() > kSlotCount);
    const float scale = lastLayout.scale;
    if (scale <= 0.0f) return;
    const glm::vec2 panelOrigin = lastLayout.PanelOrigin();
    const glm::vec2 panelSize = lastLayout.PanelSize();
    renderer.DrawRect(panelOrigin + glm::vec2(0.0f, scale * 3.0f), panelSize,
                      Color(0.04f, 0.05f, 0.05f, 0.42f), kLayer);
    renderer.DrawRect(panelOrigin, panelSize, Color(0.12f, 0.15f, 0.15f, 0.94f), kLayer + 1);
    renderer.DrawRect(panelOrigin, {panelSize.x, scale},
                      Color(0.66f, 0.73f, 0.70f, 0.42f), kLayer + 2);
    if (lastLayout.hasPages) {
        DrawPageButton(renderer, lastLayout, -1);
        DrawPageButton(renderer, lastLayout, 1);
    }

    const VkDescriptorSet atlas = g_WorldRenderer->GetAtlasDescriptor();
    const glm::uvec2 atlasSize = g_WorldRenderer->GetAtlasSize();
    const bool hasAtlas = atlas != VK_NULL_HANDLE && atlasSize.x > 0 && atlasSize.y > 0;
    const glm::vec2 inset = hasAtlas ? 0.5f / glm::vec2(atlasSize) : glm::vec2(0.0f);
    auto& text = TextRenderer::GetInstance();
    for (int i = 0; i < kSlotCount; ++i) {
        const glm::vec2 pos = lastLayout.SlotOrigin(i);
        const bool occupied = i < occupiedSlots;
        const bool selected = occupied && page[i] == g_HandBlockId;
        const float border = (selected ? 2.0f : 1.0f) * scale;
        const float size = kSlotSize * scale;
        renderer.DrawRect(pos, glm::vec2(size),
                          selected ? Color(0.96f, 0.88f, 0.58f) : Color(0.39f, 0.46f, 0.44f, 0.65f),
                          kLayer + 3);
        renderer.DrawRect(pos + glm::vec2(border), glm::vec2(size - 2.0f * border),
                          selected ? Color(0.25f, 0.27f, 0.20f) : Color(0.16f, 0.19f, 0.19f),
                          kLayer + 4);

        if (!occupied) continue;
        if (hasAtlas) {
            DrawBlockPreview(renderer, atlas, inset, page[i], pos, scale);
        }

        if (!touchMode && text.IsReady()) {
            const glm::vec4 color = selected ? Color(1.0f, 0.93f, 0.72f) : Color(0.77f, 0.82f, 0.80f);
            text.DrawStringMsdf(std::string(1, static_cast<char>('1' + i)), pos.x + 4.0f * scale,
                                pos.y + 12.0f * scale, 11.0f * scale, color, kLayer + 7);
        }
    }
}

bool SelectKey(SDL_Keycode key)
{
    if (!g_World) return false;
    if (key == SDLK_PAGEUP) return ChangePage(-1);
    if (key == SDLK_PAGEDOWN) return ChangePage(1);

    int slot;
    if (key >= SDLK_1 && key <= SDLK_9) {
        slot = key - SDLK_1;
    } else if (key >= SDLK_KP_1 && key <= SDLK_KP_9) {
        slot = key - SDLK_KP_1;
    } else {
        return false;
    }
    const auto page = CurrentPage(SelectableBlocks());
    if (slot < static_cast<int>(page.size())) g_HandBlockId = page[slot];
    scrollRemainder = 0.0f;
    return true;
}

void Scroll(float delta)
{
    if (!g_World || !std::isfinite(delta)) return;
    float steps = 0.0f;
    scrollRemainder = std::modf(scrollRemainder + delta, &steps);
    if (steps == 0.0f) return;

    const auto page = CurrentPage(SelectableBlocks());
    const auto selected = std::find(page.begin(), page.end(), g_HandBlockId);
    const int current = selected == page.end() ? 0 : static_cast<int>(selected - page.begin());
    const int count = static_cast<int>(page.size());
    const int shift = static_cast<int>(std::fmod(steps, static_cast<float>(count)));
    g_HandBlockId = page[(current - shift + count) % count];
}

bool SelectAtUv(glm::vec2 uv)
{
    if (!g_World || lastLayout.scale <= 0.0f || !std::isfinite(uv.x) || !std::isfinite(uv.y)) return false;
    const glm::vec2 point = uv * lastLayout.viewSize;
    if (!Contains(point, lastLayout.PanelOrigin(), lastLayout.PanelSize())) return false;

    if (lastLayout.hasPages) {
        const glm::vec2 buttonSize = lastLayout.scale * glm::vec2(kPageButtonWidth, kSlotSize);
        if (Contains(point, lastLayout.PageButtonOrigin(-1), buttonSize)) return ChangePage(-1);
        if (Contains(point, lastLayout.PageButtonOrigin(1), buttonSize)) return ChangePage(1);
    }

    const auto page = CurrentPage(SelectableBlocks());
    const glm::vec2 slotSize(kSlotSize * lastLayout.scale);
    for (int i = 0; i < static_cast<int>(page.size()); ++i) {
        if (Contains(point, lastLayout.SlotOrigin(i), slotSize)) {
            g_HandBlockId = page[i];
            scrollRemainder = 0.0f;
            break;
        }
    }
    return true;
}

} // namespace UI::VoxelHotbar
