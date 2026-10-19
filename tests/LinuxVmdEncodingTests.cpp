#include "Animation/VmdMotion.h"
#include "Animation/MMD/Model/VMDFile.h"
#include "Core/LegacyTextEncoding.h"
#include "Core/Utf8Path.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

static void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    const auto path = Utf8Path("linux-vmd-编码测试.vmd");
    try {
        const std::string cp932("\x92\x86\x90\x53", 4); // 中心
        Require(Core::Cp932ToUtf8(cp932) == "中心", "CP932 conversion differs from UTF-8 model names");
        Require(Core::Cp932ToUtf8("ASCII") == "ASCII", "ASCII conversion changed");
        Require(Core::Cp932ToUtf8("").empty(), "empty conversion changed");
        const std::string invalid("\x81", 1);
        Require(Core::Cp932ToUtf8(invalid) == invalid, "invalid input fallback changed");

        std::vector<char> bytes;
        auto fixed = [&](const std::string& text, size_t size) {
            const auto offset = bytes.size();
            bytes.resize(offset + size, 0);
            std::memcpy(bytes.data() + offset, text.data(), std::min(size, text.size()));
        };
        auto u32 = [&](uint32_t value) {
            for (int shift = 0; shift < 32; shift += 8) bytes.push_back(char((value >> shift) & 255));
        };
        fixed("Vocaloid Motion Data 0002", 30);
        fixed(cp932, 20);
        u32(1);
        fixed(cp932, 15);
        u32(0);
        for (int i = 0; i < 6; ++i) u32(0); // translation xyz, quaternion xyz
        u32(0x3f800000); // quaternion w = 1
        fixed("", 64);
        for (int i = 0; i < 5; ++i) u32(0); // empty remaining sections

        Animation::VmdMotion motion;
        std::string error;
        Require(Animation::VmdMotion::LoadFromMemory(bytes.data(), bytes.size(), motion, error), error.c_str());
        glm::vec3 position;
        glm::quat rotation;
        Require(motion.SampleBone("中心", 0, position, rotation), "UTF-8 bone lookup failed");
        {
            std::ofstream file(path, std::ios::binary);
            file.write(bytes.data(), bytes.size());
            Require(file.good(), "fixture write failed");
        }
        mmd::VMDFile legacy;
        Require(legacy.Load(Utf8String(path)), "legacy VMD load failed");
        Require(legacy.GetModelName() == "中心" && legacy.GetMotions().at(0).boneName == "中心",
                "legacy VMD names differ from UTF-8");
        std::filesystem::remove(path);
        std::cout << "PASS: both VMD readers decode CP932 names and load UTF-8 paths on Linux\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove(path);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
