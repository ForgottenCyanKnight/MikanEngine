#include "Rendering/Denoising/DiffuseDenoiser.h"

namespace mikan::denoising {
void EnableDenoiserDeviceFeatures(const VkPhysicalDeviceFeatures& available,
                                 VkPhysicalDeviceFeatures& requested) {
#if MIKAN_ENABLE_NRD
    requested.shaderStorageImageExtendedFormats = available.shaderStorageImageExtendedFormats;
    requested.shaderStorageImageWriteWithoutFormat = available.shaderStorageImageWriteWithoutFormat;
#else
    (void)available;
    (void)requested;
#endif
}
#if MIKAN_ENABLE_NRD
std::unique_ptr<IDiffuseDenoiser> CreateNrdDiffuseDenoiser(Signal signal);
#endif
std::unique_ptr<IDiffuseDenoiser> CreateDiffuseDenoiser(Backend backend, Signal signal) {
#if MIKAN_ENABLE_NRD
    if (backend == Backend::Nrd) return CreateNrdDiffuseDenoiser(signal);
#else
    (void)backend; (void)signal;
#endif
    return nullptr;
}
} // namespace mikan::denoising
