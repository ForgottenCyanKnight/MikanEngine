#pragma once

namespace Editor {

class MainMenuBar {
public:
    static MainMenuBar& GetInstance();

    void Render(bool& showSceneView, bool& showGameView, bool& showAssetsWindow, bool& showTilemapEditor, bool& layoutInitialized);

private:
    MainMenuBar() = default;
    ~MainMenuBar() = default;
    MainMenuBar(const MainMenuBar&) = delete;
    MainMenuBar& operator=(const MainMenuBar&) = delete;
};

} // namespace Editor