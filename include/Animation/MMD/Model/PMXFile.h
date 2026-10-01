#pragma once

#include "Animation/MMD/Base/MMDTypes.h"

#include <string>
#include <vector>

namespace mmd
{
    struct PMXHeader
    {
        MMDFileString<4> m_magic;
        float m_version;
        uint8_t m_dataSize;
        uint8_t m_encode;
        uint8_t m_addUVNum;
        uint8_t m_vertexIndexSize;
        uint8_t m_textureIndexSize;
        uint8_t m_materialIndexSize;
        uint8_t m_boneIndexSize;
        uint8_t m_morphIndexSize;
        uint8_t m_rigidbodyIndexSize;
    };

    struct PMXInfo
    {
        std::string m_modelName;
        std::string m_englishModelName;
        std::string m_comment;
        std::string m_englishComment;
    };

    enum class PMXVertexWeightType : uint8_t
    {
        BDEF1,
        BDEF2,
        BDEF4,
        SDEF,
        QDEF,
    };

    struct PMXVertex
    {
        glm::vec3 m_position;
        glm::vec3 m_normal;
        glm::vec2 m_uv;
        glm::vec4 m_addUV[4];
        PMXVertexWeightType m_weightType;
        int32_t m_boneIndices[4];
        float m_boneWeights[4];
        glm::vec3 m_sdefC;
        glm::vec3 m_sdefR0;
        glm::vec3 m_sdefR1;
        float m_edgeMag;
    };

    struct PMXFace
    {
        uint32_t m_vertices[3];
    };

    struct PMXTexture
    {
        std::string m_textureName;
    };

    enum class PMXDrawModeFlags : uint8_t
    {
        BothFace = 0x01,
        GroundShadow = 0x02,
        CastSelfShadow = 0x04,
        RecieveSelfShadow = 0x08,
        DrawEdge = 0x10,
        VertexColor = 0x20,
        DrawPoint = 0x40,
        DrawLine = 0x80,
    };

    enum class PMXSphereMode : uint8_t
    {
        None,
        Mul,
        Add,
        SubTexture,
    };

    enum class PMXToonMode : uint8_t
    {
        Separate,
        Common,
    };

    struct PMXMaterial
    {
        std::string m_name;
        std::string m_englishName;
        glm::vec4 m_diffuse;
        glm::vec3 m_specular;
        float m_specularPower;
        glm::vec3 m_ambient;
        PMXDrawModeFlags m_drawMode;
        glm::vec4 m_edgeColor;
        float m_edgeSize;
        int32_t m_textureIndex;
        int32_t m_sphereTextureIndex;
        PMXSphereMode m_sphereMode;
        PMXToonMode m_toonMode;
        int32_t m_toonTextureIndex;
        std::string m_memo;
        int32_t m_numFaceVertices;
    };

    enum class PMXBoneFlags : uint16_t
    {
        Index = 0x0001,
        Rotatable = 0x0002,
        Movable = 0x0004,
        Visible = 0x0008,
        Controllable = 0x0010,
        HasIK = 0x0020,
        LocalAppend = 0x0080,
        LocalRot = 0x0100,
        LocalTrans = 0x0200,
        FixedAxis = 0x0400,
        LocalAxis = 0x0800,
        DeformAfterPhysics = 0x1000,
        ExternalParent = 0x2000,
    };

    struct PMXBone
    {
        std::string m_name;
        std::string m_englishName;
        glm::vec3 m_position;
        int32_t m_parentBone;
        int32_t m_deformLevel;
        PMXBoneFlags m_boneFlags;
        glm::vec3 m_positionOffset;
        int32_t m_linkBoneIndex;
        int32_t m_appendBone;
        float m_appendWeight;
        glm::vec3 m_fixedAxis;
        glm::vec3 m_localAxisX;
        glm::vec3 m_localAxisZ;
        int32_t m_externalParentBone;
        int32_t m_ikTargetBone;
        int32_t m_ikIteration;
        float m_ikLoopAngle;

        bool m_hasIK;
    };

    struct PMXIKLink
    {
        int32_t m_boneIndex;
        bool m_enableLimit;
        glm::vec3 m_limitMin;
        glm::vec3 m_limitMax;
    };

    struct PMXIK
    {
        int32_t m_ikBoneIndex;      // The bone that has IK
        int32_t m_targetBoneIndex;  // The target bone (usually foot)
        int32_t m_iteration;
        float m_limitAngle;
        std::vector<PMXIKLink> m_links;
    };

    enum class PMXMorphType : uint8_t
    {
        Group,
        Vertex,
        Bone,
        UV,
        AddUV,
        AddUV1,
        AddUV2,
        AddUV3,
        Material,
        Flip,
        Impulse,
    };

    struct PMXMorphVertex
    {
        int32_t m_index;
        glm::vec3 m_position;
    };

    struct PMXMorphUV
    {
        int32_t m_index;
        glm::vec4 m_uv;
    };

    struct PMXMorphBone
    {
        int32_t m_index;
        glm::vec3 m_position;
        glm::quat m_rotation;
    };

    struct PMXMorphMaterial
    {
        int32_t m_index;
        bool m_isMulti;
        uint8_t m_opType;
        glm::vec4 m_diffuse;
        glm::vec3 m_specular;
        float m_specularPower;
        glm::vec3 m_ambient;
        glm::vec4 m_edgeColor;
        float m_edgeSize;
        glm::vec4 m_textureColor;
        glm::vec4 m_sphereColor;
        glm::vec4 m_toonColor;
    };

    struct PMXMorphGroup { int32_t m_index; float m_weight; };

    struct PMXMorph
    {
        std::string m_name;
        std::string m_englishName;
        uint8_t m_controlPanel;
        PMXMorphType m_morphType;
        uint32_t m_morphCount;
        std::vector<PMXMorphVertex> m_vertices;
        std::vector<PMXMorphUV> m_uvs;
        std::vector<PMXMorphUV> m_addUVs;
        std::vector<PMXMorphBone> m_bones;
        std::vector<PMXMorphMaterial> m_materials;
        std::vector<PMXMorphGroup> m_groups;
    };

    struct PMXDisplayFrame
    {
        enum class TargetType : uint8_t
        {
            BoneIndex,
            MorphIndex,
        };

        struct Target
        {
            TargetType m_type;
            int32_t m_index;
        };

        enum class FrameType : uint8_t
        {
            DefaultFrame,   // 0: Default frame
            SpecialFrame,   // 1: Special frame
        };

        std::string m_name;
        std::string m_englishName;
        FrameType m_flag;
        std::vector<Target> m_targets;
    };

    struct PMXRigidBody
    {
        std::string m_name;
        std::string m_englishName;

        int32_t m_boneIndex;
        uint8_t m_group;
        uint16_t m_collisionGroup;

        enum class Shape : uint8_t
        {
            Sphere,
            Box,
            Capsule,
        };
        Shape m_shape;
        glm::vec3 m_shapeSize;

        glm::vec3 m_translate;
        glm::vec3 m_rotate;

        float m_mass;
        float m_translateDimmer;
        float m_rotateDimmer;
        float m_repulsion;
        float m_friction;

        enum class Operation : uint8_t
        {
            Static,
            Dynamic,
            DynamicAndBoneMerge
        };
        Operation m_op;
    };

    struct PMXJoint
    {
        std::string m_name;
        std::string m_englishName;

        enum class JointType : uint8_t
        {
            SpringDOF6,
            DOF6,
            P2P,
            ConeTwist,
            Slider,
            Hinge,
        };
        JointType m_type;
        int32_t m_rigidbodyAIndex;
        int32_t m_rigidbodyBIndex;

        glm::vec3 m_translate;
        glm::vec3 m_rotate;

        glm::vec3 m_translateLowerLimit;
        glm::vec3 m_translateUpperLimit;
        glm::vec3 m_rotateLowerLimit;
        glm::vec3 m_rotateUpperLimit;

        glm::vec3 m_springTranslateFactor;
        glm::vec3 m_springRotateFactor;
    };

    struct PMXModel
    {
        PMXHeader m_header;
        PMXInfo m_info;
        std::vector<PMXVertex> m_vertices;
        std::vector<PMXFace> m_faces;
        std::vector<PMXTexture> m_textures;
        std::vector<PMXMaterial> m_materials;
        std::vector<PMXBone> m_bones;
        std::vector<PMXIK> m_iks;  // IK information for bones
        std::vector<PMXMorph> m_morphs;
        std::vector<PMXDisplayFrame> m_displayFrames;
        std::vector<PMXRigidBody> m_rigidBodies;
        std::vector<PMXJoint> m_joints;
    };

    class PMXFile
    {
    public:
        PMXFile() = default;
        ~PMXFile() = default;

        bool Load(const std::string& filename);
        bool Load(std::istream& stream);
        void Destroy();

        const PMXModel& GetPMXModel() const { return m_model; }

    private:
        bool ReadHeader(std::istream& stream);
        bool ReadInfo(std::istream& stream);
        bool ReadVertices(std::istream& stream);
        bool ReadFaces(std::istream& stream);
        bool ReadTextures(std::istream& stream);
        bool ReadMaterials(std::istream& stream);
        bool ReadBones(std::istream& stream);
        bool ReadMorphs(std::istream& stream);
        bool ReadDisplayFrames(std::istream& stream);
        bool ReadRigidBodies(std::istream& stream);
        bool ReadJoints(std::istream& stream);

        PMXModel m_model;
    };
}
