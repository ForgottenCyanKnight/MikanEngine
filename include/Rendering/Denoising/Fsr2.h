#pragma once
#include "Rendering/Denoising/DlssRayReconstruction.h"
namespace mikan::denoising {
std::unique_ptr<IRayReconstruction> CreateFsr2();
}
