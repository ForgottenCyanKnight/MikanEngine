# Third-party notices

MikanEngine's own source code is released under the [MIT License](LICENSE).
The repository also contains third-party source code, prebuilt libraries and
example assets. Their upstream licenses remain applicable and are not replaced
by MikanEngine's license.

This file is an index of the bundled components. The upstream notice or license
is authoritative; binary release packages should retain the applicable notices.

## Dependencies and tools

| Component | Repository location | Upstream notice |
|---|---|---|
| SDL3 | `dependencies/SDL3/`, `lib/x64/`, Android AAR | [SDL license](https://github.com/libsdl-org/SDL/blob/main/LICENSE.txt) |
| SDL3_image | `dependencies/SDL3_image/`, `lib/x64/`, Android AAR | [SDL_image license](https://github.com/libsdl-org/SDL_image/blob/main/LICENSE.txt) |
| Assimp | `dependencies/assimp/`, `lib/x64/`, `android/app/libs/` | [Assimp license](https://github.com/assimp/assimp/blob/master/LICENSE) |
| Jolt Physics | `dependencies/JoltPhysics/` | [Jolt license](https://github.com/jrouwe/JoltPhysics/blob/master/LICENSE) |
| Box2D | `dependencies/box2d/` | [`dependencies/box2d/LICENSE`](dependencies/box2d/LICENSE) |
| Dear ImGui | `dependencies/imgui/` | [Dear ImGui license](https://github.com/ocornut/imgui/blob/master/LICENSE.txt) |
| ImGuizmo | `dependencies/ImGuizmo/` | [`dependencies/ImGuizmo/LICENSE`](dependencies/ImGuizmo/LICENSE) |
| GLM | `dependencies/glm/` | [`dependencies/glm/copying.txt`](dependencies/glm/copying.txt) |
| miniaudio | `dependencies/miniaudio/` | [miniaudio license](https://github.com/mackron/miniaudio/blob/master/LICENSE) |
| msdfgen | `dependencies/msdfgen/` | [msdfgen license](https://github.com/Chlumsky/msdfgen/blob/master/LICENSE.txt) |
| stb | `dependencies/stb/` | [stb license](https://github.com/nothings/stb/blob/master/LICENSE) |
| tinyxml2 | `dependencies/tinyxml2/` | [tinyxml2 license](https://github.com/leethomason/tinyxml2/blob/master/LICENSE.txt) |
| zlib | `dependencies/zlib/` | [zlib license](https://zlib.net/zlib_license.html) |
| Vulkan headers | `dependencies/vulkan/` | [Vulkan-Headers license](https://github.com/KhronosGroup/Vulkan-Headers/blob/main/LICENSE.txt) |
| glslang and SPIR-V tools | `tools/glslang/` | [glslang license and notices](https://github.com/KhronosGroup/glslang/blob/main/LICENSE.txt) |
| KTX-Software | `tools/ktx/` | [KTX-Software license and notices](https://github.com/KhronosGroup/KTX-Software/blob/main/LICENSE.md) |

## Example project assets

| Asset | Location | Source and license |
|---|---|---|
| Universal Animation Library | `projects/third-person-navigation/animations/quaternius/` | [Quaternius](https://quaternius.com/packs/universalanimationlibrary.html), CC0 |
| Lantern 01 | `projects/third-person-navigation/models/2.0/Lantern/` | [Poly Haven](https://polyhaven.com/a/Lantern_01), CC0 |
| Island of Monkeys heightmap | `projects/third-person-navigation/terrain/prototype/` | See [`SOURCE.md`](projects/third-person-navigation/terrain/prototype/SOURCE.md), CC BY 4.0 |
| Terrain surface textures | `projects/third-person-navigation/terrain/prototype/materials/` | See [`SOURCE.md`](projects/third-person-navigation/terrain/prototype/SOURCE.md), CC0 |

The remaining project media should be accompanied by a source and license
record before being included in a binary redistribution. In particular, review
the demo audio and UI images under the selected project before publishing a
release package.
