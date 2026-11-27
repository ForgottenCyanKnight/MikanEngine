#pragma once

#include "Platform/Export.h"
#include <string>

namespace Core {

MIKAN_API bool WriteAssetFileAtomically(const std::string& filepath,
                                         const std::string& contents,
                                         const char* kind);

} // namespace Core
