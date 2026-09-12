#include "ModelRenderer.h"
#include "Core/RenderGlobals.h"
#include "VulkanManager.h"

#include <cmath>
#include <cstdio>

// ===== 骨骼动画控制 =====
// 动画控制接口独立于 Vulkan 资源创建和命令录制；蒙皮矩阵刷新仍由
// ModelRenderer.cpp 提供，以保持动态 UBO 偏移与渲染绑定状态的单一实现。
void ModelRenderer::UpdateAnimation(float deltaTime) {
    auto& md = m_ModelData;
    if (!md.hasSkinning && (!md.hasAnimation || !md.animPlaying || m_MeshData.animations.empty())) return;

    if (md.hasAnimation && md.animPlaying && !m_MeshData.animations.empty()) {
        const int clipCount = (int)m_MeshData.animations.size();
        if (md.currentClip < 0 || md.currentClip >= clipCount) md.currentClip = 0;
        const AnimationClip& clip = m_MeshData.animations[md.currentClip];

        md.animTime += deltaTime * md.animSpeed;
        if (clip.duration > 0.0f) {
            if (md.animLoop) {
                md.animTime = fmodf(md.animTime, clip.duration);
            } else if (md.animTime >= clip.duration) {
                md.animTime = clip.duration;
                md.animPlaying = false;
            }
        }

        // [diag] 动画时间异常检测：NaN/Inf 会让骨骼采样与蒙皮矩阵全坏（交换链重建后模型消失排查）
        if (!std::isfinite(md.animTime)) {
            static int s_timeDiag = 0;
            if (s_timeDiag < 5) {
                s_timeDiag++;
                printf("[ModelRenderer][diag] ANIM TIME BAD: path='%s' time=%f clip=%d loop=%d playing=%d\n",
                       m_ModelData.modelPath.c_str(), md.animTime, md.currentClip, md.animLoop ? 1 : 0, md.animPlaying ? 1 : 0);
            }
            md.animTime = 0.0f;
            md.animPlaying = true;
        }

        // 采样动画 -> 更新骨骼局部/全局变换
        ModelLoader::SampleAnimation(clip, md.animTime, m_MeshData.bones);
    }

    RefreshBoneMatricesAndSkinning();
}

bool ModelRenderer::ApplyBoneLocalPose(const std::vector<glm::mat4>& localTransforms) {
    if (!m_ModelData.hasSkinning || localTransforms.size() != m_MeshData.bones.size()) {
        return false;
    }

    for (size_t i = 0; i < m_MeshData.bones.size(); ++i) {
        m_MeshData.bones[i].localTransform = localTransforms[i];
    }

    // 重新计算 globalTransform。骨骼父节点必须先算，根骨骼保留加载阶段的
    // ancestorTransform（例如模型的 Z_UP/Armature 轴修正）。
    std::vector<bool> computed(m_MeshData.bones.size(), false);
    for (size_t pass = 0; pass < m_MeshData.bones.size(); ++pass) {
        bool any = false;
        for (size_t i = 0; i < m_MeshData.bones.size(); ++i) {
            if (computed[i]) continue;
            Bone& bone = m_MeshData.bones[i];
            const int parent = bone.parentIndex;
            const bool validParent = parent >= 0 && parent < (int)m_MeshData.bones.size();
            if (!validParent || computed[(size_t)parent]) {
                bone.globalTransform = validParent
                    ? m_MeshData.bones[(size_t)parent].globalTransform * bone.localTransform
                    : bone.ancestorTransform * bone.localTransform;
                bone.position = glm::vec3(bone.globalTransform[3]);
                bone.rotation = glm::quat_cast(bone.globalTransform);
                computed[i] = true;
                any = true;
            }
        }
        if (!any) break;
    }

    // 若资源存在损坏的父索引环，仍为剩余骨骼保留一个确定姿态，避免把旧帧
    // 的 globalTransform 带入本帧蒙皮。
    for (size_t i = 0; i < m_MeshData.bones.size(); ++i) {
        if (computed[i]) continue;
        Bone& bone = m_MeshData.bones[i];
        bone.globalTransform = bone.ancestorTransform * bone.localTransform;
        bone.position = glm::vec3(bone.globalTransform[3]);
        bone.rotation = glm::quat_cast(bone.globalTransform);
    }

    RefreshBoneMatricesAndSkinning();
    return true;
}

void ModelRenderer::PlayAnimation(int clipIndex, bool loop) {
    if (m_MeshData.animations.empty()) return;
    m_ModelData.currentClip = glm::clamp(clipIndex, 0, (int)m_MeshData.animations.size() - 1);
    m_ModelData.animLoop = loop;
    m_ModelData.animTime = 0.0f;
    m_ModelData.animPlaying = true;
}

int ModelRenderer::GetAnimationCount() const {
    return (int)m_MeshData.animations.size();
}

const char* ModelRenderer::GetAnimationName() const {
    if (m_MeshData.animations.empty()) return "";
    const int idx = m_ModelData.currentClip;
    if (idx < 0 || idx >= (int)m_MeshData.animations.size()) return "";
    return m_MeshData.animations[idx].name.c_str();
}
