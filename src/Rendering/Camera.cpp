#include "Camera.h"
#include "ECS/ECS.h"
#include "ECS/SceneECS.h"
#include <glm/gtc/matrix_transform.hpp>
#include <fstream>
#include <iomanip>
#include <cmath>

// 外部声明相机锁定的实体
extern ECS::Entity cameraLockedEntity;

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
    
    Position = newPosition;
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
