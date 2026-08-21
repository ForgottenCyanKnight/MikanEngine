#pragma once
#include "Platform/Export.h"
#include <glm/glm.hpp>
#include <string>

class MIKAN_API Camera {
public:
    static const float SENSITIVITY;
    static const float SPEED;

    glm::vec3 Position;
    glm::vec3 Front;
    glm::vec3 Up;
    glm::vec3 Right;
    glm::vec3 WorldUp;
    float Yaw;
    float Pitch;
    float MovementSpeed;
    float MouseSensitivity;

    Camera(glm::vec3 position = glm::vec3(0.0f, 0.0f, 3.0f));

    glm::mat4 GetViewMatrix() const;
    void ProcessMouseMovement(float xoffset, float yoffset);
    void ProcessKeyboard(int direction, float deltaTime);
    void UpdateCameraVectors();
    
    // 碰撞检测相关
    bool CheckCollision(const glm::vec3& newPosition) const;
    void SaveState(const std::string& path) const;
    bool LoadState(const std::string& path);
    void Update(float deltaTime);
};
