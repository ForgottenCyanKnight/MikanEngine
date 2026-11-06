#pragma once
#include "Core/VulkanContext.h"
#include <cstdlib>
#include "Platform/Export.h"

namespace mikan::rt {
// Shared Game.dll state; do not put a mutable function-static in this header,
// because Editor.dll must select the same tier as the rendering runtime.
MIKAN_API uint32_t GetNeeDiffuseSamples();
MIKAN_API uint32_t GetNeeDirectSamples();
MIKAN_API void SetNeeDiffuseSamples(uint32_t samples);
enum class NeeQualityTier : uint32_t { Performance, Balanced, Quality };
MIKAN_API NeeQualityTier GetNeeQualityTier();
MIKAN_API void SetNeeQualityTier(NeeQualityTier tier);
MIKAN_API bool GetRestirPathTracing();
MIKAN_API void SetRestirPathTracing(bool enabled);
MIKAN_API bool GetRestirTemporalReuse();

}
