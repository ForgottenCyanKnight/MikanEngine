// AudioSourceSystem.cpp - 音频源系统（类似 Unity AudioSource）
// 场景树实体挂 AudioSourceComponent → 本系统每帧驱动：
//   - playOnAwake: 场景加载后首次 Update 自动播放一次
//   - playRequested/stopRequested: 插件置 true 触发（系统处理后复位）
//   - clip 变化: 重新 LoadAudio 并（若曾播放）续播
//   - volume/loop 变化: 实时应用（loop 变化需重启）
#include "ECS/Systems/AudioSourceSystem.h"
#include "ECS/Coordinator.h"
#include "ECS/Components.h"
#include "ECS/SceneECS.h"
#include "Core/AudioManager.h"
#include "Core/ProjectManager.h"

#include <string>

namespace ECS {

AudioSourceSystem& AudioSourceSystem::GetInstance() {
    static AudioSourceSystem instance;
    return instance;
}

std::string AudioSourceSystem::TrimExt(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    const size_t sep = path.find_last_of("/\\");
    return (dot != std::string::npos && (sep == std::string::npos || dot > sep)) ? path.substr(0, dot) : path;
}

void AudioSourceSystem::Update(float) {
    auto& sceneECS = SceneECS::GetInstance();
    for (const auto& root : sceneECS.GetRootEntities()) {
        VisitEntity(root);
    }
}

void AudioSourceSystem::VisitEntity(Entity e) {
    auto& coordinator = Coordinator::GetInstance();
    if (coordinator.HasComponent<AudioSourceComponent>(e)) {
        auto& as = coordinator.GetComponent<AudioSourceComponent>(e);
        auto& am = AudioManager::GetInstance();

        // clip 变化 → 重载音频（先停旧名，防同文件残留实例）
        if (as.clip != as.lastClip) {
            if (!as.lastClip.empty()) am.StopAudio(TrimExt(as.lastClip));
            if (!as.clip.empty()) {
                am.LoadAudio(TrimExt(as.clip),
                             ProjectManager::GetInstance().ResolveAssetPath("assets/audio/" + as.clip));
            }
            as.lastClip = as.clip;
        }

        if (!as.clip.empty()) {
            const std::string name = TrimExt(as.clip);
            // playOnAwake: 首次自动播放（仅一次）
            if (!as.awakeStarted && as.playOnAwake) {
                am.PlayAudio(name, as.volume, as.loop);
                as.awakeStarted = true;
            }
            // 手动触发播放（插件置 playRequested=true；同时标记 awakeStarted 防重复自动播）
            if (as.playRequested) {
                am.PlayAudio(name, as.volume, as.loop);
                as.awakeStarted = true;
                as.playRequested = false;
            }
            // 手动停止
            if (as.stopRequested) {
                am.StopAudio(name);
                as.stopRequested = false;
            }
            // volume/loop 变化实时应用（loop 变化需停止重播）
            if (as.lastVolume != as.volume || as.lastLoop != as.loop) {
                if (am.IsPlaying(name)) {
                    am.SetVolume(name, as.volume);
                    if (as.lastLoop != as.loop) {
                        am.StopAudio(name);
                        am.PlayAudio(name, as.volume, as.loop);
                    }
                }
                as.lastVolume = as.volume;
                as.lastLoop = as.loop;
            }
            // 状态同步（供插件/编辑器读取）
            as.playing = am.IsPlaying(name);
        }
    }
    for (const auto& child : SceneECS::GetInstance().GetChildren(e)) {
        VisitEntity(child);
    }
}

} // namespace ECS
