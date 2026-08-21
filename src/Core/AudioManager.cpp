// AudioManager.cpp - 音频管理器(miniaudio 版)
// 单头文件库集成: 本文件是唯一包含 miniaudio 实现(MINIAUDIO_IMPLEMENTATION)的编译单元。
// 接口与 SDL3 版兼容; 能力升级: WAV/MP3/OGG/FLAC 解码、多实例、重播、音量、循环。
// 后端: WASAPI(Windows 默认); 禁用 DSound/WinMM 减少链接依赖(仅需 ole32)。
// OGG/Vorbis: stb_vorbis 实现由独立 C 编译单元 src/StbVorbis.c 提供(其 alloc 成员与
// C++ 关键字冲突, 不能按 C++ 编译实现); 本文件只以 STB_VORBIS_HEADER_ONLY 取声明,
// 并定义 STB_VORBIS_INCLUDE_STB_VORBIS_H 让 miniaudio 启用 Vorbis 后端(MA_HAS_VORBIS)。
#define STB_VORBIS_HEADER_ONLY
#include "stb/stb_vorbis.c"
#ifndef STB_VORBIS_INCLUDE_STB_VORBIS_H
#define STB_VORBIS_INCLUDE_STB_VORBIS_H
#endif
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DSOUND
#define MA_NO_WINMM
#include "miniaudio/miniaudio.h"

#include "AudioManager.h"
#include <iostream>

AudioManager* AudioManager::instance = nullptr;

AudioManager::AudioManager() = default;

AudioManager::~AudioManager() {
    Shutdown();
}

AudioManager& AudioManager::GetInstance() {
    if (!instance) {
        instance = new AudioManager();
    }
    return *instance;
}

void AudioManager::DestroyInstance() {
    delete instance;
    instance = nullptr;
}

bool AudioManager::Initialize() {
    if (m_engine) {
        return true;
    }
    m_engine = new ma_engine;
    ma_engine_config cfg = ma_engine_config_init();
    // 默认输出设备 + 44100Hz; 全部解码/播放由 miniaudio 内部线程管理
    if (ma_engine_init(&cfg, m_engine) != MA_SUCCESS) {
        delete m_engine;
        m_engine = nullptr;
        std::cerr << "miniaudio engine init failed" << std::endl;
        return false;
    }
    std::cout << "Audio manager initialized (miniaudio " << ma_version_string() << ")" << std::endl;
    return true;
}

void AudioManager::Shutdown() {
    if (!m_engine) {
        return;
    }
    for (auto& pair : m_audio) {
        for (ma_sound* s : pair.second.slots) {
            if (s) {
                ma_sound_stop(s);
                ma_sound_uninit(s);
                delete s;
            }
        }
    }
    m_audio.clear();
    ma_engine_uninit(m_engine);
    delete m_engine;
    m_engine = nullptr;
    std::cout << "Audio manager shutdown (miniaudio)" << std::endl;
}

bool AudioManager::LoadAudio(const std::string& name, const std::string& filePath) {
    if (!m_engine) {
        std::cerr << "Audio manager not initialized" << std::endl;
        return false;
    }

    // 同名重载: 先销毁旧实例池
    auto it = m_audio.find(name);
    if (it != m_audio.end()) {
        for (ma_sound* s : it->second.slots) {
            if (s) {
                ma_sound_stop(s);
                ma_sound_uninit(s);
                delete s;
            }
        }
        m_audio.erase(it);
    }

    // 预创建实例池: 每个槽独立解码(MA_SOUND_FLAG_DECODE → 内存, 重播/循环简单;
    // 超大 BGM 可去掉该 flag 走流式, 但多实例下每槽独立 IO 文件)
    AudioEntry entry;
    entry.slots.resize(kSlotsPerAudio);
    for (int i = 0; i < kSlotsPerAudio; ++i) {
        ma_sound* s = new ma_sound;
        const ma_uint32 flags = MA_SOUND_FLAG_DECODE;
        ma_result res = ma_sound_init_from_file(m_engine, filePath.c_str(), flags, nullptr, nullptr, s);
        if (res != MA_SUCCESS) {
            std::cerr << "Failed to load audio: " << name << " (" << filePath
                      << ") err=" << ma_result_description(res) << std::endl;
            delete s;
            return false;
        }
        entry.slots[i] = s;
    }
    m_audio[name] = std::move(entry);
    std::cout << "Loaded audio: " << name << " (" << filePath << ", " << kSlotsPerAudio << " instances)" << std::endl;
    return true;
}

bool AudioManager::PlayAudio(const std::string& name, float volume, bool loop) {
    if (!m_engine) {
        std::cerr << "Audio manager not initialized" << std::endl;
        return false;
    }
    auto it = m_audio.find(name);
    if (it == m_audio.end()) {
        std::cerr << "Audio not found: " << name << std::endl;
        return false;
    }
    AudioEntry& entry = it->second;

    // 多实例: round-robin 找空闲槽(未在播放); 全忙则替换游标指向的下一个
    ma_sound* chosen = nullptr;
    for (size_t i = 0; i < entry.slots.size(); ++i) {
        size_t idx = (entry.nextSlot + i) % entry.slots.size();
        ma_sound* s = entry.slots[idx];
        if (s && !ma_sound_is_playing(s)) {
            chosen = s;
            entry.nextSlot = (idx + 1) % entry.slots.size();
            break;
        }
    }
    if (!chosen) {
        chosen = entry.slots[entry.nextSlot];
        entry.nextSlot = (entry.nextSlot + 1) % entry.slots.size();
    }

    const float gain = (volume >= 0.0f && volume <= 1.0f) ? volume : entry.defaultGain;
    ma_sound_seek_to_pcm_frame(chosen, 0); // 重播: 回到开头
    ma_sound_set_volume(chosen, gain);
    ma_sound_set_looping(chosen, loop);
    ma_sound_start(chosen);
    return true;
}

void AudioManager::StopAudio(const std::string& name) {
    auto it = m_audio.find(name);
    if (it == m_audio.end()) return;
    for (ma_sound* s : it->second.slots) {
        if (s) ma_sound_stop(s);
    }
}

void AudioManager::StopAll() {
    for (auto& pair : m_audio) {
        for (ma_sound* s : pair.second.slots) {
            if (s) ma_sound_stop(s);
        }
    }
}

bool AudioManager::SetVolume(const std::string& name, float volume) {
    auto it = m_audio.find(name);
    if (it == m_audio.end()) return false;
    const float gain = (volume >= 0.0f && volume <= 1.0f) ? volume : 1.0f;
    it->second.defaultGain = gain;
    for (ma_sound* s : it->second.slots) {
        if (s) ma_sound_set_volume(s, gain);
    }
    return true;
}

bool AudioManager::IsPlaying(const std::string& name) {
    auto it = m_audio.find(name);
    if (it == m_audio.end()) return false;
    for (ma_sound* s : it->second.slots) {
        if (s && ma_sound_is_playing(s)) return true;
    }
    return false;
}
