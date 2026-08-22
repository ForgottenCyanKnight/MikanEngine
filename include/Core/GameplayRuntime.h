#pragma once

#include "Platform/Export.h"
#include <string>

namespace Core {

// Renderer-independent gameplay runtime used by MikanTestRunner.
// It owns ECS/scene, Jolt, Box2D, scripts and game-plugin lifecycle only.
class MIKAN_API GameplayRuntime {
public:
    GameplayRuntime() = default;
    ~GameplayRuntime();

    GameplayRuntime(const GameplayRuntime&) = delete;
    GameplayRuntime& operator=(const GameplayRuntime&) = delete;

    bool Initialize();
    bool LoadScene(const std::string& scenePath, const std::string& requestedGame);
    void Tick(float deltaTime);
    void Shutdown();

    bool IsInitialized() const { return m_initialized; }
    const std::string& GetActiveGame() const { return m_activeGame; }

    static bool DumpState(const std::string& path, int frames, const char* runtimeLayer, float fps = 0.0f);

private:
    bool m_initialized = false;
    bool m_gameStarted = false;
    std::string m_activeGame;
};

} // namespace Core

extern "C" MIKAN_API int MikanGameplayTestMain(int argc, char* argv[]);
