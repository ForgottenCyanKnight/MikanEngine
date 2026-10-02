#pragma once

#include "Animation/MMD/Model/MMDNode.h"
#include "Animation/MMD/Model/MMDIkSolver.h"
#include "Animation/MMD/Model/MMDMorph.h"
#include "Animation/MMD/Model/MMDMaterial.h"
#include "Animation/MMD/Model/PMXFile.h"

#include <vector>
#include <string>
#include <memory>
#include <future>
#include <thread>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>

namespace mmd
{
    class MMDPhysics;
    class MMDRigidBody;
    class MMDJoint;
    class VMDAnimation;

    struct MMDSubMesh
    {
        int m_beginIndex;
        int m_vertexCount;
        int m_materialID;
    };

    class MMDModel
    {
    public:
        MMDModel();
        virtual ~MMDModel();

        virtual size_t GetVertexCount() const = 0;
        virtual const glm::vec3* GetPositions() const = 0;
        virtual const glm::vec3* GetNormals() const = 0;
        virtual const glm::vec2* GetUVs() const = 0;
        virtual const glm::vec3* GetUpdatedPositions() const = 0;
        virtual const glm::vec3* GetUpdatedNormals() const = 0;
        virtual const glm::vec2* GetUpdatedUVs() const = 0;

        virtual size_t GetIndexCount() const = 0;
        virtual const uint32_t* GetIndices() const = 0;

        virtual size_t GetMaterialCount() const = 0;
        virtual const MMDMaterial* GetMaterials() const = 0;

        virtual size_t GetSubMeshCount() const = 0;
        virtual const MMDSubMesh* GetSubMeshes() const = 0;

        virtual MMDPhysics* GetMMDPhysics() = 0;

        virtual void InitializeAnimation() = 0;
        virtual void BeginAnimation() = 0;
        virtual void EndAnimation() = 0;
        virtual void UpdateMorphAnimation() = 0;
        virtual void UpdateNodeAnimation(bool afterPhysicsAnim) = 0;
        virtual void ResetPhysics() = 0;
        virtual void UpdatePhysicsAnimation(float elapsed) = 0;
        virtual void Update() = 0;
        virtual void SetParallelUpdateHint(uint32_t parallelCount) = 0;

        virtual size_t GetNodeCount() const = 0;
        virtual MMDNode* GetNode(size_t index) = 0;
        virtual size_t FindNodeIndex(const std::string& name) = 0;

        virtual size_t GetIKSolverCount() const = 0;
        virtual MMDIkSolver* GetIKSolver(size_t index) = 0;
        virtual size_t FindIKSolverIndex(const std::string& name) = 0;

        virtual size_t GetMorphCount() const = 0;
        virtual MMDMorph* GetMorph(size_t index) = 0;
        virtual size_t FindMorphIndex(const std::string& name) = 0;

        virtual void SetModelMatrix(const glm::mat4& matrix) = 0;
        virtual const glm::mat4& GetModelMatrix() const = 0;

        virtual void SetVelocity(const glm::vec3& velocity) = 0;
        virtual void SetAngularVelocity(const glm::vec3& angularVelocity) = 0;
        virtual void ApplyForce(const glm::vec3& force) = 0;
        virtual void ApplyTorque(const glm::vec3& torque) = 0;

        void UpdateAllAnimation(VMDAnimation* vmdAnim, float vmdFrame, float physicsElapsed);

    protected:
        glm::mat4 m_modelMatrix;
    };

    class MMDModelImpl : public MMDModel
    {
    public:
        MMDModelImpl();
        virtual ~MMDModelImpl();

        bool Create(const std::string& pmxFile);
        void Destroy();

        size_t GetVertexCount() const override { return m_positions.size(); }
        const glm::vec3* GetPositions() const override { return m_positions.data(); }
        const glm::vec3* GetNormals() const override { return m_normals.data(); }
        const glm::vec2* GetUVs() const override { return m_uvs.data(); }
        const glm::vec3* GetUpdatedPositions() const override { return m_updatedPositions.data(); }
        const glm::vec3* GetUpdatedNormals() const override { return m_updatedNormals.data(); }
        const glm::vec2* GetUpdatedUVs() const override { return m_updatedUVs.data(); }

        size_t GetIndexCount() const override { return m_indices.size(); }
        const uint32_t* GetIndices() const override { return m_indices.data(); }

        size_t GetMaterialCount() const override { return m_materials.size(); }
        const MMDMaterial* GetMaterials() const override { return m_materials.data(); }

        size_t GetSubMeshCount() const override { return m_subMeshes.size(); }
        const MMDSubMesh* GetSubMeshes() const override { return m_subMeshes.data(); }

        MMDPhysics* GetMMDPhysics() override { return m_physics.get(); }

        void InitializeAnimation() override;
        void BeginAnimation() override;
        void EndAnimation() override;
        void UpdateMorphAnimation() override;
        void UpdateNodeAnimation(bool afterPhysicsAnim) override;
        void ResetPhysics() override;
        void UpdatePhysicsAnimation(float elapsed) override;
        void Update() override;
        void SetParallelUpdateHint(uint32_t parallelCount) override;

        size_t GetNodeCount() const override { return m_nodes.size(); }
        MMDNode* GetNode(size_t index) override;
        size_t FindNodeIndex(const std::string& name) override;

        size_t GetIKSolverCount() const override { return m_ikSolvers.size(); }
        MMDIkSolver* GetIKSolver(size_t index) override;
        size_t FindIKSolverIndex(const std::string& name) override;

        size_t GetMorphCount() const override { return m_morphs.size(); }
        MMDMorph* GetMorph(size_t index) override;
        size_t FindMorphIndex(const std::string& name) override;

        void SetModelMatrix(const glm::mat4& matrix) override { m_modelMatrix = matrix; }
        const glm::mat4& GetModelMatrix() const override { return m_modelMatrix; }

        void SetVelocity(const glm::vec3& velocity) override;
        void SetAngularVelocity(const glm::vec3& angularVelocity) override;
        void ApplyForce(const glm::vec3& force) override;
        void ApplyTorque(const glm::vec3& torque) override;

    private:
        void UpdateSkinning();
        void UpdateSkinningParallel();
        void UpdateSkinningRange(size_t startVertex, size_t vertexCount);
        void SetupParallelUpdate();

    private:
        std::vector<glm::vec3> m_positions;
        std::vector<glm::vec3> m_normals;
        std::vector<glm::vec2> m_uvs;

        std::vector<glm::vec3> m_updatedPositions;
        std::vector<glm::vec3> m_updatedNormals;
        std::vector<glm::vec2> m_updatedUVs;

        std::vector<uint32_t> m_indices;
        std::vector<MMDMaterial> m_materials;
        std::vector<MMDMaterial> m_initMaterials;
        std::vector<MMDMaterial> m_materialAdditions;
        std::vector<uint8_t> m_morphVisiting;
        std::vector<MMDSubMesh> m_subMeshes;

        std::vector<std::unique_ptr<MMDNode>> m_nodes;
        std::vector<MMDNode*> m_sortedNodes;
        std::vector<std::unique_ptr<MMDIkSolver>> m_ikSolvers;
        std::vector<std::unique_ptr<MMDMorph>> m_morphs;
        std::vector<std::unique_ptr<MMDRigidBody>> m_rigidBodies;
        std::vector<std::unique_ptr<MMDJoint>> m_joints;

        std::unique_ptr<MMDPhysics> m_physics;

        bool m_afterPhysicsAnim;
        float m_fps;

        // Skinning data
        std::vector<int32_t> m_boneIndices;
        std::vector<float> m_boneWeights;
        std::vector<mmd::PMXVertexWeightType> m_weightTypes;
        std::vector<glm::vec3> m_sdefC;
        std::vector<glm::vec3> m_sdefR0;
        std::vector<glm::vec3> m_sdefR1;

        // Morph data - stores vertex offsets for each morph
        struct MorphVertexData {
            int32_t m_vertexIndex;
            glm::vec3 m_positionOffset;
            glm::vec3 m_normalOffset;
        };
        struct MorphData {
            std::string m_name;
            std::vector<MorphVertexData> m_vertices;
        };
        std::vector<MorphData> m_morphData;
        std::vector<PMXMorph> m_pmxMorphs;
        std::vector<glm::vec3> m_morphPositions;  // Accumulated morph offsets
        std::vector<glm::vec3> m_morphNormals;    // Accumulated morph normal offsets

        // Parallel update data (like saba)
        uint32_t m_parallelUpdateCount;
        std::vector<std::pair<size_t, size_t>> m_updateRanges; // (offset, count) pairs
        std::vector<std::future<void>> m_parallelUpdateFutures;
        std::vector<glm::mat4> m_skinningMatrices;
    };
}
