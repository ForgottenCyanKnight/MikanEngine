#include "AtmosphereLUT.h"
#include "EngineConfig.h"
#include "Core/Log.h"
#include <SDL3/SDL_iostream.h>

#include <chrono>
#include <iostream>
#include <vector>

namespace {
constexpr uint32_t TRANS_W = 256;
constexpr uint32_t TRANS_H = 64;
constexpr uint32_t SCAT_W = 256;   // NU(8) x MU_S(32)
constexpr uint32_t SCAT_H = 128;   // MU
constexpr uint32_t SCAT_D = 32;    // R
constexpr uint32_t IRR_W = 64;     // ground irradiance LUT
constexpr uint32_t IRR_H = 16;
constexpr uint32_t MULTI_SCATTER_ORDERS = 4;   // 2026-08-11: 用户要求 4 阶多重散射 16F 精度对比版（16F 下 density 可能下溢——正是要看的对比效果）
} // namespace

AtmosphereLUT::~AtmosphereLUT()
{
    Cleanup();
}

bool AtmosphereLUT::Init(VkDevice device, VkPhysicalDevice physicalDevice,
                         uint32_t skyWidth, uint32_t skyHeight)
{
    if (m_Initialized) Cleanup();
    m_Device = device;
    m_PhysicalDevice = physicalDevice;
    m_SkyW = skyWidth;
    m_SkyH = skyHeight;
    if (m_Device == VK_NULL_HANDLE) return false;

    // transmittance LUT (2D RGBA16F, storage + sampled)
    if (!CreateImage(TRANS_W, TRANS_H, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     m_TransmittanceImage, m_TransmittanceMemory)) return false;
    // scattering LUT (3D RGBA32F——2026-08-11: 16F→32F，多重散射密度 ~1e-9 在 16F 下溢为 0)
    if (!CreateImage(SCAT_W, SCAT_H, SCAT_D, VK_FORMAT_R32G32B32A32_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     m_ScatteringImage, m_ScatteringMemory)) return false;
#if 1   // 2026-08-11: 多重散射已恢复（Bruneton 4 阶；回退原因 LogLuv32 二次 gamma 误判已修正）
    // density LUT (3D RGBA32F, multi-scatter iteration intermediate——16F 下溢 1e-9)
    if (!CreateImage(SCAT_W, SCAT_H, SCAT_D, VK_FORMAT_R32G32B32A32_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     m_DensityImage, m_DensityMemory)) return false;
    // irradiance LUT (2D RGBA32F, ground indirect irradiance, 累计——渲染用)
    if (!CreateImage(IRR_W, IRR_H, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     m_IrradianceImage, m_IrradianceMemory)) return false;
    // 2026-08-11 严格 n-1 阶：deltaMulti=上一阶纯多次散射（3D，含相函数，下一阶 density 入射场）
    if (!CreateImage(SCAT_W, SCAT_H, SCAT_D, VK_FORMAT_R32G32B32A32_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     m_DeltaMultipleImage, m_DeltaMultipleMemory)) return false;
    // deltaIrr=上一阶纯间接辐照度（2D，下一阶 density 地面反弹场）
    if (!CreateImage(IRR_W, IRR_H, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     m_DeltaIrradianceImage, m_DeltaIrradianceMemory)) return false;
#endif
    // skyRT（主天空：圆柱投影 128×64 单层——2026-08-11 用户拍板恢复；cubemap IBL 设施改走独立着色器 atmo_sky_cube.comp）
    if (!CreateImage(m_SkyW, m_SkyH, 1, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     m_SkyRTImage, m_SkyRTMemory)) return false;

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    viewInfo.image = m_TransmittanceImage;
    if (vkCreateImageView(m_Device, &viewInfo, nullptr, &m_TransmittanceView) != VK_SUCCESS) { LOGI("[AtmosphereLUT] CreateImageView FAILED transmittance"); return false; }
    viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;   // scattering/density/deltaMulti/irradiance/deltaIrr 32F（transmittance 保持 16F）
    viewInfo.image = m_ScatteringImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    if (vkCreateImageView(m_Device, &viewInfo, nullptr, &m_ScatteringView) != VK_SUCCESS) { LOGI("[AtmosphereLUT] CreateImageView FAILED scattering"); return false; }
#if 1   // 2026-08-11: 多重散射已恢复（Bruneton 4 阶）
    viewInfo.image = m_DensityImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    if (vkCreateImageView(m_Device, &viewInfo, nullptr, &m_DensityView) != VK_SUCCESS) { LOGI("[AtmosphereLUT] CreateImageView FAILED density"); return false; }
    viewInfo.image = m_IrradianceImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    if (vkCreateImageView(m_Device, &viewInfo, nullptr, &m_IrradianceView) != VK_SUCCESS) { LOGI("[AtmosphereLUT] CreateImageView FAILED irradiance"); return false; }
    viewInfo.image = m_DeltaMultipleImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    if (vkCreateImageView(m_Device, &viewInfo, nullptr, &m_DeltaMultipleView) != VK_SUCCESS) { LOGI("[AtmosphereLUT] CreateImageView FAILED deltaMultiple"); return false; }
    viewInfo.image = m_DeltaIrradianceImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    if (vkCreateImageView(m_Device, &viewInfo, nullptr, &m_DeltaIrradianceView) != VK_SUCCESS) { LOGI("[AtmosphereLUT] CreateImageView FAILED deltaIrr"); return false; }
#endif
    viewInfo.image = m_SkyRTImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;   // 2026-08-11：主天空圆柱投影 2D
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(m_Device, &viewInfo, nullptr, &m_SkyRTView) != VK_SUCCESS) { LOGI("[AtmosphereLUT] CreateImageView FAILED skyRT"); return false; }

    // 2026-08-12：天空环境 cubemap（IBL——独立 64×64×6 CUBE_COMPATIBLE，7 级 mip 预滤波）
    // ⚠️ 2026-08-12 用户拍板：弃 LogLuv32（8bit 量化色带）——R16G16B16A16_SFLOAT 后降 B10G11R11_UFLOAT（r11g11b10 线性 HDR 够用，带宽减半）
    if (!CreateImage(m_SkyCubeW, m_SkyCubeW, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     m_SkyCubeImage, m_SkyCubeMemory, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, m_SkyCubeMips)) { LOGI("[AtmosphereLUT] CreateImage FAILED skyCube"); return false; }
    VkImageViewCreateInfo cubeViewInfo = {};
    cubeViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    cubeViewInfo.image = m_SkyCubeImage;
    cubeViewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    cubeViewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, m_SkyCubeMips, 0, 6 };
    cubeViewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    if (vkCreateImageView(m_Device, &cubeViewInfo, nullptr, &m_SkyCubeView) != VK_SUCCESS) { LOGI("[AtmosphereLUT] CreateImageView FAILED skyCube"); return false; }
    cubeViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;   // compute 写 6 层（mip0）
    if (vkCreateImageView(m_Device, &cubeViewInfo, nullptr, &m_SkyCubeArrayView) != VK_SUCCESS) { LOGI("[AtmosphereLUT] CreateImageView FAILED skyCubeArray"); return false; }
    // 2026-08-12：每 mip 的 2DArray view（GGX 预滤波 dst 写——baseMipLevel = m）
    for (uint32_t m = 0; m < m_SkyCubeMips; m++) {
        VkImageViewCreateInfo mipViewInfo = cubeViewInfo;
        mipViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        mipViewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 6 };
        if (vkCreateImageView(m_Device, &mipViewInfo, nullptr, &m_SkyCubeMipViews[m]) != VK_SUCCESS) { LOGI("[AtmosphereLUT] CreateImageView FAILED skyCubeMip %u", m); return false; }
    }

    // 2026-08-15：split-sum BRDF LUT（128×128 R16G16B16A16_SFLOAT——Fermion/learnopengl 移植，替代 Karis 近似）
    if (!CreateImage(128, 128, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     m_BRDFLutImage, m_BRDFLutMemory)) { LOGI("[AtmosphereLUT] CreateImage FAILED brdfLUT"); return false; }
    VkImageViewCreateInfo brdfViewInfo = {};
    brdfViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    brdfViewInfo.image = m_BRDFLutImage;
    brdfViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    brdfViewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    brdfViewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(m_Device, &brdfViewInfo, nullptr, &m_BRDFLutView) != VK_SUCCESS) { LOGI("[AtmosphereLUT] CreateImageView FAILED brdfLUT"); return false; }

    // 2026-08-12：SH 辐照度投影 SSBO（shOut 144B @0 + wgRes 384×36×4B @144——两级归约；STORAGE 写 + UNIFORM 读 + TRANSFER_DST fill）
    VkBufferCreateInfo shbInfo = {};
    shbInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    shbInfo.size = 144 + 96 * 36 * 4;
    shbInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;   // TRANSFER_DST：vkCmdFillBuffer 每帧清零
    shbInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_Device, &shbInfo, nullptr, &m_SkyCubeSHBuffer) != VK_SUCCESS) { LOGI("[AtmosphereLUT] CreateBuffer FAILED skyCubeSH"); return false; }
    VkMemoryRequirements shbReq;
    vkGetBufferMemoryRequirements(m_Device, m_SkyCubeSHBuffer, &shbReq);
    VkMemoryAllocateInfo shbAlloc = {};
    shbAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    shbAlloc.allocationSize = shbReq.size;
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &props);
    uint32_t shbType = VK_MAX_MEMORY_TYPES;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        // 临时调试：HOST_VISIBLE 便于回读 dump 系数；定位后改回 DEVICE_LOCAL
        if ((shbReq.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            shbType = i; break;
        }
    }
    if (shbType == VK_MAX_MEMORY_TYPES) { LOGI("[AtmosphereLUT] SH buffer no memory type"); return false; }
    shbAlloc.memoryTypeIndex = shbType;
    if (vkAllocateMemory(m_Device, &shbAlloc, nullptr, &m_SkyCubeSHMemory) != VK_SUCCESS) { LOGI("[AtmosphereLUT] AllocateMemory FAILED skyCubeSH"); return false; }
    vkBindBufferMemory(m_Device, m_SkyCubeSHBuffer, m_SkyCubeSHMemory, 0);

    if (!CreateSamplers()) { LOGI("[AtmosphereLUT] CreateSamplers FAILED"); return false; }
    if (!CreatePipelines()) { LOGI("[AtmosphereLUT] CreatePipelines FAILED"); return false; }
    CreateDescriptors();

    m_Initialized = true;
    printf("[AtmosphereLUT] initialized: LUT %ux%u + %ux%ux%u, skyRT=%ux%u (RGBA8 LogLuv32), multi-scatter orders=%u\n",
        TRANS_W, TRANS_H, SCAT_W, SCAT_H, SCAT_D, m_SkyW, m_SkyH, MULTI_SCATTER_ORDERS);
    return true;
}

void AtmosphereLUT::Cleanup()
{
    if (m_Device == VK_NULL_HANDLE) return;
    if (m_SkyRTSampler) vkDestroySampler(m_Device, m_SkyRTSampler, nullptr);
    if (m_SkyCubeSampler) vkDestroySampler(m_Device, m_SkyCubeSampler, nullptr);
    if (m_BRDFLutSampler) vkDestroySampler(m_Device, m_BRDFLutSampler, nullptr);   // 2026-08-15
    if (m_LUTSampler) vkDestroySampler(m_Device, m_LUTSampler, nullptr);
    if (m_SkyRTView) vkDestroyImageView(m_Device, m_SkyRTView, nullptr);
    if (m_SkyRTArrayView) vkDestroyImageView(m_Device, m_SkyRTArrayView, nullptr);
    if (m_SkyCubeView) vkDestroyImageView(m_Device, m_SkyCubeView, nullptr);
    if (m_BRDFLutView) vkDestroyImageView(m_Device, m_BRDFLutView, nullptr);   // 2026-08-15
    if (m_SkyCubeArrayView) vkDestroyImageView(m_Device, m_SkyCubeArrayView, nullptr);
#if 1   // 2026-08-11: 多重散射已恢复（Bruneton 4 阶）
    if (m_DeltaIrradianceView) vkDestroyImageView(m_Device, m_DeltaIrradianceView, nullptr);
    if (m_DeltaMultipleView) vkDestroyImageView(m_Device, m_DeltaMultipleView, nullptr);
    if (m_IrradianceView) vkDestroyImageView(m_Device, m_IrradianceView, nullptr);
    if (m_DensityView) vkDestroyImageView(m_Device, m_DensityView, nullptr);
#endif
    if (m_ScatteringView) vkDestroyImageView(m_Device, m_ScatteringView, nullptr);
    if (m_TransmittanceView) vkDestroyImageView(m_Device, m_TransmittanceView, nullptr);
    if (m_SkyRTImage) vkDestroyImage(m_Device, m_SkyRTImage, nullptr);
    if (m_SkyCubeImage) vkDestroyImage(m_Device, m_SkyCubeImage, nullptr);
    if (m_BRDFLutImage) vkDestroyImage(m_Device, m_BRDFLutImage, nullptr);   // 2026-08-15
#if 1
    if (m_DeltaIrradianceImage) vkDestroyImage(m_Device, m_DeltaIrradianceImage, nullptr);
    if (m_DeltaMultipleImage) vkDestroyImage(m_Device, m_DeltaMultipleImage, nullptr);
    if (m_IrradianceImage) vkDestroyImage(m_Device, m_IrradianceImage, nullptr);
    if (m_DensityImage) vkDestroyImage(m_Device, m_DensityImage, nullptr);
#endif
    if (m_ScatteringImage) vkDestroyImage(m_Device, m_ScatteringImage, nullptr);
    if (m_TransmittanceImage) vkDestroyImage(m_Device, m_TransmittanceImage, nullptr);
    if (m_SkyRTMemory) vkFreeMemory(m_Device, m_SkyRTMemory, nullptr);
    if (m_SkyCubeMemory) vkFreeMemory(m_Device, m_SkyCubeMemory, nullptr);
    if (m_BRDFLutMemory) vkFreeMemory(m_Device, m_BRDFLutMemory, nullptr);   // 2026-08-15
#if 1
    if (m_DeltaIrradianceMemory) vkFreeMemory(m_Device, m_DeltaIrradianceMemory, nullptr);
    if (m_DeltaMultipleMemory) vkFreeMemory(m_Device, m_DeltaMultipleMemory, nullptr);
    if (m_IrradianceMemory) vkFreeMemory(m_Device, m_IrradianceMemory, nullptr);
    if (m_DensityMemory) vkFreeMemory(m_Device, m_DensityMemory, nullptr);
#endif
    if (m_ScatteringMemory) vkFreeMemory(m_Device, m_ScatteringMemory, nullptr);
    if (m_TransmittanceMemory) vkFreeMemory(m_Device, m_TransmittanceMemory, nullptr);
    DestroyPipeline(m_SkyPipe);
    DestroyPipeline(m_SkyCubePipe);   // 2026-08-12：IBL cubemap
    DestroyPipeline(m_PanoToCubePipe);   // 2026-08-12：skyRT→cube 重投影
    DestroyPipeline(m_ShProjPipe);   // 2026-08-12：SH 投影
    DestroyPipeline(m_CubePrefilterPipe);   // 2026-08-12：GGX 预滤波
    DestroyPipeline(m_BRDFLutPipe);   // 2026-08-15：split-sum BRDF LUT
    for (int i = 0; i < 8; i++) {
        if (m_SkyCubeMipViews[i]) vkDestroyImageView(m_Device, m_SkyCubeMipViews[i], nullptr);
    }
    if (m_SkyCubeSHBuffer) vkDestroyBuffer(m_Device, m_SkyCubeSHBuffer, nullptr);
    if (m_SkyCubeSHMemory) vkFreeMemory(m_Device, m_SkyCubeSHMemory, nullptr);
#if 1
    DestroyPipeline(m_IrradiancePipe);
    DestroyPipeline(m_MultiscatterPipe);
    DestroyPipeline(m_DensityPipe);
#endif
    DestroyPipeline(m_ScatteringPipe);
    DestroyPipeline(m_TransmittancePipe);
    // 2026-08-17：纹理全部销毁重建后必须重置 frame cache——否则 RecreateSwapChain（全屏切换）后
    // 太阳方向未变 → DispatchSky/DispatchSkyCube 命中 cache 跳过生成 → 新纹理保持未定义内容 → IBL 环境光丢失
    m_LastSunDir = glm::vec3(0.0f, 0.0f, 0.0f);
    m_LastAltitude = -1.0f;
    m_CubeLastSunDir = glm::vec3(0.0f, 0.0f, 0.0f);
    m_CubeLastAltitude = -1.0f;
    m_Device = VK_NULL_HANDLE;
    m_Initialized = false;
}

bool AtmosphereLUT::CreateImage(uint32_t width, uint32_t height, uint32_t depth, VkFormat format,
                                VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory,
                                uint32_t arrayLayers, VkImageCreateFlags flags, uint32_t mipLevels)
{
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { width, height, depth };
    info.mipLevels = mipLevels;   // 2026-08-11：skyRT 传 8（128² 全 mip）
    info.arrayLayers = arrayLayers;   // 2026-08-11：skyRT 硬件 cubemap=6（CUBE_COMPATIBLE）
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.flags = flags;   // 2026-08-11：skyRT 传 VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_Device, &info, nullptr, &image) != VK_SUCCESS) {
        LOGI("[AtmosphereLUT] CreateImage FAILED %ux%ux%u fmt=%d usage=%u flags=%u mips=%u layers=%u", width, height, depth, (int)format, (uint32_t)usage, (uint32_t)flags, mipLevels, arrayLayers);
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(m_Device, image, &memReq);
    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = memReq.size;
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &props);
    uint32_t typeBits = memReq.memoryTypeBits;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            alloc.memoryTypeIndex = i;
            break;
        }
    }
    if (vkAllocateMemory(m_Device, &alloc, nullptr, &memory) != VK_SUCCESS) {
        LOGI("[AtmosphereLUT] AllocateMemory FAILED %ux%ux%u size=%llu", width, height, depth, (unsigned long long)memReq.size);
        return false;
    }
    vkBindImageMemory(m_Device, image, memory, 0);
    return true;
}

bool AtmosphereLUT::CreateSamplers()
{
    VkSamplerCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = 0.0f;
    if (vkCreateSampler(m_Device, &info, nullptr, &m_LUTSampler) != VK_SUCCESS) return false;

    // 2026-08-11 用户拍板：主天空回圆柱（单层无 mip）——sky sampler 恢复简单（maxLod=0；cubemap IBL 设施走独立着色器）
    // 2026-08-12 用户：柱面两侧环绕拼接——u（经度）改 REPEAT（v 保持 CLAMP：仰角不能环绕）；
    // 修复 cube 重投影/合成采样在经度 ±180°（u=0/1）的接缝（CLAMP 会钳边缘行 → 光带/拼接缝）
    VkSamplerCreateInfo skyRTInfo = info;
    skyRTInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (vkCreateSampler(m_Device, &skyRTInfo, nullptr, &m_SkyRTSampler) != VK_SUCCESS) return false;
    // 2026-08-12：IBL cubemap sampler（mip 预滤波——粗糙度模糊：mipmapMode=LINEAR + maxLod=7）
    {
        VkSamplerCreateInfo cubeInfo = info;
        cubeInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        cubeInfo.maxLod = (float)m_SkyCubeMips;
        if (vkCreateSampler(m_Device, &cubeInfo, nullptr, &m_SkyCubeSampler) != VK_SUCCESS) return false;
    }
    // 2026-08-15：BRDF LUT sampler（无 mip，LINEAR + CLAMP）
    {
        VkSamplerCreateInfo lutInfo = info;
        if (vkCreateSampler(m_Device, &lutInfo, nullptr, &m_BRDFLutSampler) != VK_SUCCESS) return false;
    }
    return true;
}

void AtmosphereLUT::DestroyPipeline(ComputePipeline& pipe)
{
    if (m_Device == VK_NULL_HANDLE) return;
    if (pipe.pipeline) vkDestroyPipeline(m_Device, pipe.pipeline, nullptr);
    if (pipe.layout) vkDestroyPipelineLayout(m_Device, pipe.layout, nullptr);
    if (pipe.setLayout) vkDestroyDescriptorSetLayout(m_Device, pipe.setLayout, nullptr);
    if (pipe.pool) vkDestroyDescriptorPool(m_Device, pipe.pool, nullptr);
    if (pipe.module) vkDestroyShaderModule(m_Device, pipe.module, nullptr);
    pipe = ComputePipeline();
}

bool AtmosphereLUT::CreatePipelines()
{
    struct PipeDef {
        const char* spv;
        ComputePipeline* out;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        uint32_t pcSize = 0;
    };
    std::vector<VkDescriptorSetLayoutBinding> transBindings = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
    };
    std::vector<VkDescriptorSetLayoutBinding> scatBindings = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
    };
#if 1   // 2026-08-11: 多重散射已恢复（Bruneton 4 阶，严格 n-1 阶版）
    std::vector<VkDescriptorSetLayoutBinding> densityBindings = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },          // densityLUT 写
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, // singleScatLUT（=scattering 初始单次）
        { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, // transmittanceLUT
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, // deltaIrrLUT（上一阶辐照度）
        { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, // deltaMultiLUT（上一阶纯多次散射）
    };
    std::vector<VkDescriptorSetLayoutBinding> multiscatterBindings = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },          // scatteringLUT 读改写
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, // densityLUT
        { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, // transmittanceLUT
        { 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },          // deltaMultiLUT 写
    };
    std::vector<VkDescriptorSetLayoutBinding> irradianceBindings = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },          // irradianceLUT 读改写（累计）
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },          // deltaIrrLUT 写
        { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, // singleScatLUT
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, // deltaMultiLUT
    };
#endif
    std::vector<VkDescriptorSetLayoutBinding> skyBindings = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
    };
    // 2026-08-12：SH 投影——读 atmo cube（sampler）+ 写系数 SSBO（binding1=shOut、binding2=wgRes）
    std::vector<VkDescriptorSetLayoutBinding> shProjBindings = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
    };
    // 2026-08-12：真 GGX 预滤波——读 cube mip0（sampler）+ 写 dst mip（storage image2DArray）
    std::vector<VkDescriptorSetLayoutBinding> prefilterBindings = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
    };
    // 2026-08-15：BRDF LUT——写 128×128 storage image（一次性）
    std::vector<VkDescriptorSetLayoutBinding> brdfLutBindings = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
    };
    // 2026-08-12：panoToCube——读 skyRT（sampler）+ 写 cube（storage image2DArray）+ 读 trans/scat LUT（天顶 per-pixel）
    std::vector<VkDescriptorSetLayoutBinding> panoBindings = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },  // skyRT（LogLuv32 全景）
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },           // cube array（16F）
        { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },  // transmittanceLUT
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },  // scatteringLUT
    };
    PipeDef defs[] = {
        { "atmo_transmittance.comp.spv", &m_TransmittancePipe, transBindings, 0 },
        { "atmo_scattering.comp.spv",   &m_ScatteringPipe,   scatBindings,  0 },
#if 1   // 2026-08-11: 多重散射已恢复（Bruneton 4 阶）
        { "atmo_density.comp.spv",      &m_DensityPipe,      densityBindings, 4 },      // int order
        { "atmo_multiscatter.comp.spv", &m_MultiscatterPipe, multiscatterBindings, 0 },
        { "atmo_irradiance.comp.spv",   &m_IrradiancePipe,   irradianceBindings, 4 },   // int order
#endif
        { "atmo_sky.comp.spv",          &m_SkyPipe,          skyBindings,   32 }, // vec4 sunDir + ivec2 skySize
        { "atmo_sky_cube.comp.spv",     &m_SkyCubePipe,      skyBindings,   32 }, // 2026-08-12：IBL cubemap（同 sky push）
        { "atmo_pano_to_cube.comp.spv", &m_PanoToCubePipe,   panoBindings,  32 }, // 2026-08-12：skyRT→cube 重投影（同 sky push 结构）
        { "atmo_sh_proj.comp.spv",      &m_ShProjPipe,       shProjBindings, 4 }, // 2026-08-12：SH 投影（int pass——两次 dispatch）
        { "atmo_cube_prefilter.comp.spv", &m_CubePrefilterPipe, prefilterBindings, 12 }, // 2026-08-12：GGX 预滤波（float roughness + uint dstMip + uint faceSize）
        { "brdf_lut.comp.spv", &m_BRDFLutPipe, brdfLutBindings, 0 }, // 2026-08-15：split-sum BRDF LUT（一次性）
    };

    for (PipeDef& def : defs) {
        // shader module
        std::string path = EngineConfig::GetShaderPath(def.spv);
        std::vector<char> code;
#ifdef __ANDROID__
        {
            // Android：APK assets 不是真实文件系统，fopen 读不到；SDL_IOFromFile 相对路径 fallback 到 assets://
            SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
            if (io == nullptr) {
                fprintf(stderr, "[AtmosphereLUT] shader not found: %s\n", path.c_str());
                LOGI("[AtmosphereLUT] shader not found: %s", path.c_str());
                return false;
            }
            Sint64 sz = SDL_GetIOSize(io);
            if (sz <= 0) { SDL_CloseIO(io); LOGI("[AtmosphereLUT] empty shader: %s", path.c_str()); return false; }
            code.resize((size_t)sz);
            if (SDL_ReadIO(io, code.data(), (size_t)sz) != (size_t)sz) { SDL_CloseIO(io); return false; }
            SDL_CloseIO(io);
        }
#else
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) {
            fprintf(stderr, "[AtmosphereLUT] shader not found: %s\n", path.c_str());
            return false;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        code.resize(size);
        fread(code.data(), 1, size, f);
        fclose(f);
#endif
        VkShaderModuleCreateInfo moduleInfo = {};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = code.size();
        moduleInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        if (vkCreateShaderModule(m_Device, &moduleInfo, nullptr, &def.out->module) != VK_SUCCESS) return false;

        // descriptor set layout
        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = (uint32_t)def.bindings.size();
        layoutInfo.pBindings = def.bindings.data();
        if (vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &def.out->setLayout) != VK_SUCCESS) return false;

        // pipeline layout (optional push constant)
        VkPushConstantRange pcRange = {};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.offset = 0;
        pcRange.size = def.pcSize;
        VkPipelineLayoutCreateInfo pipeLayoutInfo = {};
        pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeLayoutInfo.setLayoutCount = 1;
        pipeLayoutInfo.pSetLayouts = &def.out->setLayout;
        if (def.pcSize > 0) {
            pipeLayoutInfo.pushConstantRangeCount = 1;
            pipeLayoutInfo.pPushConstantRanges = &pcRange;
        }
        if (vkCreatePipelineLayout(m_Device, &pipeLayoutInfo, nullptr, &def.out->layout) != VK_SUCCESS) return false;

        // pipeline
        VkComputePipelineCreateInfo pipeInfo = {};
        pipeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeInfo.stage.module = def.out->module;
        pipeInfo.stage.pName = "main";
        pipeInfo.layout = def.out->layout;
        if (vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &def.out->pipeline) != VK_SUCCESS) return false;

        // descriptor pool (cover ALL binding types in the set)
        std::vector<VkDescriptorPoolSize> poolSizes;
        for (const auto& b : def.bindings) {
            bool found = false;
            for (auto& ps : poolSizes) {
                if (ps.type == b.descriptorType) { ps.descriptorCount += b.descriptorCount; found = true; break; }
            }
            if (!found) poolSizes.push_back({ b.descriptorType, b.descriptorCount });
        }
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = (uint32_t)poolSizes.size();
        poolInfo.pPoolSizes = poolSizes.data();
        // 2026-08-15：prefilter 每 mip 独立 set（mip1-6 + 默认 = 7 sets；录制中更新同一 set 会导致 GPU 执行时全部读最后一次 view）
        if (def.out == &m_CubePrefilterPipe) {
            poolInfo.maxSets = m_SkyCubeMips;
            for (auto& ps : poolSizes) ps.descriptorCount *= m_SkyCubeMips;
        }
        if (vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &def.out->pool) != VK_SUCCESS) return false;

        VkDescriptorSetAllocateInfo setInfo = {};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setInfo.descriptorPool = def.out->pool;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &def.out->setLayout;
        if (vkAllocateDescriptorSets(m_Device, &setInfo, &def.out->set) != VK_SUCCESS) return false;
    }
    return true;
}

void AtmosphereLUT::CreateDescriptors()
{
    auto write = [](VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                    const VkDescriptorImageInfo& info, std::vector<VkWriteDescriptorSet>& writes) {
        writes.push_back({});
        VkWriteDescriptorSet& w = writes.back();
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = type;
        w.pImageInfo = &info;
    };

    std::vector<VkWriteDescriptorSet> writes;
    VkDescriptorImageInfo transInfo = { m_LUTSampler, m_TransmittanceView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo transReadInfo = { m_LUTSampler, m_TransmittanceView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo scatInfo = { m_LUTSampler, m_ScatteringView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo scatReadInfo = { m_LUTSampler, m_ScatteringView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
#if 1   // 2026-08-11: 多重散射已恢复（Bruneton 4 阶，严格 n-1 阶版）
    VkDescriptorImageInfo densityInfo = { m_LUTSampler, m_DensityView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo densityReadInfo = { m_LUTSampler, m_DensityView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo irrInfo = { m_LUTSampler, m_IrradianceView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo irrReadInfo = { m_LUTSampler, m_IrradianceView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo deltaMultiInfo = { m_LUTSampler, m_DeltaMultipleView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo deltaMultiReadInfo = { m_LUTSampler, m_DeltaMultipleView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo deltaIrrInfo = { m_LUTSampler, m_DeltaIrradianceView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo deltaIrrReadInfo = { m_LUTSampler, m_DeltaIrradianceView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
#endif
    VkDescriptorImageInfo skyInfo = { VK_NULL_HANDLE, m_SkyRTView, VK_IMAGE_LAYOUT_GENERAL };   // 2026-08-11：CUBE view（imageCube storage 写 6 面）

    // transmittance: write LUT
    write(m_TransmittancePipe.set, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, transInfo, writes);
    // scattering: write 3D LUT + read transmittance
    write(m_ScatteringPipe.set, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, scatInfo, writes);
    write(m_ScatteringPipe.set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, transReadInfo, writes);
#if 1   // 2026-08-11: 多重散射已恢复（Bruneton 4 阶，严格 n-1 阶版）
    // density: write densityLUT + read singleScat(=scattering 初始单次)/transmittance/deltaIrr/deltaMulti
    write(m_DensityPipe.set, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, densityInfo, writes);
    write(m_DensityPipe.set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, scatReadInfo, writes);
    write(m_DensityPipe.set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, transReadInfo, writes);
    write(m_DensityPipe.set, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, deltaIrrReadInfo, writes);
    write(m_DensityPipe.set, 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, deltaMultiReadInfo, writes);
    // multiscatter: read-modify-write scattering + read density/transmittance + write deltaMulti
    write(m_MultiscatterPipe.set, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, scatInfo, writes);
    write(m_MultiscatterPipe.set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, densityReadInfo, writes);
    write(m_MultiscatterPipe.set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, transReadInfo, writes);
    write(m_MultiscatterPipe.set, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, deltaMultiInfo, writes);
    // irradiance: read-modify-write irradiance + write deltaIrr + read singleScat/deltaMulti
    write(m_IrradiancePipe.set, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, irrInfo, writes);
    write(m_IrradiancePipe.set, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, deltaIrrInfo, writes);
    write(m_IrradiancePipe.set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, scatReadInfo, writes);
    write(m_IrradiancePipe.set, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, deltaMultiReadInfo, writes);
#endif
    // sky: write skyRT + read both LUTs
    write(m_SkyPipe.set, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, skyInfo, writes);
    write(m_SkyPipe.set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, transReadInfo, writes);
    write(m_SkyPipe.set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, scatReadInfo, writes);
    // 2026-08-12：skyCube（IBL）——写 6 层 array + 读 LUT
    VkDescriptorImageInfo skyCubeInfo = { VK_NULL_HANDLE, m_SkyCubeArrayView, VK_IMAGE_LAYOUT_GENERAL };
    write(m_SkyCubePipe.set, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, skyCubeInfo, writes);
    write(m_SkyCubePipe.set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, transReadInfo, writes);
    write(m_SkyCubePipe.set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, scatReadInfo, writes);

    // 2026-08-12：panoToCube——读 skyRT（sampler，READ_ONLY）+ 写 cube（storage，GENERAL）+ 读 LUT（天顶 per-pixel）
    VkDescriptorImageInfo skyRTReadInfo = { m_SkyRTSampler, m_SkyRTView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    write(m_PanoToCubePipe.set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, skyRTReadInfo, writes);
    write(m_PanoToCubePipe.set, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, skyCubeInfo, writes);
    write(m_PanoToCubePipe.set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, transReadInfo, writes);
    write(m_PanoToCubePipe.set, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, scatReadInfo, writes);

    // 2026-08-12：SH 投影——读 cube（sampler）+ 写系数 SSBO（⚠️ cube 布局是 SHADER_READ_ONLY——descriptor 声明须匹配）
    VkDescriptorImageInfo skyCubeReadInfo = { m_SkyCubeSampler, m_SkyCubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };    write(m_ShProjPipe.set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, skyCubeReadInfo, writes);
    VkDescriptorBufferInfo shOutBufInfo = { m_SkyCubeSHBuffer, 0, 144 };
    writes.push_back({});
    VkWriteDescriptorSet& shw = writes.back();
    shw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    shw.dstSet = m_ShProjPipe.set;
    shw.dstBinding = 1;
    shw.descriptorCount = 1;
    shw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    shw.pBufferInfo = &shOutBufInfo;
    VkDescriptorBufferInfo wgResBufInfo = { m_SkyCubeSHBuffer, 144, 96 * 36 * 4 };
    writes.push_back({});
    VkWriteDescriptorSet& wgrw = writes.back();
    wgrw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wgrw.dstSet = m_ShProjPipe.set;
    wgrw.dstBinding = 2;
    wgrw.descriptorCount = 1;
    wgrw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wgrw.pBufferInfo = &wgResBufInfo;

    // 2026-08-12：GGX 预滤波——读 cube（sampler mip0）+ 写 dst（每 mip dispatch 前更新 view）
    VkDescriptorImageInfo prefilterSrcInfo = { m_SkyCubeSampler, m_SkyCubeView, VK_IMAGE_LAYOUT_GENERAL };
    write(m_CubePrefilterPipe.set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, prefilterSrcInfo, writes);
    VkDescriptorImageInfo prefilterDstInfo = { VK_NULL_HANDLE, m_SkyCubeMipViews[0], VK_IMAGE_LAYOUT_GENERAL };
    write(m_CubePrefilterPipe.set, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, prefilterDstInfo, writes);

    // 2026-08-15：prefilter 每 mip 独立 set（mip1-6）——⚠️ 录制中 vkUpdateDescriptorSets 同一 set：
    // GPU 执行时所有 dispatch 读最后一次更新的 view → mip1-5 全黑（用户实测"0 层级正常其他层级黑"）
    for (uint32_t m = 1; m < m_SkyCubeMips; m++) {
        VkDescriptorSetAllocateInfo sa = {};
        sa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        sa.descriptorPool = m_CubePrefilterPipe.pool;
        sa.descriptorSetCount = 1;
        sa.pSetLayouts = &m_CubePrefilterPipe.setLayout;
        if (vkAllocateDescriptorSets(m_Device, &sa, &m_PrefilterSets[m]) != VK_SUCCESS) {
            printf("[Atmo] prefilter set alloc FAILED m=%u\n", m);
            return;
        }
        std::vector<VkWriteDescriptorSet> preWrites;
        write(m_PrefilterSets[m], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, prefilterSrcInfo, preWrites);
        VkDescriptorImageInfo mipDstInfo = { VK_NULL_HANDLE, m_SkyCubeMipViews[m], VK_IMAGE_LAYOUT_GENERAL };
        write(m_PrefilterSets[m], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, mipDstInfo, preWrites);
        vkUpdateDescriptorSets(m_Device, (uint32_t)preWrites.size(), preWrites.data(), 0, nullptr);
    }

    // 2026-08-15：BRDF LUT 输出（storage image）
    VkDescriptorImageInfo brdfLutInfo = { VK_NULL_HANDLE, m_BRDFLutView, VK_IMAGE_LAYOUT_GENERAL };
    write(m_BRDFLutPipe.set, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, brdfLutInfo, writes);

    vkUpdateDescriptorSets(m_Device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
}

void AtmosphereLUT::ImageBarrier(VkCommandBuffer cmd, VkImage image,
                                 VkImageLayout oldLayout, VkImageLayout newLayout,
                                 VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                                 VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                                 uint32_t baseMip, uint32_t mipCount,
                                 uint32_t baseLayer, uint32_t layerCount)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, baseLayer, layerCount };
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool AtmosphereLUT::Generate(VkCommandPool commandPool, VkQueue queue)
{
    if (!m_Initialized || m_Device == VK_NULL_HANDLE || commandPool == VK_NULL_HANDLE) return false;

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(m_Device, &allocInfo, &cmd) != VK_SUCCESS) return false;

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // 1) transmittance: UNDEFINED -> GENERAL (write)
    ImageBarrier(cmd, m_TransmittanceImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 0, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_TransmittancePipe.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_TransmittancePipe.layout, 0, 1, &m_TransmittancePipe.set, 0, nullptr);
    vkCmdDispatch(cmd, (TRANS_W + 7) / 8, (TRANS_H + 7) / 8, 1);

    // 2) transmittance -> SHADER_READ_ONLY; scattering UNDEFINED -> GENERAL
    ImageBarrier(cmd, m_TransmittanceImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ImageBarrier(cmd, m_ScatteringImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 0, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
#if 1   // 2026-08-11: 多重散射已恢复（Bruneton 4 阶，严格 n-1 阶版）
    ImageBarrier(cmd, m_DensityImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 0, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ImageBarrier(cmd, m_IrradianceImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 0, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    ImageBarrier(cmd, m_DeltaMultipleImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 0, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ImageBarrier(cmd, m_DeltaIrradianceImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                 0, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
#endif

    // 3) single scattering (write scattering GENERAL)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ScatteringPipe.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ScatteringPipe.layout, 0, 1, &m_ScatteringPipe.set, 0, nullptr);
    vkCmdDispatch(cmd, (SCAT_W + 7) / 8, (SCAT_H + 7) / 8, (SCAT_D + 3) / 4);

    // 4) clear irradiance（累计）+ deltaIrr（纯上一阶基座）为 0 + 布局 settle
#if 1   // 2026-08-11: 多重散射已恢复（Bruneton 4 阶，严格 n-1 阶版）
    ImageBarrier(cmd, m_IrradianceImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    ImageBarrier(cmd, m_DeltaIrradianceImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue clearZero = {};
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, m_IrradianceImage, VK_IMAGE_LAYOUT_GENERAL, &clearZero, 1, &range);
    vkCmdClearColorImage(cmd, m_DeltaIrradianceImage, VK_IMAGE_LAYOUT_GENERAL, &clearZero, 1, &range);
    // scattering（order==1 入射场=单次）、density（被 irradiance/multiscatter 读）、deltaMulti（order>=2 入射场）-> READ_ONLY
    ImageBarrier(cmd, m_ScatteringImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ImageBarrier(cmd, m_DensityImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ImageBarrier(cmd, m_DeltaMultipleImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ImageBarrier(cmd, m_DeltaIrradianceImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    // irradiance（累计）保持 GENERAL：让后续 irradiance pass 的读改写看到 clear（TRANSFER->COMPUTE 同步）
    ImageBarrier(cmd, m_IrradianceImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
#endif

    // 5) multi-scatter iteration（严格 n-1 阶：order=0 → 入射场=单次散射；order>=1 → 入射场=上一阶 deltaMulti）
    //    每阶：density（写 densityLUT，读 singleScat/deltaIrr/deltaMulti）→
    //          irradiance（写 deltaIrr + 累加 irradiance，读 density/singleScat/deltaMulti）→
    //          multiscatter（写 deltaMulti + 累加 scattering，读 density/transmittance）
#if 1   // 2026-08-11: 多重散射已恢复（Bruneton 4 阶，严格 n-1 阶版）
    for (uint32_t order = 0; order < MULTI_SCATTER_ORDERS; order++) {
        const int32_t nMinus1 = (int32_t)order + 1;   // 入射场散射阶：0→1（单次）、1→2（纯 2 阶 deltaMulti）、...

        // --- density: write densityLUT（GENERAL）；读 scattering/trans/deltaIrr/deltaMulti（均 READ_ONLY 保持）---
        ImageBarrier(cmd, m_DensityImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_DensityPipe.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_DensityPipe.layout, 0, 1, &m_DensityPipe.set, 0, nullptr);
        vkCmdPushConstants(cmd, m_DensityPipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int32_t), &nMinus1);
        vkCmdDispatch(cmd, (SCAT_W + 7) / 8, (SCAT_H + 7) / 8, (SCAT_D + 3) / 4);

        // --- density -> READ_ONLY（irradiance/multiscatter 读）；deltaIrr -> GENERAL（irradiance 写）；irradiance 同步（保持 GENERAL）---
        ImageBarrier(cmd, m_DensityImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ImageBarrier(cmd, m_DeltaIrradianceImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ImageBarrier(cmd, m_IrradianceImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_IrradiancePipe.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_IrradiancePipe.layout, 0, 1, &m_IrradiancePipe.set, 0, nullptr);
        vkCmdPushConstants(cmd, m_IrradiancePipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int32_t), &nMinus1);
        vkCmdDispatch(cmd, (IRR_W + 7) / 8, (IRR_H + 7) / 8, 1);

        // --- deltaIrr -> READ_ONLY（下阶 density 读）；deltaMulti -> GENERAL（multiscatter 写）；scattering -> GENERAL（multiscatter 读改写）---
        ImageBarrier(cmd, m_DeltaIrradianceImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ImageBarrier(cmd, m_DeltaMultipleImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ImageBarrier(cmd, m_ScatteringImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_MultiscatterPipe.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_MultiscatterPipe.layout, 0, 1, &m_MultiscatterPipe.set, 0, nullptr);
        vkCmdDispatch(cmd, (SCAT_W + 7) / 8, (SCAT_H + 7) / 8, (SCAT_D + 3) / 4);

        // next order: scattering/deltaMulti/deltaIrr -> READ_ONLY（下阶 density 读入射场）
        ImageBarrier(cmd, m_ScatteringImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ImageBarrier(cmd, m_DeltaMultipleImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ImageBarrier(cmd, m_DeltaIrradianceImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
#endif

    // scattering -> READ_ONLY（sky samples it；循环末已是 READ_ONLY，此处仅冗余同步：READ_ONLY->READ_ONLY 无布局转换）
    ImageBarrier(cmd, m_ScatteringImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    VkResult submitRes = vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    if (submitRes != VK_SUCCESS) {
        vkFreeCommandBuffers(m_Device, commandPool, 1, &cmd);
        LOGI("[AtmosphereLUT] Generate vkQueueSubmit FAILED: %d", (int)submitRes);
        return false;
    }
    vkDeviceWaitIdle(m_Device);
    vkFreeCommandBuffers(m_Device, commandPool, 1, &cmd);
    printf("[AtmosphereLUT] LUT generated (transmittance %ux%u, scattering %ux%ux%u)\n",
        TRANS_W, TRANS_H, SCAT_W, SCAT_H, SCAT_D);

    return true;
}

VkSampler AtmosphereLUT::GetSkyRTSampler()
{
    return m_SkyRTSampler;
}

void AtmosphereLUT::DispatchSky(VkCommandBuffer commandBuffer, const glm::vec3& sunDir, float altitudeMeters)
{
    if (!m_Initialized) return;

    // 2026-08-11: frame cache — sky panorama depends on sun direction AND camera altitude.
    // If neither changed, keep last frame's result: skip dispatch AND all layout barriers
    // (skyRT stays SHADER_READ_ONLY). ⚠️ m_LastSunDir 初始必须 (0,0,0)、m_LastAltitude 初始 -1（首帧必跑）。
    const float altClamped = glm::max(altitudeMeters, 0.0f);
    const glm::vec3 sunDirN = glm::normalize(sunDir);
    if (glm::dot(m_LastSunDir, sunDirN) > 0.99999f && m_LastAltitude == altClamped) {
        return;
    }
    m_LastSunDir = sunDirN;
    m_LastAltitude = altClamped;

    // skyRT: read (prev frame composite) -> write (GENERAL)
    ImageBarrier(commandBuffer, m_SkyRTImage, m_SkyRTLayout, VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    m_SkyRTLayout = VK_IMAGE_LAYOUT_GENERAL;

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_SkyPipe.pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_SkyPipe.layout, 0, 1, &m_SkyPipe.set, 0, nullptr);

    struct SkyPC {
        glm::vec4 sunDir;   // .xyz=sun direction; .w=exposure (physical radiance -> display, default 15)
        int32_t skySize[2];
        float altitudeMeters;   // 2026-08-11：相机海拔（用户约定 max(0, 相机y+200)）——skyRT 随海拔实时变化
    } pc = {};
    pc.sunDir = glm::vec4(sunDirN, 10.0f);   // exposure 对照官方 demo.js（默认 10）——2026-08-11 用户反馈白天偏黑，6→10 对齐官方；4 阶多重散射时代 6 防过曝偏保守
    pc.skySize[0] = (int32_t)m_SkyW;
    pc.skySize[1] = (int32_t)m_SkyH;
    pc.altitudeMeters = altClamped;
    vkCmdPushConstants(commandBuffer, m_SkyPipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkyPC), &pc);

    vkCmdDispatch(commandBuffer, (m_SkyW + 7) / 8, (m_SkyH + 7) / 8, 1);   // 2026-08-11：主天空圆柱投影（单层）——cubemap IBL 走独立着色器

    // skyRT: write -> read (composite render pass samples it)
    ImageBarrier(commandBuffer, m_SkyRTImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    m_SkyRTLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// 2026-08-12：天空环境 cubemap dispatch（IBL/反射专用——6 face × m_SkyCubeW²，各向同性无柱面极区聚集）
void AtmosphereLUT::DispatchSkyCube(VkCommandBuffer commandBuffer, const glm::vec3& sunDir, float altitudeMeters)
{
    if (!m_Initialized) return;

    const float altClamped = glm::max(altitudeMeters, 0.0f);
    const glm::vec3 sunDirN = glm::normalize(sunDir);
    // ⚠️ 2026-08-12 用户拍板：IBL（cube/SH/blit）比 skyRT 更新频率更低——太阳方向变化 > ~5.7°（dot 0.995）才重算；
    // skyRT（显示背景）保持 0.99999 严格——太阳微变天空及时更新，反射低频跟随即可
    if (glm::dot(m_CubeLastSunDir, sunDirN) > 0.995f && glm::abs(m_CubeLastAltitude - altClamped) < 100.0f) {
        return;   // 独立 frame cache（IBL 低频：太阳小变化不重算 cube/SH/blit）
    }
    m_CubeLastSunDir = sunDirN;
    m_CubeLastAltitude = altClamped;

    // skyCube: read (prev frame composite) -> write (GENERAL)——6 层全 mip 转换（mip1-6 由 blit 生成）
    ImageBarrier(commandBuffer, m_SkyCubeImage, m_SkyCubeLayout, VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 0, m_SkyCubeMips, 0, 6);
    m_SkyCubeLayout = VK_IMAGE_LAYOUT_GENERAL;

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_SkyCubePipe.pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_SkyCubePipe.layout, 0, 1, &m_SkyCubePipe.set, 0, nullptr);

    struct SkyCubePC {
        glm::vec4 sunDir;
        int32_t skySize[2];
        float altitudeMeters;
    } pc = {};
    pc.sunDir = glm::vec4(sunDirN, 10.0f);   // 与 skyRT 同曝光
    pc.skySize[0] = (int32_t)m_SkyCubeW;
    pc.skySize[1] = (int32_t)m_SkyCubeW;
    pc.altitudeMeters = altClamped;
    vkCmdPushConstants(commandBuffer, m_SkyCubePipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkyCubePC), &pc);

    vkCmdDispatch(commandBuffer, (m_SkyCubeW + 7) / 8, (m_SkyCubeW + 7) / 8, 6);   // z = face（6 层）写 mip0

    // 2026-08-15 恢复真 GGX 预滤波（Fermion/learnopengl 移植——2026-08-12 曾建后被 blit box 平均取代；用户要求物理 PBR 参考 Fermion）
    DispatchCubePrefilter(commandBuffer);
    DispatchBRDFLut(commandBuffer);   // 2026-08-15：split-sum BRDF LUT（首帧一次性）

    // skyCube: write -> read (composite IBL samples it)——全 mip
    ImageBarrier(commandBuffer, m_SkyCubeImage, m_SkyCubeLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 0, m_SkyCubeMips, 0, 6);
    m_SkyCubeLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // 2026-08-12：cube 已更新 → 同步重算 SH 投影（在生成路径内——避免首帧 frame cache 误跳）
    DispatchSHProj(commandBuffer, sunDirN, altClamped);
}

// 2026-08-12：skyRT 全景 → cube 重投影（复用 skyRT 已算好的散射——不做 24576 次 GetSkyRadiance）
// ⚠️ 与 DispatchSkyCube 共享低频 frame cache（0.995——IBL 太阳小变化不重算）
// ⚠️ skyRT 须已是 SHADER_READ_ONLY（DispatchSky 尾部转换；若 skyRT 帧缓存跳过则保持上帧布局）
void AtmosphereLUT::DispatchPanoToCube(VkCommandBuffer commandBuffer, const glm::vec3& sunDir, float altitudeMeters)
{
    if (!m_Initialized) return;
    const float altClamped = glm::max(altitudeMeters, 0.0f);
    const glm::vec3 sunDirN = glm::normalize(sunDir);
    if (glm::dot(m_CubeLastSunDir, sunDirN) > 0.995f && glm::abs(m_CubeLastAltitude - altClamped) < 100.0f) {
        return;   // IBL 低频 cache（与 DispatchSkyCube 同）
    }
    m_CubeLastSunDir = sunDirN;
    m_CubeLastAltitude = altClamped;
    auto t0 = std::chrono::high_resolution_clock::now();

    // skyCube: read -> write（GENERAL）——6 层全 mip（mip1-6 由 blit 生成）
    ImageBarrier(commandBuffer, m_SkyCubeImage, m_SkyCubeLayout, VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 0, m_SkyCubeMips, 0, 6);
    m_SkyCubeLayout = VK_IMAGE_LAYOUT_GENERAL;

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_PanoToCubePipe.pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_PanoToCubePipe.layout, 0, 1, &m_PanoToCubePipe.set, 0, nullptr);

    struct SkyCubePC {
        glm::vec4 sunDir;
        int32_t skySize[2];
        float altitudeMeters;
    } pc = {};
    pc.sunDir = glm::vec4(sunDirN, 10.0f);
    pc.skySize[0] = (int32_t)m_SkyCubeW;
    pc.skySize[1] = (int32_t)m_SkyCubeW;
    pc.altitudeMeters = altClamped;
    vkCmdPushConstants(commandBuffer, m_PanoToCubePipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkyCubePC), &pc);

    vkCmdDispatch(commandBuffer, (m_SkyCubeW + 7) / 8, (m_SkyCubeW + 7) / 8, 6);   // z = face（重投影写 mip0）

    // blit box 平均 mip 预滤波（与 DispatchSkyCube 同）
    DispatchCubePrefilter(commandBuffer);   // 2026-08-15：真 GGX 预滤波（替代 blit；每 mip 独立 descriptor set）
    DispatchBRDFLut(commandBuffer);   // 2026-08-15：split-sum BRDF LUT（首帧一次性；⚠️ 必须执行——负责 BRDF LUT image UNDEFINED→READ_ONLY 布局转换）

    // skyCube: write -> read（composite IBL samples it）——全 mip
    ImageBarrier(commandBuffer, m_SkyCubeImage, m_SkyCubeLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 0, m_SkyCubeMips, 0, 6);
    m_SkyCubeLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // cube 已更新 → 同步重算 SH 投影
    DispatchSHProj(commandBuffer, sunDirN, altClamped);
}

// 2026-08-12：SH 辐照度投影（读 atmo cube——线性直读——3 阶 9 系数 RGB 写 SSBO）
// ⚠️ 竞态修复：SSBO 用 vkCmdFillBuffer 清零——两次 dispatch：pass0 投影（wg 归约）→ pass1 汇总+卷积核
// 仅由 DispatchSkyCube 生成路径调用（cube 更新时）——无需 frame cache
// 2026-08-15：真 GGX 预滤波（Fermion/learnopengl 移植）——逐 mip dispatch；src = mip0 全链 sampler
void AtmosphereLUT::DispatchCubePrefilter(VkCommandBuffer commandBuffer)
{
    for (uint32_t m = 1; m < m_SkyCubeMips; m++) {
        // 同步：mip 0..m-1 写完成 → 可读（src sampler 全链；dst mip m 无竞争）
        ImageBarrier(commandBuffer, m_SkyCubeImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     0, m, 0, 6);

        // 2026-08-15：每 mip 独立 set（m_PrefilterSets[m]——录制中更新同一 set 会导致 GPU 全读最后一次 view → mip1-5 黑）
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_CubePrefilterPipe.pipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_CubePrefilterPipe.layout, 0, 1, &m_PrefilterSets[m], 0, nullptr);

        uint32_t faceSize = m_SkyCubeW >> m;
        struct PrefilterPC { float roughness; uint32_t dstMip; uint32_t faceSize; } ppc = {};
        ppc.roughness = float(m) / float(m_SkyCubeMips - 1);
        ppc.dstMip = m;
        ppc.faceSize = faceSize;
        vkCmdPushConstants(commandBuffer, m_CubePrefilterPipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PrefilterPC), &ppc);
        vkCmdDispatch(commandBuffer, (faceSize + 7) / 8, (faceSize + 7) / 8, 6);
    }
}

// 2026-08-15：split-sum BRDF LUT（128×128，一次性——首帧 cube 生成路径内调用）
void AtmosphereLUT::DispatchBRDFLut(VkCommandBuffer commandBuffer)
{
    if (m_BRDFLutReady) return;
    m_BRDFLutReady = true;
    printf("[AtmoDetail] BRDF LUT generated (128x128, first frame)\n");

    // UNDEFINED/GENERAL → GENERAL（storage 写）
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_BRDFLutImage;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_BRDFLutPipe.pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_BRDFLutPipe.layout, 0, 1, &m_BRDFLutPipe.set, 0, nullptr);
    vkCmdDispatch(commandBuffer, 16, 16, 1);   // 128×128 / 8

    // GENERAL → SHADER_READ_ONLY（合成端采样）
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void AtmosphereLUT::DispatchSHProj(VkCommandBuffer commandBuffer, const glm::vec3& sunDir, float altitudeMeters)
{
    if (!m_Initialized || !m_SkyCubeSHBuffer) return;
    const float altClamped = glm::max(altitudeMeters, 0.0f);
    const glm::vec3 sunDirN = glm::normalize(sunDir);
    (void)altClamped; (void)sunDirN;   // 保留签名（仅作文档）——实际无条件执行
    // cube: compute/transfer 写 -> SH 投影读（同 READ_ONLY 布局）
    ImageBarrier(commandBuffer, m_SkyCubeImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 0, 1, 0, 6);

    // 清零 SSBO（TRANSFER——跨 wg 竞态安全；shOut 144B + wgRes 55296B）
    vkCmdFillBuffer(commandBuffer, m_SkyCubeSHBuffer, 0, 144 + 96 * 36 * 4, 0);
    VkBufferMemoryBarrier fillBarrier = {};
    fillBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    fillBarrier.buffer = m_SkyCubeSHBuffer;
    fillBarrier.size = VK_WHOLE_SIZE;
    fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fillBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 1, &fillBarrier, 0, nullptr);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_ShProjPipe.pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_ShProjPipe.layout, 0, 1, &m_ShProjPipe.set, 0, nullptr);

    // pass 0：投影（原子累加）
    int pass = 0;
    vkCmdPushConstants(commandBuffer, m_ShProjPipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pass), &pass);
    vkCmdDispatch(commandBuffer, (m_SkyCubeW * m_SkyCubeW * 6 + 63) / 64, 1, 1);

    // 投影完成（跨 dispatch 天然同步）-> 乘核写
    VkBufferMemoryBarrier projBarrier = {};
    projBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    projBarrier.buffer = m_SkyCubeSHBuffer;
    projBarrier.size = VK_WHOLE_SIZE;
    projBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    projBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 1, &projBarrier, 0, nullptr);

    // pass 1：卷积核 A_l×π（1 wg——gid0 唯一写者）
    pass = 1;
    vkCmdPushConstants(commandBuffer, m_ShProjPipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pass), &pass);
    vkCmdDispatch(commandBuffer, 1, 1, 1);

    // SSBO: SHADER_WRITE -> SHADER_READ（合成 fragment 读）
    VkBufferMemoryBarrier bufBarrier = {};
    bufBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bufBarrier.buffer = m_SkyCubeSHBuffer;
    bufBarrier.size = VK_WHOLE_SIZE;
    bufBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bufBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 1, &bufBarrier, 0, nullptr);
}

// 2026-08-12 临时调试：回读 SSBO 打印 SH 系数（HOST_VISIBLE 内存）
void AtmosphereLUT::DumpSHCoefs(const char* tag)
{
    static int dumpCount = 0;
    if (dumpCount >= 5 || !m_SkyCubeSHBuffer) return;
    dumpCount++;
    void* data = nullptr;
    if (vkMapMemory(m_Device, m_SkyCubeSHMemory, 0, 144, 0, &data) != VK_SUCCESS) return;
    const float* f = (const float*)data;
    printf("[SH %s] c0=(%.4f %.4f %.4f) c1=(%.4f %.4f %.4f) c2=(%.4f %.4f %.4f) | c4=(%.4f %.4f %.4f) c8=(%.4f %.4f %.4f)\n",
        tag, f[0], f[1], f[2], f[4], f[5], f[6], f[8], f[9], f[10], f[16], f[17], f[18], f[32], f[33], f[34]);
    vkUnmapMemory(m_Device, m_SkyCubeSHMemory);
}
