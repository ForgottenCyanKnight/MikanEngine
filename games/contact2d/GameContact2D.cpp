// GameContact2D.cpp - 2D 接触/触发事件演示插件
// 场景 assets/contact2d.json: 组件路径物理(rigidbody2d + collider2d),
// 动态 Box 受重力下落: 先穿过 Sensor(悬浮触发器: dynamic + 零重力, 保证 end 事件稳定),
// 再落到 Ground(碰撞 onEnter)。演示 Contact2DComponent 用法: 场景加载后给实体挂组件并
// 绑定回调, 回调在播放态物理结算后触发。传感器 end 事件高速分离时由引擎补偿(见 Physics2DSystem)。
#include "GameContact2D.h"
#include "ECS/SceneECS.h"
#include "ECS/Coordinator.h"
#include "ECS/Components.h"
#include "Core/SaveSystem.h"
#include "Core/Camera2DSystem.h"
#include "Core/ProjectManager.h"
#include "Core/PaletteManager.h"
#include "Core/SceneManager.h"
#include "Rendering/Renderer2D.h"
#include "AudioManager.h"
#include "SceneSerializer.h"
#include <json.hpp>
#include <fstream>
#include <iostream>

namespace {
// 实体显示名(NameComponent 缺失时退回 id)
std::string EntityName(ECS::Entity e) {
    auto& coordinator = ECS::Coordinator::GetInstance();
    if (coordinator.HasComponent<ECS::NameComponent>(e))
        return coordinator.GetComponent<ECS::NameComponent>(e).name;
    return "#" + std::to_string(e);
}
}

namespace Game {

GameContact2D& GameContact2D::GetInstance() {
    static GameContact2D instance;
    return instance;
}

void GameContact2D::OnSceneLoaded() {
    auto& sceneECS = ECS::SceneECS::GetInstance();
    auto& coordinator = ECS::Coordinator::GetInstance();
    m_box = sceneECS.FindByName("Box");
    m_sensor = sceneECS.FindByName("Sensor");
    if (m_box == ECS::INVALID_ENTITY || m_sensor == ECS::INVALID_ENTITY) {
        std::cerr << "[Contact2D] Box/Sensor not found in scene" << std::endl;
        return;
    }

    // 箱子: 碰撞 + 传感器重叠都监听(事件双方都会收到回调, 这里只看箱子侧)
    {
        ECS::Contact2DComponent c;
        c.onEnter = [](ECS::Entity other) {
            std::cout << "[Contact2D] Box 接触/进入 " << EntityName(other) << " (enter)" << std::endl;
            if (EntityName(other) == "Ground") {
                // 撞地音效(miniaudio); 不再触发屏幕震动(测试反馈落地后镜头抖动奇怪)
                AudioManager::GetInstance().PlayAudio("hit", 0.8f, false);
            }
        };
        c.onExit = [](ECS::Entity other) {
            std::cout << "[Contact2D] Box 离开 " << EntityName(other) << " (exit)" << std::endl;
        };
        coordinator.AddComponent<ECS::Contact2DComponent>(m_box, c);
    }
    // 传感器: 触发器视角监听进入/离开
    {
        ECS::Contact2DComponent c;
        c.onEnter = [](ECS::Entity other) {
            std::cout << "[Contact2D] Sensor 被 " << EntityName(other) << " 进入 (trigger enter)" << std::endl;
        };
        c.onExit = [](ECS::Entity other) {
            std::cout << "[Contact2D] Sensor 被 " << EntityName(other) << " 离开 (trigger exit)" << std::endl;
        };
        coordinator.AddComponent<ECS::Contact2DComponent>(m_sensor, c);
    }
    std::cout << "[Contact2D] 绑定完成: Box(碰撞) + Sensor(触发器), 按播放运行查看日志" << std::endl;

    // ===== 序5 智能相机字段验证(Cinemachine 字段反序列化 + 系统驱动) =====
    {
        ECS::Entity cam = sceneECS.FindByName("Camera2D");
        if (cam != ECS::INVALID_ENTITY && coordinator.HasComponent<ECS::Camera2DComponent>(cam)) {
            auto& cc = coordinator.GetComponent<ECS::Camera2DComponent>(cam);
            std::cout << "[Contact2D] Camera2D followTargetName='" << cc.followTargetName
                      << "' dampX=" << cc.dampX << " dampY=" << cc.dampY
                      << " (Cinemachine 字段已反序列化, 相机将跟随 Box)" << std::endl;
        }
    }

    // ===== 存档 API 演示(序4): 写→读→校验 往返 =====
    {
        nlohmann::json demo;
        demo["bestScore"] = 42;
        demo["unlocked"] = true;
        demo["lastLevel"] = "world1-3";
        const bool saved = Save::Save("contact2d_demo", demo.dump());
        bool roundtrip = false;
        if (saved) {
            auto loaded = Save::Load("contact2d_demo");
            roundtrip = loaded &&
                        nlohmann::json::parse(*loaded)["bestScore"] == 42 &&
                        nlohmann::json::parse(*loaded)["unlocked"] == true;
            if (!roundtrip) Save::Delete("contact2d_demo");
        }
        std::cout << "[Contact2D] SaveSystem 往返: " << (roundtrip ? "OK" : "FAILED")
                  << " (saves/contact2d_demo.json)" << std::endl;
    }

    // ===== 2D 场景序列化验证(完善 2D 序列化): 序列化当前场景, 检查 2D 组件键齐全 =====
    {
        const std::string scene = ECS::SceneSerializer().SerializeScene();
        auto hasKey = [&scene](const char* k) { return scene.find(k) != std::string::npos; };
        const bool ok = hasKey("\"camera2d\"") && hasKey("\"rigidbody2d\"") &&
                        hasKey("\"collider2d\"") && hasKey("\"sprite2d\"");
        std::cout << "[Contact2D] 2D 序列化键检查: " << (ok ? "OK" : "MISSING")
                  << " (camera2d/rigidbody2d/collider2d/sprite2d)" << std::endl;
        if (!ok) std::cerr << scene.substr(0, 400) << std::endl;
    }

    // ===== 音频(miniaudio 版): 验证 WAV/MP3/OGG 加载与播放 =====
    {
        auto& am = AudioManager::GetInstance();
        auto& pm = ProjectManager::GetInstance();
        const bool hitOk = am.LoadAudio("hit", pm.ResolveAssetPath("assets/audio/hit.wav"));
        const bool mp3Ok = am.LoadAudio("mp3test", pm.ResolveAssetPath("assets/audio/test.mp3"));
        const bool oggOk = am.LoadAudio("oggtest", pm.ResolveAssetPath("assets/audio/test.ogg"));
        const bool bgmOk = am.LoadAudio("bgm", pm.ResolveAssetPath("assets/audio/bgm.wav"));

        bool mp3Playing = false, oggPlaying = false;
        if (mp3Ok) { am.PlayAudio("mp3test", 0.4f, false); mp3Playing = am.IsPlaying("mp3test"); }
        if (oggOk) { am.PlayAudio("oggtest", 0.4f, false); oggPlaying = am.IsPlaying("oggtest"); }
        if (bgmOk) am.PlayAudio("bgm", 0.25f, true); // 循环 BGM
        std::cout << "[Contact2D] 音频: hit=" << hitOk
                  << " mp3=" << mp3Ok << (mp3Playing ? "(播放中)" : "(!)")
                  << " ogg=" << oggOk << (oggPlaying ? "(播放中)" : "(!)")
                  << " BGM循环=" << (bgmOk && am.IsPlaying("bgm") ? "true" : "false") << std::endl;
    }

    // ===== 精灵帧动画(序2): 加载网上素材 diamond.png, 从 diamond.json 读帧表 =====
    {
        auto& pm = ProjectManager::GetInstance();
        Renderer2D::GetInstance().LoadTexture("diamond", pm.ResolveAssetPath("assets/textures/diamond.png"));
        ECS::Entity anim = sceneECS.FindByName("Anim");
        if (anim != ECS::INVALID_ENTITY && coordinator.HasComponent<ECS::SpriteAnimationComponent>(anim)) {
            auto& sa = coordinator.GetComponent<ECS::SpriteAnimationComponent>(anim);
            std::ifstream ifs(pm.ResolveAssetPath("assets/textures/diamond.json"));
            if (ifs) {
                auto j = nlohmann::json::parse(ifs);
                auto& tex = j["textures"][0];
                sa.texWidth = tex["size"]["w"].get<int>();
                sa.texHeight = tex["size"]["h"].get<int>();
                for (auto& f : tex["frames"]) {
                    int x = f["frame"]["x"].get<int>();
                    int y = f["frame"]["y"].get<int>();
                    int w = f["frame"]["w"].get<int>();
                    int h = f["frame"]["h"].get<int>();
                    sa.frames.push_back(glm::ivec4(x, y, w, h));
                }
                std::cout << "[Contact2D] 帧动画: " << sa.frames.size() << " 帧 (diamond "
                          << sa.texWidth << "x" << sa.texHeight << ", fps=" << sa.fps << ")" << std::endl;
            } else {
                std::cerr << "[Contact2D] 无法读取 diamond.json" << std::endl;
            }
        }
    }

    // ===== 调色板交换(用户优先级): diamond 白色 → 金色变体, 供 "AnimGold" 精灵使用 =====
    {
        const bool palOk = PaletteManager::GetInstance().ApplyPalette(
            "assets/textures/diamond.png", "assets/palettes/gold.json", "diamond_gold");
        std::cout << "[Contact2D] 调色板交换: " << (palOk ? "OK" : "FAILED")
                  << " (diamond → diamond_gold, AnimGold 精灵使用)" << std::endl;
    }
}

void GameContact2D::OnUpdate(float deltaTime) {    // 序6 验证: 每 2 秒重播一次 hit(旧版"只能响一次"; 现在每次 PlayAudio 都有新数据入队)
    m_audioTimer += deltaTime;
    if (m_audioTimer >= 2.0f) {
        m_audioTimer = 0.0f;
        static int repCount = 0;
        AudioManager::GetInstance().PlayAudio("hit", 0.7f, false);
        ++repCount;
        // 立即查询(同帧): 播放入队后应 IsPlaying=true; 2 秒后旧实例早已播完(0.18s)
        const bool hitPlaying = AudioManager::GetInstance().IsPlaying("hit");
        const bool bgmPlaying = AudioManager::GetInstance().IsPlaying("bgm");
        std::cout << "[Contact2D] PlayAudio(hit) 第 " << repCount << " 次: "
                  << (hitPlaying ? "IsPlaying=true(入队成功)" : "IsPlaying=false(!)")
                  << ", BGM循环=" << (bgmPlaying ? "true" : "false") << std::endl;
    }

    // 序2 验证: 每 1 秒打印一次动画帧号与 UV(确认帧在推进)
    m_animTimer += deltaTime;
    if (m_animTimer >= 1.0f) {
        m_animTimer = 0.0f;
        auto& coordinator = ECS::Coordinator::GetInstance();
        ECS::Entity anim = ECS::SceneECS::GetInstance().FindByName("Anim");
        if (anim != ECS::INVALID_ENTITY &&
            coordinator.HasComponent<ECS::SpriteAnimationComponent>(anim)) {
            auto& sa = coordinator.GetComponent<ECS::SpriteAnimationComponent>(anim);
            auto& spr = coordinator.GetComponent<ECS::Sprite2DComponent>(anim);
            std::cout << "[Contact2D] 动画帧 " << sa.currentFrame << "/" << sa.frames.size()
                      << " uv0=(" << spr.uv0.x << "," << spr.uv0.y << ")"
                      << " uv1=(" << spr.uv1.x << "," << spr.uv1.y << ")" << std::endl;
        }
    }
}

void GameContact2D::OnKey(SDL_Keycode key) {
    // 场景切换演示(游戏事件触发): 按 N 在 contact2d / level2 之间切换
    if (key == SDLK_N) {
        const std::string& cur = SceneManager::GetInstance().GetCurrentScene();
        const bool inLevel2 = cur.find("level2") != std::string::npos;
        const std::string next = inLevel2 ? "assets/contact2d.json" : "assets/level2.json";
        std::cout << "[Contact2D] 按 N 切换场景 -> " << next << std::endl;
        SceneManager::GetInstance().ChangeScene(next);
    }
}

} // namespace Game

// ===== 插件导出(引擎 GameManager::LoadPlugin 约定)=====
extern "C" __declspec(dllexport) const char* GetGameModuleName() {
    return "contact2d";
}

extern "C" __declspec(dllexport) Game::IGameModule* CreateGameModule() {
    return &Game::GameContact2D::GetInstance();
}
