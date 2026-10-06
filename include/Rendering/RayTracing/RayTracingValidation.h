#pragma once
#include "Platform/Export.h"
// Standalone process only: owns the global Vulkan context, no engine/window initialization.
MIKAN_API int RunVoxRayTracingValidation(int argc,char** argv);
