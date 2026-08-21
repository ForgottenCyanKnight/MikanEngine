# GPU 蒙皮：UBO 定稿现状与 SSBO 恢复/拓展指南

> 2026-08-06 更新。本文记录当前蒙皮方案的实现位置，以及未来恢复 SSBO（或做上限拓展）的完整步骤。
> 背景：曾误判"vertex 动态索引 UBO 数组在本机 NVIDIA 桌面触发 VK_ERROR_DEVICE_LOST"而改用 SSBO；
> 复测证实该崩溃是 **boneBufInfo 悬垂指针 UB + 未 clamp 越界** 叠加所致，修复后 **UBO 固定 64 稳定**（headless 300 帧 glb + 120 帧 obj 无崩溃）。
> 因移动端 UBO 走 uniform 快路径（性能更优），最终定稿 **UBO 固定 64 全平台**，SSBO 仅保留在备份目录。

## 一、当前实现（UBO 固定 64，勿随意回退）

| 位置 | 内容 |
|------|------|
| `engine/shaders/glsl/model.vert` | `#define MAX_BONES 128` + `layout(binding = 4) uniform BoneMatricesUBO { mat4 bones[MAX_BONES]; } boneData;`；蒙皮分支 `bones[clamp(ivec4(inBoneIDs), 0, MAX_BONES-1)]` 加权 |
| `include/Rendering/ModelLoader.h` | `inline constexpr int MAX_BONES = 128;`（C++ 与 shader 同步；128 骨骼=8KB UBO，低于 Vulkan 16KB 规范下限） |
| `include/Rendering/DescriptorSetCache.h` | `bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER`（stageFlags=VERTEX）；`poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` |
| `src/Rendering/ModelRenderer.cpp` 材质回调（SetupDescriptorSets 内） | `boneBufInfo` **声明在回调函数体级**（勿放块内——曾因 pBufferInfo 悬垂 UB 崩溃）；binding 4 无条件写 pBufferInfo（buffer=uniformBuffer, range=MAX_BONES×64） |
| `src/Rendering/ModelRenderer.cpp` `LoadModel` 开头 | `DescriptorSetCache::GetInstance().ClearCache()`（descriptor 类型变化后必须清缓存，否则长驻编辑器进程命中旧 set → binding 无资源 → 不渲染） |
| `src/Rendering/ModelRenderer.cpp` `CreateUniformBuffer` | buffer size = `MAX_BONES * sizeof(glm::mat4)` = 8KB；usage = `UNIFORM_BUFFER|UNIFORM_TEXEL_BUFFER|STORAGE`（兼容保留）；`boneBufferView` 创建残留（未使用，无害） |

验证方法：`descriptor callback: writes=2 b4=WRITTEN buf=... range=8192`（8KB=128 骨骼）+ headless 跑骨骼模型无 DEVICE_LOST。

## 二、恢复 SSBO 的步骤（反向修改，4 处）

SSBO 版本备份在项目根 `_ssbo_bak_050725/`（含 `model.vert`、`DescriptorSetCache.h`、`ModelRenderer.cpp` 三个文件，可直接对照或整份替换）。

1. **model.vert**：UBO 声明改回无界 SSBO：
   ```glsl
   layout(binding = 4) readonly buffer BoneMatricesSSBO { mat4 bones[]; } boneData;
   ```
   （蒙皮分支 `boneData.bones[skinIds.x]` 访问代码两种声明通用，无需改）
2. **DescriptorSetCache.h**：`bindings[4].descriptorType` → `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`；`poolSizes[1].type` → `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`
3. **ModelRenderer.cpp 材质回调**：binding 4 写入 `descriptorType` → `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`（`boneBufInfo` 保持在函数体级；`ClearCache()` 保留）
4. **重新编译**：`tools/compile_shaders.ps1`（重编 model.vert.spv）→ `tools/build.ps1 -Target EngineMain`

注意：UBO/SSBO 切换后必须清 descriptor 缓存（LoadModel 已自动），否则长驻编辑器进程命中旧类型 set。

## 三、未来上限拓展方案（暂缓，备忘）

**约束**：UBO 数组大小受 `maxUniformBufferRange` 限制（Vulkan 规范最小保证 16KB → 最多 256 骨骼贴边）；SSBO 无上限。`MAX_BONES` 是编译期常量（C++ `ModelLoader.h` 与 shader `model.vert` 同步）。

- **已实施**：`MAX_BONES` 64 → **128**（2026-08-06，8KB UBO 全设备合法，覆盖顶配移动角色）
- **完整双路径**（超 256 骨骼的极端模型）：骨骼数 >128/256 用 SSBO。需要：
  - shader 双变体（UBO 版 + SSBO 版，`#define` 编译两次）
  - **两套 DescriptorSetCache**（UBO layout + SSBO layout——注意 set layout 是全引擎共享的，同一进程内 binding 4 不能逐模型混用两种类型）
  - 加载模型时按骨骼数选类型 + pipeline 按模型分派
