#include "NativeAudio.h"

#include "Core/Log.h"
#include "Core/ProjectManager.h"
#include "Core/RuntimeCapabilities.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <json.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

namespace PixelHop {

NativeAudio::~NativeAudio() {
    Shutdown();
}

bool NativeAudio::Initialize() {
    if (m_stream || !Core::GetRuntimeCapabilities().rendering) return true;
    Shutdown();

    const auto load = [](const char* relative, std::size_t& size) {
        const auto path = ProjectManager::GetInstance().ResolveAssetPath(relative);
        return std::unique_ptr<void, decltype(&SDL_free)>(SDL_LoadFile(path.c_str(), &size), SDL_free);
    };
    std::size_t jsonBytes = 0;
    std::size_t pcmBytes = 0;
    const auto jsonFile = load("assets/audio.json", jsonBytes);
    const auto pcmFile = load("assets/audio.f32", pcmBytes);
    if (!jsonFile || !pcmFile || pcmBytes % sizeof(float) != 0) {
        LOGE("[PixelHop] Could not load native sound assets");
        return false;
    }
    const auto descriptor = nlohmann::json::parse(
        static_cast<const char*>(jsonFile.get()),
        static_cast<const char*>(jsonFile.get()) + jsonBytes, nullptr, false);
    if (descriptor.is_discarded() || descriptor.value("sampleRate", 0) != 48000 ||
        descriptor.value("format", std::string()) != "f32le") {
        LOGE("[PixelHop] Unsupported sound descriptor");
        return false;
    }
    try {
        m_samples.resize(pcmBytes / sizeof(float));
        std::memcpy(m_samples.data(), pcmFile.get(), pcmBytes);
        for (const auto& item : descriptor.at("clips")) {
            const Clip clip{item.at("offsetFrames").get<std::size_t>(), item.at("frames").get<std::size_t>()};
            if (clip.offset > m_samples.size() || clip.frames > m_samples.size() - clip.offset) {
                LOGE("[PixelHop] Sound clip is outside the PCM asset");
                return false;
            }
            m_clips.push_back(clip);
        }
        for (const auto& [name, notes] : descriptor.at("events").items()) {
            auto& event = m_events[name];
            for (const auto& item : notes) {
                const Note note{item.at("clip").get<std::size_t>(), item.at("delayFrames").get<std::uint64_t>()};
                if (note.clip >= m_clips.size()) {
                    LOGE("[PixelHop] Sound event references an invalid clip");
                    return false;
                }
                event.push_back(note);
            }
        }
    } catch (const nlohmann::json::exception& error) {
        LOGE("[PixelHop] Invalid sound descriptor: %s", error.what());
        return false;
    }

    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            LOGI("[PixelHop] Audio device unavailable: %s", SDL_GetError());
            return false;
        }
        m_initializedAudio = true;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_F32LE, 1, 48000};
    m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Feed, this);
    if (!m_stream || !SDL_ResumeAudioStreamDevice(m_stream)) {
        LOGI("[PixelHop] Audio playback unavailable: %s", SDL_GetError());
        Shutdown();
        return false;
    }
    LOGI("[PixelHop] Native 48 kHz sound playback initialized");
    return true;
}

void NativeAudio::Shutdown() {
    // DestroyAudioStream waits for any callback before its backing samples are released.
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
    m_samples.clear();
    m_clips.clear();
    m_events.clear();
    m_voices.clear();
    m_frame = 0;
    if (m_initializedAudio) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        m_initializedAudio = false;
    }
}

void NativeAudio::Play(const std::string& event) {
    if (!m_stream) return;
    const auto found = m_events.find(event);
    if (found == m_events.end()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& note : found->second) {
        m_voices.push_back({note.clip, m_frame + note.delay});
    }
}

void NativeAudio::SetMuted(bool muted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_muted = muted;
}

void NativeAudio::Feed(void* userdata, SDL_AudioStream* stream, int additionalAmount, int) {
    if (additionalAmount > 0) static_cast<NativeAudio*>(userdata)->Mix(stream, additionalAmount);
}

void NativeAudio::Mix(SDL_AudioStream* stream, int bytes) {
    std::array<float, 2048> output{};
    std::size_t remaining = (static_cast<std::size_t>(bytes) + sizeof(float) - 1) / sizeof(float);
    while (remaining > 0) {
        const auto count = std::min(remaining, output.size());
        std::fill_n(output.begin(), count, 0.0f);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto endFrame = m_frame + count;
            for (auto& voice : m_voices) {
                const auto& clip = m_clips[voice.clip];
                if (voice.startFrame >= endFrame) continue;
                if (!voice.started) {
                    // Original beep() checks mute when each delayed note begins.
                    // Notes already playing continue through a mute toggle.
                    voice.started = true;
                    voice.suppressed = m_muted;
                }
                if (voice.suppressed) continue;
                const auto firstFrame = std::max(m_frame, voice.startFrame);
                const auto lastFrame = std::min(endFrame, voice.startFrame + clip.frames);
                for (auto frame = firstFrame; frame < lastFrame; ++frame) {
                    output[frame - m_frame] += m_samples[clip.offset + frame - voice.startFrame];
                }
            }
            m_frame = endFrame;
            m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(), [&](const Voice& voice) {
                return voice.startFrame + m_clips[voice.clip].frames <= endFrame;
            }), m_voices.end());
        }
        if (!SDL_PutAudioStreamData(stream, output.data(), static_cast<int>(count * sizeof(float)))) return;
        remaining -= count;
    }
}

} // namespace PixelHop
