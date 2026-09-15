#include "SnakeGame.h"

extern "C" __declspec(dllexport) const char* GetGameModuleName() {
    return "snake";
}

extern "C" __declspec(dllexport) Game::IGameModule* CreateGameModule() {
    return &Game::SnakeGame::GetInstance();
}
