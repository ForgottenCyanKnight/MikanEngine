#include "Animation/MMD/Model/MMDMorph.h"
#include <algorithm>

namespace mmd
{
    MMDMorph::MMDMorph()
        : m_weight(0.0f)
        , m_baseMorph(nullptr)
    {
    }

    MMDMorph::~MMDMorph()
    {
    }

    MMDMorphManager::MMDMorphManager()
    {
    }

    MMDMorphManager::~MMDMorphManager()
    {
    }

    MMDMorph* MMDMorphManager::AddMorph()
    {
        auto morph = std::make_unique<MMDMorph>();
        MMDMorph* morphPtr = morph.get();
        m_morphs.push_back(std::move(morph));
        return morphPtr;
    }

    void MMDMorphManager::Clear()
    {
        m_morphs.clear();
    }

    size_t MMDMorphManager::FindMorphIndex(const std::string& name)
    {
        for (size_t i = 0; i < m_morphs.size(); i++) {
            if (m_morphs[i]->GetName() == name) {
                return i;
            }
        }
        return static_cast<size_t>(-1);
    }
}
