// Native playback of the original Pixel Hop Twins sound effects by stm1978.
// Original source is MIT and the author dedicates its creative assets to CC0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_AudioStream;

namespace PixelHop {

class NativeAudio {
public:
    NativeAudio() = default;
    ~NativeAudio();
    NativeAudio(const NativeAudio&) = delete;
    NativeAudio& operator=(const NativeAudio&) = delete;

    bool Initialize();
    void Shutdown();
    void Play(const std::string& event);
    void SetMuted(bool muted);

private:
    struct Clip {
        std::size_t offset = 0;
        std::size_t frames = 0;
    };
    struct Note {
        std::size_t clip = 0;
        std::uint64_t delay = 0;
    };
    struct Voice {
        std::size_t clip = 0;
        std::uint64_t startFrame = 0;
        bool started = false;
        bool suppressed = false;
    };

    static void Feed(void* userdata, SDL_AudioStream* stream, int additionalAmount, int totalAmount);
    void Mix(SDL_AudioStream* stream, int bytes);

    SDL_AudioStream* m_stream = nullptr;
    bool m_initializedAudio = false;
    bool m_muted = false;
    std::uint64_t m_frame = 0;
    std::vector<float> m_samples;
    std::vector<Clip> m_clips;
    std::unordered_map<std::string, std::vector<Note>> m_events;
    std::vector<Voice> m_voices;
    std::mutex m_mutex;
};

} // namespace PixelHop
