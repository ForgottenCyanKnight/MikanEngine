#include "Core/ViewportResolution.h"
#include "Editor/ViewportControls.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

int main() {
    using namespace ViewportResolution;
    for (const auto& preset : Presets) {
        assert(preset.width * 9 == preset.height * 16);
        assert(Closest(preset.width, preset.height).width == preset.width);
        assert(!NeedsRebuild(preset.width, preset.height, preset.width, preset.height));
    }
    assert(Closest(1920, 1040).height == 1080);
    assert(Closest(-100, -100).width == 640);
    assert(Closest(std::numeric_limits<int>::max(), std::numeric_limits<int>::max()).width == 3840);
    assert(NeedsRebuild(1920, 1080, 1280, 720));
    // Window/panel extents change only the display rectangle, never render allocation.
    for (const auto available : {ImVec2(600, 900), ImVec2(1900, 500), ImVec2(1280, 720)}) {
        const auto image = Editor::FitViewportImage(available);
        assert(image.x <= available.x + 0.001f && image.y <= available.y + 0.001f);
        assert(std::abs(image.x * 9 - image.y * 16) < 0.01f);
        assert(!NeedsRebuild(1920, 1080, 1920, 1080));
    }
    std::cout << "PASS: fixed 16:9 presets, legacy normalization, resize policy and letterboxing\n";
}
