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
Source: https://github.com/NVIDIA/DLSS. Unmodified SDK files, complete license and provenance/hashes are retained in dependencies/DLSS/LICENSE.txt and dependencies/DLSS-sdk-manifest.json. The GGX specular guide approximation is adapted from the RR integration guide appendix; the upstream SDK terms apply to that material.
Desktop builds deploy only the RR release runtime and its license beside Game.dll. NVIDIA-only capability gating selects RR; non-NVIDIA systems retain NRD/TAA. The upstream license is proprietary and is not replaced by the engine MIT license.

## NVIDIA Streamline 2.14.1 (optional DLSS Frame Generation)
Headers and release runtimes in dependencies/Streamline. See license.txt and bin/nvngx_dlss.license.txt, bin/reflex.license.txt. Source: https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1

## NVIDIA Falcor ReSTIR PT Enhanced

Upstream: NVIDIA Falcor 9.0, commit 759aad033ff610fb0d82c74f7e0a508d0096d5f2.
Source: https://github.com/NVIDIAGameWorks/Falcor/tree/759aad033ff610fb0d82c74f7e0a508d0096d5f2/Source/Modules/ReSTIRPathTracing
The Enhanced shader integration and reciprocal neighbor data retain NVIDIA's
BSD-3-Clause notice in dependencies/Falcor/LICENSE.txt and the shader sources.
Engine-specific scene/material integration remains under the engine's MIT license.
Light selection uses the engine's pre-tree power/area CDF; the optional position/
normal-aware local proposal remains available for NEE. No external light-tree
implementation is included in the current runtime.


---

# Third-party notices / 第三方许可证

MikanEngine 自有代码的根 LICENSE 不替代第三方代码、SDK、工具、数据和素材的许可证。
本目录收集当前识别到的 vendored 代码与 SDK 的原始许可文本；不对它们统一重授权。

## 原文与来源

原文位于 [dependencies](dependencies/license-manifest.json)，依赖目录内也补入了缺少的独立文件。
manifest.json 记录每份原文的来源路径、上游 URL/版本引用、大小及 SHA-256。已有 NRD、DLSS、Streamline 等许可证保持原样。
嵌入式许可证直接从本地源文件原文提取；无法确认确切 vendored commit 的条目明确注明，不把下载时的 master 当作本地代码版本。

| 组件 | 本地版本依据 | 范围 | 许可/声明文件数 |
|---|---|---|---|
| imgui | 1.92.6 WIP; upstream v1.92.6 license snapshot | runtime | 4 |
| assimp-runtime | Windows DLL aiGetVersion=5.0.0 | runtime | 1 |
| JoltPhysics | JPH_VERSION 5.5.0 | runtime | 1 |
| msdfgen | local config says 1.2/2025; exact upstream commit unrecorded | runtime | 1 |
| SDL3 | headers 3.4.2 | runtime | 2 |
| SDL3_image | headers 3.2.4 | runtime | 1 |
| zlib | ZLIB_VERSION 1.3 | runtime | 1 |
| tinyxml2 | header 11.0.0 | runtime | 1 |
| stb | vendored stb_truetype/stb_vorbis; exact commit unrecorded | runtime | 3 |
| json | header 3.11.3 | runtime | 1 |
| vma | vendored header copyright 2017-2025; exact tag unrecorded | runtime | 2 |
| vulkan | VK_HEADER_VERSION 346 | runtime | 3 |
| Saba | adapted MMD sources; exact original commit unrecorded | runtime | 1 |
| glslang | validator reports 16.2.0 | build-tool | 1 |
| KTX | toktx reports 4.4.2 | runtime | 17 |
| NRD | NRD 4.18.0; existing NVIDIA RTX SDKs license | runtime | 1 |
| DLSS | existing NVIDIA RTX SDKs license; see DLSS-sdk-manifest.json | runtime | 1 |
| Streamline | vendored Streamline SDK | runtime | 3 |
| Box2D | vendored Box2D | runtime | 1 |
| ImGuizmo | vendored ImGuizmo | runtime | 1 |
| GLM | GLM 1.0.0 | runtime | 1 |
| DXC | vendored original file | build-tool | 4 |
| STBN | existing STBN asset license; original notice records commit 48b2839e4d8b7f0202ac72c6b0ae720d235a5b8b | runtime-data | 1 |
| ShaderMake | NRD CMake fetch 9c978c37ccd1c00b8669d633735823fe25ee3647 | runtime/build | 2 |
| MathLib | NRD CMake fetch v11 | runtime/build | 1 |
| SDL3_image-codecs | official SDL_image 3.2.4 VC x64 optional notices | optional-runtime | 5 |
| miniaudio | miniaudio 0.11.25 | runtime | 1 |
| FastNoiseLite | FastNoiseLite 1.1.1 | runtime | 1 |
| assimp-headers | vendored headers copyright 2006-2026; separate from 5.0 runtime DLL | runtime | 1 |
| Falcor | 9.0, commit 759aad033ff610fb0d82c74f7e0a508d0096d5f2 | rendering | 1 |

## 特殊组成

- NRD、DLSS 等保留各自现有 NVIDIA SDK 协议，不替换成 MIT。
- Enhanced ReSTIR 保留 Falcor 的 NVIDIA BSD-3-Clause 声明；引擎选光恢复为本地功率/面积 CDF，移除外部灯光树实现。
- KTX 包含 Apache-2.0 和多种第三方/例外文本，LICENSE.md 不是唯一许可文件。
- Assimp 的本地新版头文件声明与 5.0.0 DLL 的原始许可证分别保留。
- SDL_image 官方 3.2.4 包中的 x64 可选编解码声明一并保存；这不表示发布包实际包含每种可选 DLL。
- 模型、纹理、字体、音乐及游戏项目素材需按各自来源保留授权，本代码库清单不替这些素材授予使用权。

## 发布与验证

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/collect_third_party_licenses.ps1 -ValidateOnly
powershell -NoProfile -ExecutionPolicy Bypass -File tools/collect_third_party_licenses.ps1 -OutDir <release-directory>
```

publish.ps1 会先校验原文与哈希，再将本文件、引擎根 LICENSE 和 dependencies/ 对应依赖目录（含来源清单）复制到发布目录。
校验发现缺失或被修改的原文时失败，不会悄悄发布不完整的许可目录。无需联网收集许可证。
增加或升级第三方依赖时，应同步其原始许可和 manifest，保留 SDK 原有版权/商标要求。
