#include "Animation/MMD/Model/VMDAnimation.h"
#include "Animation/MMD/Model/MMDModel.h"
#include <algorithm>
#include <cmath>

namespace mmd
{
    namespace {
        // Cache the upper-bound index, including the before/after-track cases.
        // A seek, reverse step, or changed key list falls back to binary search.
        template<class Key,class Time>
        auto CachedUpperBound(const std::vector<Key>& keys,Time time,size_t& index) {
            if(index<=keys.size() && (index==0 || keys[index-1].m_time<=time) &&
               (index==keys.size() || time<keys[index].m_time))
                return keys.begin()+index;
            const auto bound=std::upper_bound(keys.begin(),keys.end(),time,
                [](Time frame,const Key& key) { return frame<key.m_time; });
            index=static_cast<size_t>(bound-keys.begin());
            return bound;
        }
    }
    float VMDBezier::EvalX(float t) const
    {
        const float remaining = 1.0f - t;
        return 3.0f * remaining * remaining * t * m_cp1.x
            + 3.0f * remaining * t * t * m_cp2.x + t * t * t;
    }

    float VMDBezier::EvalY(float t) const
    {
        const float remaining = 1.0f - t;
        return 3.0f * remaining * remaining * t * m_cp1.y
            + 3.0f * remaining * t * t * m_cp2.y + t * t * t;
    }
    glm::vec2 VMDBezier::Eval(float t) const
    {
        return glm::vec2(EvalX(t), EvalY(t));
    }

    float VMDBezier::FindBezierX(float time) const
    {
        const float x=std::clamp(time,0.0f,1.0f);
        float lo=0,hi=1;
        for(int i=0;i<24;++i) { const float t=(lo+hi)*0.5f;if(EvalX(t)<x) lo=t;else hi=t; }
        return (lo+hi)*0.5f;
    }
    VMDNodeController::VMDNodeController()
        : m_node(nullptr)
        , m_startKeyIndex(0)
    {
    }

    VMDNodeController::~VMDNodeController()
    {
    }

    void VMDNodeController::SetNode(MMDNode* node)
    {
        m_node = node;
    }

    void VMDNodeController::Evaluate(float t, float weight)
    {
        if (!m_node) return;
        if (m_keys.empty()) { m_node->SetAnimationTranslate(glm::vec3(0)); m_node->SetAnimationRotate(glm::quat(1,0,0,0)); return; }

        const auto bound=CachedUpperBound(m_keys,static_cast<int32_t>(t),m_startKeyIndex);
        if (bound==m_keys.begin() || bound==m_keys.end()) {
            const auto& key0=bound==m_keys.begin()?m_keys.front():m_keys.back();
            glm::vec3 pos = glm::mix(m_node->GetBaseAnimationTranslate(),key0.m_translate,weight);
            glm::quat rot = glm::slerp(m_node->GetBaseAnimationRotate(),key0.m_rotate,weight);
            m_node->SetAnimationTranslate(pos);
            m_node->SetAnimationRotate(rot);
        } else {
            const auto& key0 = *(bound-1);
            const auto& key1 = *bound;
            float t0 = static_cast<float>(key0.m_time);
            float t1 = static_cast<float>(key1.m_time);
            float factor = (t - t0) / (t1 - t0);



            glm::vec3 pos = glm::mix(m_node->GetBaseAnimationTranslate(),glm::vec3(glm::mix(key0.m_translate.x,key1.m_translate.x,key0.m_txBezier.EvalY(key0.m_txBezier.FindBezierX(factor))), glm::mix(key0.m_translate.y,key1.m_translate.y,key0.m_tyBezier.EvalY(key0.m_tyBezier.FindBezierX(factor))), glm::mix(key0.m_translate.z,key1.m_translate.z,key0.m_tzBezier.EvalY(key0.m_tzBezier.FindBezierX(factor)))),weight);
            glm::quat rot = glm::slerp(m_node->GetBaseAnimationRotate(), glm::slerp(key0.m_rotate, key1.m_rotate, key0.m_rotBezier.EvalY(key0.m_rotBezier.FindBezierX(factor))), weight);

            m_node->SetAnimationTranslate(pos);
            m_node->SetAnimationRotate(rot);
        }

        m_node->UpdateLocalTransform();
    }

    void VMDNodeController::AddKey(const VMDNodeAnimationKey& key)
    {
        m_keys.push_back(key);
    }

    void VMDNodeController::SortKeys()
    {
        std::sort(m_keys.begin(), m_keys.end(),
            [](const VMDNodeAnimationKey& a, const VMDNodeAnimationKey& b) {
                return a.m_time < b.m_time;
            });
    }

    VMDMorphController::VMDMorphController()
        : m_morph(nullptr)
        , m_startKeyIndex(0)
    {
    }

    VMDMorphController::~VMDMorphController()
    {
    }

    void VMDMorphController::SetMorph(MMDMorph* morph)
    {
        m_morph = morph;
    }

    void VMDMorphController::Evaluate(float t, float weight)
    {
        if (!m_morph || m_keys.empty()) {
            return;
        }

        const auto bound=CachedUpperBound(m_keys,t,m_startKeyIndex);
        const size_t start=bound==m_keys.begin()?0:static_cast<size_t>(bound-m_keys.begin()-1);
        const auto& key0 = m_keys[start];
        if (start + 1 >= m_keys.size()) {
            m_morph->SetWeight(key0.m_weight * weight);
        } else {
            const auto& key1 = m_keys[start + 1];
            float t0 = static_cast<float>(key0.m_time);
            float t1 = static_cast<float>(key1.m_time);
            float factor = std::clamp((t - t0) / (t1 - t0),0.0f,1.0f);
            float w = glm::mix(key0.m_weight, key1.m_weight, factor) * weight;
            m_morph->SetWeight(w);
        }
    }

    void VMDMorphController::AddKey(const VMDMorphAnimationKey& key)
    {
        m_keys.push_back(key);
    }

    void VMDMorphController::SortKeys()
    {
        std::sort(m_keys.begin(), m_keys.end(),
            [](const VMDMorphAnimationKey& a, const VMDMorphAnimationKey& b) {
                return a.m_time < b.m_time;
            });
    }

    VMDIKController::VMDIKController()
        : m_ikSolver(nullptr)
        , m_startKeyIndex(0)
    {
    }

    VMDIKController::~VMDIKController()
    {
    }

    void VMDIKController::SetIKSolver(MMDIkSolver* ikSolver)
    {
        m_ikSolver = ikSolver;
    }

    void VMDIKController::Evaluate(float t, float weight)
    {
        if (!m_ikSolver) return;
        if (m_keys.empty()) { m_ikSolver->Enable(true); return; }
        const auto bound=CachedUpperBound(m_keys,static_cast<int32_t>(t),m_startKeyIndex);
        const bool enable=(bound==m_keys.begin()?m_keys.front():*(bound-1)).m_enable;
        m_ikSolver->Enable(weight<1.0f?m_ikSolver->GetBaseAnimationEnabled():enable);
    }

    void VMDIKController::AddKey(const VMDIKAnimationKey& key)
    {
        m_keys.push_back(key);
    }

    void VMDIKController::SortKeys()
    {
        std::sort(m_keys.begin(), m_keys.end(),
            [](const VMDIKAnimationKey& a, const VMDIKAnimationKey& b) {
                return a.m_time < b.m_time;
            });
    }

    VMDAnimation::VMDAnimation()
        : m_maxKeyTime(0)
    {
    }

    VMDAnimation::~VMDAnimation()
    {
    }

    bool VMDAnimation::Create(MMDModel* model)
    {
        m_model = model;
        return true;
    }

    bool VMDAnimation::Add(const VMDFile& vmd)
    {
        InitializeControllers(vmd);
        return true;
    }

    void VMDAnimation::Destroy()
    {
        m_model = nullptr;
        m_nodeControllers.clear();
        m_morphControllers.clear();
        m_fallbackMorphControllers.clear();
        m_ikControllers.clear();
    }

    void VMDAnimation::Evaluate(float t, float weight)
    {
        for (auto& controller : m_nodeControllers) controller.Evaluate(t, weight);

        for (auto& controller : m_morphControllers) {
            controller.Evaluate(t, weight);
        }
        for(auto& controller:m_fallbackMorphControllers) {
            auto* morph=controller.GetMorph();
            const float original=morph->GetWeight();
            controller.Evaluate(t,weight);
            morph->SetWeight(std::max(original,morph->GetWeight()));
        }

        for (auto& controller : m_ikControllers) {
            controller.Evaluate(t, weight);
        }
    }

    void VMDAnimation::InitializeControllers(const VMDFile& vmd)
    {
        CreateNodeControllers(vmd);
        CreateMorphControllers(vmd);
        CreateIKControllers(vmd);

        m_maxKeyTime = std::max(m_maxKeyTime, static_cast<int32_t>(vmd.GetMaxFrame()));
    }

    void VMDAnimation::CreateNodeControllers(const VMDFile& vmd)
    {
        if (!m_model) return;

        std::map<std::string, std::vector<const VMDMotion*>> motionMap;
        for (const auto& motion : vmd.GetMotions()) {
            motionMap[motion.boneName].push_back(&motion);
        }

        for (const auto& pair : motionMap) {
            const std::string& boneName = pair.first;
            const auto& motions = pair.second;

            size_t nodeIndex = m_model->FindNodeIndex(boneName);
            if (nodeIndex >= m_model->GetNodeCount()) {
                continue;
            }

            MMDNode* node = m_model->GetNode(nodeIndex);
            if (!node) continue;

            VMDNodeController controller;
            controller.SetNode(node);

            for (const auto* motion : motions) {
                VMDNodeAnimationKey key;
                key.m_time = static_cast<int32_t>(motion->frameNo);
                // Convert from MMD coordinate system to OpenGL coordinate system
                // Flip Z axis for position
                key.m_translate = glm::vec3(motion->position.x, motion->position.y, -motion->position.z);
                // Convert rotation from MMD coordinate system to OpenGL coordinate system
                // MMD uses right-handed system, OpenGL uses right-handed system with flipped Z
                // We need to invert the rotation around X and Y axes
                key.m_rotate = glm::quat(motion->rotation.w, -motion->rotation.x, -motion->rotation.y, motion->rotation.z);

                const uint8_t* interp = motion->interpolation;
                key.m_txBezier.m_cp1 = glm::vec2(interp[0] / 127.0f, interp[4] / 127.0f);
                key.m_txBezier.m_cp2 = glm::vec2(interp[8] / 127.0f, interp[12] / 127.0f);
                key.m_tyBezier.m_cp1 = glm::vec2(interp[1] / 127.0f, interp[5] / 127.0f);
                key.m_tyBezier.m_cp2 = glm::vec2(interp[9] / 127.0f, interp[13] / 127.0f);
                key.m_tzBezier.m_cp1 = glm::vec2(interp[2] / 127.0f, interp[6] / 127.0f);
                key.m_tzBezier.m_cp2 = glm::vec2(interp[10] / 127.0f, interp[14] / 127.0f);
                key.m_rotBezier.m_cp1 = glm::vec2(interp[3] / 127.0f, interp[7] / 127.0f);
                key.m_rotBezier.m_cp2 = glm::vec2(interp[11] / 127.0f, interp[15] / 127.0f);

                controller.AddKey(key);
            }

            controller.SortKeys();
            auto existing=std::find_if(m_nodeControllers.begin(),m_nodeControllers.end(),[&](const auto& c){return c.GetNode()==node;});
            if(existing==m_nodeControllers.end()) m_nodeControllers.push_back(std::move(controller));
            else { for(const auto& key:controller.GetKeys()) existing->AddKey(key); existing->SortKeys(); }
        }
    }

    void VMDAnimation::CreateMorphControllers(const VMDFile& vmd)
    {
        if (!m_model) return;

        std::map<std::string, std::vector<const VMDMorph*>> morphMap;
        for (const auto& morph : vmd.GetMorphs()) {
            morphMap[morph.morphName].push_back(&morph);
        }

        for (const auto& pair : morphMap) {
            const std::string& morphName = pair.first;
            const auto& morphs = pair.second;

            size_t morphIndex = m_model->FindMorphIndex(morphName);
            if (morphIndex >= m_model->GetMorphCount()) {
                // Some atlas-based models expose independent eyelids instead of a blink.
                // Use both only when the model has no exact standard blink/smile morph.
                std::vector<std::string> targets;
                if(morphName=="まばたき") targets={"ウィンク","ウィンク右"};
                else if(morphName=="笑い") targets={"ウィンク２","ウィンク２右"};
                bool complete=!targets.empty();
                for(const auto& target:targets) if(m_model->FindMorphIndex(target)>=m_model->GetMorphCount()) complete=false;
                if(complete) for(const auto& target:targets) {
                    VMDMorphController fallback;
                    fallback.SetMorph(m_model->GetMorph(m_model->FindMorphIndex(target)));
                    for(const auto* key:morphs) fallback.AddKey({static_cast<int32_t>(key->frameNo),key->weight});
                    fallback.SortKeys();m_fallbackMorphControllers.push_back(std::move(fallback));
                }
                continue;
            }

            MMDMorph* morph = m_model->GetMorph(morphIndex);
            if (!morph) continue;

            VMDMorphController controller;
            controller.SetMorph(morph);

            for (const auto* morphData : morphs) {
                VMDMorphAnimationKey key;
                key.m_time = static_cast<int32_t>(morphData->frameNo);
                key.m_weight = morphData->weight;
                controller.AddKey(key);
            }

            controller.SortKeys();
            auto existing=std::find_if(m_morphControllers.begin(),m_morphControllers.end(),[&](const auto& c){return c.GetMorph()==morph;});
            if(existing==m_morphControllers.end()) m_morphControllers.push_back(std::move(controller));
            else { for(const auto& key:controller.GetKeys()) existing->AddKey(key); existing->SortKeys(); }
        }
    }

    void VMDAnimation::CreateIKControllers(const VMDFile& vmd)
    {
        if (!m_model) return;

        std::map<std::string, std::vector<const VMDIK*>> ikMap;
        for (const auto& ik : vmd.GetIKs()) {
            ikMap[ik.ikName].push_back(&ik);
        }

        for (const auto& pair : ikMap) {
            const std::string& ikName = pair.first;
            const auto& iks = pair.second;

            size_t ikIndex = m_model->FindIKSolverIndex(ikName);
            if (ikIndex >= m_model->GetIKSolverCount()) {
                continue;
            }

            MMDIkSolver* ikSolver = m_model->GetIKSolver(ikIndex);
            if (!ikSolver) continue;

            VMDIKController controller;
            controller.SetIKSolver(ikSolver);

            for (const auto* ikData : iks) {
                VMDIKAnimationKey key;
                key.m_time = static_cast<int32_t>(ikData->frameNo);
                key.m_enable = ikData->enable != 0;
                controller.AddKey(key);
            }

            controller.SortKeys();
            auto existing=std::find_if(m_ikControllers.begin(),m_ikControllers.end(),[&](const auto& c){return c.GetIKSolver()==ikSolver;});
            if(existing==m_ikControllers.end()) m_ikControllers.push_back(std::move(controller));
            else { for(const auto& key:controller.GetKeys()) existing->AddKey(key); existing->SortKeys(); }
        }
    }
}
