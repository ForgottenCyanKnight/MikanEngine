#include "Animation/MMD/Model/MMDNode.h"
#include <algorithm>

namespace mmd
{
    MMDNode::MMDNode()
        : m_parent(nullptr)
        , m_localScale(1.0f, 1.0f, 1.0f)
    {
        m_localTransform = glm::mat4(1.0f);
        m_globalTransform = glm::mat4(1.0f);
        m_inverseInitTransform = glm::mat4(1.0f);
        m_localPosition = glm::vec3(0.0f);
        m_localRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        m_initPosition = glm::vec3(0.0f);
        m_initRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        m_initScale = glm::vec3(1.0f, 1.0f, 1.0f);
        m_animTranslate = glm::vec3(0.0f);
        m_animRotate = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }

    MMDNode::~MMDNode()
    {
    }

    void MMDNode::AddChild(MMDNode* child)
    {
        if (child) {
            child->SetParent(this);
            m_children.push_back(child);
        }
    }

    void MMDNode::UpdateLocalTransform()
    {
        // Combine base transform and animation transform (saba style)
        glm::vec3 animatedPos = GetAnimatedPosition();
        glm::quat animatedRot = GetAnimatedRotation();

        glm::mat4 T = glm::translate(glm::mat4(1.0f), animatedPos);
        glm::mat4 R = glm::mat4_cast(animatedRot);
        glm::mat4 S = glm::scale(glm::mat4(1.0f), m_localScale);

        m_localTransform = T * R * S;

        OnUpdateLocalTransform();

    }

    void MMDNode::UpdateAppendTransform()
    {
        if (!m_appendNode) return;
        if (m_isAppendRotate) {
            auto rotation = !m_isAppendLocal && m_appendNode->m_appendNode
                ? m_appendNode->m_appendRotate : m_appendNode->GetAnimateRotate();
            if (m_appendNode->m_enableIK) rotation=m_appendNode->GetIKRotate()*rotation;
            m_appendRotate=glm::slerp(glm::quat(1,0,0,0),rotation,m_appendWeight);
        }
        if (m_isAppendTranslate) {
            const auto translation = !m_isAppendLocal && m_appendNode->m_appendNode
                ? m_appendNode->m_appendTranslate : m_appendNode->m_localPosition-m_appendNode->m_initPosition;
            m_appendTranslate=translation*m_appendWeight;
        }
        UpdateLocalTransform();
    }

    void MMDNode::UpdateGlobalTransform()
    {
        if (m_parent) {
            m_globalTransform = m_parent->m_globalTransform * m_localTransform;
        } else {
            m_globalTransform = m_localTransform;
        }

        for (auto child : m_children) {
            child->UpdateGlobalTransform();
        }
    }

    void MMDNode::UpdateChildTransform()
    {
        for (auto child : m_children) {
            child->UpdateGlobalTransform();
        }
    }

    void MMDNode::UpdateLocalFromGlobal()
    {
        // Calculate local transform from global transform
        if (m_parent) {
            m_localTransform = glm::inverse(m_parent->m_globalTransform) * m_globalTransform;
        } else {
            m_localTransform = m_globalTransform;
        }

        // Extract position, rotation and scale from local transform
        // Keep the bind pose immutable; physics writes only the local matrix.

        // Extract rotation (this is a simplified version, may need proper matrix decomposition)
        glm::mat3 rotationMat(m_localTransform);
        (void)rotationMat;
    }

    MMDNodeManager::MMDNodeManager()
    {
    }

    MMDNodeManager::~MMDNodeManager()
    {
    }

    MMDNode* MMDNodeManager::AddNode()
    {
        auto node = std::make_unique<MMDNode>();
        MMDNode* nodePtr = node.get();
        size_t index = m_nodes.size();
        m_nodes.push_back(std::move(node));
        m_nameToIndex[nodePtr->GetName()] = index;
        return nodePtr;
    }

    void MMDNodeManager::Clear()
    {
        m_nodes.clear();
        m_nameToIndex.clear();
    }

    size_t MMDNodeManager::FindNodeIndex(const std::string& name)
    {
        auto it = m_nameToIndex.find(name);
        if (it != m_nameToIndex.end()) {
            return it->second;
        }
        return static_cast<size_t>(-1);
    }
}
