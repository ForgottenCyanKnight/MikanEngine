#pragma once

#include <string>

namespace Core {

bool WriteAssetFileAtomically(const std::string& filepath,
                              const std::string& contents,
                              const char* kind);

} // namespace Core
