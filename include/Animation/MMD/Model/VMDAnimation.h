#pragma once

#include "Animation/MMD/Model/MMDNode.h"
#include "Animation/MMD/Model/MMDIkSolver.h"
#include "Animation/MMD/Model/MMDMorph.h"
#include "Animation/MMD/Model/VMDFile.h"

#include <string>
#include <vector>
#include <memory>
#include <map>

namespace mmd
{
    struct VMDBezier
    {
        float EvalX(float t) const;
        float EvalY(float t) const;
        glm::vec2 Eval(float t) const;
        float FindBezierX(float time) const;

        glm::vec2 m_cp1;
        glm::vec2 m_cp2;
    };

    struct VMDNodeAnimationKey
    {
        int32_t m_time;
        glm::vec3 m_translate;
        glm::quat m_rotate;
        VMDBezier m_txBezier;
        VMDBezier m_tyBezier;
        VMDBezier m_tzBezier;
        VMDBezier m_rotBezier;
    };

    struct VMDMorphAnimationKey
    {
        int32_t m_time;
        float m_weight;
    };

    struct VMDIKAnimationKey
    {
        int32_t m_time;
        bool m_enable;
    };

    class VMDNodeController
    {
    public:
        VMDNodeController();
        ~VMDNodeController();

        void SetNode(MMDNode* node);
        void Evaluate(float t, float weight = 1.0f);
        void AddKey(const VMDNodeAnimationKey& key);
        void SortKeys();

        MMDNode* GetNode() const { return m_node; }
        const std::vector<VMDNodeAnimationKey>& GetKeys() const { return m_keys; }

    private:
        MMDNode* m_node;
        std::vector<VMDNodeAnimationKey> m_keys;
        size_t m_startKeyIndex;
    };

    class VMDMorphController
    {
    public:
        VMDMorphController();
        ~VMDMorphController();

        void SetMorph(MMDMorph* morph);
        void Evaluate(float t, float weight = 1.0f);
        void AddKey(const VMDMorphAnimationKey& key);
        void SortKeys();

        MMDMorph* GetMorph() const { return m_morph; }
        const std::vector<VMDMorphAnimationKey>& GetKeys() const { return m_keys; }

    private:
        MMDMorph* m_morph;
        std::vector<VMDMorphAnimationKey> m_keys;
        size_t m_startKeyIndex;
    };

    class VMDIKController
    {
    public:
        VMDIKController();
        ~VMDIKController();

        void SetIKSolver(MMDIkSolver* ikSolver);
        void Evaluate(float t, float weight = 1.0f);
        void AddKey(const VMDIKAnimationKey& key);
        void SortKeys();

        MMDIkSolver* GetIKSolver() const { return m_ikSolver; }
        const std::vector<VMDIKAnimationKey>& GetKeys() const { return m_keys; }

    private:
        MMDIkSolver* m_ikSolver;
        std::vector<VMDIKAnimationKey> m_keys;
        size_t m_startKeyIndex;
    };

    class MMDModel;

    class VMDAnimation
    {
    public:
        VMDAnimation();
        ~VMDAnimation();

        bool Create(MMDModel* model);
        bool Add(const VMDFile& vmd);
        void Destroy();

        void Evaluate(float t, float weight = 1.0f);

        int32_t GetMaxKeyTime() const { return m_maxKeyTime; }
        void SetMaxKeyTime(int32_t maxKeyTime) { m_maxKeyTime = maxKeyTime; }

        MMDModel* GetModel() const { return m_model; }

    private:
        void InitializeControllers(const VMDFile& vmd);
        void CreateNodeControllers(const VMDFile& vmd);
        void CreateMorphControllers(const VMDFile& vmd);
        void CreateIKControllers(const VMDFile& vmd);

        MMDModel* m_model;
        std::vector<VMDNodeController> m_nodeControllers;
        std::vector<VMDMorphController> m_morphControllers;
        std::vector<VMDMorphController> m_fallbackMorphControllers;
        std::vector<VMDIKController> m_ikControllers;
        int32_t m_maxKeyTime;
    };
}
