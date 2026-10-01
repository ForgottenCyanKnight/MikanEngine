#pragma once

#include "Animation/MMD/Model/MMDNode.h"

#include <string>
#include <vector>
#include <memory>

namespace mmd
{
    class MMDIkSolver
    {
    public:
        MMDIkSolver();
        ~MMDIkSolver();

        void SetName(const std::string& name) { m_name = name; }
        const std::string& GetName() const { return m_name; }

        void SetIKNode(MMDNode* node) { m_ikNode = node; }
        void SetTargetNode(MMDNode* node) { m_ikTarget = node; }
        MMDNode* GetIKNode() const { return m_ikNode; }
        MMDNode* GetTargetNode() const { return m_ikTarget; }

        void SetIterateCount(uint32_t count) { m_iterateCount = count; }
        uint32_t GetIterateCount() const { return m_iterateCount; }

        void SetLimitAngle(float angle) { m_limitAngle = angle; }
        float GetLimitAngle() const { return m_limitAngle; }

        void Enable(bool enable) { m_enable = enable; }
        bool IsEnabled() const { return m_enable; }

        void AddIKChain(MMDNode* node, bool isKnee = false);
        void AddIKChain(MMDNode* node, bool axisLimit, const glm::vec3& limitMin, const glm::vec3& limitMax);

        void Solve();

        void SaveBaseAnimation() { m_baseAnimationEnabled = m_enable; }
        void LoadBaseAnimation() { m_enable = m_baseAnimationEnabled; }
        void ClearBaseAnimation() { m_baseAnimationEnabled = true; }
        bool GetBaseAnimationEnabled() const { return m_baseAnimationEnabled; }

    private:
        struct IKChain
        {
            MMDNode* m_node;
            bool m_enableAxisLimit;
            glm::vec3 m_limitMax;
            glm::vec3 m_limitMin;
            glm::vec3 m_prevAngle;
            glm::quat m_saveIKRot;
            float m_planeModeAngle;
        };

        void AddIKChain(IKChain&& chain);
        void SolveCore(uint32_t iteration);

        enum class SolveAxis
        {
            X,
            Y,
            Z,
        };
        void SolvePlane(uint32_t iteration, size_t chainIdx, SolveAxis solveAxis);

    private:
        std::string m_name;
        std::vector<IKChain> m_chains;
        MMDNode* m_ikNode;
        MMDNode* m_ikTarget;
        uint32_t m_iterateCount;
        float m_limitAngle;
        bool m_enable;
        bool m_baseAnimationEnabled;
    };

    class MMDIKManager
    {
    public:
        MMDIKManager();
        ~MMDIKManager();

        MMDIkSolver* AddIKSolver();
        void Clear();

        size_t GetIKSolverCount() const { return m_ikSolvers.size(); }
        MMDIkSolver* GetIKSolver(size_t index) { return m_ikSolvers[index].get(); }
        size_t FindIKSolverIndex(const std::string& name);

    private:
        std::vector<std::unique_ptr<MMDIkSolver>> m_ikSolvers;
    };
}
