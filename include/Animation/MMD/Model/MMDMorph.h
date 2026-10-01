#pragma once

#include "Animation/MMD/Base/MMDTypes.h"

#include <string>
#include <vector>
#include <memory>

namespace mmd
{
    class MMDMorph
    {
    public:
        MMDMorph();
        virtual ~MMDMorph();

        void SetName(const std::string& name) { m_name = name; }
        const std::string& GetName() const { return m_name; }

        void SetWeight(float weight) { m_weight = weight; }
        float GetWeight() const { return m_weight; }

        void SetBaseMorph(MMDMorph* morph) { m_baseMorph = morph; }
        MMDMorph* GetBaseMorph() const { return m_baseMorph; }

        virtual void Update() {}

    protected:
        std::string m_name;
        float m_weight;
        MMDMorph* m_baseMorph;
    };

    class MMDMorphManager
    {
    public:
        MMDMorphManager();
        ~MMDMorphManager();

        MMDMorph* AddMorph();
        void Clear();

        size_t GetMorphCount() const { return m_morphs.size(); }
        MMDMorph* GetMorph(size_t index) { return m_morphs[index].get(); }
        size_t FindMorphIndex(const std::string& name);

    private:
        std::vector<std::unique_ptr<MMDMorph>> m_morphs;
    };
}
