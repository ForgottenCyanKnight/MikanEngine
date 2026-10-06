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

## NVIDIA Real-Time Denoisers (NRD)
NRD 4.18.0, commit c3d8999f8b30dca3b1c07d9aeab235b29d9b1a28.
Source: https://github.com/NVIDIA-RTX/NRD
License: dependencies/NRD/LICENSE.txt. The denoiser shader adapters use encoding formulas documented in NRD/Shaders/NRD.hlsli.

## NVIDIA spatiotemporal blue noise (STBN)

Official vector2 assets, commit 48b2839e4d8b7f0202ac72c6b0ae720d235a5b8b.
Source: https://github.com/NVIDIA-RTX/STBN (Assets/STBN.zip).
Local asset, provenance and license: engine/textures/stbn/.
Upstream License.txt contains both the non-commercial generation license and
the commercial NVIDIA RTX SDK license and supplement. The complete unmodified
terms are retained in engine/textures/stbn/License.txt.

## NVIDIA DLSS Ray Reconstruction

Official NVIDIA DLSS/NGX SDK, commit 374959484e79a640feaba44c93ac8cfb0a03f5b5; signed RR release runtime nvngx_dlssd.dll version 310.9.1.0.
Source: https://github.com/NVIDIA/DLSS. Unmodified SDK files, complete license and provenance/hashes are retained in dependencies/DLSS/LICENSE.txt and dependencies/DLSS/SDK_MANIFEST.json. The GGX specular guide approximation is adapted from the RR integration guide appendix; the upstream SDK terms apply to that material.
Desktop builds deploy only the RR release runtime and its license beside Game.dll. NVIDIA-only capability gating selects RR; non-NVIDIA systems retain NRD/TAA. The upstream license is proprietary and is not replaced by the engine MIT license.

## NVIDIA Streamline 2.14.1 (optional DLSS Frame Generation)
Headers and release runtimes in dependencies/Streamline. See license.txt and bin/nvngx_dlss.license.txt, bin/reflex.license.txt. Source: https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1
