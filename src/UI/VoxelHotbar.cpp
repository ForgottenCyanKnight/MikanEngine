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
#include <string>

namespace UI::VoxelHotbar {
namespace {

constexpr std::array kBlocks{
    BlockManager::GRASSBLOCK, BlockManager::DIRT, BlockManager::STONE,
    BlockManager::SAND, BlockManager::WOOD, BlockManager::PLANK,
    BlockManager::BRICK, BlockManager::GLASS, BlockManager::LEAVES};
constexpr int kSlotCount = static_cast<int>(kBlocks.size());
constexpr int kAtlasTiles = 16;
constexpr int kLayer = 90;
constexpr float kSlotSize = 50.0f;
constexpr float kGap = 5.0f;
constexpr float kPadding = 7.0f;
constexpr float kBottomMargin = 18.0f;
constexpr float kBarWidth = kSlotCount * kSlotSize + (kSlotCount - 1) * kGap + kPadding * 2.0f;
constexpr float kBarHeight = kSlotSize + kPadding * 2.0f;

struct Layout {
    glm::vec2 viewSize{0.0f};
    glm::vec2 origin{0.0f};
    float scale = 0.0f;

    glm::vec2 SlotOrigin(int index) const
    {
        return origin + scale * glm::vec2(kPadding + index * (kSlotSize + kGap), kPadding);
    }
};

Layout lastLayout;
float scrollRemainder = 0.0f;

Layout MakeLayout(uint32_t width, uint32_t height, bool touchMode, float bottomInset)
{
    const glm::vec2 viewSize(width, height);
    const float desiredScale = touchMode ? viewSize.y / 480.0f
                                        : std::clamp(viewSize.y / 720.0f, 0.7f, 1.5f);
    const float scale = std::min({
        desiredScale,
        (viewSize.x - 16.0f) / kBarWidth,
        (viewSize.y - bottomInset) / (kBarHeight + kBottomMargin)});
    return {viewSize, {(viewSize.x - kBarWidth * scale) * 0.5f,
                       viewSize.y - bottomInset - (kBarHeight + kBottomMargin) * scale}, scale};
}

glm::vec4 Color(float r, float g, float b, float a = 1.0f)
{
    return {r, g, b, a * GetUIOpacity()};
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

    lastLayout = MakeLayout(width, height, touchMode, bottomInset);
    const float scale = lastLayout.scale;
    if (scale <= 0.0f) return;
    const glm::vec2 panelSize = scale * glm::vec2(kBarWidth, kBarHeight);
    renderer.DrawRect(lastLayout.origin + glm::vec2(0.0f, scale * 3.0f), panelSize,
                      Color(0.04f, 0.05f, 0.05f, 0.42f), kLayer);
    renderer.DrawRect(lastLayout.origin, panelSize, Color(0.12f, 0.15f, 0.15f, 0.94f), kLayer + 1);
    renderer.DrawRect(lastLayout.origin, {panelSize.x, scale},
                      Color(0.66f, 0.73f, 0.70f, 0.42f), kLayer + 2);

    const VkDescriptorSet atlas = g_WorldRenderer->GetAtlasDescriptor();
    const glm::uvec2 atlasSize = g_WorldRenderer->GetAtlasSize();
    const bool hasAtlas = atlas != VK_NULL_HANDLE && atlasSize.x > 0 && atlasSize.y > 0;
    const glm::vec2 inset = hasAtlas ? 0.5f / glm::vec2(atlasSize) : glm::vec2(0.0f);
    auto& text = TextRenderer::GetInstance();
    for (int i = 0; i < kSlotCount; ++i) {
        const glm::vec2 pos = lastLayout.SlotOrigin(i);
        const bool selected = kBlocks[i] == g_HandBlockId;
        const float border = (selected ? 2.0f : 1.0f) * scale;
        const float size = kSlotSize * scale;
        renderer.DrawRect(pos, glm::vec2(size),
                          selected ? Color(0.96f, 0.88f, 0.58f) : Color(0.39f, 0.46f, 0.44f, 0.65f),
                          kLayer + 3);
        renderer.DrawRect(pos + glm::vec2(border), glm::vec2(size - 2.0f * border),
                          selected ? Color(0.25f, 0.27f, 0.20f) : Color(0.16f, 0.19f, 0.19f),
                          kLayer + 4);

        if (hasAtlas) {
            DrawBlockPreview(renderer, atlas, inset, kBlocks[i], pos, scale);
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
    if (key >= SDLK_1 && key <= SDLK_9) {
        g_HandBlockId = kBlocks[key - SDLK_1];
    } else if (key >= SDLK_KP_1 && key <= SDLK_KP_9) {
        g_HandBlockId = kBlocks[key - SDLK_KP_1];
    } else {
        return false;
    }
    scrollRemainder = 0.0f;
    return true;
}

void Scroll(float delta)
{
    if (!g_World || !std::isfinite(delta)) return;
    float steps = 0.0f;
    scrollRemainder = std::modf(scrollRemainder + delta, &steps);
    if (steps == 0.0f) return;

    const auto selected = std::find(kBlocks.begin(), kBlocks.end(), g_HandBlockId);
    const int current = selected == kBlocks.end() ? 0 : static_cast<int>(selected - kBlocks.begin());
    const int shift = static_cast<int>(std::fmod(steps, static_cast<float>(kSlotCount)));
    g_HandBlockId = kBlocks[(current - shift + kSlotCount) % kSlotCount];
}

bool SelectAtUv(glm::vec2 uv)
{
    if (!g_World || lastLayout.scale <= 0.0f || !std::isfinite(uv.x) || !std::isfinite(uv.y)) return false;
    const glm::vec2 point = uv * lastLayout.viewSize;
    const float size = kSlotSize * lastLayout.scale;
    for (int i = 0; i < kSlotCount; ++i) {
        const glm::vec2 slot = lastLayout.SlotOrigin(i);
        if (point.x >= slot.x && point.x < slot.x + size && point.y >= slot.y && point.y < slot.y + size) {
            g_HandBlockId = kBlocks[i];
            scrollRemainder = 0.0f;
            return true;
        }
    }
    return false;
}

} // namespace UI::VoxelHotbar
