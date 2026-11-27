#include "Core/KinematicTileCollision2D.h"
#include "Core/TmxLoader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/common.hpp>
#include <utility>

namespace {

constexpr float kEdgeInset = 0.001f;
constexpr int kCollisionIterations = 10;

bool IsFinite(glm::vec2 value) {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

} // namespace

bool KinematicTileCollision2D::Build(const Tmx::Map& map, glm::vec2 origin,
                                     std::string_view fallbackSolidLayer,
                                     std::string_view excludedLayer) {
    if (map.width <= 0 || map.height <= 0 || map.tileWidth <= 0 || map.tileHeight <= 0 ||
        !IsFinite(origin)) {
        return false;
    }
    const auto width = static_cast<size_t>(map.width);
    const auto height = static_cast<size_t>(map.height);
    std::vector<bool> solid;
    if (height > solid.max_size() / width) return false;
    solid.resize(width * height, false);

    const bool hasCollisionTiles = std::any_of(map.tilesets.begin(), map.tilesets.end(),
        [](const auto& tileset) { return !tileset.collidable.empty(); });
    for (const auto& layer : map.layers) {
        if (!layer.visible || layer.name == excludedLayer) continue;
        const bool isFallbackSolidLayer = !hasCollisionTiles && layer.name == fallbackSolidLayer;
        if (!hasCollisionTiles && !isFallbackSolidLayer) continue;

        const int rows = std::min(layer.height, map.height);
        const int columns = std::min(layer.width, map.width);
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < columns; ++x) {
                const auto source = static_cast<size_t>(y) * layer.width + x;
                if (source >= layer.gids.size()) break;
                const int gid = static_cast<uint32_t>(layer.gids[source]) & 0x0fffffffu;
                if (gid == 0) continue;

                const Tmx::Tileset* tileset = hasCollisionTiles ? map.FindTileset(gid) : nullptr;
                const bool isSolid = isFallbackSolidLayer ||
                    (tileset && tileset->firstGid > 0 && tileset->collidable.count(gid - tileset->firstGid) != 0);
                if (isSolid) solid[static_cast<size_t>(y) * width + x] = true;
            }
        }
    }

    m_grid = {map.width, map.height};
    m_tileSize = {map.tileWidth, map.tileHeight};
    m_origin = origin;
    m_solid = std::move(solid);
    return true;
}

bool KinematicTileCollision2D::Overlaps(glm::vec2 position, glm::vec2 size) const {
    if (m_solid.empty() || !IsFinite(position) || !IsFinite(size) ||
        size.x <= 0.0f || size.y <= 0.0f) {
        return false;
    }

    const glm::vec2 relative = position - m_origin;
    const glm::vec2 inset = glm::min(size * 0.5f, glm::vec2(kEdgeInset));
    const glm::vec2 first = glm::floor(relative / m_tileSize);
    const glm::vec2 last = glm::floor((relative + size - inset) / m_tileSize);
    if (first.x < 0.0f || static_cast<double>(last.x) >= m_grid.x) return true;
    if (last.y < 0.0f || static_cast<double>(first.y) >= m_grid.y ||
        first.x > last.x || first.y > last.y) {
        return false;
    }

    const int left = static_cast<int>(first.x);
    const int right = static_cast<int>(last.x);
    const int top = static_cast<int>(std::max(first.y, 0.0f));
    const int bottom = static_cast<int>(std::min<double>(last.y, m_grid.y - 1));
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            if (m_solid[static_cast<size_t>(y) * m_grid.x + x]) return true;
        }
    }
    return false;
}

bool KinematicTileCollision2D::MoveAxis(glm::vec2& position, glm::vec2 size,
                                       float distance, int axis) const {
    glm::vec2 offset(0.0f);
    offset[axis] = distance;
    if (!Overlaps(position + offset, size)) {
        position += offset;
        return false;
    }

    float free = 0.0f;
    float blocked = 1.0f;
    for (int i = 0; i < kCollisionIterations; ++i) {
        const float middle = (free + blocked) * 0.5f;
        if (Overlaps(position + middle * offset, size)) blocked = middle;
        else free = middle;
    }
    position += free * offset;
    return true;
}

KinematicTileCollision2D::MoveResult KinematicTileCollision2D::Move(
        glm::vec2 position, glm::vec2 size, glm::vec2 displacement) const {
    MoveResult result{position};
    if (!IsFinite(position) || !IsFinite(size) || !IsFinite(displacement) ||
        size.x <= 0.0f || size.y <= 0.0f) {
        return result;
    }
    for (int axis = 0; axis < 2; ++axis) {
        const float distance = displacement[axis];
        const float stepLimit = m_tileSize[axis];
        const float target = result.position[axis] + distance;
        if (!std::isfinite(target)) continue;
        if (m_solid.empty() || std::abs(distance) <= stepLimit) {
            result.blocked[axis] = MoveAxis(result.position, size, distance, axis);
            continue;
        }

        const bool vertical = axis == 1;
        const float airAbove = m_origin.y - size.y;
        const float airBelow = m_origin.y + m_grid.y * m_tileSize.y;
        // Outside the vertical extent of the grid we can cross empty air
        // directly, even when a respawn or launch begins far from the map.
        if (vertical && !Overlaps(result.position, size)) {
            if (distance > 0.0f && result.position.y < airAbove) {
                result.position.y = std::min(airAbove, target);
            } else if (distance < 0.0f && result.position.y > airBelow) {
                result.position.y = std::max(airBelow, target);
            }
        }
        while (result.position[axis] != target) {
            const float previous = result.position[axis];
            const double remaining = static_cast<double>(target) - previous;
            const bool movingIntoAir = vertical && (remaining > 0.0 ? previous >= airBelow : previous <= airAbove);
            const bool lastStep = movingIntoAir || std::abs(remaining) <= stepLimit;
            const float step = lastStep ? static_cast<float>(remaining) : std::copysign(stepLimit, distance);
            result.blocked[axis] = MoveAxis(result.position, size, step, axis);
            if (result.blocked[axis] || lastStep || result.position[axis] == previous) break;
        }
    }
    result.grounded = result.blocked.y && displacement.y > 0.0f;
    return result;
}
