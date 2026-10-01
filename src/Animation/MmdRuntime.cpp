#include "Animation/MmdRuntime.h"
#include "Animation/MMD/Model/MMDModel.h"
#include "Animation/MMD/Model/VMDAnimation.h"
#include "Animation/MMD/Physics/MMDPhysics.h"
#include "Core/Utf8Path.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include "json.hpp"

namespace Animation {
namespace {
bool FlipPmxTextureV(const std::string& path) {
    std::ifstream input(Utf8Path(path+".import.json"));
    if(!input) return false;
    try {
        const auto settings=nlohmann::json::parse(input);
        return settings.value("textureVOrigin",std::string("top"))=="bottom";
    } catch(const std::exception& e) {
        std::cerr<<"[PMX import] invalid settings: "<<path<<": "<<e.what()<<'\n';
        return false;
    }
}
glm::vec2 ImportedPmxUV(glm::vec2 uv,bool flipV) { if(flipV) uv.y=1-uv.y;return uv; }
}
struct MmdRuntime::Impl {
    mmd::MMDModelImpl model;
    mmd::VMDAnimation animation;
    bool hasMotion=false, physicsWasEnabled=false;
    bool flipTextureV=false;
    float lastFrame=-1;
};
MmdRuntime::MmdRuntime() = default;
MmdRuntime::~MmdRuntime() = default;
bool MmdRuntime::Load(const std::string& modelPath,const std::string& motionPath) {
    return Load(modelPath,motionPath,{});
}
bool MmdRuntime::Load(const std::string& modelPath,const std::string& motionPath,const std::string& faceMotionPath) {
    m_impl.reset();
    auto state=std::make_unique<Impl>();
    if(!state->model.Create(modelPath)) return false;
    state->flipTextureV=FlipPmxTextureV(modelPath);
    state->model.SetParallelUpdateHint(1);
    state->model.InitializeAnimation();
    if(!motionPath.empty() || !faceMotionPath.empty()) {
        if(!state->animation.Create(&state->model)) return false;
    }
    if(!motionPath.empty()) {
        mmd::VMDFile file;
        if(!file.Load(motionPath) || !state->animation.Add(file)) return false;
        state->hasMotion=true;
    }
    if(!faceMotionPath.empty()) {
        mmd::VMDFile file;
        if(!file.Load(faceMotionPath) || !state->animation.Add(file)) return false;
        state->hasMotion=true;
    }
    m_impl=std::move(state);
    Update(0,0,false);
    return true;
}
void MmdRuntime::Update(float frame,float dt,bool physics) {
    if(!m_impl || !std::isfinite(frame)) return;
    auto& s=*m_impl;
    const bool discontinuity=s.lastFrame>=0 && (frame<s.lastFrame || std::fabs(frame-s.lastFrame)>15);
    if(physics && (!s.physicsWasEnabled || discontinuity)) {
        // Saba SyncPhysics: blend into the requested pose, keeping the visible VMD clock fixed.
        for(size_t i=0;i<s.model.GetNodeCount();++i) s.model.GetNode(i)->SaveBaseAnimation();
        s.model.ResetPhysics();
        for(int i=0;i<30;++i) {
            s.model.BeginAnimation();
            if(s.hasMotion) s.animation.Evaluate(frame,float(i+1)/30);
            s.model.UpdateMorphAnimation();s.model.UpdateNodeAnimation(false);
            s.model.UpdatePhysicsAnimation(1.0f/30);
            s.model.UpdateNodeAnimation(true);s.model.EndAnimation();
        }
        for(size_t i=0;i<s.model.GetNodeCount();++i) s.model.GetNode(i)->ClearBaseAnimation();
    }
    s.model.BeginAnimation();
    if(s.hasMotion) s.animation.Evaluate(frame);
    s.model.UpdateMorphAnimation();
    s.model.UpdateNodeAnimation(false);
    if(physics) {
        s.model.UpdatePhysicsAnimation(dt);
    } else s.model.ResetPhysics();
    s.model.UpdateNodeAnimation(true);
    s.model.EndAnimation();
    s.model.Update();
    s.physicsWasEnabled=physics;
    s.lastFrame=frame;
}
void MmdRuntime::ResetPhysics() { if(m_impl) m_impl->model.ResetPhysics(); }
std::vector<mmd::MMDMaterial> MmdRuntime::GetMaterials() const {
    if(!m_impl) return {};
    const auto& model=m_impl->model;
    return {model.GetMaterials(),model.GetMaterials()+model.GetMaterialCount()};
}
size_t MmdRuntime::GetBoneCount() const { return m_impl?m_impl->model.GetNodeCount():0; }
size_t MmdRuntime::GetMorphCount() const { return m_impl?m_impl->model.GetMorphCount():0; }
float MmdRuntime::GetLastFrame() const { return m_impl?static_cast<float>(m_impl->animation.GetMaxKeyTime()):0; }
void MmdRuntime::GetPhysicsBodies(std::vector<glm::mat4>& out) const {
    out.clear();if(m_impl) m_impl->model.GetMMDPhysics()->GetBodyTransforms(out);
}
void MmdRuntime::GetVertices(std::vector<glm::vec3>& p,std::vector<glm::vec3>& n,std::vector<glm::vec2>& uv) const {
    if(!m_impl) { p.clear();n.clear();uv.clear();return; }
    const auto count=m_impl->model.GetVertexCount();
    p.assign(m_impl->model.GetUpdatedPositions(),m_impl->model.GetUpdatedPositions()+count);
    n.assign(m_impl->model.GetUpdatedNormals(),m_impl->model.GetUpdatedNormals()+count);
    uv.assign(m_impl->model.GetUpdatedUVs(),m_impl->model.GetUpdatedUVs()+count);
    if(m_impl->flipTextureV) for(auto& v:uv) v.y=1-v.y;
}
bool MmdRuntime::ApplyVertices(ModelLoadResult& result) const {
    if(!m_impl) return false;
    const auto& model=m_impl->model;
    for(auto& sm:result.meshData.subMeshes) {
        if(sm.mmdVertexIndices.size()!=sm.vertices.size()) return false;
        for(size_t i=0;i<sm.vertices.size();++i) {
            const auto index=sm.mmdVertexIndices[i];
            if(index>=model.GetVertexCount()) return false;
            auto& v=sm.vertices[i];
            v.Position=model.GetUpdatedPositions()[index];
            v.Normal=PackSnorm3(model.GetUpdatedNormals()[index]);
            v.TexCoords=PackHalf2(ImportedPmxUV(model.GetUpdatedUVs()[index],m_impl->flipTextureV));
            v.BoneIDs=glm::u8vec4(255);v.BoneWeights=glm::u8vec4(0);
        }
    }
    return true;
}
bool LoadPmxMesh(const std::string& path,ModelLoadResult& result) {
    mmd::PMXFile file;
    if(!file.Load(path)) return false;
    const auto& pmx=file.GetPMXModel();
    const bool flipTextureV=FlipPmxTextureV(path);
    if(pmx.m_vertices.empty() || pmx.m_faces.empty()) return false;
    result={};
    auto& mesh=result.meshData;
    mesh.isMmd=true;
    for(size_t i=0;i<pmx.m_bones.size();++i) {
        const auto& b=pmx.m_bones[i];
        Bone bone{};
        bone.name=b.m_name;bone.parentIndex=b.m_parentBone;
        bone.initialPosition=glm::vec3(b.m_position.x,b.m_position.y,-b.m_position.z);
        bone.initialRotation=bone.rotation=glm::quat(1,0,0,0);
        bone.position=bone.initialPosition;
        bone.bindMatrix=bone.globalTransform=glm::translate(glm::mat4(1),bone.position);
        bone.offsetMatrix=glm::inverse(bone.bindMatrix);bone.ancestorTransform=glm::mat4(1);
        auto local=bone.position;
        if(b.m_parentBone>=0 && b.m_parentBone<static_cast<int>(pmx.m_bones.size())) {
            auto p=pmx.m_bones[b.m_parentBone].m_position;local-=glm::vec3(p.x,p.y,-p.z);
        }
        bone.bindLocalTransform=bone.localTransform=glm::translate(glm::mat4(1),local);
        mesh.bones.push_back(bone);
    }
    size_t first=0;
    for(size_t mi=0;mi<pmx.m_materials.size();++mi) {
        const auto& mat=pmx.m_materials[mi];
        if(mat.m_numFaceVertices<0 || mat.m_numFaceVertices%3!=0) return false;
        const size_t count=static_cast<size_t>(mat.m_numFaceVertices);
        if(first+count>pmx.m_faces.size()*3) return false;
        MaterialTextureInfo mt{};
        mt.materialName=mat.m_name;mt.diffuse=mat.m_diffuse;
        mt.specular=mat.m_specular;mt.specularPower=mat.m_specularPower;mt.ambient=mat.m_ambient;
        mt.metallic=0;mt.roughness=std::clamp(std::sqrt(2.0f/(std::max(0.0f,mat.m_specularPower)+2)),0.04f,1.0f);
        mt.doubleSided=(static_cast<uint8_t>(mat.m_drawMode)&1)!=0; mt.alphaMode=mat.m_diffuse.a<1?2:0;
        if(mat.m_textureIndex>=0 && mat.m_textureIndex<static_cast<int>(pmx.m_textures.size())) {
            auto name=pmx.m_textures[mat.m_textureIndex].m_textureName;
            std::replace(name.begin(),name.end(),'\\','/');
            const auto texturePath=Utf8Path(path).parent_path()/Utf8Path(name);
            std::error_code error;
            if(!name.empty() && std::filesystem::is_regular_file(texturePath,error)) {
                mt.diffuseTexturePath=Utf8String(texturePath);mt.hasTexture=true;
            }
        }
        mesh.materialTextures.push_back(mt);
        if(count==0) continue;
        SubMesh sm{};
        sm.name=sm.materialName=mat.m_name;sm.materialIndex=static_cast<int>(mi);
        sm.metallic=mt.metallic;sm.roughness=mt.roughness;sm.doubleSided=mt.doubleSided;sm.alphaMode=mt.alphaMode;
        std::unordered_map<uint32_t,uint32_t> remap;
        for(size_t j=0;j<count;j+=3) for(int corner=0;corner<3;++corner) {
            // Reflect Z and winding together to preserve front faces.
            const auto index=pmx.m_faces[(first+j)/3].m_vertices[corner==0?0:3-corner];
            if(index<0 || static_cast<size_t>(index)>=pmx.m_vertices.size()) return false;
            auto [it,added]=remap.emplace(index,static_cast<uint32_t>(sm.vertices.size()));
            if(added) {
                const auto& p=pmx.m_vertices[index];Vertex v{};
                v.Position=glm::vec3(p.m_position.x,p.m_position.y,-p.m_position.z);
                v.Normal=PackSnorm3(glm::vec3(p.m_normal.x,p.m_normal.y,-p.m_normal.z));v.TexCoords=PackHalf2(ImportedPmxUV(p.m_uv,flipTextureV));
                v.BoneIDs=glm::u8vec4(255);v.BoneWeights=glm::u8vec4(0);
                v.Tangent=PackSnorm3(glm::vec3(1,0,0),1);
                sm.vertices.push_back(v);sm.mmdVertexIndices.push_back(index);
            }
            sm.indices.push_back(it->second);
        }
        mesh.subMeshes.push_back(std::move(sm));first+=count;
    }
    if(first!=pmx.m_faces.size()*3) return false;
    result.materialTextures=mesh.materialTextures;
    return !mesh.subMeshes.empty();
}
}
