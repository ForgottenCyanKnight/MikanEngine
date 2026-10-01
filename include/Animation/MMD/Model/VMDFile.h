#pragma once

#include "Animation/MMD/Base/MMDTypes.h"
#include <string>
#include <vector>
#include <memory>

namespace mmd
{
    enum class VMDVersion
    {
        V1_0,
        V2_0
    };

    struct VMDMotion
    {
        std::string boneName;
        uint32_t frameNo;
        glm::vec3 position;
        glm::quat rotation;
        uint8_t interpolation[64];

        VMDMotion() : frameNo(0), position(0.0f), rotation(1.0f, 0.0f, 0.0f, 0.0f)
        {
            boneName.clear();
            memset(interpolation, 0, sizeof(interpolation));
        }
    };

    struct VMDMorph
    {
        std::string morphName;
        uint32_t frameNo;
        float weight;

        VMDMorph() : frameNo(0), weight(0.0f)
        {
            morphName.clear();
        }
    };

    struct VMDCamera
    {
        uint32_t frameNo;
        float distance;
        glm::vec3 position;
        glm::vec3 rotation;
        uint8_t interpolation[24];
        float fov;
        uint8_t isPerspective;

        VMDCamera() : frameNo(0), distance(0.0f), position(0.0f), rotation(0.0f), fov(45.0f), isPerspective(1)
        {
            memset(interpolation, 0, sizeof(interpolation));
        }
    };

    struct VMDLight
    {
        uint32_t frameNo;
        glm::vec3 color;
        glm::vec3 direction;

        VMDLight() : frameNo(0), color(1.0f), direction(0.0f, -1.0f, 0.0f) {}
    };

    struct VMDIK
    {
        std::string ikName;
        uint32_t frameNo;
        uint8_t enable;

        VMDIK() : frameNo(0), enable(1)
        {
            ikName.clear();
        }
    };

    class VMDFile
    {
    public:
        VMDFile();
        ~VMDFile();

        bool Load(const std::string& filename);
        void Destroy();

        const std::string& GetModelName() const { return m_modelName; }
        const std::vector<VMDMotion>& GetMotions() const { return m_motions; }
        const std::vector<VMDMorph>& GetMorphs() const { return m_morphs; }
        const std::vector<VMDCamera>& GetCameras() const { return m_cameras; }
        const std::vector<VMDLight>& GetLights() const { return m_lights; }
        const std::vector<VMDIK>& GetIKs() const { return m_iks; }
        VMDVersion GetVersion() const { return m_version; }
        uint32_t GetMaxFrame() const;

    private:
        VMDVersion m_version;
        std::string m_modelName;
        std::vector<VMDMotion> m_motions;
        std::vector<VMDMorph> m_morphs;
        std::vector<VMDCamera> m_cameras;
        std::vector<VMDLight> m_lights;
        std::vector<VMDIK> m_iks;
    };
}
