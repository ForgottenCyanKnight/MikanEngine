#pragma once
#include "Platform/Export.h"
#include <glm/glm.hpp>
#include <string>

class MIKAN_API Camera {
public:
    static const float SENSITIVITY;
    static const float SPEED;
    static constexpr float DEFAULT_FOV = 45.0f;
    static constexpr float DEFAULT_FAR_PLANE = 500.0f;

    glm::vec3 Position;
    glm::vec3 Front;
    glm::vec3 Up;
    glm::vec3 Right;
    glm::vec3 WorldUp;
    float Yaw;
    float Pitch;
    float MovementSpeed;
    float MouseSensitivity;
    float FieldOfView = DEFAULT_FOV;
    float FarPlane = DEFAULT_FAR_PLANE;

    // Editor navigation/projection settings; independent of ECS game cameras.
    void SetEditorParameters(float speed, float fov, float farPlane, float sensitivity = SENSITIVITY);

    Camera(glm::vec3 position = glm::vec3(0.0f, 0.0f, 3.0f));

    glm::mat4 GetViewMatrix() const;
    void ProcessMouseMovement(float xoffset, float yoffset);
    void ProcessKeyboard(int direction, float deltaTime);
    void UpdateCameraVectors();
    
    void SaveState(const std::string& path) const;
    bool LoadState(const std::string& path);
    void Update(float deltaTime);
};
