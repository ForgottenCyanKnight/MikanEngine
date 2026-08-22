// SceneSerializer_3D.cpp - 3D 组件的场景序列化（自 SceneSerializer.cpp 拆分，函数体一字未改）
// 与 SceneSerializer_2D.cpp 同属 SceneSerializer 类的成员函数定义（声明见 include/Core/SceneSerializer.h）。
// 拆分目的：改某类组件的序列化时只读对应文件，避免读 2000 行。
#include "Core/SceneSerializer.h"
#include "ECS/Components.h"
#include "ECS/Coordinator.h"
#include "ECS/Types.h"
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <iomanip>

namespace ECS {

std::string SceneSerializer::SerializeMeshComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<MeshComponent>(entity);
    
    std::stringstream json;
    json << "      \"mesh\": {" << std::endl;
    json << "        \"type\": " << (int)component.type << "," << std::endl;
    json << "        \"modelPath\": \"" << EscapeString(ConvertToRelativePath(component.modelPath)) << "\"" << std::endl;
    json << "      }";
    return json.str();
}

std::string SceneSerializer::SerializeRenderComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<RenderComponent>(entity);
    
    std::stringstream json;
    json << "      \"render\": {" << std::endl;
    json << "        \"visible\": " << (component.visible ? "true" : "false") << "," << std::endl;
    json << "        \"castShadow\": " << (component.castShadow ? "true" : "false") << "," << std::endl;
    json << "        \"receiveShadow\": " << (component.receiveShadow ? "true" : "false") << "," << std::endl;
    json << "        \"showAABB\": " << (component.showAABB ? "true" : "false") << "," << std::endl;
    json << "        \"showOBB\": " << (component.showOBB ? "true" : "false") << std::endl;
    json << "      }";
    return json.str();
}

std::string SceneSerializer::SerializeCameraComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<CameraComponent>(entity);
    
    std::stringstream json;
    json << "      \"camera\": {" << std::endl;
    json << "        \"fov\": " << component.fov << "," << std::endl;
    json << "        \"nearPlane\": " << component.nearPlane << "," << std::endl;
    json << "        \"farPlane\": " << component.farPlane << "," << std::endl;
    json << "        \"isMainCamera\": " << (component.isMainCamera ? "true" : "false") << "," << std::endl;
    json << "        \"isOrthographic\": " << (component.isOrthographic ? "true" : "false") << "," << std::endl;
    json << "        \"orthographicSize\": " << component.orthographicSize << "," << std::endl;
    json << "        \"enableFrustumCulling\": " << (component.enableFrustumCulling ? "true" : "false") << "," << std::endl;
    json << "        \"showFrustumWireframe\": " << (component.showFrustumWireframe ? "true" : "false") << "," << std::endl;
    json << "        \"useSubMeshCulling\": " << (component.useSubMeshCulling ? "true" : "false") << "," << std::endl;
    json << "        \"showBVHWireframe\": " << (component.showBVHWireframe ? "true" : "false") << "," << std::endl;
    json << "        \"showCollisionWireframe\": " << (component.showCollisionWireframe ? "true" : "false") << "," << std::endl;
    json << "        \"useBVHCulling\": " << (component.useBVHCulling ? "true" : "false") << "," << std::endl;
    json << "        \"thirdPersonEnabled\": " << (component.thirdPersonEnabled ? "true" : "false") << "," << std::endl;
    json << "        \"thirdPersonTargetName\": \"" << EscapeString(component.thirdPersonTargetName) << "\"," << std::endl;
    json << "        \"thirdPersonTargetOffset\": [" << component.thirdPersonTargetOffset.x << ", "
        << component.thirdPersonTargetOffset.y << ", " << component.thirdPersonTargetOffset.z << "]," << std::endl;
    json << "        \"thirdPersonDistance\": " << component.thirdPersonDistance << "," << std::endl;
    json << "        \"thirdPersonMinDistance\": " << component.thirdPersonMinDistance << "," << std::endl;
    json << "        \"thirdPersonMaxDistance\": " << component.thirdPersonMaxDistance << "," << std::endl;
    json << "        \"thirdPersonYaw\": " << component.thirdPersonYaw << "," << std::endl;
    json << "        \"thirdPersonPitch\": " << component.thirdPersonPitch << "," << std::endl;
    json << "        \"thirdPersonMinPitch\": " << component.thirdPersonMinPitch << "," << std::endl;
    json << "        \"thirdPersonMaxPitch\": " << component.thirdPersonMaxPitch << "," << std::endl;
    json << "        \"thirdPersonOrbitSensitivity\": " << component.thirdPersonOrbitSensitivity << "," << std::endl;
    json << "        \"thirdPersonZoomSensitivity\": " << component.thirdPersonZoomSensitivity << "," << std::endl;
    json << "        \"thirdPersonPositionDamping\": " << component.thirdPersonPositionDamping << "," << std::endl;
    json << "        \"thirdPersonRotationDamping\": " << component.thirdPersonRotationDamping << "," << std::endl;
    json << "        \"thirdPersonCaptureMouse\": " << (component.thirdPersonCaptureMouse ? "true" : "false") << "," << std::endl;
    json << "        \"thirdPersonPreserveDistanceWhenOccluded\": " << (component.thirdPersonPreserveDistanceWhenOccluded ? "true" : "false") << "," << std::endl;
    json << "        \"thirdPersonCollisionEnabled\": " << (component.thirdPersonCollisionEnabled ? "true" : "false") << "," << std::endl;
    json << "        \"thirdPersonCollisionRadius\": " << component.thirdPersonCollisionRadius << "," << std::endl;
    json << "        \"thirdPersonCollisionBuffer\": " << component.thirdPersonCollisionBuffer << "," << std::endl;
    json << "        \"thirdPersonCollisionMinDistance\": " << component.thirdPersonCollisionMinDistance << "," << std::endl;
    json << "        \"thirdPersonCollisionDampingIn\": " << component.thirdPersonCollisionDampingIn << "," << std::endl;
    json << "        \"thirdPersonCollisionDampingOut\": " << component.thirdPersonCollisionDampingOut << "," << std::endl;
    json << "        \"thirdPersonCollisionSmoothingTime\": " << component.thirdPersonCollisionSmoothingTime << "," << std::endl;
    json << "        \"thirdPersonAimEnabled\": " << (component.thirdPersonAimEnabled ? "true" : "false") << "," << std::endl;
    json << "        \"thirdPersonAimShoulderOffset\": " << component.thirdPersonAimShoulderOffset << "," << std::endl;
    json << "        \"thirdPersonAimFov\": " << component.thirdPersonAimFov << "," << std::endl;
    json << "        \"thirdPersonAimSensitivity\": " << component.thirdPersonAimSensitivity << "," << std::endl;
    json << "        \"thirdPersonAimPositionDamping\": " << component.thirdPersonAimPositionDamping << "," << std::endl;
    json << "        \"thirdPersonAimRotationDamping\": " << component.thirdPersonAimRotationDamping << "," << std::endl;
    json << "        \"thirdPersonLockOnEnabled\": " << (component.thirdPersonLockOnEnabled ? "true" : "false") << "," << std::endl;
    json << "        \"thirdPersonLockTargetName\": \"" << EscapeString(component.thirdPersonLockTargetName) << "\"," << std::endl;
    json << "        \"thirdPersonLockOnMaxDistance\": " << component.thirdPersonLockOnMaxDistance << "," << std::endl;
    json << "        \"thirdPersonLockOnLookAtBlend\": " << component.thirdPersonLockOnLookAtBlend << std::endl;
    json << "      }";
    return json.str();
}

std::string SceneSerializer::SerializeLightComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<LightComponent>(entity);
    
    std::stringstream json;
    json << "      \"light\": {" << std::endl;
    json << "        \"type\": " << (int)component.type << "," << std::endl;
    json << "        \"color\": [" << component.color.x << ", " << component.color.y << ", " << component.color.z << "]," << std::endl;
    json << "        \"intensity\": " << component.intensity << "," << std::endl;
    json << "        \"range\": " << component.range << "," << std::endl;
    json << "        \"spotAngle\": " << component.spotAngle << "," << std::endl;
    json << "        \"castShadow\": " << (component.castShadow ? "true" : "false") << std::endl;
    json << "      }";
    return json.str();
}

std::string SceneSerializer::SerializeMaterialComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<MaterialComponent>(entity);
    
    std::stringstream json;
    json << "      \"material\": {" << std::endl;
    json << "        \"albedoPath\": \"" << EscapeString(ConvertToRelativePath(component.albedoPath)) << "\"," << std::endl;
    json << "        \"normalPath\": \"" << EscapeString(ConvertToRelativePath(component.normalPath)) << "\"," << std::endl;
    json << "        \"roughnessPath\": \"" << EscapeString(ConvertToRelativePath(component.roughnessPath)) << "\"," << std::endl;
    json << "        \"metallicPath\": \"" << EscapeString(ConvertToRelativePath(component.metallicPath)) << "\"," << std::endl;
    json << "        \"aoPath\": \"" << EscapeString(ConvertToRelativePath(component.aoPath)) << "\"," << std::endl;
    json << "        \"emissivePath\": \"" << EscapeString(ConvertToRelativePath(component.emissivePath)) << "\"," << std::endl;
    json << "        \"albedoColor\": [" << component.albedoColor.x << ", " << component.albedoColor.y << ", " << component.albedoColor.z << "]," << std::endl;
    json << "        \"metallic\": " << component.metallic << "," << std::endl;
    json << "        \"roughness\": " << component.roughness << "," << std::endl;
    json << "        \"ao\": " << component.ao << "," << std::endl;
    json << "        \"emissiveIntensity\": " << component.emissiveIntensity << "," << std::endl;
    json << "        \"useAlbedoTexture\": " << (component.useAlbedoTexture ? "true" : "false") << "," << std::endl;
    json << "        \"useNormalTexture\": " << (component.useNormalTexture ? "true" : "false") << "," << std::endl;
    json << "        \"useRoughnessTexture\": " << (component.useRoughnessTexture ? "true" : "false") << "," << std::endl;
    json << "        \"useMetallicTexture\": " << (component.useMetallicTexture ? "true" : "false") << "," << std::endl;
    json << "        \"useAOTexture\": " << (component.useAOTexture ? "true" : "false") << "," << std::endl;
    json << "        \"useEmissiveTexture\": " << (component.useEmissiveTexture ? "true" : "false") << "," << std::endl;
    json << "        \"albedoSamplerType\": " << component.albedoSamplerType << "," << std::endl;
    json << "        \"normalSamplerType\": " << component.normalSamplerType << "," << std::endl;
    json << "        \"roughnessSamplerType\": " << component.roughnessSamplerType << "," << std::endl;
    json << "        \"metallicSamplerType\": " << component.metallicSamplerType << "," << std::endl;
    json << "        \"aoSamplerType\": " << component.aoSamplerType << "," << std::endl;
    json << "        \"emissiveSamplerType\": " << component.emissiveSamplerType << std::endl;
    json << "      }";
    return json.str();
}

std::string SceneSerializer::SerializeRigidBodyComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<ECS::RigidBodyComponent>(entity);
    
    std::stringstream json;
    json << "      \"rigidBody\": {" << std::endl;
    json << "        \"type\": " << (int)component.type << "," << std::endl;
    json << "        \"shapeType\": " << (int)component.shapeType << "," << std::endl;
    json << "        \"size\": [" << component.size.x << ", " << component.size.y << ", " << component.size.z << "]," << std::endl;
    json << "        \"offset\": [" << component.offset.x << ", " << component.offset.y << ", " << component.offset.z << "]," << std::endl;
    json << "        \"mass\": " << component.mass << "," << std::endl;
    json << "        \"restitution\": " << component.restitution << "," << std::endl;
    json << "        \"useGravity\": " << (component.useGravity ? "true" : "false") << "," << std::endl;
    json << "        \"isTrigger\": " << (component.isTrigger ? "true" : "false") << "," << std::endl;
    json << "        \"useOBB\": " << (component.useOBB ? "true" : "false") << "," << std::endl;
    json << "        \"syncWithModel\": " << (component.syncWithModel ? "true" : "false") << "," << std::endl;
    json << "        \"autoFitToModel\": " << (component.autoFitToModel ? "true" : "false") << "," << std::endl;
    json << "        \"collisionModelPath\": \"" << EscapeString(ConvertToRelativePath(component.collisionModelPath)) << "\"," << std::endl;
    json << "        \"collisionPrecision\": " << component.collisionPrecision << "," << std::endl;
    json << "        \"useConvexHull\": " << (component.useConvexHull ? "true" : "false") << "," << std::endl;
    json << "        \"maxConvexHullVertices\": " << component.maxConvexHullVertices << "," << std::endl;
    json << "        \"generatePerSubmesh\": " << (component.generatePerSubmesh ? "true" : "false") << std::endl;
    json << "      }";
    return json.str();
}

std::string SceneSerializer::SerializeColliderComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<ECS::ColliderComponent>(entity);
    
    std::stringstream json;
    json << "      \"collider\": {" << std::endl;
    json << "        \"type\": " << (int)component.type << "," << std::endl;
    json << "        \"size\": [" << component.size.x << ", " << component.size.y << ", " << component.size.z << "]," << std::endl;
    json << "        \"offset\": [" << component.offset.x << ", " << component.offset.y << ", " << component.offset.z << "]," << std::endl;
    json << "        \"isTrigger\": " << (component.isTrigger ? "true" : "false") << "," << std::endl;
    json << "        \"useOBB\": " << (component.useOBB ? "true" : "false") << "," << std::endl;
    json << "        \"syncWithModel\": " << (component.syncWithModel ? "true" : "false") << "," << std::endl;
    json << "        \"autoFitToModel\": " << (component.autoFitToModel ? "true" : "false") << "," << std::endl;
    json << "        \"modelPath\": \"" << EscapeString(ConvertToRelativePath(component.modelPath)) << "\"" << std::endl;
    json << "      }";
    return json.str();
}

std::string SceneSerializer::SerializeVoxModelComponent(Entity entity) {
    auto& coordinator = Coordinator::GetInstance();
    auto& component = coordinator.GetComponent<ECS::VoxModelComponent>(entity);
    
    std::stringstream json;
    json << "      \"voxModel\": {" << std::endl;
    json << "        \"voxPath\": \"" << EscapeString(ConvertToRelativePath(component.voxPath)) << "\"," << std::endl;
    json << "        \"isStatic\": " << (component.isStatic ? "true" : "false") << std::endl;
    json << "      }";
    return json.str();
}

    //


void SceneSerializer::DeserializeMeshComponent(Entity entity, const std::string& jsonString) {
    
    std::string meshJson = ExtractValue(jsonString, "mesh");
    
    std::string typeStr = ExtractValue(meshJson, "type");
    
    std::string modelPath = ExtractValue(meshJson, "modelPath");
    
    //
    if (!modelPath.empty() && modelPath.front() == '"' && modelPath.back() == '"') {
        modelPath = modelPath.substr(1, modelPath.size() - 2);
    }
    
    //
    //
    int type = 0;
    if (!typeStr.empty()) {
        type = std::stoi(typeStr);
    }
    
    auto& coordinator = Coordinator::GetInstance();
    MeshComponent mesh;
    mesh.type = (MeshType)type;
    mesh.modelPath = modelPath;
    coordinator.AddComponent<MeshComponent>(entity, mesh);
    
    //
    if (!coordinator.HasComponent<RenderComponent>(entity)) {
        RenderComponent render;
        render.visible = true;
        coordinator.AddComponent<RenderComponent>(entity, render);
    }
}

void SceneSerializer::DeserializeRenderComponent(Entity entity, const std::string& jsonString) {
    std::string renderJson = ExtractValue(jsonString, "render");
    
    bool visible = ExtractBoolValue(renderJson, "visible");
    bool castShadow = ExtractBoolValue(renderJson, "castShadow");
    bool receiveShadow = ExtractBoolValue(renderJson, "receiveShadow");
    bool showAABB = ExtractBoolValue(renderJson, "showAABB");
    bool showOBB = ExtractBoolValue(renderJson, "showOBB");
    
    auto& coordinator = Coordinator::GetInstance();
    RenderComponent render;
    render.visible = visible;
    render.castShadow = castShadow;
    render.receiveShadow = receiveShadow;
    render.showAABB = showAABB;
    render.showOBB = showOBB;
    coordinator.AddComponent<RenderComponent>(entity, render);
}

void SceneSerializer::DeserializeCameraComponent(Entity entity, const std::string& jsonString) {

    std::string cameraJson = ExtractValue(jsonString, "camera");
    
    std::string fovStr = ExtractValue(cameraJson, "fov");
    float fov = 60.0f;
    if (!fovStr.empty()) {
        try {
            fov = std::stof(fovStr);
        } catch (...) {
            fov = 60.0f;
        }
    }
    
    std::string nearPlaneStr = ExtractValue(cameraJson, "nearPlane");
    float nearPlane = 0.1f;
    if (!nearPlaneStr.empty()) {
        try {
            nearPlane = std::stof(nearPlaneStr);
        } catch (...) {
            nearPlane = 0.1f;
        }
    }
    
    std::string farPlaneStr = ExtractValue(cameraJson, "farPlane");
    float farPlane = 1000.0f;
    if (!farPlaneStr.empty()) {
        try {
            farPlane = std::stof(farPlaneStr);
        } catch (...) {
            farPlane = 1000.0f;
        }
    }
    
    bool isMainCamera = ExtractBoolValue(cameraJson, "isMainCamera");
    bool isOrthographic = ExtractBoolValue(cameraJson, "isOrthographic");
    
    std::string orthographicSizeStr = ExtractValue(cameraJson, "orthographicSize");
    float orthographicSize = 5.0f;
    if (!orthographicSizeStr.empty()) {
        try {
            orthographicSize = std::stof(orthographicSizeStr);
        } catch (...) {
            orthographicSize = 5.0f;
        }
    }
    
    //
    std::string enableFrustumCullingStr = ExtractValue(cameraJson, "enableFrustumCulling");
    bool enableFrustumCulling = enableFrustumCullingStr.empty() ? true : ExtractBoolValue(cameraJson, "enableFrustumCulling");
    
    std::string showFrustumWireframeStr = ExtractValue(cameraJson, "showFrustumWireframe");
    bool showFrustumWireframe = showFrustumWireframeStr.empty() ? true : ExtractBoolValue(cameraJson, "showFrustumWireframe");
    
    //
    std::string useSubMeshCullingStr = ExtractValue(cameraJson, "useSubMeshCulling");
    bool useSubMeshCulling = useSubMeshCullingStr.empty() ? false : ExtractBoolValue(cameraJson, "useSubMeshCulling");
    
    //
    std::string showBVHWireframeStr = ExtractValue(cameraJson, "showBVHWireframe");
    bool showBVHWireframe = showBVHWireframeStr.empty() ? false : ExtractBoolValue(cameraJson, "showBVHWireframe");

    // 兼容上一版错误命名的相机专属开关：如果旧存档中存在它，迁移为全局碰撞体显示。
    std::string showCollisionWireframeStr = ExtractValue(cameraJson, "showCollisionWireframe");
    const bool hasCurrentCollisionWireframe = !showCollisionWireframeStr.empty();
    if (!hasCurrentCollisionWireframe) {
        showCollisionWireframeStr = ExtractValue(cameraJson, "thirdPersonShowCollisionWireframe");
    }
    const bool showCollisionWireframe = showCollisionWireframeStr.empty()
        ? false
        : ExtractBoolValue(cameraJson, hasCurrentCollisionWireframe
            ? "showCollisionWireframe" : "thirdPersonShowCollisionWireframe");

    // 2026-08-17：补齐 useBVHCulling（手写 fallback 与反射字段表对齐；缺省=组件默认 false）
    std::string useBVHCullingStr = ExtractValue(cameraJson, "useBVHCulling");
    bool useBVHCulling = useBVHCullingStr.empty() ? false : ExtractBoolValue(cameraJson, "useBVHCulling");

    bool thirdPersonEnabled = ExtractBoolValue(cameraJson, "thirdPersonEnabled");
    std::string thirdPersonTargetName = ExtractValue(cameraJson, "thirdPersonTargetName");
    if (thirdPersonTargetName.size() >= 2 && thirdPersonTargetName.front() == '"' && thirdPersonTargetName.back() == '"') {
        thirdPersonTargetName = UnescapeString(thirdPersonTargetName.substr(1, thirdPersonTargetName.size() - 2));
    }
    std::vector<float> thirdPersonTargetOffset = ParseFloatArray(ExtractValue(cameraJson, "thirdPersonTargetOffset"));

    auto readCameraFloat = [&](const char* key, float fallback) {
        const std::string value = ExtractValue(cameraJson, key);
        if (value.empty()) return fallback;
        try { return std::stof(value); } catch (...) { return fallback; }
    };
    
    auto& coordinator = Coordinator::GetInstance();
    CameraComponent camera;
    camera.fov = fov;
    camera.nearPlane = nearPlane;
    camera.farPlane = farPlane;
    camera.isMainCamera = isMainCamera;
    camera.isOrthographic = isOrthographic;
    camera.orthographicSize = orthographicSize;
    camera.enableFrustumCulling = enableFrustumCulling;
    camera.showFrustumWireframe = showFrustumWireframe;
    camera.useSubMeshCulling = useSubMeshCulling;
    camera.showBVHWireframe = showBVHWireframe;
    camera.showCollisionWireframe = showCollisionWireframe;
    camera.useBVHCulling = useBVHCulling;
    camera.thirdPersonEnabled = thirdPersonEnabled;
    camera.thirdPersonTargetName = thirdPersonTargetName;
    if (thirdPersonTargetOffset.size() >= 3) {
        camera.thirdPersonTargetOffset = glm::vec3(thirdPersonTargetOffset[0], thirdPersonTargetOffset[1], thirdPersonTargetOffset[2]);
    }
    camera.thirdPersonDistance = readCameraFloat("thirdPersonDistance", camera.thirdPersonDistance);
    camera.thirdPersonMinDistance = readCameraFloat("thirdPersonMinDistance", camera.thirdPersonMinDistance);
    camera.thirdPersonMaxDistance = readCameraFloat("thirdPersonMaxDistance", camera.thirdPersonMaxDistance);
    camera.thirdPersonYaw = readCameraFloat("thirdPersonYaw", camera.thirdPersonYaw);
    camera.thirdPersonPitch = readCameraFloat("thirdPersonPitch", camera.thirdPersonPitch);
    camera.thirdPersonMinPitch = readCameraFloat("thirdPersonMinPitch", camera.thirdPersonMinPitch);
    camera.thirdPersonMaxPitch = readCameraFloat("thirdPersonMaxPitch", camera.thirdPersonMaxPitch);
    camera.thirdPersonOrbitSensitivity = readCameraFloat("thirdPersonOrbitSensitivity", camera.thirdPersonOrbitSensitivity);
    camera.thirdPersonZoomSensitivity = readCameraFloat("thirdPersonZoomSensitivity", camera.thirdPersonZoomSensitivity);
    camera.thirdPersonPositionDamping = readCameraFloat("thirdPersonPositionDamping", camera.thirdPersonPositionDamping);
    camera.thirdPersonRotationDamping = readCameraFloat("thirdPersonRotationDamping", camera.thirdPersonRotationDamping);
    std::string thirdPersonCaptureMouseStr = ExtractValue(cameraJson, "thirdPersonCaptureMouse");
    camera.thirdPersonCaptureMouse = thirdPersonCaptureMouseStr.empty()
        ? camera.thirdPersonCaptureMouse : ExtractBoolValue(cameraJson, "thirdPersonCaptureMouse");
    std::string thirdPersonPreserveDistanceWhenOccludedStr = ExtractValue(cameraJson, "thirdPersonPreserveDistanceWhenOccluded");
    camera.thirdPersonPreserveDistanceWhenOccluded = thirdPersonPreserveDistanceWhenOccludedStr.empty()
        ? camera.thirdPersonPreserveDistanceWhenOccluded : ExtractBoolValue(cameraJson, "thirdPersonPreserveDistanceWhenOccluded");
    std::string thirdPersonCollisionEnabledStr = ExtractValue(cameraJson, "thirdPersonCollisionEnabled");
    camera.thirdPersonCollisionEnabled = thirdPersonCollisionEnabledStr.empty()
        ? camera.thirdPersonCollisionEnabled : ExtractBoolValue(cameraJson, "thirdPersonCollisionEnabled");
    camera.thirdPersonCollisionRadius = readCameraFloat("thirdPersonCollisionRadius", camera.thirdPersonCollisionRadius);
    camera.thirdPersonCollisionBuffer = readCameraFloat("thirdPersonCollisionBuffer", camera.thirdPersonCollisionBuffer);
    camera.thirdPersonCollisionMinDistance = readCameraFloat("thirdPersonCollisionMinDistance", camera.thirdPersonCollisionMinDistance);
    camera.thirdPersonCollisionDampingIn = readCameraFloat("thirdPersonCollisionDampingIn", camera.thirdPersonCollisionDampingIn);
    camera.thirdPersonCollisionDampingOut = readCameraFloat("thirdPersonCollisionDampingOut", camera.thirdPersonCollisionDampingOut);
    camera.thirdPersonCollisionSmoothingTime = readCameraFloat("thirdPersonCollisionSmoothingTime", camera.thirdPersonCollisionSmoothingTime);
    camera.thirdPersonAimEnabled = ExtractBoolValue(cameraJson, "thirdPersonAimEnabled");
    camera.thirdPersonAimShoulderOffset = readCameraFloat("thirdPersonAimShoulderOffset", camera.thirdPersonAimShoulderOffset);
    camera.thirdPersonAimFov = readCameraFloat("thirdPersonAimFov", camera.thirdPersonAimFov);
    camera.thirdPersonAimSensitivity = readCameraFloat("thirdPersonAimSensitivity", camera.thirdPersonAimSensitivity);
    camera.thirdPersonAimPositionDamping = readCameraFloat("thirdPersonAimPositionDamping", camera.thirdPersonAimPositionDamping);
    camera.thirdPersonAimRotationDamping = readCameraFloat("thirdPersonAimRotationDamping", camera.thirdPersonAimRotationDamping);
    camera.thirdPersonLockOnEnabled = ExtractBoolValue(cameraJson, "thirdPersonLockOnEnabled");
    camera.thirdPersonLockTargetName = ExtractValue(cameraJson, "thirdPersonLockTargetName");
    if (camera.thirdPersonLockTargetName.size() >= 2 && camera.thirdPersonLockTargetName.front() == '"' && camera.thirdPersonLockTargetName.back() == '"') {
        camera.thirdPersonLockTargetName = UnescapeString(camera.thirdPersonLockTargetName.substr(1, camera.thirdPersonLockTargetName.size() - 2));
    }
    camera.thirdPersonLockOnMaxDistance = readCameraFloat("thirdPersonLockOnMaxDistance", camera.thirdPersonLockOnMaxDistance);
    camera.thirdPersonLockOnLookAtBlend = readCameraFloat("thirdPersonLockOnLookAtBlend", camera.thirdPersonLockOnLookAtBlend);
    coordinator.AddComponent<CameraComponent>(entity, camera);
}

void SceneSerializer::DeserializeLightComponent(Entity entity, const std::string& jsonString) {

    std::string lightJson = ExtractValue(jsonString, "light");
    
    std::string typeStr = ExtractValue(lightJson, "type");
    int type = 0;
    if (!typeStr.empty()) {
        try {
            type = std::stoi(typeStr);
        } catch (...) {
            type = 0;
        }
    }
    
    std::vector<float> color = ParseFloatArray(ExtractValue(lightJson, "color"));
    
    std::string intensityStr = ExtractValue(lightJson, "intensity");
    float intensity = 1.0f;
    if (!intensityStr.empty()) {
        try {
            intensity = std::stof(intensityStr);
        } catch (...) {
            intensity = 1.0f;
        }
    }
    
    std::string rangeStr = ExtractValue(lightJson, "range");
    float range = 10.0f;
    if (!rangeStr.empty()) {
        try {
            range = std::stof(rangeStr);
        } catch (...) {
            range = 10.0f;
        }
    }
    
    std::string spotAngleStr = ExtractValue(lightJson, "spotAngle");
    float spotAngle = 45.0f;
    if (!spotAngleStr.empty()) {
        try {
            spotAngle = std::stof(spotAngleStr);
        } catch (...) {
            spotAngle = 45.0f;
        }
    }
    
    // 2026-08-13：点光源阴影开关（默认 false——旧场景文件无此字段）
    std::string castShadowStr = ExtractValue(lightJson, "castShadow");
    bool castShadow = castShadowStr == "true";
    
    auto& coordinator = Coordinator::GetInstance();
    LightComponent light;
    light.type = (LightComponent::Type)type;
    light.castShadow = castShadow;   // 2026-08-13：阴影开关（此前解析了但漏赋值——开关恒 false）
    if (color.size() == 3) {
        light.color = glm::vec3(color[0], color[1], color[2]);
    } else {
        light.color = glm::vec3(1.0f);
    }
    light.intensity = intensity;
    light.range = range;
    light.spotAngle = spotAngle;
    coordinator.AddComponent<LightComponent>(entity, light);
}

void SceneSerializer::DeserializeMaterialComponent(Entity entity, const std::string& jsonString) {
    std::string materialJson = ExtractValue(jsonString, "material");
    
    std::string albedoPath = ExtractValue(materialJson, "albedoPath");
    std::string normalPath = ExtractValue(materialJson, "normalPath");
    std::string roughnessPath = ExtractValue(materialJson, "roughnessPath");
    std::string metallicPath = ExtractValue(materialJson, "metallicPath");
    std::string aoPath = ExtractValue(materialJson, "aoPath");
    std::string emissivePath = ExtractValue(materialJson, "emissivePath");
    
    //
    if (!albedoPath.empty() && albedoPath.front() == '"' && albedoPath.back() == '"') {
        albedoPath = albedoPath.substr(1, albedoPath.size() - 2);
    }
    if (!normalPath.empty() && normalPath.front() == '"' && normalPath.back() == '"') {
        normalPath = normalPath.substr(1, normalPath.size() - 2);
    }
    if (!roughnessPath.empty() && roughnessPath.front() == '"' && roughnessPath.back() == '"') {
        roughnessPath = roughnessPath.substr(1, roughnessPath.size() - 2);
    }
    if (!metallicPath.empty() && metallicPath.front() == '"' && metallicPath.back() == '"') {
        metallicPath = metallicPath.substr(1, metallicPath.size() - 2);
    }
    if (!aoPath.empty() && aoPath.front() == '"' && aoPath.back() == '"') {
        aoPath = aoPath.substr(1, aoPath.size() - 2);
    }
    if (!emissivePath.empty() && emissivePath.front() == '"' && emissivePath.back() == '"') {
        emissivePath = emissivePath.substr(1, emissivePath.size() - 2);
    }
    
    std::vector<float> albedoColor = ParseFloatArray(ExtractValue(materialJson, "albedoColor"));
    
    std::string metallicStr = ExtractValue(materialJson, "metallic");
    float metallic = 0.0f;
    if (!metallicStr.empty()) {
        try {
            metallic = std::stof(metallicStr);
        } catch (...) {
            metallic = 0.0f;
        }
    }
    
    std::string roughnessStr = ExtractValue(materialJson, "roughness");
    float roughness = 0.5f;
    if (!roughnessStr.empty()) {
        try {
            roughness = std::stof(roughnessStr);
        } catch (...) {
            roughness = 0.5f;
        }
    }
    
    std::string aoStr = ExtractValue(materialJson, "ao");
    float ao = 1.0f;
    if (!aoStr.empty()) {
        try {
            ao = std::stof(aoStr);
        } catch (...) {
            ao = 1.0f;
        }
    }
    
    std::string emissiveStr = ExtractValue(materialJson, "emissiveIntensity");
    float emissiveIntensity = 0.0f;
    if (!emissiveStr.empty()) {
        try {
            emissiveIntensity = std::stof(emissiveStr);
        } catch (...) {
            emissiveIntensity = 0.0f;
        }
    }
    
    bool useAlbedoTexture = ExtractBoolValue(materialJson, "useAlbedoTexture");
    bool useNormalTexture = ExtractBoolValue(materialJson, "useNormalTexture");
    bool useRoughnessTexture = ExtractBoolValue(materialJson, "useRoughnessTexture");
    bool useMetallicTexture = ExtractBoolValue(materialJson, "useMetallicTexture");
    bool useAOTexture = ExtractBoolValue(materialJson, "useAOTexture");
    bool useEmissiveTexture = ExtractBoolValue(materialJson, "useEmissiveTexture");
    
    //
    int albedoSamplerType = 0;
    int normalSamplerType = 0;
    int roughnessSamplerType = 0;
    int metallicSamplerType = 0;
    int aoSamplerType = 0;
    int emissiveSamplerType = 0;
    
    std::string samplerTypeStr = ExtractValue(materialJson, "albedoSamplerType");
    if (!samplerTypeStr.empty()) {
        try {
            albedoSamplerType = std::stoi(samplerTypeStr);
        } catch (...) {
            albedoSamplerType = 0;
        }
    }
    
    samplerTypeStr = ExtractValue(materialJson, "normalSamplerType");
    if (!samplerTypeStr.empty()) {
        try {
            normalSamplerType = std::stoi(samplerTypeStr);
        } catch (...) {
            normalSamplerType = 0;
        }
    }
    
    samplerTypeStr = ExtractValue(materialJson, "roughnessSamplerType");
    if (!samplerTypeStr.empty()) {
        try {
            roughnessSamplerType = std::stoi(samplerTypeStr);
        } catch (...) {
            roughnessSamplerType = 0;
        }
    }
    
    samplerTypeStr = ExtractValue(materialJson, "metallicSamplerType");
    if (!samplerTypeStr.empty()) {
        try {
            metallicSamplerType = std::stoi(samplerTypeStr);
        } catch (...) {
            metallicSamplerType = 0;
        }
    }
    
    samplerTypeStr = ExtractValue(materialJson, "aoSamplerType");
    if (!samplerTypeStr.empty()) {
        try {
            aoSamplerType = std::stoi(samplerTypeStr);
        } catch (...) {
            aoSamplerType = 0;
        }
    }
    
    samplerTypeStr = ExtractValue(materialJson, "emissiveSamplerType");
    if (!samplerTypeStr.empty()) {
        try {
            emissiveSamplerType = std::stoi(samplerTypeStr);
        } catch (...) {
            emissiveSamplerType = 0;
        }
    }
    
    auto& coordinator = Coordinator::GetInstance();
    MaterialComponent material;
    material.albedoPath = albedoPath;
    material.normalPath = normalPath;
    material.roughnessPath = roughnessPath;
    material.metallicPath = metallicPath;
    material.aoPath = aoPath;
    material.emissivePath = emissivePath;
    if (albedoColor.size() == 3) {
        material.albedoColor = glm::vec3(albedoColor[0], albedoColor[1], albedoColor[2]);
    }
    material.metallic = metallic;
    material.roughness = roughness;
    material.ao = ao;
    material.emissiveIntensity = emissiveIntensity;
    material.useAlbedoTexture = useAlbedoTexture;
    material.useNormalTexture = useNormalTexture;
    material.useRoughnessTexture = useRoughnessTexture;
    material.useMetallicTexture = useMetallicTexture;
    material.useAOTexture = useAOTexture;
    material.useEmissiveTexture = useEmissiveTexture;
    material.albedoSamplerType = albedoSamplerType;
    material.normalSamplerType = normalSamplerType;
    material.roughnessSamplerType = roughnessSamplerType;
    material.metallicSamplerType = metallicSamplerType;
    material.aoSamplerType = aoSamplerType;
    material.emissiveSamplerType = emissiveSamplerType;
    coordinator.AddComponent<MaterialComponent>(entity, material);
}

void SceneSerializer::DeserializeRigidBodyComponent(Entity entity, const std::string& jsonString) {
    std::string rigidBodyJson = ExtractValue(jsonString, "rigidBody");
    
    std::string typeStr = ExtractValue(rigidBodyJson, "type");
    int type = 0;
    if (!typeStr.empty()) {
        try {
            type = std::stoi(typeStr);
        } catch (...) {
            type = 0;
        }
    }
    
    std::string shapeTypeStr = ExtractValue(rigidBodyJson, "shapeType");
    int shapeType = 0;
    if (!shapeTypeStr.empty()) {
        try {
            shapeType = std::stoi(shapeTypeStr);
        } catch (...) {
            shapeType = 0;
        }
    }
    
    std::vector<float> size = ParseFloatArray(ExtractValue(rigidBodyJson, "size"));
    std::vector<float> offset = ParseFloatArray(ExtractValue(rigidBodyJson, "offset"));
    
    std::string massStr = ExtractValue(rigidBodyJson, "mass");
    float mass = 1.0f;
    if (!massStr.empty()) {
        try {
            mass = std::stof(massStr);
        } catch (...) {
            mass = 1.0f;
        }
    }
    
    std::string restitutionStr = ExtractValue(rigidBodyJson, "restitution");
    float restitution = 0.5f;
    if (!restitutionStr.empty()) {
        try {
            restitution = std::stof(restitutionStr);
        } catch (...) {
            restitution = 0.5f;
        }
    }
    
    bool useGravity = ExtractBoolValue(rigidBodyJson, "useGravity");
    bool isTrigger = ExtractBoolValue(rigidBodyJson, "isTrigger");
    bool useOBB = ExtractBoolValue(rigidBodyJson, "useOBB");
    bool syncWithModel = ExtractBoolValue(rigidBodyJson, "syncWithModel");
    std::string autoFitToModelStr = ExtractValue(rigidBodyJson, "autoFitToModel");
    bool autoFitToModel = !autoFitToModelStr.empty() && ExtractBoolValue(rigidBodyJson, "autoFitToModel");

    std::string collisionModelPath = ExtractValue(rigidBodyJson, "collisionModelPath");
    if (!collisionModelPath.empty() && collisionModelPath.front() == '"' && collisionModelPath.back() == '"') {
        collisionModelPath = UnescapeString(collisionModelPath.substr(1, collisionModelPath.size() - 2));
    }

    std::string collisionPrecisionStr = ExtractValue(rigidBodyJson, "collisionPrecision");
    float collisionPrecision = 0.01f;
    if (!collisionPrecisionStr.empty()) {
        try {
            collisionPrecision = std::stof(collisionPrecisionStr);
        } catch (...) {
            collisionPrecision = 0.01f;
        }
    }

    std::string useConvexHullStr = ExtractValue(rigidBodyJson, "useConvexHull");
    bool useConvexHull = useConvexHullStr.empty() ? true : ExtractBoolValue(rigidBodyJson, "useConvexHull");

    std::string maxConvexHullVerticesStr = ExtractValue(rigidBodyJson, "maxConvexHullVertices");
    int maxConvexHullVertices = 256;
    if (!maxConvexHullVerticesStr.empty()) {
        try {
            maxConvexHullVertices = std::stoi(maxConvexHullVerticesStr);
        } catch (...) {
            maxConvexHullVertices = 256;
        }
    }

    std::string generatePerSubmeshStr = ExtractValue(rigidBodyJson, "generatePerSubmesh");
    bool generatePerSubmesh = !generatePerSubmeshStr.empty() &&
                              ExtractBoolValue(rigidBodyJson, "generatePerSubmesh");
    
    auto& coordinator = Coordinator::GetInstance();
    ECS::RigidBodyComponent rigidBody;
    rigidBody.type = (ECS::RigidBodyComponent::Type)type;
    rigidBody.shapeType = (ECS::RigidBodyComponent::ShapeType)shapeType;
    if (size.size() == 3) {
        rigidBody.size = glm::vec3(size[0], size[1], size[2]);
    }
    if (offset.size() == 3) {
        rigidBody.offset = glm::vec3(offset[0], offset[1], offset[2]);
    }
    rigidBody.mass = mass;
    rigidBody.restitution = restitution;
    rigidBody.useGravity = useGravity;
    rigidBody.isTrigger = isTrigger;
    rigidBody.useOBB = useOBB;
    rigidBody.syncWithModel = syncWithModel;
    rigidBody.autoFitToModel = autoFitToModel;
    rigidBody.collisionModelPath = collisionModelPath;
    rigidBody.collisionPrecision = collisionPrecision;
    rigidBody.useConvexHull = useConvexHull;
    rigidBody.maxConvexHullVertices = maxConvexHullVertices;
    rigidBody.generatePerSubmesh = generatePerSubmesh;
    coordinator.AddComponent<ECS::RigidBodyComponent>(entity, rigidBody);
}

void SceneSerializer::DeserializeColliderComponent(Entity entity, const std::string& jsonString) {
    std::string colliderJson = ExtractValue(jsonString, "collider");
    
    std::string typeStr = ExtractValue(colliderJson, "type");
    int type = 0;
    if (!typeStr.empty()) {
        try {
            type = std::stoi(typeStr);
        } catch (...) {
            type = 0;
        }
    }
    
    std::vector<float> size = ParseFloatArray(ExtractValue(colliderJson, "size"));
    std::vector<float> offset = ParseFloatArray(ExtractValue(colliderJson, "offset"));
    
    bool isTrigger = ExtractBoolValue(colliderJson, "isTrigger");
    bool useOBB = ExtractBoolValue(colliderJson, "useOBB");
    bool syncWithModel = ExtractBoolValue(colliderJson, "syncWithModel");
    std::string autoFitToModelStr = ExtractValue(colliderJson, "autoFitToModel");
    bool autoFitToModel = !autoFitToModelStr.empty() && ExtractBoolValue(colliderJson, "autoFitToModel");
    std::string modelPath = UnescapeString(ExtractValue(colliderJson, "modelPath"));
    //
    //
    auto& coordinator = Coordinator::GetInstance();
    ECS::ColliderComponent collider;
    collider.type = (ECS::ColliderComponent::Type)type;
    if (size.size() == 3) {
        collider.size = glm::vec3(size[0], size[1], size[2]);
    }
    if (offset.size() == 3) {
        collider.offset = glm::vec3(offset[0], offset[1], offset[2]);
    }
    collider.isTrigger = isTrigger;
    collider.useOBB = useOBB;
    collider.syncWithModel = syncWithModel;
    collider.autoFitToModel = autoFitToModel;
    collider.modelPath = modelPath;
    coordinator.AddComponent<ECS::ColliderComponent>(entity, collider);
}

void SceneSerializer::DeserializeVoxModelComponent(Entity entity, const std::string& jsonString) {
    std::string voxModelJson = ExtractValue(jsonString, "voxModel");
    
    std::string voxPath = ExtractValue(voxModelJson, "voxPath");
    
    //
    if (!voxPath.empty() && voxPath.front() == '"' && voxPath.back() == '"') {
        voxPath = voxPath.substr(1, voxPath.size() - 2);
    }
    
    //
    std::string isStaticStr = ExtractValue(voxModelJson, "isStatic");
    bool isStatic = true;
    if (!isStaticStr.empty()) {
        isStatic = (isStaticStr == "true");
    }
    
    auto& coordinator = Coordinator::GetInstance();
    ECS::VoxModelComponent voxModel;
    voxModel.voxPath = voxPath;
    voxModel.isStatic = isStatic;
    coordinator.AddComponent<ECS::VoxModelComponent>(entity, std::move(voxModel));
    
    //
    if (!coordinator.HasComponent<RenderComponent>(entity)) {
        RenderComponent render;
        render.visible = true;
        coordinator.AddComponent<RenderComponent>(entity, render);
    }
}

    //


} // namespace ECS
