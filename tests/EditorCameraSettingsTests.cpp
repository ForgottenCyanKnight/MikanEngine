#include "Rendering/Camera.h"
#include "Core/SceneSerializer.h"
#include "ECS/SceneECS.h"
#include "json.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

extern MIKAN_API Camera g_Camera;
static void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    auto& scene = ECS::SceneECS::GetInstance();
    try {
        scene.Init();
        ECS::SceneSerializer serializer;
        g_Camera.Position = {1.0f, 2.0f, 3.0f};
        g_Camera.SetEditorParameters(3.5f, 72.0f, 2400.0f, 0.25f);
        const auto serialized = serializer.SerializeScene();
        const auto document = nlohmann::json::parse(serialized);
        Require(document["editorCamera"]["speed"] == 3.5f, "Speed not saved");
        Require(document["editorCamera"]["fov"] == 72.0f, "FOV not saved");
        Require(document["editorCamera"]["farPlane"] == 2400.0f, "Far plane not saved");
        Require(document["editorCamera"]["sensitivity"] == 0.25f, "Sensitivity not saved");
        g_Camera.SetEditorParameters(1.0f, 20.0f, 10.0f);
        Require(serializer.DeserializeScene(serialized), "Reload failed");
        Require(g_Camera.MovementSpeed == 3.5f && g_Camera.FieldOfView == 72.0f &&
                g_Camera.FarPlane == 2400.0f && g_Camera.MouseSensitivity == 0.25f, "Round trip changed parameters");
        const float previousYaw = g_Camera.Yaw;
        g_Camera.ProcessMouseMovement(4.0f, 0.0f);
        Require(std::abs(g_Camera.Yaw - previousYaw - 1.0f) < 0.0001f, "Mouse rotation ignores sensitivity");
        const auto oldScene = R"({"entities":[],"editorCamera":{"position":[0,0,0],"yaw":-90,"pitch":0}})";
        Require(serializer.DeserializeScene(oldScene), "Old scene failed");
        Require(g_Camera.MovementSpeed == Camera::SPEED && g_Camera.FieldOfView == Camera::DEFAULT_FOV &&
                g_Camera.FarPlane == Camera::DEFAULT_FAR_PLANE && g_Camera.MouseSensitivity == Camera::SENSITIVITY,
                "Old scene inherited previous settings");
        g_Camera.SetEditorParameters(-1.0f, 400.0f, -100.0f);
        Require(g_Camera.MovementSpeed > 0.0f && g_Camera.FieldOfView < 180.0f &&
                g_Camera.FarPlane > 0.1f, "Invalid projection range");
        g_Camera.SetEditorParameters(std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
        Require(std::isfinite(g_Camera.MovementSpeed) && std::isfinite(g_Camera.FieldOfView) &&
                std::isfinite(g_Camera.FarPlane), "Non-finite settings accepted");
        scene.Shutdown();
        std::cout << "PASS: editor camera settings/sensitivity round trip, mouse rotation, legacy defaults and bounds\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        scene.Shutdown();
        return 1;
    }
}
