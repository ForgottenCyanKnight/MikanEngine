#include "Animation/MmdRuntime.h"
#include "Animation/MmdPlaybackClock.h"
#include "Animation/VmdMotion.h"
#include "Animation/MMD/Model/VMDAnimation.h"
#include "Animation/MMD/Model/MMDModel.h"
#include "Animation/MMD/Model/MMDIkSolver.h"
#include "Animation/MMD/Physics/MMDPhysics.h"
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <Jolt/Physics/Collision/ContactListener.h>
#ifdef _WIN32
#include <windows.h>
#endif

static void Check(bool condition,const char* label) {
    if(!condition) throw std::runtime_error(label);
}
static float Difference(const std::vector<glm::vec3>& a,const std::vector<glm::vec3>& b) {
    Check(a.size()==b.size(),"vertex count changed");
    float difference=0;
    for(size_t i=0;i<a.size();++i) {
        Check(std::isfinite(a[i].x)&&std::isfinite(a[i].y)&&std::isfinite(a[i].z),"nonfinite vertex");
        difference=std::max(difference,glm::length(a[i]-b[i]));
    }
    return difference;
}
struct FixtureModel : mmd::MMDModelImpl {
    mmd::MMDPhysics physics;
    mmd::MMDPhysics* GetMMDPhysics() override { return &physics; }
};
static void PhysicsContract() {
    FixtureModel model;Check(model.physics.Create(),"Jolt world create");
    mmd::MMDNode node;node.SetGlobalTransform(glm::translate(glm::mat4(1),glm::vec3(0,5,0)));
    mmd::PMXRigidBody p{};p.m_translate=glm::vec3(0,5,0);p.m_shapeSize=glm::vec3(0.2f);
    p.m_shape=mmd::PMXRigidBody::Shape::Sphere;p.m_op=mmd::PMXRigidBody::Operation::Static;
    p.m_mass=2;p.m_friction=0.3f;p.m_repulsion=0.2f;p.m_collisionGroup=2;
    mmd::MMDRigidBody kinematic;Check(kinematic.Create(p,&model,&node),"kinematic sphere create");
    model.physics.AddRigidBody(&kinematic);
    node.SetGlobalTransform(glm::translate(glm::mat4(1),glm::vec3(1,5,0)));
    kinematic.CalcLocalTransform();model.physics.Update(1.0f/60);
    Check(std::fabs(kinematic.GetTransform()[3].x-1)<1e-4f,"kinematic bone synchronization");
    p.m_op=mmd::PMXRigidBody::Operation::Dynamic;p.m_translate=glm::vec3(2,8,0);
    p.m_shape=mmd::PMXRigidBody::Shape::Box;p.m_group=1;p.m_collisionGroup=5;
    mmd::MMDRigidBody dynamic;Check(dynamic.Create(p,&model,nullptr),"dynamic box create");
    model.physics.AddRigidBody(&dynamic);
    Check(kinematic.GetRigidBody()->GetCollisionGroup().CanCollide(dynamic.GetRigidBody()->GetCollisionGroup()),"Saba enabled collision mask was inverted");
    Check(dynamic.GetRigidBody()->GetMotionProperties()->GetMotionQuality()==JPH::EMotionQuality::Discrete,"Saba baseline unexpectedly enables CCD");
    Check(std::fabs(dynamic.GetRigidBody()->GetInverseInertia().Multiply3x3(JPH::Vec3::sAxisX()).GetX()-18.75f)<1e-3f,"Bullet box inertia differs");
    JPH::ContactManifold manifold;JPH::ContactSettings contact;
    model.physics.GetDynamicsWorld()->GetContactListener()->OnContactAdded(*kinematic.GetRigidBody(),*dynamic.GetRigidBody(),manifold,contact);
    Check(std::fabs(contact.mCombinedFriction-0.09f)<1e-6f && std::fabs(contact.mCombinedRestitution-0.04f)<1e-6f,"Bullet contact material product differs");
    const float y=dynamic.GetTransform()[3].y;
    for(int i=0;i<15;++i) model.physics.Update(1.0f/60);
    Check(dynamic.GetTransform()[3].y<y-0.1f,"Jolt gravity step");
    dynamic.Reset();Check(std::fabs(dynamic.GetTransform()[3].y-y)<1e-5f,"rigid body reset");
    p.m_group=2;p.m_collisionGroup=0;p.m_op=mmd::PMXRigidBody::Operation::DynamicAndBoneMerge;
    p.m_translate=glm::vec3(3,8,0);p.m_shape=mmd::PMXRigidBody::Shape::Capsule;
    mmd::MMDNode alignedNode;alignedNode.SetGlobalTransform(glm::translate(glm::mat4(1),p.m_translate));
    mmd::MMDRigidBody aligned;Check(aligned.Create(p,&model,&alignedNode),"aligned capsule create");
    model.physics.AddRigidBody(&aligned);
    Check(!dynamic.GetRigidBody()->GetCollisionGroup().CanCollide(aligned.GetRigidBody()->GetCollisionGroup()),"zero mask must disable PMX body collisions");
    for(int i=0;i<10;++i) model.physics.Update(1.0f/60);
    aligned.ReflectGlobalTransform();Check(std::fabs(alignedNode.GetGlobalTransform()[3].y-8)<1e-4f,"mode 2 preserves animated bone translation");
    mmd::PMXJoint j{};j.m_type=mmd::PMXJoint::JointType::SpringDOF6;j.m_translate=glm::vec3(2,8,0);
    j.m_translateLowerLimit=glm::vec3(-0.1f,-0.2f,-0.3f);j.m_translateUpperLimit=glm::vec3(0.4f,0.5f,0.6f);
    j.m_rotateLowerLimit=glm::vec3(-0.1f);j.m_rotateUpperLimit=glm::vec3(0.2f);
    j.m_springTranslateFactor=glm::vec3(10);j.m_springRotateFactor=glm::vec3(3);
    j.m_rotate=glm::vec3(0.2f,0.3f,-0.1f);
    j.m_translateLowerLimit.x=1;j.m_translateUpperLimit.x=-1;
    mmd::MMDJoint joint;Check(joint.CreateJoint(j,&dynamic,&aligned),"six DOF spring create");
    model.physics.AddJoint(&joint);
    auto* constraint=static_cast<JPH::SixDOFConstraint*>(joint.GetConstraint());
    const auto localAxis=constraint->GetConstraintToBody1Matrix().GetAxisX();
    const auto worldAxis=glm::mat3(dynamic.GetTransform())*glm::vec3(localAxis.GetX(),localAxis.GetY(),localAxis.GetZ());
    const auto expectedFrame=glm::rotate(glm::mat4(1),j.m_rotate.z,glm::vec3(0,0,1))*glm::rotate(glm::mat4(1),-j.m_rotate.y,glm::vec3(0,1,0))*glm::rotate(glm::mat4(1),-j.m_rotate.x,glm::vec3(1,0,0));
    Check(glm::length(worldAxis-glm::vec3(expectedFrame[0]))<1e-5f,"Saba joint Z-Y-X reference frame differs");
    Check(std::fabs(constraint->GetLimitsMin(JPH::SixDOFConstraintSettings::TranslationZ)+0.6f)<1e-5f,"Z limit handedness");
    Check(constraint->IsFreeAxis(JPH::SixDOFConstraintSettings::TranslationX),"PMX reversed limits must remain free");
    Check(constraint->GetMotorState(JPH::SixDOFConstraintSettings::RotationX)==JPH::EMotorState::Position,"angular spring motor");
    for(int i=0;i<120;++i) model.physics.Update(1.0f/60);
    Check(std::isfinite(dynamic.GetTransform()[3].y),"spring stability");
    model.physics.RemoveJoint(&joint);joint.Destroy();
    std::cout<<"PASS: Jolt shapes, modes, masks, limits, springs, reset\n";
}
static void PhysicsTimingContract() {
    for(int fps:{30,60,120,240}) {
        mmd::MMDPhysics physics;Check(physics.Create(),"timing world create");
        Check(physics.GetFPS()==120 && physics.GetMaxSubStepCount()==10,"Saba stepping defaults differ");
        for(int i=0;i<fps*2;++i)physics.Update(1.0f/fps);
        Check(physics.GetSimulationStepCount()==240 && physics.GetDroppedTime()<1e-6,"render rate changes physics time");
        physics.ResetAccumulator();physics.Update(0.5f);
        Check(physics.GetLastSubStepCount()==10 && std::fabs(physics.GetDroppedTime()-50.0/120)<1e-6,"Bullet bounded catch-up differs");
    }
    FixtureModel model;Check(model.physics.Create(),"damping world create");
    mmd::PMXRigidBody p{};p.m_op=mmd::PMXRigidBody::Operation::Dynamic;p.m_mass=1;
    p.m_shape=mmd::PMXRigidBody::Shape::Sphere;p.m_shapeSize=glm::vec3(.2f);p.m_translate=glm::vec3(0,10,0);
    p.m_translateDimmer=.5f;p.m_rotateDimmer=.25f;
    mmd::MMDRigidBody body;Check(body.Create(p,&model,nullptr),"damping body create");model.physics.AddRigidBody(&body);
    body.GetRigidBody()->SetLinearVelocity(JPH::Vec3(2,0,0));body.GetRigidBody()->SetAngularVelocity(JPH::Vec3(0,2,0));
    const float dt=1.0f/120;model.physics.Update(dt);
    Check(std::fabs(body.GetRigidBody()->GetLinearVelocity().GetX()-2*std::pow(.5f,dt))<1e-5f,"Bullet exponential linear damping differs");
    Check(std::fabs(body.GetRigidBody()->GetAngularVelocity().GetY()-2*std::pow(.75f,dt))<1e-5f,"Bullet exponential angular damping differs");
    std::cout<<"PASS: 120 Hz physics at 30/60/120/240 FPS, ten-step catch-up, Bullet damping\n";
}
static void AnimationContract(const std::string& pmx,const std::string& vmd) {
    mmd::PMXFile file;Check(file.Load(pmx),"PMX diagnostic parse");
    const auto& data=file.GetPMXModel();
    size_t noCollision=0,allCollision=0,kinematic=0;size_t jointTypes[7]={};float maxLimit=0;for(const auto& j:data.m_joints) {if(static_cast<int>(j.m_type)<7) ++jointTypes[static_cast<int>(j.m_type)];for(int i=0;i<3;++i) maxLimit=std::max(maxLimit,std::max(std::fabs(j.m_translateLowerLimit[i]),std::fabs(j.m_translateUpperLimit[i])));}std::cout<<"joint type0="<<jointTypes[0]<<" max translation limit="<<maxLimit<<'\n';
    float minMass=1e20f,maxSpring=0;
    for(const auto& b:data.m_rigidBodies) {noCollision+=b.m_collisionGroup==65535;allCollision+=b.m_collisionGroup==0;kinematic+=b.m_op==mmd::PMXRigidBody::Operation::Static;minMass=std::min(minMass,b.m_mass);}
    for(const auto& j:data.m_joints) for(int i=0;i<3;++i) maxSpring=std::max(maxSpring,std::max(j.m_springTranslateFactor[i],j.m_springRotateFactor[i]));
    std::cout<<"masks disabled="<<noCollision<<" all="<<allCollision<<" kinematic="<<kinematic<<" minMass="<<minMass<<" maxSpring="<<maxSpring<<'\n';
    Animation::MmdRuntime first,second;
    Check(first.Load(pmx,vmd),"PMX/VMD load");Check(second.Load(pmx,vmd),"second model instance");
    Check(first.GetBoneCount()>0,"bones missing");Check(first.GetLastFrame()>0,"VMD timeline missing");
    std::vector<glm::vec3> p,n,reference,other;std::vector<glm::vec2> uv;
    first.Update(120,0,false);first.GetVertices(reference,n,uv);
    glm::vec3 lower(1e20f),upper(-1e20f);
    for(const auto& v:reference) { lower=glm::min(lower,v);upper=glm::max(upper,v); }
    const float modelExtent=glm::length(upper-lower);
    for(int i=0;i<60;++i) first.Update(120,0,false);
    first.GetVertices(p,n,uv);Check(Difference(p,reference)<1e-5f,"repeated evaluation drifts");
    second.Update(300,0,false);second.GetVertices(other,n,uv);
    Check(Difference(other,reference)>0.001f,"animation frame has no deformation");
    first.GetVertices(p,n,uv);Check(Difference(p,reference)<1e-5f,"model instances share mutable state");
    for(int i=0;i<120;++i) first.Update(120,1.0f/60,true);
    first.GetVertices(p,n,uv);std::cout<<"stationary physics difference="<<Difference(p,reference)<<'\n';
    first.Update(120,0,false);
    const auto simulationStart=std::chrono::steady_clock::now();
    for(int i=0;i<600;++i) {
        first.Update(120+i*0.5f,1.0f/60,true);
        std::vector<glm::mat4> transforms;first.GetPhysicsBodies(transforms);
        for(size_t b=0;b<transforms.size();++b) for(int col=0;col<4;++col) for(int row=0;row<4;++row) if(!std::isfinite(transforms[b][col][row])) {
            std::cerr<<"nonfinite body="<<b<<" frame="<<i<<" name="<<data.m_rigidBodies[b].m_name<<'\n';
            throw std::runtime_error("nonfinite rigid body");
        }
    }
    std::cout<<"steady simulation average ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-simulationStart).count()/600<<'\n';
    first.GetVertices(p,n,uv);const float motion=Difference(p,reference);
    std::vector<glm::mat4> bodies;first.GetPhysicsBodies(bodies);float bodyExtent=0;for(const auto& b:bodies) bodyExtent=std::max(bodyExtent,glm::length(glm::vec3(b[3])));std::cout<<"rigid body extent="<<bodyExtent<<'\n';for(size_t i=0;i<bodies.size();++i) if(glm::length(glm::vec3(bodies[i][3]))>50) std::cout<<"far body="<<i<<" "<<data.m_rigidBodies[i].m_name<<" mode="<<static_cast<int>(data.m_rigidBodies[i].m_op)<<" bindY="<<data.m_rigidBodies[i].m_translate.y<<" position="<<bodies[i][3].x<<","<<bodies[i][3].y<<","<<bodies[i][3].z<<'\n';
    Check(motion>0.001f && motion<modelExtent*2,"physics/animation output exceeds model scale");
    for(const auto& normal:n) Check(std::isfinite(normal.x)&&std::isfinite(normal.y)&&std::isfinite(normal.z),"nonfinite normal");
    second.Update(419.5f,0,false);second.GetVertices(other,n,uv);
    std::cout<<"physics-only difference="<<Difference(p,other)<<'\n';
    first.Update(120,0,false);first.GetVertices(p,n,uv);Check(Difference(p,reference)<1e-5f,"physics disable/reset retains stale pose");
    Check(first.Load(pmx,vmd),"model reload");first.Update(120,0,false);first.GetVertices(p,n,uv);
    Check(Difference(p,reference)<1e-5f,"reload retains stale animation state");
    std::cout<<"PASS: PMX bones="<<first.GetBoneCount()<<" morphs="<<first.GetMorphCount()
        <<" vertices="<<p.size()<<" deformation="<<motion<<"; 600 frames, repeat, reset, reload, independent instances\n";
}
static void MaterialOperationContract(const std::string& pmx,const std::string& face) {
    mmd::PMXFile file;Check(file.Load(pmx),"material fixture load");
    const auto& source=file.GetPMXModel();
    mmd::MMDModelImpl model;Check(model.Create(pmx),"material model create");
    model.InitializeAnimation();model.UpdateMorphAnimation();
    size_t tested=0;
    for(size_t i=0;i<source.m_morphs.size();++i) {
        const auto& morph=source.m_morphs[i];
        if(morph.m_morphType!=mmd::PMXMorphType::Material)continue;
        model.BeginAnimation();model.GetMorph(i)->SetWeight(0.5f);model.UpdateMorphAnimation();
        auto expected=source.m_materials;
        for(const auto& op:morph.m_materials) {
            auto apply=[&](size_t index) {
                if(op.m_opType==0) expected[index].m_diffuse*=glm::mix(glm::vec4(1),op.m_diffuse,0.5f);
                else expected[index].m_diffuse+=op.m_diffuse*0.5f;
            };
            if(op.m_index==-1)for(size_t j=0;j<expected.size();++j)apply(j);
            else if(op.m_index>=0 && size_t(op.m_index)<expected.size())apply(op.m_index);
        }
        for(size_t j=0;j<expected.size();++j)
            Check(glm::length(model.GetMaterials()[j].m_diffuse-expected[j].m_diffuse)<1e-4f,"PMX material operation differs from authored value");
        model.BeginAnimation();model.UpdateMorphAnimation();
        for(size_t j=0;j<expected.size();++j)
            Check(glm::length(model.GetMaterials()[j].m_diffuse-source.m_materials[j].m_diffuse)<1e-5f,"material operation retains previous weight");
        ++tested;
    }
    if(model.FindMorphIndex("まばたき")>=model.GetMorphCount() && model.FindMorphIndex("ウィンク")<model.GetMorphCount() && model.FindMorphIndex("ウィンク右")<model.GetMorphCount()) {
        mmd::VMDFile vmd;Check(vmd.Load(face),"blink fixture load");
        const mmd::VMDMorph* blink=nullptr;
        for(const auto& key:vmd.GetMorphs()) if(key.morphName=="まばたき" && (!blink || key.weight>blink->weight))blink=&key;
        Check(blink && blink->weight>0,"blink fixture has no active key");
        mmd::VMDAnimation animation;Check(animation.Create(&model)&&animation.Add(vmd),"blink animation create");
        model.BeginAnimation();animation.Evaluate(float(blink->frameNo));
        Check(model.GetMorph(model.FindMorphIndex("ウィンク"))->GetWeight()>0.5f && model.GetMorph(model.FindMorphIndex("ウィンク右"))->GetWeight()>0.5f,"fallback blink does not close both eyelids");
    }
    std::cout<<"PASS: material operations="<<tested<<", reset and optional bilateral blink mapping\n";
}
static void ImportUvContract(const std::string& pmx) {
    mmd::PMXFile file;Check(file.Load(pmx),"UV fixture load");
    Animation::MmdRuntime runtime;Check(runtime.Load(pmx,{}),"UV runtime load");
    ModelLoadResult result;Check(Animation::LoadPmxMesh(pmx,result),"UV mesh load");
    std::vector<glm::vec3> p,n;std::vector<glm::vec2> uv;runtime.GetVertices(p,n,uv);
    const auto& original=file.GetPMXModel().m_vertices;
    for(size_t i=0;i<uv.size();++i)
        Check(glm::length(uv[i]-glm::vec2(original[i].m_uv.x,1-original[i].m_uv.y))<1e-6f,"Klee import ignores bottom-origin UV");
    for(const auto& mesh:result.meshData.subMeshes)for(size_t i=0;i<mesh.vertices.size();++i)
        Check(glm::length(UnpackHalf2(mesh.vertices[i].TexCoords)-uv[mesh.mmdVertexIndices[i]])<0.001f,"static and animated imported UV differ");
    Check(runtime.ApplyVertices(result),"imported UV apply");
    for(const auto& mesh:result.meshData.subMeshes)for(size_t i=0;i<mesh.vertices.size();++i)
        Check(glm::length(UnpackHalf2(mesh.vertices[i].TexCoords)-uv[mesh.mmdVertexIndices[i]])<0.001f,"ApplyVertices loses UV convention");
    std::cout<<"PASS: Klee bottom-origin UV, static/animated/ApplyVertices consistent, vertices="<<uv.size()<<'\n';
}
static float MaterialDifference(const std::vector<mmd::MMDMaterial>& a,const std::vector<mmd::MMDMaterial>& b) {
    Check(a.size()==b.size(),"material count changed");
    float result=0;
    for(size_t i=0;i<a.size();++i) {
        Check(std::isfinite(a[i].m_diffuse.a),"nonfinite material alpha");
        result=std::max(result,glm::length(a[i].m_diffuse-b[i].m_diffuse));
        result=std::max(result,glm::length(a[i].m_textureMul-b[i].m_textureMul));
        result=std::max(result,glm::length(a[i].m_textureAdd-b[i].m_textureAdd));
    }
    return result;
}
static void MorphContract(const std::string& pmx,const std::string& motion) {
    Animation::MmdRuntime model;Check(model.Load(pmx,motion),"morph VMD load");
    std::vector<glm::vec3> baseline,p,n;std::vector<glm::vec2> uv;
    model.Update(0,0,false);model.GetVertices(baseline,n,uv);
    const auto baselineMaterials=model.GetMaterials();
    float change=0,materialChange=0;
    for(int i=1;i<=20;++i) {
        model.Update(model.GetLastFrame()*i/20,0,false);model.GetVertices(p,n,uv);
        change=std::max(change,Difference(p,baseline));
        materialChange=std::max(materialChange,MaterialDifference(model.GetMaterials(),baselineMaterials));
    }
    Check(change>0.001f || materialChange>0.001f,"morph VMD changes neither vertices nor materials");
    model.Update(0,0,false);
    Check(MaterialDifference(model.GetMaterials(),baselineMaterials)<1e-5f,"material morph accumulates after seek");
    std::cout<<"PASS: facial morph deformation="<<change<<" material="<<materialChange<<'\n';
}
static void LayeredContract(const std::string& pmx,const std::string& body,const std::string& face) {
    Animation::MmdRuntime combined,bodyOnly,faceOnly;
    Check(combined.Load(pmx,body,face),"combined VMD load");
    Check(bodyOnly.Load(pmx,body)&&faceOnly.Load(pmx,face),"comparison VMD load");
    Check(combined.GetLastFrame()==std::max(bodyOnly.GetLastFrame(),faceOnly.GetLastFrame()),"face truncates body timeline");
    std::vector<glm::vec3> a,b,n;std::vector<glm::vec2> uv;
    float difference=0;
    for(int i=1;i<=20;++i) {
        const float frame=faceOnly.GetLastFrame()*i/20;
        combined.Update(frame,0,false);bodyOnly.Update(frame,0,false);
        combined.GetVertices(a,n,uv);bodyOnly.GetVertices(b,n,uv);
        difference=std::max(difference,Difference(a,b));
        difference=std::max(difference,MaterialDifference(combined.GetMaterials(),bodyOnly.GetMaterials()));
    }
    Check(difference>0.001f,"facial layer lost when combined with body motion");
    std::cout<<"PASS: body + facial layer; last frame="<<combined.GetLastFrame()<<" facial difference="<<difference<<'\n';
}
static void CameraContract(const std::string& path) {
    Animation::VmdMotion motion;std::string error;
    Check(Animation::VmdMotion::LoadFromFile(path,motion,error),"camera VMD load");
    Check(motion.HasCameraTrack(),"camera track missing");
    const auto& keys=motion.GetCameraKeyframes();

    for(size_t i=1;i<keys.size();++i) {
        Animation::VmdCameraKeyframe sample;
        Check(motion.SampleCamera((keys[i-1].frame+keys[i].frame)*0.5f,sample),"camera sampling");
        Check(std::isfinite(sample.distance)&&std::isfinite(sample.position.x)&&std::isfinite(sample.rotation.y),"nonfinite camera");
        if(keys[i].frame-keys[i-1].frame==1)
            Check(glm::length(sample.position-keys[i-1].position)<1e-5f,"camera cut interpolated");
    }
    // Real camera curves can be symmetric at their midpoint. Use an asymmetric
    // fixture to distinguish independent channel Bezier interpolation from mix().
    std::vector<std::uint8_t> bytes(50,0);
    std::memcpy(bytes.data(),"Vocaloid Motion Data 0002",25);
    const auto integer=[&](std::uint32_t value) {
        for(int i=0;i<4;++i) bytes.push_back(static_cast<std::uint8_t>(value>>(i*8)));
    };
    const auto scalar=[&](float value) {
        std::uint32_t bits;std::memcpy(&bits,&value,4);integer(bits);
    };
    integer(0);integer(0);integer(2);
    for(int key=0;key<2;++key) {
        integer(key*10);scalar(-40.0f-key*40);
        scalar(key*10.0f);scalar(key*20.0f);scalar(key*30.0f);
        scalar(0);scalar(0);scalar(0);
        for(int channel=0;channel<6;++channel) {
            bytes.push_back(channel==0?0:20);bytes.push_back(channel==0?0:107);
            bytes.push_back(channel==0?127:20);bytes.push_back(channel==0?127:107);
        }
        integer(45);bytes.push_back(static_cast<std::uint8_t>(key));
    }
    Animation::VmdMotion fixture;Animation::VmdCameraKeyframe sample;
    Check(Animation::VmdMotion::LoadFromMemory(bytes.data(),bytes.size(),fixture,error),"camera fixture parse");
    Check(fixture.SampleCamera(5,sample),"camera fixture track missing");
    Check(sample.position.x>9 && std::fabs(sample.position.y-10)<1e-4f && std::fabs(sample.distance+60)<1e-4f,"independent camera Bezier channels");
    Check(sample.perspective,"perspective switches before its key");
    fixture.SampleCamera(10,sample);Check(!sample.perspective,"perspective key not applied");
    std::cout<<"PASS: camera keys="<<keys.size()<<" curves and cuts\n";
}
static void SabaPoseContract() {
    mmd::MMDNode source,append,chained;
    source.SetLocalPosition(glm::vec3(2,0,0));source.SaveInitialTRS();
    source.SetLocalPosition(glm::vec3(4,0,0));source.SetAnimationTranslate(glm::vec3(20,0,0));
    const auto rotation=glm::angleAxis(0.8f,glm::vec3(0,1,0));
    source.SetAnimationRotate(rotation);
    append.ConfigureAppend(&source,0.5f,true,true,false);append.UpdateAppendTransform();
    Check(glm::length(append.GetAnimatedPosition()-glm::vec3(1,0,0))<1e-5f,"append translation must use base morph displacement, not VMD displacement");
    Check(std::fabs(glm::angle(append.GetAnimatedRotation())-0.4f)<1e-5f,"weighted append rotation");
    chained.ConfigureAppend(&append,0.5f,true,true,false);chained.UpdateAppendTransform();
    Check(glm::length(chained.GetAnimatedPosition()-glm::vec3(0.5f,0,0))<1e-5f,"append chain translation");
    Check(std::fabs(glm::angle(chained.GetAnimatedRotation())-0.2f)<1e-5f,"append chain rotation");
    source.EnableIK(true);source.SetIKRotate(rotation);append.UpdateAppendTransform();
    Check(std::fabs(glm::angle(append.GetAnimatedRotation())-0.8f)<1e-5f,"append rotation omits source IK");
    append.ResetAppendTransform();append.UpdateLocalTransform();
    Check(glm::length(append.GetAnimatedPosition())<1e-5f,"append reset retains previous pose");
    mmd::MMDIkSolver solver;mmd::VMDIKController control;control.SetIKSolver(&solver);
    mmd::VMDIKAnimationKey on{},off{};on.m_time=0;on.m_enable=true;off.m_time=30;off.m_enable=false;
    control.AddKey(on);control.AddKey(off);control.SortKeys();
    control.Evaluate(29.9f);Check(solver.IsEnabled(),"IK enable switches before authored key");
    control.Evaluate(30);Check(!solver.IsEnabled(),"IK disable key not applied");
    control.Evaluate(0);Check(solver.IsEnabled(),"IK seek retains disabled state");
    std::cout<<"PASS: Saba append transforms, chained inheritance, IK contribution and authored IK switch times\n";
}
static void InterpolationContract() {
    SabaPoseContract();
    mmd::VMDBezier curve;
    curve.m_cp1=glm::vec2(0,16.0f/127);
    curve.m_cp2=glm::vec2(58.0f/127,119.0f/127);
    Check(std::fabs(curve.EvalY(curve.FindBezierX(0)))<1e-5f,"VMD key starts at a different pose: invalid Bezier basis");
    Check(std::fabs(curve.EvalY(curve.FindBezierX(1))-1)<1e-5f,"VMD key does not finish at its authored pose");
    float previous=0;
    for(int i=0;i<=100;++i) {
        const float value=curve.EvalY(curve.FindBezierX(i/100.0f));
        Check(value>=previous-1e-5f && value>=-1e-5f && value<=1.00001f,"VMD interpolation reverses or overshoots");
        previous=value;
    }
    mmd::MMDNode node;mmd::VMDNodeController controller;controller.SetNode(&node);
    mmd::VMDNodeAnimationKey start{},end{};
    start.m_time=0;end.m_time=30;start.m_rotate=glm::quat(1,0,0,0);
    end.m_rotate=glm::angleAxis(glm::radians(90.0f),glm::vec3(0,1,0));
    start.m_translate=glm::vec3(0);end.m_translate=glm::vec3(10,0,0);
    start.m_txBezier=start.m_tyBezier=start.m_tzBezier=start.m_rotBezier=curve;
    end.m_txBezier=end.m_tyBezier=end.m_tzBezier=end.m_rotBezier=curve;
    controller.AddKey(start);controller.AddKey(end);controller.SortKeys();
    controller.Evaluate(0);
    Check(glm::length(node.GetAnimationTranslate())<1e-5f && glm::angle(node.GetAnimationRotate())<1e-5f,"bone jumps at start key");
    controller.Evaluate(30);
    Check(glm::length(node.GetAnimationTranslate()-end.m_translate)<1e-5f,"bone misses end key");
    end.m_txBezier.m_cp1=end.m_txBezier.m_cp2=glm::vec2(0.5f);
    mmd::VMDNodeController distinct;distinct.SetNode(&node);distinct.AddKey(start);distinct.AddKey(end);distinct.SortKeys();
    distinct.Evaluate(15);
    Check(std::fabs(node.GetAnimationTranslate().x-10*curve.EvalY(curve.FindBezierX(0.5f)))<1e-5f,"Saba outgoing key curve not used");
    std::cout<<"PASS: authored VMD key endpoints, bounded monotonic interpolation, bone pose continuity\n";
}
static void PlaybackClockContract() {
    for (int fps : {5,30,60,120,240,1000}) {
        Animation::MmdPlaybackClock clock;
        double advanced=0;
        int updates=0;
        for (int i=0;i<fps*2;++i) {
            const float elapsed=clock.Advance(1.0/double(fps));
            advanced+=elapsed;
            if (elapsed>0) ++updates;
        }
        Check(std::fabs(advanced*30.0-60.0)<0.001,"render FPS changes VMD playback speed");
        Check(updates<=240,"MMD pose updates exceed 120 Hz");
    }
    Animation::MmdPlaybackClock clock;
    Check(clock.Advance(0)==0 && clock.Advance(-1)==0,"invalid clock input advances playback");
    Check(clock.Advance(1.0/240)==0,"clock advances before a 120 Hz tick");
    clock.Reset();
    Check(clock.Advance(1.0/240)==0,"reset retains fractional tick debt");
    Check(std::fabs(clock.Advance(1.0/240)-1.0/120)<1e-7,"high FPS loses accumulated time");
    Check(std::fabs(clock.Advance(0.5)-0.5)<1e-7,"long render frame slows playback");
    std::cout<<"PASS: 120 Hz playback clock at 5/30/60/120/240/1000 FPS, pause/reset and long frames\n";
}
static void PoseReuseContract(const char* modelPath,const char* motionPath) {
    Animation::MmdRuntime runtime;
    Check(runtime.Load(modelPath,motionPath),"pose reuse model load");
    std::vector<glm::vec3> positions,normals,repeatedPositions,repeatedNormals;
    std::vector<glm::vec2> uvs,repeatedUvs;
    runtime.Update(15,0,false);
    runtime.GetVertices(positions,normals,uvs);
    auto revision=runtime.GetPoseRevision();
    for(int i=0;i<60;++i) runtime.Update(15,0,false);
    Check(runtime.GetPoseRevision()==revision,"frozen pose evaluated repeatedly");
    runtime.GetVertices(repeatedPositions,repeatedNormals,repeatedUvs);
    Check(Difference(positions,repeatedPositions)==0 && Difference(normals,repeatedNormals)==0 &&
          uvs==repeatedUvs,"reused pose changed vertices or UVs");
    runtime.Update(16,0,false);
    Check(runtime.GetPoseRevision()>revision,"new VMD frame reused stale pose");
    runtime.Update(16,0,true);
    revision=runtime.GetPoseRevision();
    runtime.Update(16,0,true);
    Check(runtime.GetPoseRevision()==revision,"frozen physics pose evaluated repeatedly");
    runtime.Update(16,1.0f/120,true);
    Check(runtime.GetPoseRevision()>revision,"same VMD frame suppressed positive physics time");
    revision=runtime.GetPoseRevision();runtime.ResetPhysics();runtime.Update(16,0,true);
    Check(runtime.GetPoseRevision()>revision,"physics reset reused stale pose");
    revision=runtime.GetPoseRevision();runtime.Update(16,0,false);
    Check(runtime.GetPoseRevision()>revision,"physics toggle reused stale pose");
    revision=runtime.GetPoseRevision();runtime.Update(0,0,false);
    Check(runtime.GetPoseRevision()>revision,"seek reused stale pose");
    std::cout<<"PASS: frozen pose reuse, new frame, seek, physics toggle/reset and positive physics time\n";
}
static void KeyCursorContract() {
    mmd::MMDMorph morph;
    mmd::VMDMorphController controller;controller.SetMorph(&morph);
    std::vector<mmd::VMDMorphAnimationKey> keys;
    for(int i=0;i<3000;++i) {
        mmd::VMDMorphAnimationKey key{};key.m_time=i*3;key.m_weight=float(i%7)/6;
        keys.push_back(key);controller.AddKey(key);
    }
    controller.SortKeys();
    auto verify=[&](float frame) {
        size_t index=0;
        while(index+1<keys.size() && keys[index+1].m_time<=frame) ++index;
        float expected=keys[index].m_weight;
        if(index+1<keys.size()) {
            const float factor=std::clamp((frame-keys[index].m_time)/
                float(keys[index+1].m_time-keys[index].m_time),0.0f,1.0f);
            expected=glm::mix(expected,keys[index+1].m_weight,factor);
        }
        controller.Evaluate(frame,0.75f);
        Check(std::fabs(morph.GetWeight()-expected*0.75f)<1e-7f,"cached morph interval differs from linear reference");
    };
    for(int i=0;i<18000;++i) verify(i*0.5f);
    for(float frame:{-10.0f,12000.0f,10.25f,10.5f,0.0f,8997.0f,200.0f,199.5f,5000.25f}) verify(frame);
    mmd::VMDMorphAnimationKey extra{};extra.m_time=9000;extra.m_weight=1;
    keys.push_back(extra);controller.AddKey(extra);controller.SortKeys();verify(8998.5f);
    std::cout<<"PASS: cached key interval matches reference for fractional playback, reverse, seek, endpoints and added keys\n";
}
int main(int argc,char** argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    JPH::RegisterDefaultAllocator();JPH::Factory::sInstance=new JPH::Factory;JPH::RegisterTypes();
    int result=0;
    try { KeyCursorContract(); }
    catch(const std::exception& e) {
        std::cerr<<"FAIL: "<<e.what()<<'\n';
        JPH::UnregisterTypes();delete JPH::Factory::sInstance;JPH::Factory::sInstance=nullptr;
        return 1;
    }
    if(argc>=3 && argv[1][0]!='-') {
        try { PoseReuseContract(argv[1],argv[2]); }
        catch(const std::exception& e) {
            std::cerr<<"FAIL: "<<e.what()<<'\n';
            JPH::UnregisterTypes();delete JPH::Factory::sInstance;JPH::Factory::sInstance=nullptr;
            return 1;
        }
    }
    try { PlaybackClockContract();InterpolationContract();if(argc==2 && std::strcmp(argv[1],"--interpolation")==0) {} else if(argc==3 && std::strcmp(argv[1],"--klee-uv")==0) { ImportUvContract(argv[2]); } else if(argc==5 && std::strcmp(argv[1],"--materials")==0) { MaterialOperationContract(argv[2],argv[4]);MorphContract(argv[2],argv[4]);LayeredContract(argv[2],argv[3],argv[4]); } else { PhysicsTimingContract();PhysicsContract();if(argc<3 || argc>5) throw std::runtime_error("usage: MikanMmdTests model.pmx motion.vmd [face.vmd] [camera.vmd]");AnimationContract(argv[1],argv[2]);if(argc>=4) { MorphContract(argv[1],argv[3]);LayeredContract(argv[1],argv[2],argv[3]); } if(argc==5) CameraContract(argv[4]); } }
    catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n';result=1; }
    JPH::UnregisterTypes();delete JPH::Factory::sInstance;JPH::Factory::sInstance=nullptr;
    return result;
}
