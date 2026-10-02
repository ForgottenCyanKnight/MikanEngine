#include "ModelRenderer.h"
#include "ModelRendererInternals.h"
#include "Core/RenderGlobals.h"
#include "VulkanManager.h"
#include "Core/Log.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

// 蒙皮 UBO 3 帧槽动态偏移（dynamic UBO——动画更新处设值，10 处 descriptor 绑定读取；无动画模型恒 0）
uint32_t g_BoneDynamicOffset = 0;

namespace {

// A palette is a complete MAX_BONES pose. Keep enough slots for the current
// scene and the shadow/SceneView/GameView recordings of a frame while retaining
// a fixed descriptor (no per-draw descriptor allocation).
constexpr size_t kSharedBonePaletteCount = 512;
constexpr size_t kSharedBonePaletteMatricesPerFrame =
    kSharedBonePaletteCount * MAX_BONES;
constexpr VkDeviceSize kSharedBonePaletteSize =
    static_cast<VkDeviceSize>(kSharedBonePaletteMatricesPerFrame) *
    ModelRenderData::MAX_FRAMES_IN_FLIGHT * sizeof(glm::mat4);

VulkanBuffer g_SharedBonePalette;
std::unordered_map<const void*, uint32_t> g_SharedBonePaletteBases;
size_t g_SharedBonePaletteCursor = 0;
uint64_t g_SharedBonePaletteFrameSerial = UINT64_MAX;
bool g_SharedBonePaletteOverflowReported = false;

} // namespace

namespace ModelRendererDetail {

void EnsureSharedBonePalette()
{
    if (g_SharedBonePalette.GetBuffer() != VK_NULL_HANDLE) {
        if (g_SharedBonePalette.GetMappedPtr() == nullptr) {
            g_SharedBonePalette.Map();
        }
        return;
    }
    if (g_Device == VK_NULL_HANDLE) return;

    if (!g_SharedBonePalette.Create(
            kSharedBonePaletteSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        LOGE("[ModelRenderer] failed to create shared bone palette (%llu bytes)",
                     static_cast<unsigned long long>(kSharedBonePaletteSize));
        return;
    }
    g_SharedBonePalette.Map();
    if (g_SharedBonePalette.GetMappedPtr() == nullptr) {
        LOGE("[ModelRenderer] failed to map shared bone palette");
        g_SharedBonePalette.Cleanup();
    }
}

void BeginSharedBonePaletteFrame()
{
    g_SharedBonePaletteFrameSerial = GetCurrentFrameSerial();
    g_SharedBonePaletteCursor = 0;
    g_SharedBonePaletteBases.clear();
    g_SharedBonePaletteOverflowReported = false;
}

void ReleaseSharedBonePalette()
{
    g_SharedBonePaletteBases.clear();
    g_SharedBonePaletteCursor = 0;
    g_SharedBonePaletteFrameSerial = UINT64_MAX;
    g_SharedBonePaletteOverflowReported = false;
    g_SharedBonePalette.Cleanup();
}

VkBuffer GetSharedBonePaletteBuffer()
{
    return g_SharedBonePalette.GetBuffer();
}

uint32_t GetSharedBonePaletteBase(const ModelRenderer* renderer)
{
    if (renderer == nullptr || !renderer->HasSkinning()) {
        return kInvalidSharedBonePaletteBase;
    }

    EnsureSharedBonePalette();
    void* mapped = g_SharedBonePalette.GetMappedPtr();
    if (mapped == nullptr) return kInvalidSharedBonePaletteBase;

    const uint64_t frameSerial = GetCurrentFrameSerial();
    if (g_SharedBonePaletteFrameSerial != frameSerial) {
        BeginSharedBonePaletteFrame();
    }

    // Normal animation renderers expose the shared sampled-pose identity, so
    // equal poses occupy one GPU palette slot.  VMD/custom poses deliberately
    // expose no token and remain isolated by renderer pointer.
    const void* paletteKey = renderer->GetAnimationPoseIdentity();
    if (paletteKey == nullptr) paletteKey = renderer;

    const auto existing = g_SharedBonePaletteBases.find(paletteKey);
    if (existing != g_SharedBonePaletteBases.end()) {
        return existing->second;
    }
    if (g_SharedBonePaletteCursor >= kSharedBonePaletteCount) {
        if (!g_SharedBonePaletteOverflowReported) {
            g_SharedBonePaletteOverflowReported = true;
            LOGW("[ModelRenderer] shared bone palette capacity exhausted (%zu poses); "
                         "falling back to per-renderer bone UBO for overflow",
                         kSharedBonePaletteCount);
        }
        return kInvalidSharedBonePaletteBase;
    }

    const uint32_t frameIndex = GetCurrentFrameIndex() % ModelRenderData::MAX_FRAMES_IN_FLIGHT;
    const size_t matrixBase = static_cast<size_t>(frameIndex) * kSharedBonePaletteMatricesPerFrame +
                              g_SharedBonePaletteCursor * MAX_BONES;
    const uint32_t paletteBase = static_cast<uint32_t>(matrixBase);
    auto* destination = static_cast<glm::mat4*>(mapped) + matrixBase;
    const auto& source = renderer->GetBoneMatrices();
    for (size_t bone = 0; bone < MAX_BONES; ++bone) {
        destination[bone] = bone < source.size() ? source[bone] : glm::mat4(1.0f);
    }

    ++g_SharedBonePaletteCursor;
    g_SharedBonePaletteBases.emplace(paletteKey, paletteBase);
    return paletteBase;
}

void ApplySharedBonePalette(ModelInstanceData& instance, const ModelRenderer* renderer)
{
    instance.skinData = glm::uvec4(0);
    const uint32_t paletteBase = GetSharedBonePaletteBase(renderer);
    if (paletteBase == kInvalidSharedBonePaletteBase) return;
    instance.skinData = glm::uvec4(paletteBase, 1u, 0u, 0u);
}

} // namespace ModelRendererDetail

void ModelRenderer::RefreshBoneMatricesAndSkinning(
    const std::vector<glm::mat4>* precomputedMatrices) {
    auto& md = m_ModelData;
    if (!md.hasSkinning && !md.hasAnimation) return;

    // A shared animation pose already contains global * offset matrices.  The
    // per-renderer path remains available for bind/VMD/custom poses.
    if (precomputedMatrices != nullptr && !precomputedMatrices->empty()) {
        md.boneMatrices = *precomputedMatrices;
        if (md.boneMatrices.size() < MAX_BONES) {
            md.boneMatrices.resize(MAX_BONES, glm::mat4(1.0f));
        } else if (md.boneMatrices.size() > MAX_BONES) {
            md.boneMatrices.resize(MAX_BONES);
        }
    } else {
        // 蒙皮矩阵（global * offsetMatrix）先生成 CPU pose；无 GPU 资源的
        // animation-only renderer 也需要这份结果供共享 palette 上传。
        md.boneMatrices.assign(MAX_BONES, glm::mat4(1.0f));
        for (size_t i = 0; i < m_MeshData.bones.size() && i < MAX_BONES; i++) {
            md.boneMatrices[i] = m_MeshData.bones[i].globalTransform * m_MeshData.bones[i].offsetMatrix;
        }
    }

    // 有完整 GPU renderer 时继续写入旧 UBO，供单 renderer/溢出 fallback 使用。
    if (md.boneBufferMapped) {
        g_BoneDynamicOffset = (uint32_t)(GetCurrentFrameIndex() % ModelRenderData::MAX_FRAMES_IN_FLIGHT)
                            * (uint32_t)(MAX_BONES * sizeof(glm::mat4));
        memcpy((char*)md.boneBufferMapped + g_BoneDynamicOffset, md.boneMatrices.data(), MAX_BONES * sizeof(glm::mat4));
        // [diag-20260806] GPU 蒙皮数据验证：mapped buffer 内容 vs boneMatrices（应一致；数值应合理非飞点）
        {
            static int s_gpuSkinDiag = 0;
            if (s_gpuSkinDiag < 3) {
                s_gpuSkinDiag++;
                glm::mat4* mapped = (glm::mat4*)md.boneBufferMapped;
                LOGI("[diag] GPUskin: bm[0].c0=(%.3f,%.3f,%.3f,%.3f) mapped[0].c0=(%.3f,%.3f,%.3f,%.3f) mapped[0].c3=(%.3f,%.3f,%.3f,%.3f) bones=%zu",
                    md.boneMatrices[0][0][0], md.boneMatrices[0][0][1], md.boneMatrices[0][0][2], md.boneMatrices[0][0][3],
                    mapped[0][0][0], mapped[0][0][1], mapped[0][0][2], mapped[0][0][3],
                    mapped[0][3][0], mapped[0][3][1], mapped[0][3][2], mapped[0][3][3],
                    m_MeshData.bones.size());
            }
        }
    }

    // [diag] 蒙皮矩阵异常检测（交换链重建后蒙皮模型消失排查；仅打印前几次避免刷屏）
    {
        static int s_skinDiag = 0;
        bool anyBad = false;
        for (size_t i = 0; i < m_MeshData.bones.size() && i < MAX_BONES && !anyBad; i++) {
            const glm::mat4& bm = md.boneMatrices[i];
            for (int r = 0; r < 4 && !anyBad; r++) {
                for (int c = 0; c < 4 && !anyBad; c++) {
                    if (!std::isfinite(bm[r][c])) { anyBad = true; break; }
                }
            }
            if (anyBad && s_skinDiag < 5) {
                s_skinDiag++;
                LOGI("[ModelRenderer][diag] SKIN MATRIX BAD: path='%s' bone=%zu time=%.4f",
                       m_ModelData.modelPath.c_str(), i, md.animTime);
            }
        }
    }

    // ===== CPU 蒙皮（fallback：仅当 GPU 蒙皮关闭时执行；GPU 主路径由顶点着色器蒙皮）=====
    if (!md.mmdDeformed && !g_UseGpuSkinning && md.hasSkinning && md.skinnedBufferMapped &&
        md.skinnedSubMeshes.size() == m_MeshData.subMeshes.size()) {
        for (size_t s = 0; s < m_MeshData.subMeshes.size(); s++) {
            const auto& src = m_MeshData.subMeshes[s].vertices;
            auto& dst = md.skinnedSubMeshes[s];
            dst.resize(src.size());
            for (size_t i = 0; i < src.size(); i++) {
                const Vertex& v = src[i];
                dst[i] = v;
                // 解包压缩权重/ID（UNORM uint8 → 0..1；UINT8 直读）
                const glm::vec4 w = glm::vec4(v.BoneWeights) * (1.0f / 255.0f);
                const glm::vec3 vNormal = UnpackSnorm3(v.Normal);
                const glm::vec3 vTangent = UnpackSnorm3(v.Tangent);
                const glm::vec3 vBitangent = glm::normalize(glm::cross(vNormal, vTangent)) * (v.Tangent.w >= 0 ? 1.0f : -1.0f);
                const float tw = w.x + w.y + w.z + w.w;
                if (tw > 0.001f) {
                    glm::mat4 sm(0.0f);
                    if (w.x > 0.0f) sm += w.x * md.boneMatrices[v.BoneIDs.x];
                    if (w.y > 0.0f) sm += w.y * md.boneMatrices[v.BoneIDs.y];
                    if (w.z > 0.0f) sm += w.z * md.boneMatrices[v.BoneIDs.z];
                    if (w.w > 0.0f) sm += w.w * md.boneMatrices[v.BoneIDs.w];
                    dst[i].Position = glm::vec3(sm * glm::vec4(v.Position, 1.0f));
                    // 蒙皮后重编码压缩（法线/切线 SNORM + 手性）
                    const glm::vec3 newN = glm::normalize(glm::mat3(sm) * vNormal);
                    const glm::vec3 newT = glm::normalize(glm::mat3(sm) * vTangent);
                    const glm::vec3 newB = glm::normalize(glm::mat3(sm) * vBitangent);
                    const float hand = glm::dot(glm::cross(newN, newT), newB) >= 0.0f ? 1.0f : -1.0f;
                    dst[i].Normal  = PackSnorm3(newN);
                    dst[i].Tangent = PackSnorm3(newT, hand);
                }
                // CPU 蒙皮已完成：清空骨骼权重/ID，防止顶点着色器再次蒙皮（双重蒙皮导致姿态错乱）
                dst[i].BoneWeights = glm::u8vec4(0, 0, 0, 0);
                dst[i].BoneIDs = glm::u8vec4(0xFF, 0xFF, 0xFF, 0xFF);
            }
            memcpy((char*)md.skinnedBufferMapped + md.skinnedBufferOffsets[s],
                   dst.data(), dst.size() * sizeof(Vertex));
        }
    }
}

bool ModelRenderer::ApplyMmdVertices(const std::vector<glm::vec3>& positions,
    const std::vector<glm::vec3>& normals, const std::vector<glm::vec2>& uvs) {
    auto& md=m_ModelData;
    if (!m_MeshData.isMmd || !md.skinnedBufferMapped ||
        positions.size()!=normals.size() || positions.size()!=uvs.size()) return false;
    for (const auto& sm:m_MeshData.subMeshes) {
        if (sm.mmdVertexIndices.size()!=sm.vertices.size()) return false;
        for(auto index:sm.mmdVertexIndices) if(index>=positions.size()) return false;
    }
    const bool initializeVertices=!md.mmdDeformed;
    md.mmdDeformed=true;
    md.mmdUploadPending=true;
    glm::vec3 modelMin(FLT_MAX),modelMax(-FLT_MAX);
    for(size_t s=0;s<m_MeshData.subMeshes.size();++s) {
        const auto& sm=m_MeshData.subMeshes[s];auto& dst=md.skinnedSubMeshes[s];
        if(initializeVertices || dst.size()!=sm.vertices.size()) dst=sm.vertices;
        glm::vec3 mn(FLT_MAX),mx(-FLT_MAX);
        for(size_t i=0;i<dst.size();++i) {
            const auto index=sm.mmdVertexIndices[i];auto& v=dst[i];
            v.Position=positions[index];v.Normal=PackSnorm3(normals[index]);v.TexCoords=PackHalf2(uvs[index]);
            v.BoneIDs=glm::u8vec4(255);v.BoneWeights=glm::u8vec4(0);
            mn=glm::min(mn,v.Position);mx=glm::max(mx,v.Position);
        }
        // Recompute tangent frames from the deformed mesh and UVs.
        // Retain storage between poses; accumulation order and tangent math
        // remain identical to preserve the PMX normal-map shading.
        m_MmdTangents.assign(dst.size(),glm::vec3(0));
        m_MmdBitangents.assign(dst.size(),glm::vec3(0));
        auto& tangents=m_MmdTangents;auto& bitangents=m_MmdBitangents;
        for(size_t i=0;i+2<sm.indices.size();i+=3) {
            const auto a=sm.indices[i],b=sm.indices[i+1],c=sm.indices[i+2];
            const auto e1=dst[b].Position-dst[a].Position,e2=dst[c].Position-dst[a].Position;
            const auto d1=UnpackHalf2(dst[b].TexCoords)-UnpackHalf2(dst[a].TexCoords),d2=UnpackHalf2(dst[c].TexCoords)-UnpackHalf2(dst[a].TexCoords);
            const float det=d1.x*d2.y-d1.y*d2.x;
            if(std::fabs(det)<1e-8f) continue;
            const auto t=(e1*d2.y-e2*d1.y)/det,bt=(e2*d1.x-e1*d2.x)/det;
            for(auto v:{a,b,c}) { tangents[v]+=t;bitangents[v]+=bt; }
        }
        for(size_t i=0;i<dst.size();++i) {
            const auto n=normals[sm.mmdVertexIndices[i]];
            auto t=tangents[i]-n*glm::dot(n,tangents[i]);
            if(glm::dot(t,t)<1e-10f) t=glm::cross(n,std::fabs(n.y)<0.9f?glm::vec3(0,1,0):glm::vec3(1,0,0));
            if(glm::dot(t,t)>1e-10f) t=glm::normalize(t);
            dst[i].Tangent=PackSnorm3(t,glm::dot(glm::cross(n,t),bitangents[i])<0?-1.0f:1.0f);
        }
        if(s<m_BVHData.subMeshAABBs.size()) m_BVHData.subMeshAABBs[s]=AABB(mn,mx);
        if(s<md.subMeshes.size()) md.subMeshes[s].aabb=AABB(mn,mx);
        modelMin=glm::min(modelMin,mn);modelMax=glm::max(modelMax,mx);
    }
    md.modelMinBounds=modelMin;md.modelMaxBounds=modelMax;md.modelCenter=(modelMin+modelMax)*0.5f;
    m_AnimationPose.reset();
    return true;
}

void ModelRenderer::FlushMmdVertices() {
    auto& md=m_ModelData;
    if(!md.mmdUploadPending || !md.skinnedBufferMapped) return;
    // Called while recording draws, after FrameRender has waited for fences.
    // VmdSystem evaluates before that wait, so it only stages CPU vertices.
    md.mmdFrameOffset=(GetCurrentFrameIndex()%ModelRenderData::MAX_FRAMES_IN_FLIGHT)*md.mmdFrameSize;
    for(size_t s=0;s<md.skinnedSubMeshes.size();++s) {
        const auto& vertices=md.skinnedSubMeshes[s];
        memcpy(static_cast<char*>(md.skinnedBufferMapped)+md.mmdFrameOffset+md.skinnedBufferOffsets[s],
            vertices.data(),vertices.size()*sizeof(Vertex));
    }
    md.mmdUploadPending=false;
}

VkBuffer ModelRenderer::VertexBufferForDraw(size_t subMeshIndex,VkDeviceSize& offset) {
    FlushMmdVertices();
    const auto& md=m_ModelData;
    const bool deformed=(!g_UseGpuSkinning || md.mmdDeformed) && md.hasSkinning &&
        md.skinnedVertexBuffer!=VK_NULL_HANDLE && subMeshIndex<md.skinnedBufferOffsets.size();
    offset=deformed ? md.skinnedBufferOffsets[subMeshIndex]+md.mmdFrameOffset : 0;
    return deformed ? md.skinnedVertexBuffer : md.subMeshes[subMeshIndex].vertexBuffer;
}
