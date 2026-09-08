#include "Animation/VmdMotion.h"
#include "Core/Utf8Path.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __ANDROID__
#include <SDL3/SDL_iostream.h>
#endif

namespace Animation {
namespace {

class ByteReader {
public:
    ByteReader(const void* data, std::size_t size)
        : m_data(static_cast<const std::uint8_t*>(data)), m_size(size) {}

    bool ReadBytes(void* destination, std::size_t count) {
        if (count > Remaining()) return false;
        if (count != 0) std::memcpy(destination, m_data + m_offset, count);
        m_offset += count;
        return true;
    }

    bool ReadU8(std::uint8_t& value) {
        if (Remaining() < 1) return false;
        value = m_data[m_offset++];
        return true;
    }

    bool ReadU32(std::uint32_t& value) {
        if (Remaining() < 4) return false;
        value = static_cast<std::uint32_t>(m_data[m_offset]) |
                (static_cast<std::uint32_t>(m_data[m_offset + 1]) << 8) |
                (static_cast<std::uint32_t>(m_data[m_offset + 2]) << 16) |
                (static_cast<std::uint32_t>(m_data[m_offset + 3]) << 24);
        m_offset += 4;
        return true;
    }

    bool ReadI32(std::int32_t& value) {
        std::uint32_t raw = 0;
        if (!ReadU32(raw)) return false;
        std::memcpy(&value, &raw, sizeof(value));
        return true;
    }

    bool ReadFloat(float& value) {
        std::uint32_t raw = 0;
        if (!ReadU32(raw)) return false;
        std::memcpy(&value, &raw, sizeof(value));
        return true;
    }

    std::size_t Remaining() const {
        return m_offset <= m_size ? m_size - m_offset : 0;
    }

private:
    const std::uint8_t* m_data = nullptr;
    std::size_t m_size = 0;
    std::size_t m_offset = 0;
};

template <std::size_t N>
std::string DecodeFixedName(const std::array<std::uint8_t, N>& bytes) {
    std::size_t length = 0;
    while (length < N && bytes[length] != 0) ++length;
    if (length == 0) return {};

#ifdef _WIN32
    // VMD 的日文名称通常是 Shift-JIS(CP932)，而 PMX/Assimp 的名称为 UTF-8。
    // 只在格式层做编码转换，模型和 ECS 层仍只接收 std::string。
    const int sourceLength = static_cast<int>(length);
    const char* source = reinterpret_cast<const char*>(bytes.data());
    const int wideLength = MultiByteToWideChar(932, MB_PRECOMPOSED,
                                               source, sourceLength, nullptr, 0);
    if (wideLength > 0) {
        std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
        if (MultiByteToWideChar(932, MB_PRECOMPOSED, source, sourceLength,
                                wide.data(), wideLength) == wideLength) {
            const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                                       wideLength, nullptr, 0,
                                                       nullptr, nullptr);
            if (utf8Length > 0) {
                std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
                if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength,
                                        utf8.data(), utf8Length, nullptr, nullptr) == utf8Length) {
                    return utf8;
                }
            }
        }
    }
#endif

    // 非 Windows 平台先保留原始字节；ASCII VMD 名称仍可直接匹配，后续可在
    // 平台层替换为 iconv/SDL 的 CP932 转换而不影响 VMD 解析器。
    return std::string(reinterpret_cast<const char*>(bytes.data()), length);
}

template <typename T>
bool IsFiniteValue(const T& value) {
    return std::isfinite(value);
}

bool IsFiniteVec3(const glm::vec3& value) {
    return IsFiniteValue(value.x) && IsFiniteValue(value.y) && IsFiniteValue(value.z);
}

bool IsFiniteQuat(const glm::quat& value) {
    return IsFiniteValue(value.w) && IsFiniteValue(value.x) &&
           IsFiniteValue(value.y) && IsFiniteValue(value.z);
}

float Cubic(float p0, float p1, float p2, float p3, float t) {
    const float omt = 1.0f - t;
    return omt * omt * omt * p0 +
           3.0f * omt * omt * t * p1 +
           3.0f * omt * t * t * p2 +
           t * t * t * p3;
}

float CubicDerivative(float p0, float p1, float p2, float p3, float t) {
    const float omt = 1.0f - t;
    return 3.0f * omt * omt * (p1 - p0) +
           6.0f * omt * t * (p2 - p1) +
           3.0f * t * t * (p3 - p2);
}

// VMD 骨骼插值的前 16 字节按 X/Y/Z/旋转交错存放：
// [x1...], [x2...], [y1...], [y2...]，每个通道占一个交错槽位。
float SampleVmdBezier(const std::array<std::uint8_t, 64>& data, int channel, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float x1 = static_cast<float>(data[channel]) / 127.0f;
    const float x2 = static_cast<float>(data[4 + channel]) / 127.0f;
    const float y1 = static_cast<float>(data[8 + channel]) / 127.0f;
    const float y2 = static_cast<float>(data[12 + channel]) / 127.0f;

    if ((x1 + x2 + y1 + y2) <= std::numeric_limits<float>::epsilon()) return t;

    // 反解 x(u)=t。控制点始终在 [0,1] 内，Newton + 二分回退可覆盖极端曲线。
    float u = t;
    for (int i = 0; i < 6; ++i) {
        const float x = Cubic(0.0f, x1, x2, 1.0f, u) - t;
        const float dx = CubicDerivative(0.0f, x1, x2, 1.0f, u);
        if (std::fabs(dx) < 1.0e-5f) break;
        u = std::clamp(u - x / dx, 0.0f, 1.0f);
    }
    float low = 0.0f;
    float high = 1.0f;
    for (int i = 0; i < 8; ++i) {
        const float x = Cubic(0.0f, x1, x2, 1.0f, u);
        if (std::fabs(x - t) < 1.0e-4f) break;
        if (x < t) low = u;
        else high = u;
        u = 0.5f * (low + high);
    }
    return std::clamp(Cubic(0.0f, y1, y2, 1.0f, u), 0.0f, 1.0f);
}

template <typename T, typename FrameGetter>
void SortAndDeduplicate(std::vector<T>& values, FrameGetter frameGetter) {
    std::sort(values.begin(), values.end(), [&](const T& a, const T& b) {
        return frameGetter(a) < frameGetter(b);
    });
    std::vector<T> unique;
    unique.reserve(values.size());
    for (auto& value : values) {
        if (!unique.empty() && frameGetter(unique.back()) == frameGetter(value)) {
            unique.back() = std::move(value);
        } else {
            unique.push_back(std::move(value));
        }
    }
    values = std::move(unique);
}

bool ReadCount(ByteReader& reader, std::uint32_t& count, std::size_t minimumBytes,
               const char* section, std::string& error) {
    if (!reader.ReadU32(count)) {
        error = std::string("truncated ") + section + " count";
        return false;
    }
    constexpr std::uint32_t kMaxFrames = 10'000'000;
    if (count > kMaxFrames || (minimumBytes != 0 && count > reader.Remaining() / minimumBytes)) {
        error = std::string("invalid ") + section + " count";
        return false;
    }
    return true;
}

} // namespace

bool VmdMotion::LoadFromFile(const std::string& path, VmdMotion& out, std::string& error) {
    error.clear();
#ifdef __ANDROID__
    SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
    if (io == nullptr) {
        error = "cannot open file: " + path;
        return false;
    }
    const Sint64 fileSize = SDL_GetIOSize(io);
    if (fileSize <= 0) {
        SDL_CloseIO(io);
        error = "empty file: " + path;
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
    const std::size_t bytesRead = SDL_ReadIO(io, bytes.data(), bytes.size());
    SDL_CloseIO(io);
    if (bytesRead != bytes.size()) {
        error = "failed to read file: " + path;
        return false;
    }
    return LoadFromMemory(bytes.data(), bytes.size(), out, error);
#else
    std::ifstream file(Utf8Path(path), std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        error = "cannot open file: " + path;
        return false;
    }

    const std::streamoff end = file.tellg();
    if (end <= 0) {
        error = "empty file: " + path;
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        error = "failed to read file: " + path;
        return false;
    }
    return LoadFromMemory(bytes.data(), bytes.size(), out, error);
#endif
}

bool VmdMotion::LoadFromMemory(const void* data, std::size_t size, VmdMotion& out, std::string& error) {
    error.clear();
    out = VmdMotion{};
    if (data == nullptr || size < 30) {
        error = "file is too small for a VMD header";
        return false;
    }

    ByteReader reader(data, size);
    std::array<std::uint8_t, 30> header{};
    if (!reader.ReadBytes(header.data(), header.size())) {
        error = "truncated VMD header";
        return false;
    }
    const std::string headerText(reinterpret_cast<const char*>(header.data()), header.size());
    if (headerText.rfind("Vocaloid Motion Data", 0) != 0) {
        error = "unsupported VMD header";
        return false;
    }

    // VMD 0002 在固定头之后增加 20 字节的目标模型名；旧版
    // "Vocaloid Motion Data file" 则直接从这里开始读取骨骼帧数。
    // dance.vmd 属于 0002，跳过该字段后骨骼计数位于偏移 50。
    if (headerText.rfind("Vocaloid Motion Data 0002", 0) == 0) {
        std::array<std::uint8_t, 20> modelName{};
        if (!reader.ReadBytes(modelName.data(), modelName.size())) {
            error = "truncated VMD model name";
            return false;
        }
    }

    std::uint32_t boneCount = 0;
    constexpr std::size_t kBoneFrameSize = 15 + 4 + 12 + 16 + 64;
    if (!ReadCount(reader, boneCount, kBoneFrameSize, "bone frame", error)) return false;

    out.m_boneTracks.reserve(std::min<std::uint32_t>(boneCount, 4096));
    for (std::uint32_t i = 0; i < boneCount; ++i) {
        std::array<std::uint8_t, 15> nameBytes{};
        VmdBoneKeyframe keyframe;
        float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
        if (!reader.ReadBytes(nameBytes.data(), nameBytes.size()) ||
            !reader.ReadU32(keyframe.frame) ||
            !reader.ReadFloat(keyframe.translation.x) ||
            !reader.ReadFloat(keyframe.translation.y) ||
            !reader.ReadFloat(keyframe.translation.z) ||
            !reader.ReadFloat(qx) || !reader.ReadFloat(qy) ||
            !reader.ReadFloat(qz) || !reader.ReadFloat(qw) ||
            !reader.ReadBytes(keyframe.interpolation.data(), keyframe.interpolation.size())) {
            error = "truncated VMD bone frame";
            return false;
        }

        keyframe.rotation = glm::quat(qw, qx, qy, qz);
        if (!IsFiniteVec3(keyframe.translation) || !IsFiniteQuat(keyframe.rotation)) {
            error = "non-finite value in VMD bone frame";
            return false;
        }
        const float rotationLength = glm::length(keyframe.rotation);
        if (rotationLength > 1.0e-6f) keyframe.rotation = glm::normalize(keyframe.rotation);
        else keyframe.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

        const std::string name = DecodeFixedName(nameBytes);
        if (!name.empty()) {
            auto it = out.m_boneTrackLookup.find(name);
            if (it == out.m_boneTrackLookup.end()) {
                const std::size_t index = out.m_boneTracks.size();
                out.m_boneTrackLookup.emplace(name, index);
                out.m_boneTracks.push_back(VmdBoneTrack{});
                out.m_boneTracks.back().name = name;
                it = out.m_boneTrackLookup.find(name);
            }
            out.m_boneTracks[it->second].keyframes.push_back(keyframe);
        }
        out.m_lastFrame = std::max(out.m_lastFrame, static_cast<float>(keyframe.frame));
    }

    std::uint32_t morphCount = 0;
    constexpr std::size_t kMorphFrameSize = 15 + 4 + 4;
    if (!ReadCount(reader, morphCount, kMorphFrameSize, "morph frame", error)) return false;
    for (std::uint32_t i = 0; i < morphCount; ++i) {
        std::array<std::uint8_t, 15> unusedName{};
        std::uint32_t frame = 0;
        float weight = 0.0f;
        if (!reader.ReadBytes(unusedName.data(), unusedName.size()) ||
            !reader.ReadU32(frame) || !reader.ReadFloat(weight)) {
            error = "truncated VMD morph frame";
            return false;
        }
        if (!IsFiniteValue(weight)) {
            error = "non-finite value in VMD morph frame";
            return false;
        }
        out.m_lastFrame = std::max(out.m_lastFrame, static_cast<float>(frame));
    }

    std::uint32_t cameraCount = 0;
    constexpr std::size_t kCameraFrameSize = 4 + 4 + 12 + 12 + 24 + 4 + 1;
    if (!ReadCount(reader, cameraCount, kCameraFrameSize, "camera frame", error)) return false;
    out.m_cameraKeyframes.reserve(cameraCount);
    for (std::uint32_t i = 0; i < cameraCount; ++i) {
        VmdCameraKeyframe keyframe;
        std::int32_t viewAngle = 45;
        std::uint8_t perspective = 0;
        if (!reader.ReadU32(keyframe.frame) ||
            !reader.ReadFloat(keyframe.distance) ||
            !reader.ReadFloat(keyframe.position.x) ||
            !reader.ReadFloat(keyframe.position.y) ||
            !reader.ReadFloat(keyframe.position.z) ||
            !reader.ReadFloat(keyframe.rotation.x) ||
            !reader.ReadFloat(keyframe.rotation.y) ||
            !reader.ReadFloat(keyframe.rotation.z) ||
            !reader.ReadBytes(keyframe.interpolation.data(), keyframe.interpolation.size()) ||
            !reader.ReadI32(viewAngle) || !reader.ReadU8(perspective)) {
            error = "truncated VMD camera frame";
            return false;
        }
        if (!IsFiniteValue(keyframe.distance) || !IsFiniteVec3(keyframe.position) ||
            !IsFiniteVec3(keyframe.rotation)) {
            error = "non-finite value in VMD camera frame";
            return false;
        }
        keyframe.viewAngle = static_cast<float>(viewAngle);
        keyframe.perspective = perspective == 0;
        out.m_cameraKeyframes.push_back(keyframe);
        out.m_lastFrame = std::max(out.m_lastFrame, static_cast<float>(keyframe.frame));
    }

    for (auto& track : out.m_boneTracks) {
        SortAndDeduplicate(track.keyframes, [](const VmdBoneKeyframe& keyframe) {
            return keyframe.frame;
        });
    }
    SortAndDeduplicate(out.m_cameraKeyframes, [](const VmdCameraKeyframe& keyframe) {
        return keyframe.frame;
    });

    return true;
}

const VmdBoneTrack* VmdMotion::FindBoneTrack(const std::string& boneName) const {
    const auto it = m_boneTrackLookup.find(boneName);
    return it == m_boneTrackLookup.end() ? nullptr : &m_boneTracks[it->second];
}

bool VmdMotion::SampleBone(const std::string& boneName, float frame,
                           glm::vec3& outTranslation, glm::quat& outRotation) const {
    const VmdBoneTrack* track = FindBoneTrack(boneName);
    if (track == nullptr || track->keyframes.empty()) return false;

    if (!std::isfinite(frame)) frame = 0.0f;
    frame = std::max(frame, 0.0f);
    const auto& keys = track->keyframes;
    if (frame <= static_cast<float>(keys.front().frame)) {
        outTranslation = keys.front().translation;
        outRotation = keys.front().rotation;
        return true;
    }
    if (frame >= static_cast<float>(keys.back().frame)) {
        outTranslation = keys.back().translation;
        outRotation = keys.back().rotation;
        return true;
    }

    const auto upper = std::upper_bound(keys.begin(), keys.end(), frame,
        [](float value, const VmdBoneKeyframe& keyframe) {
            return value < static_cast<float>(keyframe.frame);
        });
    const auto& next = *upper;
    const auto& previous = *(upper - 1);
    const float span = static_cast<float>(next.frame - previous.frame);
    const float linearT = span > 0.0f
        ? (frame - static_cast<float>(previous.frame)) / span : 0.0f;
    const float tx = SampleVmdBezier(previous.interpolation, 0, linearT);
    const float ty = SampleVmdBezier(previous.interpolation, 1, linearT);
    const float tz = SampleVmdBezier(previous.interpolation, 2, linearT);
    const float tr = SampleVmdBezier(previous.interpolation, 3, linearT);
    outTranslation = glm::vec3(
        previous.translation.x + (next.translation.x - previous.translation.x) * tx,
        previous.translation.y + (next.translation.y - previous.translation.y) * ty,
        previous.translation.z + (next.translation.z - previous.translation.z) * tz);
    outRotation = glm::normalize(glm::slerp(previous.rotation, next.rotation, tr));
    return true;
}

bool VmdMotion::SampleCamera(float frame, VmdCameraKeyframe& outCamera) const {
    if (m_cameraKeyframes.empty()) return false;
    if (!std::isfinite(frame)) frame = 0.0f;
    frame = std::max(frame, 0.0f);

    if (frame <= static_cast<float>(m_cameraKeyframes.front().frame)) {
        outCamera = m_cameraKeyframes.front();
        return true;
    }
    if (frame >= static_cast<float>(m_cameraKeyframes.back().frame)) {
        outCamera = m_cameraKeyframes.back();
        return true;
    }

    const auto upper = std::upper_bound(m_cameraKeyframes.begin(), m_cameraKeyframes.end(), frame,
        [](float value, const VmdCameraKeyframe& keyframe) {
            return value < static_cast<float>(keyframe.frame);
        });
    const auto& next = *upper;
    const auto& previous = *(upper - 1);
    const float span = static_cast<float>(next.frame - previous.frame);
    const float t = span > 0.0f
        ? (frame - static_cast<float>(previous.frame)) / span : 0.0f;
    outCamera = previous;
    outCamera.distance = glm::mix(previous.distance, next.distance, t);
    outCamera.position = glm::mix(previous.position, next.position, t);
    outCamera.rotation = glm::mix(previous.rotation, next.rotation, t);
    outCamera.viewAngle = glm::mix(previous.viewAngle, next.viewAngle, t);
    outCamera.perspective = t < 0.5f ? previous.perspective : next.perspective;
    return true;
}

} // namespace Animation
