#include "Animation/MMD/Model/MMDModel.h"
#include "Animation/MMD/Model/VMDAnimation.h"
#include "Animation/MMD/Physics/MMDPhysics.h"
#include <unordered_map>

namespace mmd {

    MMDModel::MMDModel()
        : m_modelMatrix(glm::mat4(1.0f))
    {
    }

    MMDModel::~MMDModel()
    {
    }

    void MMDModel::UpdateAllAnimation(VMDAnimation* vmdAnim, float vmdFrame, float physicsElapsed)
    {
        if (vmdAnim != nullptr)
        {
            vmdAnim->Evaluate(vmdFrame);
        }

        UpdateMorphAnimation();
        UpdateNodeAnimation(false);
        UpdatePhysicsAnimation(physicsElapsed);
        UpdateNodeAnimation(true);
    }

    MMDModelImpl::MMDModelImpl()
        : m_afterPhysicsAnim(false)
        , m_fps(60.0f)
        , m_parallelUpdateCount(0)
    {
    }

    MMDModelImpl::~MMDModelImpl()
    {
        Destroy();
    }

    bool MMDModelImpl::Create(const std::string& pmxFile) {
        Destroy();

        // Load PMX file
        mmd::PMXFile pmxFileObj;
        if (!pmxFileObj.Load(pmxFile)) {
            return false;
        }

        const auto& pmxModel = pmxFileObj.GetPMXModel();
        // Reject cyclic hierarchies before recursive IK and child traversal.
        for(size_t i=0;i<pmxModel.m_bones.size();++i) {
            int parent=static_cast<int>(i);size_t depth=0;
            while(parent>=0) {
                if(static_cast<size_t>(parent)>=pmxModel.m_bones.size() || ++depth>pmxModel.m_bones.size()) return false;
                parent=pmxModel.m_bones[parent].m_parentBone;
            }
        }

        // Create physics system
        m_physics = std::make_unique<MMDPhysics>();
        if (!m_physics->Create()) {
            return false;
        }

        // Load vertices
        m_positions.reserve(pmxModel.m_vertices.size());
        m_normals.reserve(pmxModel.m_vertices.size());
        m_uvs.reserve(pmxModel.m_vertices.size());
        m_updatedPositions.resize(pmxModel.m_vertices.size());
        m_updatedNormals.resize(pmxModel.m_vertices.size());
        m_updatedUVs.resize(pmxModel.m_vertices.size());

        // Resize skinning data
        m_boneIndices.resize(pmxModel.m_vertices.size() * 4);
        m_boneWeights.resize(pmxModel.m_vertices.size() * 4);
        m_weightTypes.resize(pmxModel.m_vertices.size());
        m_sdefC.resize(pmxModel.m_vertices.size());
        m_sdefR0.resize(pmxModel.m_vertices.size());
        m_sdefR1.resize(pmxModel.m_vertices.size());

        for (size_t i = 0; i < pmxModel.m_vertices.size(); i++) {
            const auto& vertex = pmxModel.m_vertices[i];
            m_positions.push_back(glm::vec3(vertex.m_position.x, vertex.m_position.y, -vertex.m_position.z));
            m_normals.push_back(glm::vec3(vertex.m_normal.x, vertex.m_normal.y, -vertex.m_normal.z));
            m_uvs.push_back(vertex.m_uv);

            // Store skinning data
            for (int j = 0; j < 4; j++) {
                m_boneIndices[i * 4 + j] = vertex.m_boneIndices[j];
                m_boneWeights[i * 4 + j] = vertex.m_boneWeights[j];
            }
            m_weightTypes[i] = vertex.m_weightType;
            m_sdefC[i] = glm::vec3(vertex.m_sdefC.x, vertex.m_sdefC.y, -vertex.m_sdefC.z);
            m_sdefR0[i] = glm::vec3(vertex.m_sdefR0.x, vertex.m_sdefR0.y, -vertex.m_sdefR0.z);
            m_sdefR1[i] = glm::vec3(vertex.m_sdefR1.x, vertex.m_sdefR1.y, -vertex.m_sdefR1.z);
        }

        // Load indices
        m_indices.reserve(pmxModel.m_faces.size() * 3);
        for (const auto& face : pmxModel.m_faces) {
            m_indices.push_back(face.m_vertices[0]);
            m_indices.push_back(face.m_vertices[1]);
            m_indices.push_back(face.m_vertices[2]);
        }

        // Load materials
        m_materials.reserve(pmxModel.m_materials.size());
        for (const auto& material : pmxModel.m_materials) {
            MMDMaterial mmdMaterial;
            mmdMaterial.m_name = material.m_name;
            mmdMaterial.m_diffuse = material.m_diffuse;
            mmdMaterial.m_specular = material.m_specular;
            mmdMaterial.m_specularPower = material.m_specularPower;
            mmdMaterial.m_ambient = material.m_ambient;
            mmdMaterial.m_flags = static_cast<MMDMaterialFlags>(material.m_drawMode);
            mmdMaterial.m_edgeColor = material.m_edgeColor;
            mmdMaterial.m_edgeSize = material.m_edgeSize;
            mmdMaterial.m_textureIndex = material.m_textureIndex;
            mmdMaterial.m_sphereTextureIndex = material.m_sphereTextureIndex;
            mmdMaterial.m_sphereMode = static_cast<MMDSphereMode>(material.m_sphereMode);
            mmdMaterial.m_toonMode = static_cast<MMDToonMode>(material.m_toonMode);
            mmdMaterial.m_toonTextureIndex = material.m_toonTextureIndex;
            mmdMaterial.m_numFaceVertices = material.m_numFaceVertices;
            m_materials.push_back(mmdMaterial);
        }

        // Load submeshes
        int currentIndex = 0;
        for (const auto& material : pmxModel.m_materials) {
            MMDSubMesh subMesh;
            subMesh.m_beginIndex = currentIndex;
            subMesh.m_vertexCount = material.m_numFaceVertices;
            subMesh.m_materialID = static_cast<int>(m_subMeshes.size());
            m_subMeshes.push_back(subMesh);
            currentIndex += material.m_numFaceVertices;
        }

        // Create nodes from bones
        //std::cout << "[MMDModel] Creating " << pmxModel.m_bones.size() << " nodes from bones" << std::endl;
        int deformAfterPhysicsCount = 0;
        for (const auto& bone : pmxModel.m_bones) {
            auto node = std::make_unique<MMDNode>();
            node->SetName(bone.m_name);
            node->SetLocalPosition(glm::vec3(bone.m_position.x, bone.m_position.y, -bone.m_position.z));
            node->SetLocalRotation(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

            // Set DeformAfterPhysics flag
            bool deformAfterPhysics = (static_cast<uint16_t>(bone.m_boneFlags) & static_cast<uint16_t>(PMXBoneFlags::DeformAfterPhysics)) != 0;
            node->SetDeformAfterPhysics(deformAfterPhysics);
            node->SetDeformDepth(bone.m_deformLevel);
            if (deformAfterPhysics) {
                deformAfterPhysicsCount++;
                //std::cout << "[MMDModel] Bone: " << bone.m_name << " has DeformAfterPhysics flag" << std::endl;
            }

            m_nodes.push_back(std::move(node));
        }
       //std::cout << "[MMDModel] Total bones with DeformAfterPhysics: " << deformAfterPhysicsCount << std::endl;

        for (size_t i=0; i<m_nodes.size(); ++i) {
            const auto& bone=pmxModel.m_bones[i];
            const auto flags=static_cast<uint16_t>(bone.m_boneFlags);
            const bool rotate=(flags & static_cast<uint16_t>(PMXBoneFlags::LocalRot))!=0;
            const bool translate=(flags & static_cast<uint16_t>(PMXBoneFlags::LocalTrans))!=0;
            if ((rotate || translate) && bone.m_appendBone>=0 && static_cast<size_t>(bone.m_appendBone)<m_nodes.size())
                m_nodes[i]->ConfigureAppend(m_nodes[bone.m_appendBone].get(),bone.m_appendWeight,rotate,translate,
                    (flags & static_cast<uint16_t>(PMXBoneFlags::LocalAppend))!=0);
            m_sortedNodes.push_back(m_nodes[i].get());
        }
        std::stable_sort(m_sortedNodes.begin(),m_sortedNodes.end(),[](const MMDNode* a,const MMDNode* b) {
            return a->GetDeformDepth()<b->GetDeformDepth();
        });

        // Create IK solvers from PMX IK data
        m_ikSolvers.clear();
        //std::cout << "[MMDModel] Creating " << pmxModel.m_iks.size() << " IK solvers" << std::endl;
        for (const auto& pmxIK : pmxModel.m_iks) {
            auto ikSolver = std::make_unique<MMDIkSolver>();

            // Set IK solver name from the IK bone name
            if (pmxIK.m_ikBoneIndex >= 0 && pmxIK.m_ikBoneIndex < static_cast<int>(pmxModel.m_bones.size())) {
                ikSolver->SetName(pmxModel.m_bones[pmxIK.m_ikBoneIndex].m_name);
            }

            // Set IK node (the bone that has IK)
            if (pmxIK.m_ikBoneIndex >= 0 && pmxIK.m_ikBoneIndex < static_cast<int>(m_nodes.size())) {
                ikSolver->SetIKNode(m_nodes[pmxIK.m_ikBoneIndex].get());
                m_nodes[pmxIK.m_ikBoneIndex]->SetIKSolver(ikSolver.get());
            }

            // Set target node (usually the foot or hand)
            if (pmxIK.m_targetBoneIndex >= 0 && pmxIK.m_targetBoneIndex < static_cast<int>(m_nodes.size())) {
                ikSolver->SetTargetNode(m_nodes[pmxIK.m_targetBoneIndex].get());
            }

            // Set iteration count and limit angle
            ikSolver->SetIterateCount(static_cast<uint32_t>(pmxIK.m_iteration));
            ikSolver->SetLimitAngle(pmxIK.m_limitAngle);

            // Add IK chain links
            for (const auto& link : pmxIK.m_links) {
                if (link.m_boneIndex >= 0 && link.m_boneIndex < static_cast<int>(m_nodes.size())) {
                    // Match Saba's PMX IK limits on all three axes.
                    glm::vec3 limitMin = -link.m_limitMax;
                    glm::vec3 limitMax = -link.m_limitMin;


                    // Debug: Print angle limits for knee joints
                    // if (link.m_enableLimit) {
                    //     std::cout << "[MMDModel] IK Link: " << m_nodes[link.m_boneIndex]->GetName()
                    //               << " Original LimitMin: (" << link.m_limitMin.x << ", " << link.m_limitMin.y << ", " << link.m_limitMin.z << ")"
                    //               << " Original LimitMax: (" << link.m_limitMax.x << ", " << link.m_limitMax.y << ", " << link.m_limitMax.z << ")"
                    //               << " Converted LimitMin: (" << limitMin.x << ", " << limitMin.y << ", " << limitMin.z << ")"
                    //               << " Converted LimitMax: (" << limitMax.x << ", " << limitMax.y << ", " << limitMax.z << ")" << std::endl;
                    // }
                    ikSolver->AddIKChain(m_nodes[link.m_boneIndex].get(), link.m_enableLimit, limitMin, limitMax);
                    m_nodes[link.m_boneIndex]->EnableIK(true);
                }
            }

            // Enable IK solver by default
            ikSolver->Enable(true);

            m_ikSolvers.push_back(std::move(ikSolver));
            //std::cout << "[MMDModel] Created IK solver: " << ikSolver->GetName() << std::endl;
        }

        // Set parent-child relationships and calculate local positions
        for (size_t i = 0; i < pmxModel.m_bones.size(); i++) {
            const auto& bone = pmxModel.m_bones[i];
            int parentIndex = bone.m_parentBone;

            // Calculate local position relative to parent
            glm::vec3 localPos;
            if (parentIndex >= 0 && parentIndex < static_cast<int>(m_nodes.size())) {
                m_nodes[i]->SetParent(m_nodes[parentIndex].get());
                m_nodes[parentIndex]->AddChild(m_nodes[i].get());

                const auto& parentBone = pmxModel.m_bones[parentIndex];
                localPos = bone.m_position - parentBone.m_position;
            } else {
                // Root bone
                localPos = bone.m_position;
            }
            localPos.z *= -1;  // Coordinate system conversion
            m_nodes[i]->SetLocalPosition(localPos);

            // Set initial global transform directly from bone position (like saba)
            glm::mat4 initGlobal = glm::translate(
                glm::mat4(1.0f),
                glm::vec3(bone.m_position.x, bone.m_position.y, -bone.m_position.z)
            );
            m_nodes[i]->SetGlobalTransform(initGlobal);
            m_nodes[i]->SetInverseInitTransform(glm::inverse(initGlobal));
            m_nodes[i]->SaveInitialTRS();
            m_nodes[i]->UpdateLocalTransform();
        }

        // Create rigid bodies
        m_rigidBodies.clear();
        //std::cout << "[MMDModel] Creating " << pmxModel.m_rigidBodies.size() << " rigid bodies" << std::endl;
        for (const auto& pmxRigidBody : pmxModel.m_rigidBodies) {
            auto rigidBody = std::make_unique<MMDRigidBody>();
            MMDNode* node = nullptr;
            std::string boneName = "None";
            if (pmxRigidBody.m_boneIndex >= 0 && pmxRigidBody.m_boneIndex < static_cast<int>(m_nodes.size())) {
                node = m_nodes[pmxRigidBody.m_boneIndex].get();
                boneName = node->GetName();
            }
            /*
            std::cout << "[MMDModel] RigidBody: " << pmxRigidBody.m_name
                      << " -> Bone: " << boneName
                      << " (Index: " << pmxRigidBody.m_boneIndex << ")"
                      << " Type: " << (int)pmxRigidBody.m_op << std::endl;
                      */
            if (rigidBody->Create(pmxRigidBody, this, node)) m_physics->AddRigidBody(rigidBody.get());
            else rigidBody.reset();
            m_rigidBodies.push_back(std::move(rigidBody));
        }
        //std::cout << "[MMDModel] Successfully created " << m_rigidBodies.size() << " rigid bodies" << std::endl;

        // Create joints
        m_joints.clear();
        for (const auto& pmxJoint : pmxModel.m_joints) {
            auto joint = std::make_unique<MMDJoint>();
            MMDRigidBody* rigidBodyA = nullptr;
            MMDRigidBody* rigidBodyB = nullptr;
            if (pmxJoint.m_rigidbodyAIndex >= 0 && pmxJoint.m_rigidbodyAIndex < static_cast<int>(m_rigidBodies.size())) {
                rigidBodyA = m_rigidBodies[pmxJoint.m_rigidbodyAIndex].get();
            }
            if (pmxJoint.m_rigidbodyBIndex >= 0 && pmxJoint.m_rigidbodyBIndex < static_cast<int>(m_rigidBodies.size())) {
                rigidBodyB = m_rigidBodies[pmxJoint.m_rigidbodyBIndex].get();
            }
            if (joint->CreateJoint(pmxJoint, rigidBodyA, rigidBodyB)) {
                m_physics->AddJoint(joint.get());
                m_joints.push_back(std::move(joint));
            }
        }

        m_pmxMorphs=pmxModel.m_morphs;
        for(const auto& pmxMorph:m_pmxMorphs) {
            auto morph=std::make_unique<MMDMorph>();morph->SetName(pmxMorph.m_name);
            m_morphs.push_back(std::move(morph));
        }
        // Initialize morph offset buffers
        m_morphPositions.resize(m_positions.size(), glm::vec3(0.0f));
        m_morphNormals.resize(m_positions.size(), glm::vec3(0.0f));

        // Initialize updated vertices
        m_updatedPositions = m_positions;
        m_updatedNormals = m_normals;
        m_updatedUVs = m_uvs;

        // Reset physics to initialize rigid bodies to their correct positions
        InitializeAnimation();
        UpdateNodeAnimation(false);
        ResetPhysics();

        return true;
    }

    void MMDModelImpl::Destroy()
    {
        if (m_physics) for (auto& joint : m_joints) m_physics->RemoveJoint(joint.get());
        m_joints.clear(); m_rigidBodies.clear(); m_physics.reset();
        m_sortedNodes.clear(); m_ikSolvers.clear(); m_morphs.clear(); m_nodes.clear();
        m_positions.clear(); m_normals.clear(); m_uvs.clear();
        m_indices.clear(); m_materials.clear(); m_initMaterials.clear(); m_subMeshes.clear();
        m_morphData.clear(); m_pmxMorphs.clear(); m_updateRanges.clear();
    }

    MMDNode* MMDModelImpl::GetNode(size_t index)
    {
        if (index < m_nodes.size())
        {
            return m_nodes[index].get();
        }
        return nullptr;
    }

    size_t MMDModelImpl::FindNodeIndex(const std::string& name)
    {
        for (size_t i = 0; i < m_nodes.size(); i++)
        {
            if (m_nodes[i]->GetName() == name)
            {
                return i;
            }
        }
        return static_cast<size_t>(-1);
    }

    MMDIkSolver* MMDModelImpl::GetIKSolver(size_t index)
    {
        if (index < m_ikSolvers.size())
        {
            return m_ikSolvers[index].get();
        }
        return nullptr;
    }

    size_t MMDModelImpl::FindIKSolverIndex(const std::string& name)
    {
        for (size_t i = 0; i < m_ikSolvers.size(); i++)
        {
            if (m_ikSolvers[i]->GetName() == name)
            {
                return i;
            }
        }
        return static_cast<size_t>(-1);
    }

    MMDMorph* MMDModelImpl::GetMorph(size_t index)
    {
        if (index < m_morphs.size())
        {
            return m_morphs[index].get();
        }
        return nullptr;
    }

    size_t MMDModelImpl::FindMorphIndex(const std::string& name)
    {
        for (size_t i = 0; i < m_morphs.size(); i++)
        {
            if (m_morphs[i]->GetName() == name)
            {
                return i;
            }
        }
        // Exact Japanese name wins. Normalize only full-width digits for exported variants.
        auto normalized=[](std::string value) {
            const char* digits[]={"０","１","２","３","４","５","６","７","８","９"};
            for(size_t d=0;d<10;++d) {
                size_t pos=0;
                while((pos=value.find(digits[d],pos))!=std::string::npos) {
                    value.replace(pos,3,1,char('0'+d));++pos;
                }
            }
            return value;
        };
        const auto canonical=normalized(name);
        for(size_t i=0;i<m_morphs.size();++i) if(normalized(m_morphs[i]->GetName())==canonical) return i;
        return static_cast<size_t>(-1);
    }

    void MMDModelImpl::InitializeAnimation()
    {
        for(auto& node:m_nodes) node->ClearBaseAnimation();
        BeginAnimation();
        for (auto& node : m_nodes) node->UpdateLocalTransform();
        for (auto& solver : m_ikSolvers) solver->Enable(true);
        for (auto& node : m_nodes) if (!node->GetParent()) node->UpdateGlobalTransform();
        for (auto node : m_sortedNodes) {
            if (node->GetAppendNode()) { node->UpdateAppendTransform(); node->UpdateGlobalTransform(); }
            if (node->GetIKSolver()) { node->GetIKSolver()->Solve(); node->UpdateGlobalTransform(); }
        }
        for (auto& node : m_nodes) if (!node->GetParent()) node->UpdateGlobalTransform();
        EndAnimation();
        ResetPhysics();
    }

    void MMDModelImpl::BeginAnimation()
    {
        for (auto& node : m_nodes) { node->LoadInitialTRS(); node->SetAnimationTranslate(glm::vec3(0)); node->SetAnimationRotate(glm::quat(1,0,0,0)); node->SetIKRotate(glm::quat(1,0,0,0)); node->ResetAppendTransform(); }
        for (auto& morph : m_morphs) morph->SetWeight(0);
    }

    void MMDModelImpl::EndAnimation()
    {
    }

    void MMDModelImpl::UpdateMorphAnimation()
    {
        if(m_initMaterials.empty()) m_initMaterials=m_materials;
        m_materials=m_initMaterials;
        // Saba accumulates multiplication and addition independently, then combines them.
        std::vector<MMDMaterial> additions(m_materials.size());
        for(auto& a:additions) {
            a.m_diffuse=glm::vec4(0);a.m_specular=glm::vec3(0);a.m_specularPower=0;
            a.m_ambient=glm::vec3(0);a.m_edgeColor=glm::vec4(0);a.m_edgeSize=0;
            a.m_textureAdd=a.m_sphereAdd=a.m_toonAdd=glm::vec4(0);
        }
        std::fill(m_morphPositions.begin(),m_morphPositions.end(),glm::vec3(0));
        m_updatedUVs=m_uvs;
        std::vector<bool> visiting(m_morphs.size(),false);
        std::function<void(size_t,float)> applyGroup=[&](size_t i,float weight) {
            if(i>=m_morphs.size() || visiting[i] || !std::isfinite(weight) || weight==0) return;
            visiting[i]=true;
            if(m_pmxMorphs[i].m_morphType==PMXMorphType::Group || m_pmxMorphs[i].m_morphType==PMXMorphType::Flip)
                for(const auto& g:m_pmxMorphs[i].m_groups) if(g.m_index>=0) applyGroup(g.m_index,weight*g.m_weight);
            const float w=weight;
            const auto& morph=m_pmxMorphs[i];
            for(const auto& v:morph.m_materials) {
                auto applyMaterial=[&](size_t index) {
                    auto& m=m_materials[index]; auto& a=additions[index];
                    if(v.m_opType==0) {
                        m.m_diffuse=glm::mix(m.m_diffuse,m.m_diffuse*v.m_diffuse,w);
                        m.m_specular=glm::mix(m.m_specular,m.m_specular*v.m_specular,w);
                        m.m_specularPower=glm::mix(m.m_specularPower,m.m_specularPower*v.m_specularPower,w);
                        m.m_ambient=glm::mix(m.m_ambient,m.m_ambient*v.m_ambient,w);
                        m.m_edgeColor=glm::mix(m.m_edgeColor,m.m_edgeColor*v.m_edgeColor,w);
                        m.m_edgeSize=glm::mix(m.m_edgeSize,m.m_edgeSize*v.m_edgeSize,w);
                        m.m_textureMul=glm::mix(m.m_textureMul,m.m_textureMul*v.m_textureColor,w);
                        m.m_sphereMul=glm::mix(m.m_sphereMul,m.m_sphereMul*v.m_sphereColor,w);
                        m.m_toonMul=glm::mix(m.m_toonMul,m.m_toonMul*v.m_toonColor,w);
                    } else if(v.m_opType==1) {
                        a.m_diffuse+=v.m_diffuse*w;a.m_specular+=v.m_specular*w;
                        a.m_specularPower+=v.m_specularPower*w;a.m_ambient+=v.m_ambient*w;
                        a.m_edgeColor+=v.m_edgeColor*w;a.m_edgeSize+=v.m_edgeSize*w;
                        a.m_textureAdd+=v.m_textureColor*w;a.m_sphereAdd+=v.m_sphereColor*w;
                        a.m_toonAdd+=v.m_toonColor*w;
                    }
                };
                if(v.m_index==-1) for(size_t mi=0;mi<m_materials.size();++mi) applyMaterial(mi);
                else if(v.m_index>=0 && size_t(v.m_index)<m_materials.size()) applyMaterial(size_t(v.m_index));
            }
            for(const auto& v:morph.m_vertices) if(v.m_index>=0 && static_cast<size_t>(v.m_index)<m_morphPositions.size())
                m_morphPositions[v.m_index]+=glm::vec3(v.m_position.x,v.m_position.y,-v.m_position.z)*w;
            for(const auto& v:morph.m_uvs) if(v.m_index>=0 && static_cast<size_t>(v.m_index)<m_updatedUVs.size())
                m_updatedUVs[v.m_index]+=glm::vec2(v.m_uv)*w;
            for(const auto& v:morph.m_bones) if(v.m_index>=0 && static_cast<size_t>(v.m_index)<m_nodes.size()) {
                auto& node=m_nodes[v.m_index];
                node->SetLocalPosition(node->GetLocalPosition()+glm::vec3(v.m_position.x,v.m_position.y,-v.m_position.z)*w);
                const glm::quat q(v.m_rotation.w,-v.m_rotation.x,-v.m_rotation.y,v.m_rotation.z);
                node->SetLocalRotation(glm::slerp(node->GetLocalRotation(),q,w));
            }
            visiting[i]=false;
        };
        for(size_t i=0;i<m_morphs.size();++i) applyGroup(i,m_morphs[i]->GetWeight());
        for(size_t i=0;i<m_materials.size();++i) {
            auto& m=m_materials[i];const auto& a=additions[i];
            m.m_diffuse+=a.m_diffuse;m.m_specular+=a.m_specular;m.m_specularPower+=a.m_specularPower;
            m.m_ambient+=a.m_ambient;m.m_edgeColor+=a.m_edgeColor;m.m_edgeSize+=a.m_edgeSize;
            m.m_textureAdd=a.m_textureAdd;m.m_sphereAdd=a.m_sphereAdd;m.m_toonAdd=a.m_toonAdd;
        }
    }
    void MMDModelImpl::UpdateNodeAnimation(bool afterPhysicsAnim)
    {
        m_afterPhysicsAnim=afterPhysicsAnim;
        for(auto node:m_sortedNodes)
            if(node->IsDeformAfterPhysics()==afterPhysicsAnim) node->UpdateLocalTransform();
        for(auto node:m_sortedNodes)
            if(node->IsDeformAfterPhysics()==afterPhysicsAnim && !node->GetParent()) node->UpdateGlobalTransform();
        for(auto node:m_sortedNodes) {
            if(node->IsDeformAfterPhysics()!=afterPhysicsAnim) continue;
            if(node->GetAppendNode()) { node->UpdateAppendTransform(); node->UpdateGlobalTransform(); }
            if(node->GetIKSolver()) { node->GetIKSolver()->Solve(); node->UpdateGlobalTransform(); }
        }
        for(auto node:m_sortedNodes)
            if(node->IsDeformAfterPhysics()==afterPhysicsAnim && !node->GetParent()) node->UpdateGlobalTransform();
    }
    void MMDModelImpl::ResetPhysics()
    {
        if(m_physics) m_physics->ResetAccumulator();
        for (auto& rigidBody : m_rigidBodies) {
            if (rigidBody) rigidBody->Reset();
        }
    }

    void MMDModelImpl::UpdatePhysicsAnimation(float elapsed)
    {
        if (m_physics) {
            m_physics->Update(elapsed);
        }

        // Reflect rigid body transforms back to nodes
        for (auto& rigidBody : m_rigidBodies) {
            if (rigidBody) rigidBody->ReflectGlobalTransform();
        }
        // Saba reflects all bodies before deriving local matrices, then refreshes roots.
        for(auto& body:m_rigidBodies) if(body && body->GetNode()) body->GetNode()->UpdateLocalFromGlobal();
        for(auto& node:m_nodes) if(!node->GetParent()) node->UpdateGlobalTransform();
    }

    void MMDModelImpl::Update()
    {
        UpdateSkinningParallel();
    }

    void MMDModelImpl::SetParallelUpdateHint(uint32_t parallelCount)
    {
        m_parallelUpdateCount = parallelCount;
        // Re-setup parallel ranges on next update
        m_updateRanges.clear();
    }

    void MMDModelImpl::SetupParallelUpdate()
    {
        // Auto-detect thread count if not set
        if (m_parallelUpdateCount == 0) {
            m_parallelUpdateCount = std::thread::hardware_concurrency();
            if (m_parallelUpdateCount == 0) {
                m_parallelUpdateCount = 4; // Default fallback
            }
        }

        // Limit max parallel count
        const uint32_t maxParallelCount = std::max(uint32_t(16), uint32_t(std::thread::hardware_concurrency()));
        if (m_parallelUpdateCount > maxParallelCount) {
            m_parallelUpdateCount = maxParallelCount;
        }

        // std::cout << "[MMDModel] Parallel update count: " << m_parallelUpdateCount << std::endl;

        // Setup update ranges
        m_updateRanges.resize(m_parallelUpdateCount);
        m_parallelUpdateFutures.resize(m_parallelUpdateCount - 1);

        const size_t vertexCount = m_positions.size();
        const size_t minVerticesPerRange = 1000;

        if (vertexCount < m_updateRanges.size() * minVerticesPerRange) {
            // Not enough vertices for all threads, some will be empty
            size_t numRanges = (vertexCount + minVerticesPerRange - 1) / minVerticesPerRange;
            for (size_t rangeIdx = 0; rangeIdx < m_updateRanges.size(); rangeIdx++) {
                if (rangeIdx < numRanges) {
                    m_updateRanges[rangeIdx].first = rangeIdx * minVerticesPerRange;
                    m_updateRanges[rangeIdx].second = std::min(minVerticesPerRange, vertexCount - m_updateRanges[rangeIdx].first);
                } else {
                    m_updateRanges[rangeIdx].first = 0;
                    m_updateRanges[rangeIdx].second = 0;
                }
            }
        } else {
            // Distribute vertices evenly
            size_t baseVertexCount = vertexCount / m_updateRanges.size();
            size_t offset = 0;
            for (size_t rangeIdx = 0; rangeIdx < m_updateRanges.size(); rangeIdx++) {
                m_updateRanges[rangeIdx].first = offset;
                m_updateRanges[rangeIdx].second = baseVertexCount;
                if (rangeIdx == 0) {
                    // First range gets remainder
                    m_updateRanges[rangeIdx].second += vertexCount % m_updateRanges.size();
                }
                offset = m_updateRanges[rangeIdx].first + m_updateRanges[rangeIdx].second;
            }
        }
    }

    void MMDModelImpl::UpdateSkinningRange(size_t startVertex, size_t vertexCount)
    {
        // Process vertices in range
        for (size_t i = startVertex; i < startVertex + vertexCount && i < m_positions.size(); i++) {
            const auto& weightType = m_weightTypes[i];
            const glm::vec3 position = m_positions[i] + m_morphPositions[i];
            const glm::vec3& normal = m_normals[i];

            glm::vec3 skinnedPosition(0.0f);
            glm::vec3 skinnedNormal(0.0f);

            switch (weightType) {
            case mmd::PMXVertexWeightType::BDEF1: {
                int boneIndex = m_boneIndices[i * 4];
                if (boneIndex >= 0 && boneIndex < static_cast<int>(m_skinningMatrices.size())) {
                    skinnedPosition = glm::vec3(m_skinningMatrices[boneIndex] * glm::vec4(position, 1.0f));
                    skinnedNormal = glm::vec3(m_skinningMatrices[boneIndex] * glm::vec4(normal, 0.0f));
                } else {
                    skinnedPosition = position;
                    skinnedNormal = normal;
                }
                break;
            }
            case mmd::PMXVertexWeightType::BDEF2: {
                int boneIndex0 = m_boneIndices[i * 4];
                int boneIndex1 = m_boneIndices[i * 4 + 1];
                float weight0 = m_boneWeights[i * 4];
                float weight1 = m_boneWeights[i * 4 + 1];

                if (boneIndex0 >= 0 && boneIndex0 < static_cast<int>(m_skinningMatrices.size())) {
                    skinnedPosition += weight0 * glm::vec3(m_skinningMatrices[boneIndex0] * glm::vec4(position, 1.0f));
                    skinnedNormal += weight0 * glm::vec3(m_skinningMatrices[boneIndex0] * glm::vec4(normal, 0.0f));
                } else {
                    skinnedPosition += weight0 * position;
                    skinnedNormal += weight0 * normal;
                }

                if (boneIndex1 >= 0 && boneIndex1 < static_cast<int>(m_skinningMatrices.size())) {
                    skinnedPosition += weight1 * glm::vec3(m_skinningMatrices[boneIndex1] * glm::vec4(position, 1.0f));
                    skinnedNormal += weight1 * glm::vec3(m_skinningMatrices[boneIndex1] * glm::vec4(normal, 0.0f));
                } else {
                    skinnedPosition += weight1 * position;
                    skinnedNormal += weight1 * normal;
                }
                break;
            }
            case mmd::PMXVertexWeightType::BDEF4: {
                for (int j = 0; j < 4; j++) {
                    int boneIndex = m_boneIndices[i * 4 + j];
                    float weight = m_boneWeights[i * 4 + j];

                    if (boneIndex >= 0 && boneIndex < static_cast<int>(m_skinningMatrices.size()) && weight > 0.0f) {
                        skinnedPosition += weight * glm::vec3(m_skinningMatrices[boneIndex] * glm::vec4(position, 1.0f));
                        skinnedNormal += weight * glm::vec3(m_skinningMatrices[boneIndex] * glm::vec4(normal, 0.0f));
                    } else if (weight > 0.0f) {
                        skinnedPosition += weight * position;
                        skinnedNormal += weight * normal;
                    }
                }
                break;
            }
            case mmd::PMXVertexWeightType::QDEF: {
                glm::quat real(0,0,0,0), dual(0,0,0,0), reference(1,0,0,0);
                bool hasReference=false;
                for(int j=0;j<4;++j) {
                    const int bone=m_boneIndices[i*4+j];
                    const float weight=m_boneWeights[i*4+j];
                    if(weight<=0) continue;
                    const auto matrix=bone>=0 && bone<static_cast<int>(m_skinningMatrices.size())
                        ? m_skinningMatrices[bone] : glm::mat4(1);
                    auto q=glm::normalize(glm::quat_cast(matrix));
                    if(!hasReference) { reference=q;hasReference=true; }
                    if(glm::dot(reference,q)<0) q=-q;
                    const glm::vec3 t(matrix[3]);
                    real+=q*weight;
                    dual+=(glm::quat(0,t.x,t.y,t.z)*q)*(0.5f*weight);
                }
                const float norm=glm::length(real);
                if(norm>1e-8f) {
                    real/=norm;dual/=norm;
                    dual-=real*glm::dot(real,dual);
                    const auto translation=(dual*glm::conjugate(real))*2.0f;
                    skinnedPosition=real*position+glm::vec3(translation.x,translation.y,translation.z);
                    skinnedNormal=real*normal;
                } else { skinnedPosition=position;skinnedNormal=normal; }
                break;
            }
            case mmd::PMXVertexWeightType::SDEF: {
                int boneIndex0 = m_boneIndices[i * 4];
                int boneIndex1 = m_boneIndices[i * 4 + 1];
                float weight0 = m_boneWeights[i * 4];
                float weight1 = m_boneWeights[i * 4 + 1];

                const glm::vec3& c = m_sdefC[i];
                const glm::vec3& r0 = m_sdefR0[i];
                const glm::vec3& r1 = m_sdefR1[i];

                // Optimized SDEF using quaternion slerp (like saba)
                // https://github.com/powroupi/blender_mmd_tools/blob/dev_test/mmd_tools/core/sdef.py
                if (boneIndex0 >= 0 && boneIndex0 < static_cast<int>(m_nodes.size()) &&
                    boneIndex1 >= 0 && boneIndex1 < static_cast<int>(m_nodes.size())) {

                    const auto& m0 = m_skinningMatrices[boneIndex0];
                    const auto& m1 = m_skinningMatrices[boneIndex1];

                    // Extract rotation from skinning matrices using quaternion
                    const auto q0 = glm::quat_cast(m0);
                    const auto q1 = glm::quat_cast(m1);

                    // Spherical linear interpolation between bone rotations
                    const auto rotQuat = glm::slerp(q0, q1, weight1);
                    const auto rotMat = glm::mat3_cast(rotQuat);

                    // Transform position using rotation and bone positions
                    const glm::vec3 pos = position;
                    skinnedPosition = rotMat * (pos - c) +
                                      glm::vec3(m0 * glm::vec4(c + r0 - (r0*weight0+r1*weight1), 1.0f)) * weight0 +
                                      glm::vec3(m1 * glm::vec4(c + r1 - (r0*weight0+r1*weight1), 1.0f)) * weight1;
                    skinnedNormal = rotMat * normal;
                } else if (boneIndex0 >= 0 && boneIndex0 < static_cast<int>(m_skinningMatrices.size())) {
                    skinnedPosition = glm::vec3(m_skinningMatrices[boneIndex0] * glm::vec4(position, 1.0f));
                    skinnedNormal = glm::vec3(m_skinningMatrices[boneIndex0] * glm::vec4(normal, 0.0f));
                } else if (boneIndex1 >= 0 && boneIndex1 < static_cast<int>(m_skinningMatrices.size())) {
                    skinnedPosition = glm::vec3(m_skinningMatrices[boneIndex1] * glm::vec4(position, 1.0f));
                    skinnedNormal = glm::vec3(m_skinningMatrices[boneIndex1] * glm::vec4(normal, 0.0f));
                } else {
                    skinnedPosition = position;
                    skinnedNormal = normal;
                }
                break;
            }
            default:
                skinnedPosition = position;
                skinnedNormal = normal;
                break;
            }

            // Apply morph offsets after skinning
            // Vertex morphs are applied in bind space before skinning.
            skinnedNormal += m_morphNormals[i];

            m_updatedPositions[i] = skinnedPosition;
            m_updatedNormals[i] = glm::dot(skinnedNormal,skinnedNormal)>1e-12f ? glm::normalize(skinnedNormal) : glm::vec3(0,1,0);
        }
    }

    void MMDModelImpl::UpdateSkinningParallel()
    {
        // Calculate skinning matrices once
        m_skinningMatrices.resize(m_nodes.size());
        for (size_t i = 0; i < m_nodes.size(); i++) {
            m_skinningMatrices[i] = m_nodes[i]->GetGlobalTransform() * m_nodes[i]->GetInverseInitTransform();
        }

        // Setup parallel ranges if needed
        if (m_updateRanges.empty() || m_updateRanges.size() != m_parallelUpdateCount) {
            SetupParallelUpdate();
        }

        // Launch parallel tasks (all except first range)
        for (size_t i = 0; i < m_parallelUpdateFutures.size(); i++) {
            size_t rangeIndex = i + 1;
            if (m_updateRanges[rangeIndex].second > 0) {
                m_parallelUpdateFutures[i] = std::async(
                    std::launch::async,
                    [this, rangeIndex]() {
                        this->UpdateSkinningRange(m_updateRanges[rangeIndex].first, m_updateRanges[rangeIndex].second);
                    }
                );
            }
        }

        // Process first range on main thread
        if (m_updateRanges[0].second > 0) {
            UpdateSkinningRange(m_updateRanges[0].first, m_updateRanges[0].second);
        }

        // Wait for all parallel tasks to complete
        for (size_t i = 0; i < m_parallelUpdateFutures.size(); i++) {
            size_t rangeIndex = i + 1;
            if (m_updateRanges[rangeIndex].second > 0) {
                m_parallelUpdateFutures[i].wait();
            }
        }
    }

    void MMDModelImpl::UpdateSkinning()
    {
        // Calculate skinning matrices: globalTransform * inverseBindMatrix
        std::vector<glm::mat4> skinningMatrices(m_nodes.size());
        for (size_t i = 0; i < m_nodes.size(); i++) {
            skinningMatrices[i] = m_nodes[i]->GetGlobalTransform() * m_nodes[i]->GetInverseInitTransform();
        }

        // Process each vertex
        for (size_t i = 0; i < m_positions.size(); i++) {
            const auto& weightType = m_weightTypes[i];
            const glm::vec3 position = m_positions[i] + m_morphPositions[i];
            const glm::vec3& normal = m_normals[i];

            glm::vec3 skinnedPosition(0.0f);
            glm::vec3 skinnedNormal(0.0f);

            switch (weightType) {
            case mmd::PMXVertexWeightType::BDEF1: {
                // Single bone weight
                int boneIndex = m_boneIndices[i * 4];
                if (boneIndex >= 0 && boneIndex < static_cast<int>(skinningMatrices.size())) {
                    skinnedPosition = glm::vec3(skinningMatrices[boneIndex] * glm::vec4(position, 1.0f));
                    skinnedNormal = glm::vec3(skinningMatrices[boneIndex] * glm::vec4(normal, 0.0f));
                } else {
                    skinnedPosition = position;
                    skinnedNormal = normal;
                }
                break;
            }
            case mmd::PMXVertexWeightType::BDEF2: {
                // Dual bone weight
                int boneIndex0 = m_boneIndices[i * 4];
                int boneIndex1 = m_boneIndices[i * 4 + 1];
                float weight0 = m_boneWeights[i * 4];
                float weight1 = m_boneWeights[i * 4 + 1];

                if (boneIndex0 >= 0 && boneIndex0 < static_cast<int>(skinningMatrices.size())) {
                    skinnedPosition += weight0 * glm::vec3(skinningMatrices[boneIndex0] * glm::vec4(position, 1.0f));
                    skinnedNormal += weight0 * glm::vec3(skinningMatrices[boneIndex0] * glm::vec4(normal, 0.0f));
                } else {
                    skinnedPosition += weight0 * position;
                    skinnedNormal += weight0 * normal;
                }

                if (boneIndex1 >= 0 && boneIndex1 < static_cast<int>(skinningMatrices.size())) {
                    skinnedPosition += weight1 * glm::vec3(skinningMatrices[boneIndex1] * glm::vec4(position, 1.0f));
                    skinnedNormal += weight1 * glm::vec3(skinningMatrices[boneIndex1] * glm::vec4(normal, 0.0f));
                } else {
                    skinnedPosition += weight1 * position;
                    skinnedNormal += weight1 * normal;
                }
                break;
            }
            case mmd::PMXVertexWeightType::BDEF4: {
                // Quad bone weight
                for (int j = 0; j < 4; j++) {
                    int boneIndex = m_boneIndices[i * 4 + j];
                    float weight = m_boneWeights[i * 4 + j];

                    if (boneIndex >= 0 && boneIndex < static_cast<int>(skinningMatrices.size()) && weight > 0.0f) {
                        skinnedPosition += weight * glm::vec3(skinningMatrices[boneIndex] * glm::vec4(position, 1.0f));
                        skinnedNormal += weight * glm::vec3(skinningMatrices[boneIndex] * glm::vec4(normal, 0.0f));
                    } else if (weight > 0.0f) {
                        skinnedPosition += weight * position;
                        skinnedNormal += weight * normal;
                    }
                }
                break;
            }
            case mmd::PMXVertexWeightType::SDEF: {
                // SDEF skinning
                int boneIndex0 = m_boneIndices[i * 4];
                int boneIndex1 = m_boneIndices[i * 4 + 1];
                float weight0 = m_boneWeights[i * 4];
                float weight1 = m_boneWeights[i * 4 + 1];

                const glm::vec3& c = m_sdefC[i];
                const glm::vec3& r0 = m_sdefR0[i];
                const glm::vec3& r1 = m_sdefR1[i];

                glm::vec3 v0(0.0f), v1(0.0f);
                if (boneIndex0 >= 0 && boneIndex0 < static_cast<int>(skinningMatrices.size())) {
                    v0 = glm::vec3(skinningMatrices[boneIndex0] * glm::vec4(c + r0, 1.0f)) -
                         glm::vec3(skinningMatrices[boneIndex0] * glm::vec4(c, 1.0f));
                }
                if (boneIndex1 >= 0 && boneIndex1 < static_cast<int>(skinningMatrices.size())) {
                    v1 = glm::vec3(skinningMatrices[boneIndex1] * glm::vec4(c + r1, 1.0f)) -
                         glm::vec3(skinningMatrices[boneIndex1] * glm::vec4(c, 1.0f));
                }

                float s0 = 1.0f;
                float s1 = 1.0f;
                if (glm::length(r0) > 0.0f) {
                    s0 = glm::dot(v0, r0) / glm::dot(r0, r0);
                }
                if (glm::length(r1) > 0.0f) {
                    s1 = glm::dot(v1, r1) / glm::dot(r1, r1);
                }

                glm::vec3 sp = position - c;
                glm::vec3 sb = c + s0 * r0 + s1 * r1;

                if (boneIndex0 >= 0 && boneIndex0 < static_cast<int>(skinningMatrices.size())) {
                    skinnedPosition += weight0 * (glm::vec3(skinningMatrices[boneIndex0] * glm::vec4(sb + sp, 1.0f)));
                    skinnedNormal += weight0 * glm::vec3(skinningMatrices[boneIndex0] * glm::vec4(normal, 0.0f));
                } else {
                    skinnedPosition += weight0 * (sb + sp);
                    skinnedNormal += weight0 * normal;
                }

                if (boneIndex1 >= 0 && boneIndex1 < static_cast<int>(skinningMatrices.size())) {
                    skinnedPosition += weight1 * (glm::vec3(skinningMatrices[boneIndex1] * glm::vec4(sb + sp, 1.0f)));
                    skinnedNormal += weight1 * glm::vec3(skinningMatrices[boneIndex1] * glm::vec4(normal, 0.0f));
                } else {
                    skinnedPosition += weight1 * (sb + sp);
                    skinnedNormal += weight1 * normal;
                }
                break;
            }
            default:
                skinnedPosition = position;
                skinnedNormal = normal;
                break;
            }

            // Store updated position and normal
            m_updatedPositions[i] = skinnedPosition;
            m_updatedNormals[i] = glm::dot(skinnedNormal,skinnedNormal)>1e-12f ? glm::normalize(skinnedNormal) : glm::vec3(0,1,0);
        }
    }

    void MMDModelImpl::SetVelocity(const glm::vec3& velocity)
    {
        if (m_physics) {
            m_physics->SetVelocity(velocity);
        }
    }

    void MMDModelImpl::SetAngularVelocity(const glm::vec3& angularVelocity)
    {
        if (m_physics) {
            m_physics->SetAngularVelocity(angularVelocity);
        }
    }

    void MMDModelImpl::ApplyForce(const glm::vec3& force)
    {
        if (m_physics) {
            m_physics->ApplyForce(force);
        }
    }

    void MMDModelImpl::ApplyTorque(const glm::vec3& torque)
    {
        if (m_physics) {
            m_physics->ApplyTorque(torque);
        }
    }

}
