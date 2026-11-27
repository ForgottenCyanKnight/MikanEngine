#include "PixelHopGame.h"

#if defined(__ANDROID__) || defined(MIKAN_OPENHARMONY)
#include "Game/GameManager.h"
namespace {
const bool registered = [] {
    Game::GameManager::GetInstance().Register(
        "pixel-hop-twins", [] { return &Game::PixelHopGame::GetInstance(); });
    return true;
}();
}
#else
#include "Game/PluginExport.h"
extern "C" MIKAN_GAME_MODULE_EXPORT const char* GetGameModuleName() {
    return "pixel-hop-twins";
}
extern "C" MIKAN_GAME_MODULE_EXPORT Game::IGameModule* CreateGameModule() {
    return &Game::PixelHopGame::GetInstance();
}
#endif
