#include "ModelRenderer.h"
#include "ModelRendererInternals.h"
#include "Core/RenderGlobals.h"
#include "VulkanManager.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// 蒙皮 UBO 3 帧槽动态偏移（dynamic UBO——动画更新处设值，10 处 descriptor 绑定读取；无动画模型恒 0）
uint32_t g_BoneDynamicOffset = 0;

void ModelRenderer::RefreshBoneMatricesAndSkinning() {
    auto& md = m_ModelData;
    if (!md.hasSkinning && !md.hasAnimation) return;

    // 蒙皮矩阵（global * offsetMatrix）写入 UBO buffer（保留，供调试/后续 GPU 蒙皮）
    if (md.boneBufferMapped) {
        g_BoneDynamicOffset = (uint32_t)(GetCurrentFrameIndex() % ModelRenderData::MAX_FRAMES_IN_FLIGHT)
                            * (uint32_t)(MAX_BONES * sizeof(glm::mat4));
        md.boneMatrices.assign(MAX_BONES, glm::mat4(1.0f));
        for (size_t i = 0; i < m_MeshData.bones.size() && i < MAX_BONES; i++) {
            md.boneMatrices[i] = m_MeshData.bones[i].globalTransform * m_MeshData.bones[i].offsetMatrix;
        }
        memcpy((char*)md.boneBufferMapped + g_BoneDynamicOffset, md.boneMatrices.data(), MAX_BONES * sizeof(glm::mat4));
        // [diag-20260806] GPU 蒙皮数据验证：mapped buffer 内容 vs boneMatrices（应一致；数值应合理非飞点）
        {
            static int s_gpuSkinDiag = 0;
            if (s_gpuSkinDiag < 3) {
                s_gpuSkinDiag++;
                glm::mat4* mapped = (glm::mat4*)md.boneBufferMapped;
                printf("[diag] GPUskin: bm[0].c0=(%.3f,%.3f,%.3f,%.3f) mapped[0].c0=(%.3f,%.3f,%.3f,%.3f) mapped[0].c3=(%.3f,%.3f,%.3f,%.3f) bones=%zu\n",
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
                printf("[ModelRenderer][diag] SKIN MATRIX BAD: path='%s' bone=%zu time=%.4f\n",
                       m_ModelData.modelPath.c_str(), i, md.animTime);
            }
        }
    }

    // ===== CPU 蒙皮（fallback：仅当 GPU 蒙皮关闭时执行；GPU 主路径由顶点着色器蒙皮）=====
    if (!g_UseGpuSkinning && md.hasSkinning && md.skinnedBufferMapped &&
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
