#pragma once

#include "Platform/Export.h"

#include <SDL3/SDL_keycode.h>
#include <glm/vec2.hpp>
#include <cstdint>

class Renderer2D;

namespace UI::VoxelHotbar {

MIKAN_API void Draw(Renderer2D& renderer, uint32_t width, uint32_t height, bool touchMode, float bottomInset);
MIKAN_API bool SelectKey(SDL_Keycode key);
MIKAN_API void Scroll(float delta);
MIKAN_API bool SelectAtUv(glm::vec2 uv);

} // namespace UI::VoxelHotbar
