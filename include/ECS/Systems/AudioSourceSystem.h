#pragma once
// AudioSourceSystem.h - 音频源系统（类似 Unity AudioSource）
// 每帧驱动场景树中挂 AudioSourceComponent 的实体：
//   playOnAwake 自动播放 / playRequested & stopRequested 控制 / clip·volume·loop 变化实时应用。
#include "Platform/Export.h"
#include "ECS/Types.h"

namespace ECS {

class MIKAN_API AudioSourceSystem {
public:
    static AudioSourceSystem& GetInstance();

    void Update(float deltaTime);  // 引擎每帧调用

private:
    AudioSourceSystem() = default;
    ~AudioSourceSystem() = default;
    AudioSourceSystem(const AudioSourceSystem&) = delete;
    AudioSourceSystem& operator=(const AudioSourceSystem&) = delete;

    void VisitEntity(Entity e);
    static std::string TrimExt(const std::string& path);  // "bgm.wav" → "bgm"
};

} // namespace ECS
