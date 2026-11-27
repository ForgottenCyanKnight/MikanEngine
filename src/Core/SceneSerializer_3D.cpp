// SceneSerializer_3D.cpp - 3D 组件的场景序列化（自 SceneSerializer.cpp 拆分，函数体一字未改）
// 与 SceneSerializer_2D.cpp 同属 SceneSerializer 类的成员函数定义（声明见 include/Core/SceneSerializer.h）。
// 拆分目的：改某类组件的序列化时只读对应文件，避免读 2000 行。
#include "Core/SceneSerializer.h"
#include "SceneJsonView.h"
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
    json << "        \"enableHiZCulling\": " << (component.enableHiZCulling ? "true" : "false") << "," << std::endl;
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
    json << "        \"angularVelocity\": [" << component.angularVelocity.x << ", "
         << component.angularVelocity.y << ", " << component.angularVelocity.z << "]," << std::endl;
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


void SceneSerializer::DeserializeMeshComponent(Entity entity, SceneJsonView json) {
    
    const auto meshJson = json.Find("mesh");
    
    std::string typeStr = meshJson.Token("type");
    
    std::string modelPath = meshJson.Token("modelPath");
    
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

void SceneSerializer::DeserializeRenderComponent(Entity entity, SceneJsonView json) {
    const auto renderJson = json.Find("render");
    
    bool visible = renderJson.Boolean("visible");
    bool castShadow = renderJson.Boolean("castShadow");
    bool receiveShadow = renderJson.Boolean("receiveShadow");
    bool showAABB = renderJson.Boolean("showAABB");
    bool showOBB = renderJson.Boolean("showOBB");
    
    auto& coordinator = Coordinator::GetInstance();
    RenderComponent render;
    render.visible = visible;
    render.castShadow = castShadow;
    render.receiveShadow = receiveShadow;
    render.showAABB = showAABB;
    render.showOBB = showOBB;
    coordinator.AddComponent<RenderComponent>(entity, render);
}

void SceneSerializer::DeserializeCameraComponent(Entity entity, SceneJsonView json) {

    const auto cameraJson = json.Find("camera");
    
    std::string fovStr = cameraJson.Token("fov");
    float fov = 60.0f;
    if (!fovStr.empty()) {
        try {
            fov = std::stof(fovStr);
        } catch (...) {
            fov = 60.0f;
        }
    }
    
    std::string nearPlaneStr = cameraJson.Token("nearPlane");
    float nearPlane = 0.1f;
    if (!nearPlaneStr.empty()) {
        try {
            nearPlane = std::stof(nearPlaneStr);
        } catch (...) {
            nearPlane = 0.1f;
        }
    }
    
    std::string farPlaneStr = cameraJson.Token("farPlane");
    float farPlane = 1000.0f;
    if (!farPlaneStr.empty()) {
        try {
            farPlane = std::stof(farPlaneStr);
        } catch (...) {
            farPlane = 1000.0f;
        }
    }
    
    bool isMainCamera = cameraJson.Boolean("isMainCamera");
    bool isOrthographic = cameraJson.Boolean("isOrthographic");
    
    std::string orthographicSizeStr = cameraJson.Token("orthographicSize");
    float orthographicSize = 5.0f;
    if (!orthographicSizeStr.empty()) {
        try {
            orthographicSize = std::stof(orthographicSizeStr);
        } catch (...) {
            orthographicSize = 5.0f;
        }
    }
    
    //
    std::string enableFrustumCullingStr = cameraJson.Token("enableFrustumCulling");
    bool enableFrustumCulling = enableFrustumCullingStr.empty() ? true : cameraJson.Boolean("enableFrustumCulling");
    
    std::string showFrustumWireframeStr = cameraJson.Token("showFrustumWireframe");
    bool showFrustumWireframe = showFrustumWireframeStr.empty() ? true : cameraJson.Boolean("showFrustumWireframe");
    
    //
    std::string useSubMeshCullingStr = cameraJson.Token("useSubMeshCulling");
    bool useSubMeshCulling = useSubMeshCullingStr.empty() ? false : cameraJson.Boolean("useSubMeshCulling");
    
    //
    std::string showBVHWireframeStr = cameraJson.Token("showBVHWireframe");
    bool showBVHWireframe = showBVHWireframeStr.empty() ? false : cameraJson.Boolean("showBVHWireframe");

    // 兼容上一版错误命名的相机专属开关：如果旧存档中存在它，迁移为全局碰撞体显示。
    std::string showCollisionWireframeStr = cameraJson.Token("showCollisionWireframe");
    const bool hasCurrentCollisionWireframe = !showCollisionWireframeStr.empty();
    if (!hasCurrentCollisionWireframe) {
        showCollisionWireframeStr = cameraJson.Token("thirdPersonShowCollisionWireframe");
    }
    const bool showCollisionWireframe = showCollisionWireframeStr.empty()
        ? false
        : cameraJson.Boolean(hasCurrentCollisionWireframe
            ? "showCollisionWireframe" : "thirdPersonShowCollisionWireframe");

    std::string useBVHCullingStr = cameraJson.Token("useBVHCulling");
    bool useBVHCulling = useBVHCullingStr.empty() ? false : cameraJson.Boolean("useBVHCulling");

    bool thirdPersonEnabled = cameraJson.Boolean("thirdPersonEnabled");
    std::string thirdPersonTargetName = cameraJson.String("thirdPersonTargetName");
    std::vector<float> thirdPersonTargetOffset = cameraJson.FloatArray("thirdPersonTargetOffset");

    auto readCameraFloat = [&](const char* key, float fallback) {
        const std::string value = cameraJson.Token(key);
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
    const std::string hiZValue = cameraJson.Token("enableHiZCulling");
    camera.enableHiZCulling = hiZValue.empty() ? true : cameraJson.Boolean("enableHiZCulling");
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
    std::string thirdPersonCaptureMouseStr = cameraJson.Token("thirdPersonCaptureMouse");
    camera.thirdPersonCaptureMouse = thirdPersonCaptureMouseStr.empty()
        ? camera.thirdPersonCaptureMouse : cameraJson.Boolean("thirdPersonCaptureMouse");
    std::string thirdPersonPreserveDistanceWhenOccludedStr = cameraJson.Token("thirdPersonPreserveDistanceWhenOccluded");
    camera.thirdPersonPreserveDistanceWhenOccluded = thirdPersonPreserveDistanceWhenOccludedStr.empty()
        ? camera.thirdPersonPreserveDistanceWhenOccluded : cameraJson.Boolean("thirdPersonPreserveDistanceWhenOccluded");
    std::string thirdPersonCollisionEnabledStr = cameraJson.Token("thirdPersonCollisionEnabled");
    camera.thirdPersonCollisionEnabled = thirdPersonCollisionEnabledStr.empty()
        ? camera.thirdPersonCollisionEnabled : cameraJson.Boolean("thirdPersonCollisionEnabled");
    camera.thirdPersonCollisionRadius = readCameraFloat("thirdPersonCollisionRadius", camera.thirdPersonCollisionRadius);
    camera.thirdPersonCollisionBuffer = readCameraFloat("thirdPersonCollisionBuffer", camera.thirdPersonCollisionBuffer);
    camera.thirdPersonCollisionMinDistance = readCameraFloat("thirdPersonCollisionMinDistance", camera.thirdPersonCollisionMinDistance);
    camera.thirdPersonCollisionDampingIn = readCameraFloat("thirdPersonCollisionDampingIn", camera.thirdPersonCollisionDampingIn);
    camera.thirdPersonCollisionDampingOut = readCameraFloat("thirdPersonCollisionDampingOut", camera.thirdPersonCollisionDampingOut);
    camera.thirdPersonCollisionSmoothingTime = readCameraFloat("thirdPersonCollisionSmoothingTime", camera.thirdPersonCollisionSmoothingTime);
    camera.thirdPersonAimEnabled = cameraJson.Boolean("thirdPersonAimEnabled");
    camera.thirdPersonAimShoulderOffset = readCameraFloat("thirdPersonAimShoulderOffset", camera.thirdPersonAimShoulderOffset);
    camera.thirdPersonAimFov = readCameraFloat("thirdPersonAimFov", camera.thirdPersonAimFov);
    camera.thirdPersonAimSensitivity = readCameraFloat("thirdPersonAimSensitivity", camera.thirdPersonAimSensitivity);
    camera.thirdPersonAimPositionDamping = readCameraFloat("thirdPersonAimPositionDamping", camera.thirdPersonAimPositionDamping);
    camera.thirdPersonAimRotationDamping = readCameraFloat("thirdPersonAimRotationDamping", camera.thirdPersonAimRotationDamping);
    camera.thirdPersonLockOnEnabled = cameraJson.Boolean("thirdPersonLockOnEnabled");
    camera.thirdPersonLockTargetName = cameraJson.String("thirdPersonLockTargetName");
    camera.thirdPersonLockOnMaxDistance = readCameraFloat("thirdPersonLockOnMaxDistance", camera.thirdPersonLockOnMaxDistance);
    camera.thirdPersonLockOnLookAtBlend = readCameraFloat("thirdPersonLockOnLookAtBlend", camera.thirdPersonLockOnLookAtBlend);
    coordinator.AddComponent<CameraComponent>(entity, camera);
}

void SceneSerializer::DeserializeLightComponent(Entity entity, SceneJsonView json) {

    const auto lightJson = json.Find("light");
    
    std::string typeStr = lightJson.Token("type");
    int type = 0;
    if (!typeStr.empty()) {
        try {
            type = std::stoi(typeStr);
        } catch (...) {
            type = 0;
        }
    }
    
    std::vector<float> color = lightJson.FloatArray("color");
    
    std::string intensityStr = lightJson.Token("intensity");
    float intensity = 1.0f;
    if (!intensityStr.empty()) {
        try {
            intensity = std::stof(intensityStr);
        } catch (...) {
            intensity = 1.0f;
        }
    }
    
    std::string rangeStr = lightJson.Token("range");
    float range = 10.0f;
    if (!rangeStr.empty()) {
        try {
            range = std::stof(rangeStr);
        } catch (...) {
            range = 10.0f;
        }
    }
    
    std::string spotAngleStr = lightJson.Token("spotAngle");
    float spotAngle = 45.0f;
    if (!spotAngleStr.empty()) {
        try {
            spotAngle = std::stof(spotAngleStr);
        } catch (...) {
            spotAngle = 45.0f;
        }
    }
    
    std::string castShadowStr = lightJson.Token("castShadow");
    bool castShadow = castShadowStr == "true";
    
    auto& coordinator = Coordinator::GetInstance();
    LightComponent light;
    light.type = (LightComponent::Type)type;
    light.castShadow = castShadow;
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

void SceneSerializer::DeserializeMaterialComponent(Entity entity, SceneJsonView json)
{
    const auto fields = json.Find("material");
    MaterialComponent material;
    material.albedoPath = fields.String("albedoPath");
    material.normalPath = fields.String("normalPath");
    material.roughnessPath = fields.String("roughnessPath");
    material.metallicPath = fields.String("metallicPath");
    material.aoPath = fields.String("aoPath");
    material.emissivePath = fields.String("emissivePath");

    const auto albedoColor = fields.FloatArray("albedoColor");
    if (albedoColor.size() == 3) material.albedoColor = {albedoColor[0], albedoColor[1], albedoColor[2]};
    material.metallic = fields.Number("metallic", 0.0f);
    material.roughness = fields.Number("roughness", 0.5f);
    material.ao = fields.Number("ao", 1.0f);
    material.emissiveIntensity = fields.Number("emissiveIntensity", 0.0f);
    material.useAlbedoTexture = fields.Boolean("useAlbedoTexture");
    material.useNormalTexture = fields.Boolean("useNormalTexture");
    material.useRoughnessTexture = fields.Boolean("useRoughnessTexture");
    material.useMetallicTexture = fields.Boolean("useMetallicTexture");
    material.useAOTexture = fields.Boolean("useAOTexture");
    material.useEmissiveTexture = fields.Boolean("useEmissiveTexture");
    material.albedoSamplerType = fields.Number("albedoSamplerType", 0);
    material.normalSamplerType = fields.Number("normalSamplerType", 0);
    material.roughnessSamplerType = fields.Number("roughnessSamplerType", 0);
    material.metallicSamplerType = fields.Number("metallicSamplerType", 0);
    material.aoSamplerType = fields.Number("aoSamplerType", 0);
    material.emissiveSamplerType = fields.Number("emissiveSamplerType", 0);
    Coordinator::GetInstance().AddComponent(entity, std::move(material));
}

void SceneSerializer::DeserializeRigidBodyComponent(Entity entity, SceneJsonView json) {
    const auto rigidBodyJson = json.Find("rigidBody");
    
    std::string typeStr = rigidBodyJson.Token("type");
    int type = 0;
    if (!typeStr.empty()) {
        try {
            type = std::stoi(typeStr);
        } catch (...) {
            type = 0;
        }
    }
    
    std::string shapeTypeStr = rigidBodyJson.Token("shapeType");
    int shapeType = 0;
    if (!shapeTypeStr.empty()) {
        try {
            shapeType = std::stoi(shapeTypeStr);
        } catch (...) {
            shapeType = 0;
        }
    }
    
    std::vector<float> size = rigidBodyJson.FloatArray("size");
    std::vector<float> offset = rigidBodyJson.FloatArray("offset");
    
    std::string massStr = rigidBodyJson.Token("mass");
    float mass = 1.0f;
    if (!massStr.empty()) {
        try {
            mass = std::stof(massStr);
        } catch (...) {
            mass = 1.0f;
        }
    }
    
    std::string restitutionStr = rigidBodyJson.Token("restitution");
    float restitution = 0.5f;
    if (!restitutionStr.empty()) {
        try {
            restitution = std::stof(restitutionStr);
        } catch (...) {
            restitution = 0.5f;
        }
    }

    const std::vector<float> angularVelocity =
        rigidBodyJson.FloatArray("angularVelocity");

    bool useGravity = rigidBodyJson.Boolean("useGravity");
    bool isTrigger = rigidBodyJson.Boolean("isTrigger");
    bool useOBB = rigidBodyJson.Boolean("useOBB");
    bool syncWithModel = rigidBodyJson.Boolean("syncWithModel");
    std::string autoFitToModelStr = rigidBodyJson.Token("autoFitToModel");
    bool autoFitToModel = !autoFitToModelStr.empty() && rigidBodyJson.Boolean("autoFitToModel");

    std::string collisionModelPath = rigidBodyJson.String("collisionModelPath");

    std::string collisionPrecisionStr = rigidBodyJson.Token("collisionPrecision");
    float collisionPrecision = 0.01f;
    if (!collisionPrecisionStr.empty()) {
        try {
            collisionPrecision = std::stof(collisionPrecisionStr);
        } catch (...) {
            collisionPrecision = 0.01f;
        }
    }

    std::string useConvexHullStr = rigidBodyJson.Token("useConvexHull");
    bool useConvexHull = useConvexHullStr.empty() ? true : rigidBodyJson.Boolean("useConvexHull");

    std::string maxConvexHullVerticesStr = rigidBodyJson.Token("maxConvexHullVertices");
    int maxConvexHullVertices = 256;
    if (!maxConvexHullVerticesStr.empty()) {
        try {
            maxConvexHullVertices = std::stoi(maxConvexHullVerticesStr);
        } catch (...) {
            maxConvexHullVertices = 256;
        }
    }

    std::string generatePerSubmeshStr = rigidBodyJson.Token("generatePerSubmesh");
    bool generatePerSubmesh = !generatePerSubmeshStr.empty() &&
                              rigidBodyJson.Boolean("generatePerSubmesh");
    
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
    if (angularVelocity.size() == 3) {
        rigidBody.angularVelocity = glm::vec3(
            angularVelocity[0], angularVelocity[1], angularVelocity[2]);
    }
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

void SceneSerializer::DeserializeColliderComponent(Entity entity, SceneJsonView json) {
    const auto colliderJson = json.Find("collider");
    
    std::string typeStr = colliderJson.Token("type");
    int type = 0;
    if (!typeStr.empty()) {
        try {
            type = std::stoi(typeStr);
        } catch (...) {
            type = 0;
        }
    }
    
    std::vector<float> size = colliderJson.FloatArray("size");
    std::vector<float> offset = colliderJson.FloatArray("offset");
    
    bool isTrigger = colliderJson.Boolean("isTrigger");
    bool useOBB = colliderJson.Boolean("useOBB");
    bool syncWithModel = colliderJson.Boolean("syncWithModel");
    std::string autoFitToModelStr = colliderJson.Token("autoFitToModel");
    bool autoFitToModel = !autoFitToModelStr.empty() && colliderJson.Boolean("autoFitToModel");
    std::string modelPath = colliderJson.String("modelPath");
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

void SceneSerializer::DeserializeVoxModelComponent(Entity entity, SceneJsonView json) {
    const auto voxModelJson = json.Find("voxModel");
    
    std::string voxPath = voxModelJson.String("voxPath");
    std::string isStaticStr = voxModelJson.Token("isStatic");
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
