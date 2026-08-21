// GameSnakeExports.cpp - 贪吃蛇插件导出(引擎 GameManager::LoadPlugin 约定)
#include "SnakeGame.h"

extern "C" __declspec(dllexport) const char* GetGameModuleName() {
    return "snake"; // 与 assets/snake.json 顶层 "game" 键一致
}

extern "C" __declspec(dllexport) Game::IGameModule* CreateGameModule() {
    return &Game::SnakeGame::GetInstance();
}
