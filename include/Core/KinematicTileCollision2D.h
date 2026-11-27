#pragma once

#include "Platform/Export.h"

#include <glm/vec2.hpp>
#include <string_view>
#include <vector>

namespace Tmx { struct Map; }

// Pixel-space tile collision for fixed-step 2D platformer movement.
class MIKAN_API KinematicTileCollision2D {
public:
    struct MoveResult {
        glm::vec2 position;
        glm::bvec2 blocked{false};
        bool grounded = false;
    };

    // Only visible layers contribute. When no tileset marks collision tiles,
    // occupied tiles in fallbackSolidLayer supply the collision grid instead.
    bool Build(const Tmx::Map& map, glm::vec2 origin, std::string_view fallbackSolidLayer,
               std::string_view excludedLayer);

    // AABBs use a top-left position and positive extents, with Y increasing
    // downward. The map's left and right edges block movement. Its top and
    // bottom are open so games can allow jumps and falls beyond the map.
    bool Overlaps(glm::vec2 position, glm::vec2 size) const;

    // Resolves X before Y, with ten bisection steps per blocked axis.
    // Larger displacements are subdivided to prevent crossing solid tiles.
    MoveResult Move(glm::vec2 position, glm::vec2 size, glm::vec2 displacement) const;

private:
    bool MoveAxis(glm::vec2& position, glm::vec2 size, float distance, int axis) const;

    glm::ivec2 m_grid{0};
    glm::vec2 m_tileSize{0.0f};
    glm::vec2 m_origin{0.0f};
    std::vector<bool> m_solid;
};
