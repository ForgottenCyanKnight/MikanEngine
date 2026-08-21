#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H
#include "Platform/Export.h"

#include <string>
#include <unordered_map>
#include <vector>

// 音频管理器(miniaudio 版): 接口与 SDL3 版保持兼容, 内部换成 miniaudio(单头文件库)。
// 能力: WAV / MP3 / OGG / FLAC 解码(内置), 每音效多实例池(默认 4, 连发音效可叠加),
//       PlayAudio 重播(seek 回开头), 音量实时生效, loop 无缝循环。
// 播放后端: miniaudio 默认 WASAPI(Windows), 编译实现见 AudioManager.cpp(MA_NO_DSOUND/MA_NO_WINMM)。
// 注意: 音频解码为内存式(MA_SOUND_FLAG_DECODE), 超大 BGM 如需流式可去掉该 flag(见 LoadAudio)。

typedef struct ma_engine ma_engine;
typedef struct ma_sound ma_sound;

class MIKAN_API AudioManager {
private:
    struct AudioEntry {
        std::vector<ma_sound*> slots;     // 实例池(每槽一个独立解码的 ma_sound)
        float defaultGain = 1.0f;         // SetVolume 记录的默认增益
        size_t nextSlot = 0;              // round-robin 分配游标
    };

    static AudioManager* instance;
    ma_engine* m_engine = nullptr;
    std::unordered_map<std::string, AudioEntry> m_audio;

    static constexpr int kSlotsPerAudio = 4; // 每音效实例数(连发叠加上限)

    AudioManager();
    ~AudioManager();

public:
    static AudioManager& GetInstance();
    static void DestroyInstance();

    bool Initialize();
    void Shutdown();

    // 加载音频(WAV/MP3/OGG/FLAC)并预创建实例池; 同名重载会替换旧音效
    bool LoadAudio(const std::string& name, const std::string& filePath);
    // 播放: volume 0-1(实时生效), loop=true 无缝循环; 多实例自动分配, 全忙时替换最旧
    bool PlayAudio(const std::string& name, float volume = 1.0f, bool loop = false);
    void StopAudio(const std::string& name); // 停止该音效全部实例
    void StopAll();
    bool SetVolume(const std::string& name, float volume); // 即时生效 + 记录为默认
    bool IsPlaying(const std::string& name); // 任一实例正在播放
};

#endif // AUDIO_MANAGER_H
