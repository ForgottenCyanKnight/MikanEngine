#include "Animation/MMD/Model/PMXFile.h"
#include "Animation/MMD/Base/MMDTypes.h"
#include <fstream>
#include "Core/Utf8Path.h"
#include <iostream>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#endif

namespace mmd
{
    bool PMXFile::Load(const std::string& filename)
    {
#ifdef __ANDROID__
        size_t size=0;void* data=SDL_LoadFile(filename.c_str(),&size);
        if(!data) return false;
        std::string bytes(static_cast<const char*>(data),size);SDL_free(data);
        std::istringstream file(bytes,std::ios::binary);
#else
        std::ifstream file(Utf8Path(filename), std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "[PMXFile] Failed to open file: " << filename << std::endl;
            return false;
        }
#endif
        return Load(file);
    }

    bool PMXFile::Load(std::istream& stream)
    {
        if (!ReadHeader(stream)) {
            std::cerr << "[PMXFile] Failed to read header" << std::endl;
            return false;
        }

        const char* magic = m_model.m_header.m_magic.c_str();
        if (magic[0] != 'P' || magic[1] != 'M' ||
            magic[2] != 'X' || magic[3] != ' ') {
            std::cerr << "[PMXFile] Invalid PMX magic" << std::endl;
            return false;
        }

        if (!ReadInfo(stream)) {
            std::cerr << "[PMXFile] Failed to read info" << std::endl;
            return false;
        }

        if (!ReadVertices(stream)) {
            std::cerr << "[PMXFile] Failed to read vertices" << std::endl;
            return false;
        }

        if (!ReadFaces(stream)) {
            std::cerr << "[PMXFile] Failed to read faces" << std::endl;
            return false;
        }

        if (!ReadTextures(stream)) {
            std::cerr << "[PMXFile] Failed to read textures" << std::endl;
            return false;
        }

        if (!ReadMaterials(stream)) {
            std::cerr << "[PMXFile] Failed to read materials" << std::endl;
            return false;
        }

        if (!ReadBones(stream)) {
            std::cerr << "[PMXFile] Failed to read bones" << std::endl;
            return false;
        }

        if (!ReadMorphs(stream)) {
            std::cerr << "[PMXFile] Warning: Failed to read morphs" << std::endl;
            return false;
        }

        if (!ReadDisplayFrames(stream)) {
            std::cerr << "[PMXFile] Warning: Failed to read display frames" << std::endl;
            return false;
        }

        if (!ReadRigidBodies(stream)) {
            std::cerr << "[PMXFile] Warning: Failed to read rigid bodies" << std::endl;
            return false;
        }

        if (!ReadJoints(stream)) {
            std::cerr << "[PMXFile] Warning: Failed to read joints" << std::endl;
            return false;
        }

        std::cout << "[PMXFile] Loaded: " << m_model.m_info.m_modelName << std::endl;
        std::cout << "  Vertices: " << m_model.m_vertices.size() << std::endl;
        std::cout << "  Faces: " << m_model.m_faces.size() << std::endl;
        std::cout << "  Textures: " << m_model.m_textures.size() << std::endl;
        std::cout << "  Materials: " << m_model.m_materials.size() << std::endl;
        std::cout << "  Bones: " << m_model.m_bones.size() << std::endl;
        std::cout << "  Morphs: " << m_model.m_morphs.size() << std::endl;
        std::cout << "  RigidBodies: " << m_model.m_rigidBodies.size() << std::endl;
        std::cout << "  Joints: " << m_model.m_joints.size() << std::endl;

        return true;
    }

    void PMXFile::Destroy()
    {
        m_model = PMXModel();
    }

    bool PMXFile::ReadHeader(std::istream& stream)
    {
        auto& header = m_model.m_header;
        Read(stream, header.m_magic);
        Read(stream, header.m_version);
        Read(stream, header.m_dataSize);
        Read(stream, header.m_encode);
        Read(stream, header.m_addUVNum);
        Read(stream, header.m_vertexIndexSize);
        Read(stream, header.m_textureIndexSize);
        Read(stream, header.m_materialIndexSize);
        Read(stream, header.m_boneIndexSize);
        Read(stream, header.m_morphIndexSize);
        Read(stream, header.m_rigidbodyIndexSize);
        auto validSize=[](uint8_t n){return n==1 || n==2 || n==4;};
        return stream.good() && header.m_dataSize==8 && header.m_encode<=1 && header.m_addUVNum<=4 &&
            (header.m_version==2.0f || header.m_version==2.1f) && validSize(header.m_vertexIndexSize) &&
            validSize(header.m_textureIndexSize) && validSize(header.m_materialIndexSize) &&
            validSize(header.m_boneIndexSize) && validSize(header.m_morphIndexSize) && validSize(header.m_rigidbodyIndexSize);
    }

    bool PMXFile::ReadInfo(std::istream& stream)
    {
        auto& info = m_model.m_info;
        auto& header = m_model.m_header;

        if (!ReadPMXString(stream, info.m_modelName, header.m_encode)) {
            return false;
        }
        if (!ReadPMXString(stream, info.m_englishModelName, header.m_encode)) {
            return false;
        }
        if (!ReadPMXString(stream, info.m_comment, header.m_encode)) {
            return false;
        }
        if (!ReadPMXString(stream, info.m_englishComment, header.m_encode)) {
            return false;
        }

        return stream.good();
    }

    bool PMXFile::ReadVertices(std::istream& stream)
    {
        int32_t numVertices;
        if (!Read(stream, numVertices) || numVertices < 0 || numVertices > 10000000) return false;

        m_model.m_vertices.resize(numVertices);
        auto& header = m_model.m_header;

        for (int32_t i = 0; i < numVertices; i++) {
            auto& vertex = m_model.m_vertices[i];

            // Initialize bone indices and weights to safe defaults
            for (int j = 0; j < 4; j++) {
                vertex.m_boneIndices[j] = -1;
                vertex.m_boneWeights[j] = 0.0f;
            }

            Read(stream, vertex.m_position);
            Read(stream, vertex.m_normal);
            Read(stream, vertex.m_uv);

            for (int j = 0; j < header.m_addUVNum; j++) {
                Read(stream, vertex.m_addUV[j]);
            }

            uint8_t weightType;
            Read(stream, weightType);
            vertex.m_weightType = static_cast<PMXVertexWeightType>(weightType);

            switch (vertex.m_weightType) {
            case PMXVertexWeightType::BDEF1:
                vertex.m_boneIndices[0] = ReadPMXValue<int32_t>(stream, header.m_boneIndexSize);
                vertex.m_boneWeights[0] = 1.0f;
                break;
            case PMXVertexWeightType::BDEF2:
                vertex.m_boneIndices[0] = ReadPMXValue<int32_t>(stream, header.m_boneIndexSize);
                vertex.m_boneIndices[1] = ReadPMXValue<int32_t>(stream, header.m_boneIndexSize);
                Read(stream, vertex.m_boneWeights[0]);
                vertex.m_boneWeights[1] = 1.0f - vertex.m_boneWeights[0];
                break;
            case PMXVertexWeightType::BDEF4:
                for (int j = 0; j < 4; j++) {
                    vertex.m_boneIndices[j] = ReadPMXValue<int32_t>(stream, header.m_boneIndexSize);
                }
                for (int j = 0; j < 4; j++) {
                    Read(stream, vertex.m_boneWeights[j]);
                }
                break;
            case PMXVertexWeightType::SDEF:
                vertex.m_boneIndices[0] = ReadPMXValue<int32_t>(stream, header.m_boneIndexSize);
                vertex.m_boneIndices[1] = ReadPMXValue<int32_t>(stream, header.m_boneIndexSize);
                Read(stream, vertex.m_boneWeights[0]);
                vertex.m_boneWeights[1] = 1.0f - vertex.m_boneWeights[0];
                Read(stream, vertex.m_sdefC);
                Read(stream, vertex.m_sdefR0);
                Read(stream, vertex.m_sdefR1);
                break;
            case PMXVertexWeightType::QDEF:
                for (int j = 0; j < 4; j++) {
                    vertex.m_boneIndices[j] = ReadPMXValue<int32_t>(stream, header.m_boneIndexSize);
                }
                for (int j = 0; j < 4; j++) {
                    Read(stream, vertex.m_boneWeights[j]);
                }
                break;
            }

            Read(stream, vertex.m_edgeMag);
        }

        return stream.good();
    }

    bool PMXFile::ReadFaces(std::istream& stream)
    {
        int32_t numFaces;
        if (!Read(stream, numFaces) || numFaces < 0 || numFaces > 10000000) return false;

        if(numFaces%3!=0) return false;
        m_model.m_faces.resize(numFaces / 3);

        auto& header = m_model.m_header;
        for (size_t i = 0; i < m_model.m_faces.size(); i++) {
            for (int j = 0; j < 3; j++) {
                m_model.m_faces[i].m_vertices[j] = ReadPMXValueU<uint32_t>(stream, header.m_vertexIndexSize);
            }
        }

        return stream.good();
    }

    bool PMXFile::ReadTextures(std::istream& stream)
    {
        int32_t numTextures;
        if (!Read(stream, numTextures) || numTextures < 0 || numTextures > 10000000) return false;

        m_model.m_textures.resize(numTextures);
        auto& header = m_model.m_header;

        for (int32_t i = 0; i < numTextures; i++) {
            if (!ReadPMXString(stream, m_model.m_textures[i].m_textureName, header.m_encode)) {
                return false;
            }
        }

        return stream.good();
    }

    bool PMXFile::ReadMaterials(std::istream& stream)
    {
        int32_t numMaterials;
        if (!Read(stream, numMaterials) || numMaterials < 0 || numMaterials > 10000000) return false;

        m_model.m_materials.resize(numMaterials);

        auto& header = m_model.m_header;

        for (int32_t i = 0; i < numMaterials; i++) {
            auto& material = m_model.m_materials[i];

            if (!ReadPMXString(stream, material.m_name, header.m_encode)) {
                return false;
            }
            if (!ReadPMXString(stream, material.m_englishName, header.m_encode)) {
                return false;
            }
            Read(stream, material.m_diffuse);
            Read(stream, material.m_specular);
            Read(stream, material.m_specularPower);
            Read(stream, material.m_ambient);

            uint8_t drawMode;
            Read(stream, drawMode);
            material.m_drawMode = static_cast<PMXDrawModeFlags>(drawMode);

            Read(stream, material.m_edgeColor);
            Read(stream, material.m_edgeSize);

            material.m_textureIndex = ReadPMXValue<int32_t>(stream, header.m_textureIndexSize);
            material.m_sphereTextureIndex = ReadPMXValue<int32_t>(stream, header.m_textureIndexSize);

            uint8_t sphereMode;
            Read(stream, sphereMode);
            material.m_sphereMode = static_cast<PMXSphereMode>(sphereMode);

            uint8_t toonMode;
            Read(stream, toonMode);
            material.m_toonMode = static_cast<PMXToonMode>(toonMode);

            material.m_toonTextureIndex = ReadPMXValue<int32_t>(stream, header.m_textureIndexSize);

            if (!ReadPMXString(stream, material.m_memo, header.m_encode)) {
                return false;
            }

            Read(stream, material.m_numFaceVertices);
        }

        return stream.good();
    }

    bool PMXFile::ReadBones(std::istream& stream)
    {
        int32_t numBones;
        if (!Read(stream, numBones) || numBones < 0 || numBones > 1000000) {
            return false;
        }

        m_model.m_bones.resize(numBones);

        auto& header = m_model.m_header;

        for (int32_t i = 0; i < numBones; i++) {
            auto& bone = m_model.m_bones[i];

            if (!ReadPMXString(stream, bone.m_name, header.m_encode)) {
                return false;
            }
            if (!ReadPMXString(stream, bone.m_englishName, header.m_encode)) {
                return false;
            }
            if (!Read(stream, bone.m_position)) {
                return false;
            }
            bone.m_parentBone = ReadPMXValue<int32_t>(stream, header.m_boneIndexSize);
            if (!stream.good()) {
                return false;
            }
            if (!Read(stream, bone.m_deformLevel)) {
                return false;
            }

            uint16_t boneFlags;
            if (!Read(stream, boneFlags)) {
                return false;
            }
            bone.m_boneFlags = static_cast<PMXBoneFlags>(boneFlags);

            if ((static_cast<uint16_t>(bone.m_boneFlags) & static_cast<uint16_t>(PMXBoneFlags::Index)) == 0) {
                if (!Read(stream, bone.m_positionOffset)) {
                    return false;
                }
            } else {
                bone.m_linkBoneIndex = ReadPMXValue<int32_t>(stream, header.m_boneIndexSize);
                if (!stream.good()) {
                    return false;
                }
            }

            if ((static_cast<uint16_t>(bone.m_boneFlags) & static_cast<uint16_t>(PMXBoneFlags::LocalRot)) ||
                (static_cast<uint16_t>(bone.m_boneFlags) & static_cast<uint16_t>(PMXBoneFlags::LocalTrans))) {
                bone.m_appendBone = ReadPMXValue<int32_t>(stream, header.m_boneIndexSize);
                if (!stream.good()) {
                    return false;
                }
                if (!Read(stream, bone.m_appendWeight)) {
                    return false;
                }
            }

            if (static_cast<uint16_t>(bone.m_boneFlags) & static_cast<uint16_t>(PMXBoneFlags::FixedAxis)) {
                if (!Read(stream, bone.m_fixedAxis)) {
                    return false;
                }
            }

            if (static_cast<uint16_t>(bone.m_boneFlags) & static_cast<uint16_t>(PMXBoneFlags::LocalAxis)) {
                if (!Read(stream, bone.m_localAxisX)) {
                    return false;
                }
                if (!Read(stream, bone.m_localAxisZ)) {
                    return false;
                }
            }

            if (static_cast<uint16_t>(bone.m_boneFlags) & static_cast<uint16_t>(PMXBoneFlags::ExternalParent)) {
                if (!Read(stream, bone.m_externalParentBone)) {
                    return false;
                }
            }

            if (static_cast<uint16_t>(bone.m_boneFlags) & static_cast<uint16_t>(PMXBoneFlags::HasIK)) {
                bone.m_ikTargetBone = ReadPMXValue<int32_t>(stream, header.m_boneIndexSize);
                if (!stream.good()) {
                    return false;
                }
                if (!Read(stream, bone.m_ikIteration)) {
                    return false;
                }
                if (!Read(stream, bone.m_ikLoopAngle)) {
                    return false;
                }

                // Read IK links
                int32_t linkCount;
                if (!Read(stream, linkCount) || linkCount < 0 || linkCount > 1000000) {
                    return false;
                }

                // Create IK structure
                PMXIK ik;
                ik.m_ikBoneIndex = i; // Current bone index
                ik.m_targetBoneIndex = bone.m_ikTargetBone;
                ik.m_iteration = bone.m_ikIteration;
                ik.m_limitAngle = bone.m_ikLoopAngle;
                ik.m_links.reserve(linkCount);

                for (int32_t j = 0; j < linkCount; j++) {
                    PMXIKLink link;
                    link.m_boneIndex = ReadPMXValue<int32_t>(stream, header.m_boneIndexSize);
                    if (!stream.good()) {
                        return false;
                    }
                    uint8_t enableLimit;
                    if (!Read(stream, enableLimit)) {
                        return false;
                    }
                    link.m_enableLimit = (enableLimit != 0);
                    if (link.m_enableLimit) {
                        if (!Read(stream, link.m_limitMin)) {
                            return false;
                        }
                        if (!Read(stream, link.m_limitMax)) {
                            return false;
                        }
                    }
                    ik.m_links.push_back(link);
                }

                m_model.m_iks.push_back(ik);
                bone.m_hasIK = true;
            } else {
                bone.m_hasIK = false;
            }
        }

        return stream.good();
    }

    bool PMXFile::ReadMorphs(std::istream& stream)
    {
        int32_t numMorphs;
        if (!Read(stream, numMorphs) || numMorphs < 0 || numMorphs > 1000000) {
            return false;
        }

        m_model.m_morphs.resize(numMorphs);

        auto& header = m_model.m_header;

        for (int32_t i = 0; i < numMorphs; i++) {
            auto& morph = m_model.m_morphs[i];

            if (!ReadPMXString(stream, morph.m_name, header.m_encode)) {
                return false;
            }
            if (!ReadPMXString(stream, morph.m_englishName, header.m_encode)) {
                return false;
            }

            if (!Read(stream, morph.m_controlPanel)) {
                return false;
            }

            uint8_t morphType;
            if (!Read(stream, morphType)) {
                return false;
            }
            morph.m_morphType = static_cast<PMXMorphType>(morphType);

            if (!Read(stream, morph.m_morphCount) || morph.m_morphCount>10000000) {
                return false;
            }

            switch (morph.m_morphType) {
            case PMXMorphType::Group:
            case PMXMorphType::Flip:
                morph.m_groups.resize(morph.m_morphCount);
                for(auto& g:morph.m_groups) {
                    g.m_index=ReadPMXValue<int32_t>(stream,header.m_morphIndexSize);
                    if(!Read(stream,g.m_weight)) return false;
                }
                break;
            case PMXMorphType::Impulse:
                for(uint32_t j=0;j<morph.m_morphCount;++j) {
                    ReadPMXValue<int32_t>(stream,header.m_rigidbodyIndexSize);
                    uint8_t local;glm::vec3 velocity,torque;
                    if(!Read(stream,local)||!Read(stream,velocity)||!Read(stream,torque)) return false;
                }
                break;
            case PMXMorphType::Vertex:
                morph.m_vertices.resize(morph.m_morphCount);
                for (uint32_t j = 0; j < morph.m_morphCount; j++) {
                    morph.m_vertices[j].m_index = ReadPMXValueU<int32_t>(stream, header.m_vertexIndexSize);
                    if (!stream.good()) {
                        return false;
                    }
                    if (!Read(stream, morph.m_vertices[j].m_position)) {
                        return false;
                    }
                }
                break;
            case PMXMorphType::UV:
                morph.m_uvs.resize(morph.m_morphCount);
                for (uint32_t j = 0; j < morph.m_morphCount; j++) {
                    morph.m_uvs[j].m_index = ReadPMXValueU<int32_t>(stream, header.m_vertexIndexSize);
                    if (!stream.good()) {
                        return false;
                    }
                    if (!Read(stream, morph.m_uvs[j].m_uv)) {
                        return false;
                    }
                }
                break;
            case PMXMorphType::AddUV:
            case PMXMorphType::AddUV1:
            case PMXMorphType::AddUV2:
            case PMXMorphType::AddUV3:
                morph.m_addUVs.resize(morph.m_morphCount);
                for (uint32_t j = 0; j < morph.m_morphCount; j++) {
                    morph.m_addUVs[j].m_index = ReadPMXValueU<int32_t>(stream, header.m_vertexIndexSize);
                    if (!stream.good()) {
                        return false;
                    }
                    if (!Read(stream, morph.m_addUVs[j].m_uv)) {
                        return false;
                    }
                }
                break;
            case PMXMorphType::Bone:
                morph.m_bones.resize(morph.m_morphCount);
                for (uint32_t j = 0; j < morph.m_morphCount; j++) {
                    morph.m_bones[j].m_index = ReadPMXValue<int32_t>(stream, header.m_boneIndexSize);
                    if (!stream.good()) {
                        return false;
                    }
                    if (!Read(stream, morph.m_bones[j].m_position)) {
                        return false;
                    }
                    if (!Read(stream, morph.m_bones[j].m_rotation)) {
                        return false;
                    }
                }
                break;
            case PMXMorphType::Material:
                morph.m_materials.resize(morph.m_morphCount);
                for (uint32_t j = 0; j < morph.m_morphCount; j++) {

                    morph.m_materials[j].m_index = ReadPMXValue<int32_t>(stream, header.m_materialIndexSize);
                    if (!stream.good()) {
                        return false;
                    }
                    if (!Read(stream, morph.m_materials[j].m_opType)) {
                        return false;
                    }
                    if (!Read(stream, morph.m_materials[j].m_diffuse)) {
                        return false;
                    }
                    if (!Read(stream, morph.m_materials[j].m_specular)) {
                        return false;
                    }
                    if (!Read(stream, morph.m_materials[j].m_specularPower)) {
                        return false;
                    }
                    if (!Read(stream, morph.m_materials[j].m_ambient)) {
                        return false;
                    }
                    if (!Read(stream, morph.m_materials[j].m_edgeColor)) {
                        return false;
                    }
                    if (!Read(stream, morph.m_materials[j].m_edgeSize)) {
                        return false;
                    }
                    if (!Read(stream, morph.m_materials[j].m_textureColor)) return false;
                    if (!Read(stream, morph.m_materials[j].m_sphereColor)) {
                        return false;
                    }
                    if (!Read(stream, morph.m_materials[j].m_toonColor)) {
                        return false;
                    }
                }
                break;
            default:
                return false;
            }
        }

        return true; // Always return true if we read all morphs
    }

    bool PMXFile::ReadDisplayFrames(std::istream& stream)
    {
        int32_t numFrames;
        if (!Read(stream, numFrames) || numFrames < 0 || numFrames > 1000000) {
            return false;
        }

        m_model.m_displayFrames.resize(numFrames);

        auto& header = m_model.m_header;

        for (int32_t i = 0; i < numFrames; i++) {
            auto& frame = m_model.m_displayFrames[i];

            if (!ReadPMXString(stream, frame.m_name, header.m_encode)) {
                return false;
            }
            if (!ReadPMXString(stream, frame.m_englishName, header.m_encode)) {
                return false;
            }

            uint8_t flag;
            if (!Read(stream, flag)) {
                return false;
            }
            frame.m_flag = static_cast<PMXDisplayFrame::FrameType>(flag);

            int32_t targetCount;
            if (!Read(stream, targetCount) || targetCount < 0 || targetCount > 1000000) {
                return false;
            }
            frame.m_targets.resize(targetCount);

            for (int32_t j = 0; j < targetCount; j++) {
                auto& target = frame.m_targets[j];

                uint8_t type;
                if (!Read(stream, type)) {
                    return false;
                }
                target.m_type = static_cast<PMXDisplayFrame::TargetType>(type);

                if (target.m_type == PMXDisplayFrame::TargetType::BoneIndex) {
                    target.m_index = ReadPMXValue<int32_t>(stream, header.m_boneIndexSize);
                    if (!stream.good()) {
                        return false;
                    }
                } else if (target.m_type == PMXDisplayFrame::TargetType::MorphIndex) {
                    target.m_index = ReadPMXValue<int32_t>(stream, header.m_morphIndexSize);
                    if (!stream.good()) {
                        return false;
                    }
                } else {
                    // Skip unknown target types
                    continue;
                }
            }
        }

        return true; // Always return true if we read all display frames
    }

    bool PMXFile::ReadRigidBodies(std::istream& stream)
    {
        int32_t numRigidBodies;
        if (!Read(stream, numRigidBodies) || numRigidBodies < 0 || numRigidBodies > 1000000) {
            return false;
        }

        m_model.m_rigidBodies.resize(numRigidBodies);

        auto& header = m_model.m_header;

        for (int32_t i = 0; i < numRigidBodies; i++) {
            auto& body = m_model.m_rigidBodies[i];

            if (!ReadPMXString(stream, body.m_name, header.m_encode)) {
                return false;
            }
            if (!ReadPMXString(stream, body.m_englishName, header.m_encode)) {
                return false;
            }

            body.m_boneIndex = ReadPMXValue<int32_t>(stream, header.m_boneIndexSize);
            if (!stream.good()) {
                return false;
            }

            uint8_t group;
            if (!Read(stream, group)) {
                return false;
            }
            body.m_group = group;

            if (!Read(stream, body.m_collisionGroup)) {
                return false;
            }

            uint8_t shape;
            if (!Read(stream, shape)) {
                return false;
            }
            body.m_shape = static_cast<PMXRigidBody::Shape>(shape);

            if (!Read(stream, body.m_shapeSize)) {
                return false;
            }
            if (!Read(stream, body.m_translate)) {
                return false;
            }
            if (!Read(stream, body.m_rotate)) {
                return false;
            }
            if (!Read(stream, body.m_mass)) {
                return false;
            }
            if (!Read(stream, body.m_translateDimmer)) {
                return false;
            }
            if (!Read(stream, body.m_rotateDimmer)) {
                return false;
            }
            if (!Read(stream, body.m_repulsion)) {
                return false;
            }
            if (!Read(stream, body.m_friction)) {
                return false;
            }

            uint8_t op;
            if (!Read(stream, op)) {
                return false;
            }
            body.m_op = static_cast<PMXRigidBody::Operation>(op);
        }

        return true; // Always return true if we read all rigid bodies
    }

    bool PMXFile::ReadJoints(std::istream& stream)
    {
        int32_t numJoints;
        if (!Read(stream, numJoints) || numJoints < 0 || numJoints > 1000000) {
            return false;
        }

        m_model.m_joints.resize(numJoints);

        auto& header = m_model.m_header;

        for (int32_t i = 0; i < numJoints; i++) {
            auto& joint = m_model.m_joints[i];

            if (!ReadPMXString(stream, joint.m_name, header.m_encode)) {
                return false;
            }
            if (!ReadPMXString(stream, joint.m_englishName, header.m_encode)) {
                return false;
            }

            uint8_t type;
            if (!Read(stream, type)) {
                return false;
            }
            joint.m_type = static_cast<PMXJoint::JointType>(type);

            joint.m_rigidbodyAIndex = ReadPMXValue<int32_t>(stream, header.m_rigidbodyIndexSize);
            if (!stream.good()) {
                return false;
            }

            joint.m_rigidbodyBIndex = ReadPMXValue<int32_t>(stream, header.m_rigidbodyIndexSize);
            if (!stream.good()) {
                return false;
            }

            if (!Read(stream, joint.m_translate)) {
                return false;
            }
            if (!Read(stream, joint.m_rotate)) {
                return false;
            }
            if (!Read(stream, joint.m_translateLowerLimit)) {
                return false;
            }
            if (!Read(stream, joint.m_translateUpperLimit)) {
                return false;
            }
            if (!Read(stream, joint.m_rotateLowerLimit)) {
                return false;
            }
            if (!Read(stream, joint.m_rotateUpperLimit)) {
                return false;
            }
            if (!Read(stream, joint.m_springTranslateFactor)) {
                return false;
            }
            if (!Read(stream, joint.m_springRotateFactor)) {
                return false;
            }
        }

        return true; // Always return true if we read all joints
    }
}
