#pragma once

#include "Animation/MMD/Base/MMDTypes.h"

#include <string>
#include <vector>
#include <memory>
#include <map>
namespace mmd
{
    class MMDIkSolver;
    class MMDNode
    {
    public:
        MMDNode();
        virtual ~MMDNode();

        void SetName(const std::string& name) { m_name = name; }
        const std::string& GetName() const { return m_name; }

        void SetLocalTransform(const glm::mat4& localTransform) { m_localTransform = localTransform; }
        const glm::mat4& GetLocalTransform() const { return m_localTransform; }

        void SetGlobalTransform(const glm::mat4& globalTransform) { m_globalTransform = globalTransform; }
        const glm::mat4& GetGlobalTransform() const { return m_globalTransform; }

        // Base transform (from PMX/PMD file)
        void SetLocalPosition(const glm::vec3& position) { m_localPosition = position; }
        const glm::vec3& GetLocalPosition() const { return m_localPosition; }

        void SetLocalRotation(const glm::quat& rotation) { m_localRotation = rotation; }
        const glm::quat& GetLocalRotation() const { return m_localRotation; }

        // Animation transform (from VMD animation)
        void SetAnimationTranslate(const glm::vec3& translate) { m_animTranslate = translate; }
        const glm::vec3& GetAnimationTranslate() const { return m_animTranslate; }

        void SetAnimationRotate(const glm::quat& rotate) { m_animRotate = rotate; }
        const glm::quat& GetAnimationRotate() const { return m_animRotate; }
        void SaveBaseAnimation() { m_baseAnimTranslate=m_animTranslate; m_baseAnimRotate=m_animRotate; }
        void LoadBaseAnimation() { m_animTranslate=m_baseAnimTranslate; m_animRotate=m_baseAnimRotate; }
        void ClearBaseAnimation() { m_baseAnimTranslate=glm::vec3(0); m_baseAnimRotate=glm::quat(1,0,0,0); }
        const glm::vec3& GetBaseAnimationTranslate() const { return m_baseAnimTranslate; }
        const glm::quat& GetBaseAnimationRotate() const { return m_baseAnimRotate; }

        // IK rotation (calculated by IK solver)
        void SetIKRotate(const glm::quat& rotate) { m_ikRotate = rotate; }
        const glm::quat& GetIKRotate() const { return m_ikRotate; }

        // Combined animation transform
        glm::vec3 GetAnimatedPosition() const { return m_localPosition + m_animTranslate + m_appendTranslate; }
        glm::quat GetAnimatedRotation() const { return (m_enableIK ? m_ikRotate : glm::quat(1,0,0,0)) * GetAnimateRotate() * m_appendRotate; }

        // Get animation rotation without IK (for IK solver)
        glm::quat GetAnimateRotate() const { return m_animRotate * m_localRotation; }

        void SetInverseInitTransform(const glm::mat4& inverseInitTransform) { m_inverseInitTransform = inverseInitTransform; }
        const glm::mat4& GetInverseInitTransform() const { return m_inverseInitTransform; }

        void SetParent(MMDNode* parent) { m_parent = parent; }
        MMDNode* GetParent() const { return m_parent; }

        void AddChild(MMDNode* child);
        size_t GetChildCount() const { return m_children.size(); }
        MMDNode* GetChild(size_t index) const { return m_children[index]; }

        void UpdateLocalTransform();
        void UpdateGlobalTransform();
        void UpdateChildTransform();

        // Update local transform from global transform (for physics simulation)
        void UpdateLocalFromGlobal();

        void SetDeformAfterPhysics(bool deform) { m_deformAfterPhysics = deform; }
        bool IsDeformAfterPhysics() const { return m_deformAfterPhysics; }

        void SetDeformDepth(int depth) { m_deformDepth = depth; }
        int GetDeformDepth() const { return m_deformDepth; }
        void SetIKSolver(MMDIkSolver* solver) { m_ikSolver = solver; }
        MMDIkSolver* GetIKSolver() const { return m_ikSolver; }
        void EnableIK(bool enable) { m_enableIK = enable; }
        void ConfigureAppend(MMDNode* node, float weight, bool rotate, bool translate, bool local)
        { m_appendNode=node; m_appendWeight=weight; m_isAppendRotate=rotate; m_isAppendTranslate=translate; m_isAppendLocal=local; }
        MMDNode* GetAppendNode() const { return m_appendNode; }
        void ResetAppendTransform() { m_appendRotate=glm::quat(1,0,0,0); m_appendTranslate=glm::vec3(0); }
        void UpdateAppendTransform();

        void SetPhysicsAffected(bool affected) { m_physicsAffected = affected; }
        bool IsPhysicsAffected() const { return m_physicsAffected; }

        // Save/Load initial transform
        void SaveInitialTRS()
        {
            m_initPosition = m_localPosition;
            m_initRotation = m_localRotation;
            m_initScale = m_localScale;
        }

        void LoadInitialTRS()
        {
            m_localPosition = m_initPosition;
            m_localRotation = m_initRotation;
            m_localScale = m_initScale;
        }

        virtual void OnBeginUpdateTransform() {}
        virtual void OnEndUpdateTransform() {}
        virtual void OnUpdateLocalTransform() {}

    protected:
        std::string m_name;
        glm::mat4 m_localTransform;
        glm::mat4 m_globalTransform;
        glm::mat4 m_inverseInitTransform;

        // Base transform from model file
        glm::vec3 m_localPosition;
        glm::quat m_localRotation;
        glm::vec3 m_localScale;

        // Initial transform (saved at initialization)
        glm::vec3 m_initPosition;
        glm::quat m_initRotation;
        glm::vec3 m_initScale;

        // Animation transform from VMD
        glm::vec3 m_animTranslate = glm::vec3(0.0f);
        glm::quat m_animRotate = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

        // IK rotation (calculated by IK solver)
        glm::quat m_ikRotate = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

        MMDNode* m_parent;
        std::vector<MMDNode*> m_children;
        bool m_physicsAffected = false;
        bool m_deformAfterPhysics = false;
        int m_deformDepth = 0;
        bool m_enableIK = false;
        MMDIkSolver* m_ikSolver = nullptr;
        MMDNode* m_appendNode = nullptr;
        float m_appendWeight = 0;
        bool m_isAppendRotate = false, m_isAppendTranslate = false, m_isAppendLocal = false;
        glm::quat m_appendRotate = glm::quat(1,0,0,0);
        glm::vec3 m_appendTranslate = glm::vec3(0);
        glm::vec3 m_baseAnimTranslate = glm::vec3(0);
        glm::quat m_baseAnimRotate = glm::quat(1,0,0,0);
    };

    class MMDNodeManager
    {
    public:
        MMDNodeManager();
        ~MMDNodeManager();

        MMDNode* AddNode();
        void Clear();

        size_t GetNodeCount() const { return m_nodes.size(); }
        MMDNode* GetNode(size_t index) { return m_nodes[index].get(); }
        size_t FindNodeIndex(const std::string& name);

    private:
        std::vector<std::unique_ptr<MMDNode>> m_nodes;
        std::map<std::string, size_t> m_nameToIndex;
    };
}
