#pragma once

#include "Platform/Export.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// VMD 是一个独立的资源格式。这个模块只负责读取和采样动作数据，
// 不依赖 ECS、渲染器或 PMX 数据结构，便于以后复用到别的动画目标。
namespace Animation {

struct MIKAN_API VmdBoneKeyframe {
    std::uint32_t frame = 0;
    glm::vec3 translation = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    std::array<std::uint8_t, 64> interpolation{};
};

struct MIKAN_API VmdBoneTrack {
    std::string name;
    std::vector<VmdBoneKeyframe> keyframes;
};

struct MIKAN_API VmdCameraKeyframe {
    std::uint32_t frame = 0;
    float distance = 0.0f;
    glm::vec3 position = glm::vec3(0.0f); // 注视点位置
    glm::vec3 rotation = glm::vec3(0.0f); // 弧度，VMD 相机旋转
    std::array<std::uint8_t, 24> interpolation{};
    float viewAngle = 45.0f;              // 度
    bool perspective = true;              // VMD: 0=透视, 1=正交
};

class MIKAN_API VmdMotion {
public:
    static bool LoadFromFile(const std::string& path, VmdMotion& out, std::string& error);
    static bool LoadFromMemory(const void* data, std::size_t size, VmdMotion& out, std::string& error);

    bool Empty() const { return m_boneTracks.empty() && m_cameraKeyframes.empty(); }
    bool HasBoneTracks() const { return !m_boneTracks.empty(); }
    bool HasCameraTrack() const { return !m_cameraKeyframes.empty(); }
    float GetLastFrame() const { return m_lastFrame; }

    const std::vector<VmdBoneTrack>& GetBoneTracks() const { return m_boneTracks; }
    const std::vector<VmdCameraKeyframe>& GetCameraKeyframes() const { return m_cameraKeyframes; }

    // 未找到骨骼通道时返回 false；调用方可保留绑定姿态。
    bool SampleBone(const std::string& boneName, float frame,
                    glm::vec3& outTranslation, glm::quat& outRotation) const;

    // 未找到相机帧时返回 false。
    bool SampleCamera(float frame, VmdCameraKeyframe& outCamera) const;

private:
    const VmdBoneTrack* FindBoneTrack(const std::string& boneName) const;

    std::vector<VmdBoneTrack> m_boneTracks;
    std::unordered_map<std::string, std::size_t> m_boneTrackLookup;
    std::vector<VmdCameraKeyframe> m_cameraKeyframes;
    float m_lastFrame = 0.0f;
};

} // namespace Animation
