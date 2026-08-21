#include "Camera.h"
#include "EngineGlobal.h"
#include "SceneRenderer.h"
#include "ECS/ECS.h"
#include "ECS/SceneECS.h"
#include "World/WorldGlobals.h"
#include "World/Chunk.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <fstream>
#include <iomanip>
#include <cmath>

// 外部声明相机锁定的实体
extern ECS::Entity cameraLockedEntity;

// 外部声明
extern SceneRenderer g_SceneRenderer;

const float Camera::SENSITIVITY = 0.1f;
const float Camera::SPEED = 20.0f;

Camera::Camera(glm::vec3 position)
    : Position(position),
    WorldUp(glm::vec3(0.0f, 1.0f, 0.0f)),
    Yaw(-90.0f),
    Pitch(0.0f),
    MovementSpeed(SPEED),
    MouseSensitivity(SENSITIVITY) {
    UpdateCameraVectors();
}

glm::mat4 Camera::GetViewMatrix() const {
    return glm::lookAt(Position, Position + Front, Up);
}

void Camera::ProcessMouseMovement(float xoffset, float yoffset) {
    xoffset *= MouseSensitivity;
    yoffset *= MouseSensitivity;

    Yaw += xoffset;
    Pitch += yoffset;

    if (Pitch > 89.0f) Pitch = 89.0f;
    if (Pitch < -89.0f) Pitch = -89.0f;

    if (Yaw > 180.0f) Yaw -= 360.0f;
    if (Yaw < -180.0f) Yaw += 360.0f;

    UpdateCameraVectors();
}

void Camera::ProcessKeyboard(int direction, float deltaTime) {
    float velocity = MovementSpeed * deltaTime;
    glm::vec3 newPosition = Position;

    switch (direction) {
    case 0:
        newPosition += Front * velocity;
        break;
    case 1:
        newPosition -= Front * velocity;
        break;
    case 2:
        newPosition -= Right * velocity;
        break;
    case 3:
        newPosition += Right * velocity;
        break;
    case 4:
        newPosition += WorldUp * velocity;
        break;
    case 5:
        newPosition -= WorldUp * velocity;
        break;
    }
    
    // 检查碰撞（如果相机碰撞已启用）
    if (!g_CameraCollisionEnabled || !CheckCollision(newPosition)) {
        Position = newPosition;
    }
}

void Camera::UpdateCameraVectors() {
    glm::vec3 front;
    front.x = cos(glm::radians(Yaw)) * cos(glm::radians(Pitch));
    front.y = sin(glm::radians(Pitch));
    front.z = sin(glm::radians(Yaw)) * cos(glm::radians(Pitch));
    Front = glm::normalize(front);

    Right = glm::normalize(glm::cross(Front, WorldUp));
    Up = glm::normalize(glm::cross(Right, Front));
}

bool Camera::CheckCollision(const glm::vec3& newPosition) const {
    // 相机碰撞盒大小
    const glm::vec3 cameraHalfExtents(0.2f, 0.2f, 0.2f);
    
    // 获取场景中的所有模型实体
    ECS::SceneECS& sceneECS = ECS::SceneECS::GetInstance();
    ECS::Coordinator& coordinator = ECS::Coordinator::GetInstance();
    std::vector<ECS::Entity> rootEntities = sceneECS.GetRootEntities();
    
    bool collision = false;
    
    // 遍历所有实体
    std::function<void(ECS::Entity)> checkEntity = [&](ECS::Entity entity) {
        if (collision) return;
        
        if (coordinator.HasComponent<ECS::MeshComponent>(entity) && coordinator.HasComponent<ECS::TransformComponent>(entity)) {
            ECS::MeshComponent& mesh = coordinator.GetComponent<ECS::MeshComponent>(entity);
            ECS::TransformComponent& transform = coordinator.GetComponent<ECS::TransformComponent>(entity);
            
            if ((mesh.type == ECS::MeshType::Model || mesh.type == ECS::MeshType::Plane) && !mesh.modelPath.empty()) {
                // 获取模型渲染器
                ModelRenderer* renderer = g_SceneRenderer.GetModelRenderer(mesh.modelPath);
                if (renderer && renderer->HasModelLoaded()) {
                    // 获取submesh的AABB列表
                    std::vector<AABB> subMeshAABBs = renderer->GetSubMeshAABBs();
                    
                    // 应用变换矩阵
                    glm::mat4 modelMatrix = transform.GetModelMatrix();
                    
                    // 检查相机与每个submesh的AABB的碰撞
                    for (const auto& localAABB : subMeshAABBs) {
                        AABB worldAABB = localAABB.Transform(modelMatrix);
                        
                        // 检查相机与AABB的碰撞
                        if (worldAABB.Contains(newPosition)) {
                            collision = true;
                            return;
                        }
                    }
                }
            }
        }
        
        // 检查子实体
        std::vector<ECS::Entity> children = sceneECS.GetChildren(entity);
        for (size_t i = 0; i < children.size() && !collision; i++) {
            checkEntity(children[i]);
        }
    };
    
    // 检查所有根实体
    for (size_t i = 0; i < rootEntities.size() && !collision; i++) {
        checkEntity(rootEntities[i]);
    }
    
    // 体素世界方块碰撞（从 OpenGL 版保留：相机 AABB 与实体方块重叠即碰撞）
    if (!collision && g_World != nullptr) {
        const glm::vec3 minCorner = newPosition - cameraHalfExtents;
        const glm::vec3 maxCorner = newPosition + cameraHalfExtents;
        constexpr int HEIGHT = Chunk::HEIGHT;

        for (int bx = (int)std::floor(minCorner.x); bx <= (int)std::floor(maxCorner.x) && !collision; bx++) {
            for (int bz = (int)std::floor(minCorner.z); bz <= (int)std::floor(maxCorner.z) && !collision; bz++) {
                for (int by = (int)std::floor(minCorner.y); by <= (int)std::floor(maxCorner.y) && !collision; by++) {
                    if (by < 0 || by >= HEIGHT) continue;
                    // 线程安全查询（World 内部锁保护，避免与网格 worker 线程写回竞争）
                    if (g_World->GetBlockAt(bx, by, bz) != 0) collision = true; // 非空气即阻挡
                }
            }
        }
    }
    
    return collision;
}

void Camera::SaveState(const std::string& path) const {
    std::ofstream file(path);
    if (file.is_open()) {
        file << std::fixed << std::setprecision(6);
        file << Position.x << " " << Position.y << " " << Position.z << "\n";
        file << Yaw << " " << Pitch << "\n";
        file.close();
    }
}

bool Camera::LoadState(const std::string& path) {
    std::ifstream file(path);
    if (file.is_open()) {
        glm::vec3 pos;
        float yaw, pitch;
        if (file >> pos.x >> pos.y >> pos.z >> yaw >> pitch) {
            Position = pos;
            Yaw = yaw;
            Pitch = pitch;
            UpdateCameraVectors();
            return true;
        }
    }
    return false;
}

void Camera::Update(float deltaTime) {
    // 检查是否有锁定的实体
    if (cameraLockedEntity != ECS::INVALID_ENTITY) {
        auto& coordinator = ECS::Coordinator::GetInstance();
        if (coordinator.HasComponent<ECS::TransformComponent>(cameraLockedEntity)) {
            // 获取锁定实体的变换组件
            auto& transform = coordinator.GetComponent<ECS::TransformComponent>(cameraLockedEntity);
            
            // 设置固定的视线方向和距离
            glm::vec3 targetDirection = glm::vec3(0.0f, 0.0f, -1.0f); // 固定看向Z轴负方向
            float distance = 5.0f; // 固定距离
            
            // 计算相机位置：实体位置 + 方向向量 * 距离
            Position = transform.position + targetDirection * distance;
            
            // 更新相机的前向向量，使其看向实体
            Front = glm::normalize(transform.position - Position);
            
            // 更新相机的右向和上向向量
            Right = glm::normalize(glm::cross(Front, WorldUp));
            Up = glm::normalize(glm::cross(Right, Front));
        }
    }
}
