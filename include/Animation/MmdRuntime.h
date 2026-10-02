#pragma once
#include "Platform/Export.h"
#include "Rendering/ModelLoader.h"
#include "Animation/MMD/Model/MMDMaterial.h"
#include <memory>
#include <cstdint>
#include <span>

namespace Animation {
// Per-entity PMX animation, IK, morphs, skinning and Jolt simulation.
class MIKAN_API MmdRuntime {
public:
    MmdRuntime();
    ~MmdRuntime();
    bool Load(const std::string& modelPath, const std::string& motionPath);
    bool Load(const std::string& modelPath, const std::string& motionPath, const std::string& faceMotionPath);
    void Update(float frame, float deltaTime, bool physicsEnabled);
    void ResetPhysics();
    uint64_t GetPoseRevision() const;
    bool ApplyVertices(ModelLoadResult& result) const;
    void GetVertices(std::vector<glm::vec3>& positions,
                     std::vector<glm::vec3>& normals, std::vector<glm::vec2>& uvs) const;
    size_t GetBoneCount() const;
    std::vector<mmd::MMDMaterial> GetMaterials() const;
    std::span<const mmd::MMDMaterial> GetMaterialView() const;
    size_t GetMorphCount() const;
    float GetLastFrame() const;
    void GetPhysicsBodies(std::vector<glm::mat4>& out) const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
// Geometry and materials are loaded without initializing a physics world.
bool LoadPmxMesh(const std::string& path, ModelLoadResult& result);
}
