#pragma once

#include <string>
#include <vector>

namespace ShaderBuildPlan {

inline constexpr const char* kFilename = "shader-build-plan.txt";

struct Entry {
    std::string output;
    std::string source;
    std::string target;
    std::vector<std::string> defines;
};

// Paths are relative to the SPIR-V and GLSL roots respectively.
std::vector<Entry> Read(const std::string& path);

} // namespace ShaderBuildPlan
