#pragma once
#ifdef _WIN32
#include "Platform/Export.h"
#include <cstdint>
#include <string>
#include <vector>

namespace SystemFonts {
struct FontData {
    std::vector<std::uint8_t> bytes;
    std::string family;
    int faceIndex = 0;
};

// Process-lifetime data read from DirectWrite's installed font collection.
// No engine font asset or hard-coded filesystem path is used.
MIKAN_API const FontData* GetChineseFont();
}
#endif
