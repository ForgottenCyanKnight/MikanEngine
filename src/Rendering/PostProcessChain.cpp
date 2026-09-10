#include "Rendering/PostProcessChain.h"
#include "Core/EngineGlobal.h"
#include "Core/EngineConfig.h"
#include "Core/VulkanContext.h"
#include "Core/Log.h"
#include "Core/VulkanManager.h"
#include "Rendering/TexturePool.h"
#include "Rendering/CloudNoise3D.h"
#include "Core/RenderGlobals.h"   // g_TexturePool
#include <SDL3/SDL_iostream.h>

#include <fstream>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <set>
#include <unordered_map>

// 内存类型查找（与 RenderTarget.cpp 同款，文件内 static）
static uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    LOGE("Failed to find suitable memory type!");
    return 0;
}

// SkyboxRenderer 若已加载则直接复用；可选资源缺失时返回 NULL，shader 侧退化处理）
static VkImageView s_BluenoiseView = VK_NULL_HANDLE;
static VkSampler s_BluenoiseSampler = VK_NULL_HANDLE;
// 不能使用 GetCurrentFrameIndex() 作为时域序列：它是 3 帧 in-flight 的槽位，
// 会按 0/1/2 循环。云的 STBN 相位需要真正单调递增的 frame sequence。
static uint32_t s_PostProcessTemporalFrame = 0;

static void EnsureBluenoise()
{
    if (s_BluenoiseView || !g_TexturePool) return;
    if (!g_TexturePool->GetTexture("bluenoise")) {
        if (!g_TexturePool->LoadTexture2D("bluenoise",
                                          EngineConfig::GetEngineTexturePath("bluenoise.png"),
                                          SamplerType::NearestRepeat))
            return;
    }
    s_BluenoiseView = g_TexturePool->GetImageView("bluenoise");
    s_BluenoiseSampler = g_TexturePool->GetSampler("bluenoise");
}

static VkImageView BluenoiseView()
{
    EnsureBluenoise();
    return s_BluenoiseView;
}

static VkSampler BluenoiseSampler()
{
    EnsureBluenoise();
    return s_BluenoiseSampler;
}

// Nubis/Meteoros 三张预计算纹理和高层 2D 云纹理只在第一次启用 cloud_view 时加载。
// 资源上传在 CloudNoise3D::Initialize 中通过一次性 command buffer 完成，
// 后续 cloud_view 只读取 SHADER_READ_ONLY_OPTIMAL 的 3D/2D image。
static void EnsureCloudNoise()
{
    CloudNoise3D& noise = GetCloudNoise3D();
    if (!noise.IsInitialized() &&
        !noise.Initialize(g_Device, g_PhysicalDevice, g_Allocator)) {
        return;
    }
}

static VkImageView CloudNoiseView()
{
    return GetCloudNoise3D().GetImageView();
}

static VkSampler CloudNoiseSampler()
{
    return GetCloudNoise3D().GetSampler();
}

static VkImageView CloudDetailNoiseView()
{
    return GetCloudNoise3D().GetDetailImageView();
}

static VkSampler CloudDetailNoiseSampler()
{
    return GetCloudNoise3D().GetDetailSampler();
}

static VkImageView CloudMotionNoiseView()
{
    return GetCloudNoise3D().GetCurlImageView();
}

static VkSampler CloudMotionNoiseSampler()
{
    return GetCloudNoise3D().GetCurlSampler();
}

static VkImageView CloudHighNoiseView()
{
    return GetCloudNoise3D().GetHighImageView();
}

static VkSampler CloudHighNoiseSampler()
{
    return GetCloudNoise3D().GetSampler();
}

static VkImageView CloudHighMapView()
{
    return GetCloudNoise3D().GetHighMapImageView();
}

static VkSampler CloudHighMapSampler()
{
    return GetCloudNoise3D().GetSampler();
}

// 懒加载（TexturePool 缓存）；areaTex 必须 Linear+Clamp、searchTex 必须 Nearest+Clamp（SMAA 要求）
static VkImageView s_SmaaAreaView = VK_NULL_HANDLE;
static VkSampler s_SmaaAreaSampler = VK_NULL_HANDLE;
static VkImageView s_SmaaSearchView = VK_NULL_HANDLE;
static VkSampler s_SmaaSearchSampler = VK_NULL_HANDLE;

static void EnsureSmaaTextures()
{
    if (s_SmaaAreaView || !g_TexturePool) return;
    if (!g_TexturePool->GetTexture("smaa_area")) {
        if (!g_TexturePool->LoadTexture2D("smaa_area",
                                          EngineConfig::GetEngineTexturePath("smaa_area.png"),
                                          SamplerType::LinearClamp))
            return;
    }
    s_SmaaAreaView = g_TexturePool->GetImageView("smaa_area");
    s_SmaaAreaSampler = g_TexturePool->GetSampler("smaa_area");
    if (!g_TexturePool->GetTexture("smaa_search")) {
        if (!g_TexturePool->LoadTexture2D("smaa_search",
                                          EngineConfig::GetEngineTexturePath("smaa_search.png"),
                                          SamplerType::NearestClamp))
            return;
    }
    s_SmaaSearchView = g_TexturePool->GetImageView("smaa_search");
    s_SmaaSearchSampler = g_TexturePool->GetSampler("smaa_search");
}

// ===== 轻量 JSON 提取（配置结构固定，无需完整 JSON 解析器）=====
namespace {

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\"");
    size_t b = s.find_last_not_of(" \t\r\n\"");
    return (a == std::string::npos || b == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
}

// 在 text 中定位 "key" 后的值起始位置（返回值第一个字符位置，找不到返回 npos）
size_t FindKeyValue(const std::string& text, const std::string& key, size_t from = 0) {
    size_t pos = text.find('"' + key + '"', from);
    if (pos == std::string::npos) return std::string::npos;
    size_t colon = text.find(':', pos + key.size() + 2);
    if (colon == std::string::npos) return std::string::npos;
    size_t v = text.find_first_not_of(" \t\r\n", colon + 1);
    return (v == std::string::npos) ? std::string::npos : v;
}

// 提取字符串值："key": "value"
std::string ExtractString(const std::string& text, const std::string& key, const std::string& def = "", size_t from = 0) {
    size_t v = FindKeyValue(text, key, from);
    if (v == std::string::npos || text[v] != '"') return def;
    size_t end = text.find('"', v + 1);
    if (end == std::string::npos) return def;
    return text.substr(v + 1, end - v - 1);
}

// 提取整数值："key": 123
bool ExtractInt(const std::string& text, const std::string& key, int& out, size_t from = 0) {
    size_t v = FindKeyValue(text, key, from);
    if (v == std::string::npos) return false;
    size_t end = text.find_first_of(",} \t\r\n", v);
    std::string num = text.substr(v, end == std::string::npos ? std::string::npos : end - v);
    try {
        out = std::stoi(num);
        return true;
    } catch (...) {
        return false;
    }
}

bool ExtractFloat(const std::string& text, const std::string& key, float& out, size_t from = 0) {
    size_t v = FindKeyValue(text, key, from);
    if (v == std::string::npos) return false;
    size_t end = text.find_first_of(",} \t\r\n", v);
    std::string num = text.substr(v, end == std::string::npos ? std::string::npos : end - v);
    try {
        out = std::stof(num);
        return true;
    } catch (...) {
        return false;
    }
}

// 在 text 中切出 balanced {} 或 [] 的子串（以 open 位置开始），返回 {content, 结束位置}
bool ExtractBalanced(const std::string& text, size_t open, std::string& content, size_t& endPos) {
    if (open >= text.size() || (text[open] != '{' && text[open] != '[')) return false;
    char close = (text[open] == '{') ? '}' : ']';
    int depth = 0;
    bool inStr = false;
    for (size_t i = open; i < text.size(); i++) {
        char c = text[i];
        if (c == '"' && (i == 0 || text[i - 1] != '\\')) inStr = !inStr;
        if (inStr) continue;
        if (c == text[open]) depth++;
        else if (c == close) {
            depth--;
            if (depth == 0) {
                content = text.substr(open, i - open + 1);
                endPos = i;
                return true;
            }
        }
    }
    return false;
}

} // namespace

// 配置驱动的后处理链实现（参考 FMDS Shadercfg.json pipline_sets：跨 pass 自由引用）

namespace {

VkFilter ParseFilter(const std::string& s) {
    if (s == "Nearest" || s == "Point") return VK_FILTER_NEAREST;
    return VK_FILTER_LINEAR;
}
VkSamplerAddressMode ParseWrap(const std::string& s) {
    if (s == "Repeat") return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (s == "Mirrored") return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

// 中间附件输出格式——per-pass 精度控制：HDR 效果用 rgba16f/r11g11b10，LDR 效果（SSAO 等）用 r8 省带宽
VkFormat ParseFormat(const std::string& s) {
    if (s == "rgba8" || s == "r8g8b8a8") return VK_FORMAT_R8G8B8A8_UNORM;
    if (s == "rg8" || s == "r8g8") return VK_FORMAT_R8G8_UNORM;
    if (s == "r11g11b10") return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    if (s == "r8") return VK_FORMAT_R8_UNORM;
    if (s == "r16") return VK_FORMAT_R16_UNORM;
    if (s == "rg16f") return VK_FORMAT_R16G16_SFLOAT;
    // 默认 rgba16f（线性 HDR，链精度基准）
    return VK_FORMAT_R16G16B16A16_SFLOAT;
}

} // namespace

PostProcessChain::~PostProcessChain() { Cleanup(); }

VkImage PostProcessChain::GetPassOutputImage(const std::string& passName) const
{
    for (const auto& rt : m_Runtime) {
        if (rt.passIndex < m_Passes.size() && m_Passes[rt.passIndex].name == passName) return rt.image;
    }
    return VK_NULL_HANDLE;
}

bool PostProcessChain::IsPassEnabled(const std::string& name) const
{
    for (const auto& def : m_Passes) {
        if (def.name == name) return def.enabled;
    }
    return false;
}

bool PostProcessChain::SetPassEnabled(const std::string& name, bool enabled)
{
    for (auto& def : m_Passes) {
        if (def.name != name) continue;
        if (def.enabled == enabled) return false;
        def.enabled = enabled;
        LOGI("[PostProcessChain] runtime pass '%s' -> %s",
             name.c_str(), enabled ? "enabled" : "disabled");
        return true;
    }
    return false;
}

int PostProcessChain::GetEnabledPassCount() const
{
    int count = 0;
    for (const auto& def : m_Passes) {
        if (def.enabled) ++count;
    }
    return count;
}

bool PostProcessChain::LoadFromJson(const std::string& path, bool preserveRuntimeStates)
{
    // 交换链重建会重新读取同一份配置；保留本次运行中由游戏设置页修改的
    // enable 状态，避免用户切换 VSync/旋转屏幕后画质恢复成 JSON 默认值。
    std::unordered_map<std::string, bool> previousStates;
    if (preserveRuntimeStates) {
        for (const auto& pass : m_Passes) previousStates[pass.name] = pass.enabled;
    }
    m_Passes.clear();
    LOGD("[PostProcessChain] loading config: %s", path.c_str());

    std::string text;
#ifdef __ANDROID__
    // Android：APK assets 不是真实文件系统，std::ifstream 读不到；SDL_IOFromFile 相对路径 fallback 到 assets://
    {
        SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
        if (io == nullptr) {
            LOGE("[PostProcessChain] Cannot open config: %s", path.c_str());
            return false;
        }
        Sint64 sz = SDL_GetIOSize(io);
        if (sz <= 0) { LOGE("[PostProcessChain] Empty config: %s", path.c_str()); SDL_CloseIO(io); return false; }
        text.resize((size_t)sz);
        if (SDL_ReadIO(io, text.data(), (size_t)sz) != (size_t)sz) { SDL_CloseIO(io); return false; }
        SDL_CloseIO(io);
    }
#else
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        LOGE("[PostProcessChain] Cannot open config: %s", path.c_str());
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
#endif
    LOGD("[PostProcessChain] config %zu bytes", text.size());

    // 定位 "passes": [...]
    size_t passesVal = FindKeyValue(text, "passes");
    if (passesVal == std::string::npos || text[passesVal] != '[') {
        LOGE("[PostProcessChain] config missing 'passes' array");
        return false;
    }
    std::string arr;
    size_t arrEnd = 0;
    if (!ExtractBalanced(text, passesVal, arr, arrEnd)) {
        LOGE("[PostProcessChain] cannot parse 'passes' array");
        return false;
    }

    // 逐个切 pass 对象 { ... }
    size_t pos = 0;
    int idx = 0;
    while (true) {
        size_t open = arr.find('{', pos);
        if (open == std::string::npos) break;
        std::string obj;
        size_t objEnd = 0;
        if (!ExtractBalanced(arr, open, obj, objEnd)) break;

        PassDef def;
        def.name = ExtractString(obj, "name");
        if (def.name.empty()) def.name = "pass_" + std::to_string(idx);
        def.shader = ExtractString(obj, "shader", "filter.frag.spv");
        def.outputFormat = ParseFormat(ExtractString(obj, "format", "rgba16f"));
        // ⚠️ 布尔值无引号，不能用 ExtractString（只认 "..." 字符串）——FindKeyValue 直接读
        {
            size_t ev = FindKeyValue(obj, "enable");
            if (ev != std::string::npos && obj.compare(ev, 5, "false") == 0) def.enabled = false;
        }
        if (auto previous = previousStates.find(def.name); previous != previousStates.end()) {
            def.enabled = previous->second;
        }
        def.before = ExtractString(obj, "before");
        {
            float s = 1.0f;
            if (ExtractFloat(obj, "scale", s)) {
                if (s > 0.0f && s <= 4.0f) def.scale = s;
                else LOGW("[PostProcessChain] pass '%s' scale %.3f 非法，回退 1.0", def.name.c_str(), s);
            }
        }

        // inputs: [ {slot,source,filter,wrap}, ... ]
        size_t inpVal = FindKeyValue(obj, "inputs");
        if (inpVal != std::string::npos && obj[inpVal] == '[') {
            std::string inpArr;
            size_t inpEnd = 0;
            if (ExtractBalanced(obj, inpVal, inpArr, inpEnd)) {
                size_t p2 = 0;
                while (true) {
                    size_t o2 = inpArr.find('{', p2);
                    if (o2 == std::string::npos) break;
                    std::string o2s;
                    size_t e2 = 0;
                    if (!ExtractBalanced(inpArr, o2, o2s, e2)) break;
                    PassInput pi;
                    int slot = 0;
                    if (ExtractInt(o2s, "slot", slot)) pi.slot = slot;
                    pi.source = ExtractString(o2s, "source", "composite");
                    pi.filter = ExtractString(o2s, "filter", "Linear");
                    pi.wrap = ExtractString(o2s, "wrap", "Clamp");
                    def.inputs.push_back(pi);
                    p2 = e2 + 1;
                }
            }
        }

        if (!def.before.empty() && def.inputs.empty()) {
            PassInput pi;
            pi.slot = 0;
            pi.source = "pass:" + def.before;
            pi.filter = "Linear";
            pi.wrap = "Clamp";
            def.inputs.push_back(pi);
        }

        m_Passes.push_back(def);
        LOGD("[PostProcessChain]   pass[%d] '%s' shader=%s inputs=%zu%s",
            idx, def.name.c_str(), def.shader.c_str(), def.inputs.size(),
            def.enabled ? "" : " DISABLED"); fflush(stdout);
        idx++;
        pos = objEnd + 1;
    }

    if (m_Passes.size() > 1) {
        std::vector<PassDef> sorted;
        std::vector<bool> placed(m_Passes.size(), false);
        bool allPlaced = false;
        while (!allPlaced) {
            allPlaced = true;
            bool progress = false;
            for (size_t i = 0; i < m_Passes.size(); i++) {
                if (placed[i]) continue;
                bool ok = true;
                if (!m_Passes[i].before.empty()) {
                    auto it = std::find_if(sorted.begin(), sorted.end(),
                        [&](const PassDef& p) { return p.name == m_Passes[i].before; });
                    if (it == sorted.end()) ok = false;
                }
                if (ok) {
                    sorted.push_back(m_Passes[i]);
                    placed[i] = true;
                    progress = true;
                    allPlaced = false;
                }
            }
            if (!progress) {   // 环或悬空 before：剩余追加末尾
                for (size_t i = 0; i < m_Passes.size(); i++) {
                    if (!placed[i]) {
                        if (!m_Passes[i].before.empty())
                            LOGW("[PostProcessChain] pass '%s' before='%s' 目标缺失/循环，追加末尾",
                                 m_Passes[i].name.c_str(), m_Passes[i].before.c_str());
                        sorted.push_back(m_Passes[i]);
                        placed[i] = true;
                    }
                }
                break;
            }
        }
        m_Passes = std::move(sorted);
    }

    LOGD("[PostProcessChain] loaded %zu passes from %s", m_Passes.size(), path.c_str());
    return !m_Passes.empty();
}

bool PostProcessChain::Build(uint32_t w, uint32_t h, VkRenderPass finalRenderPass)
{
    LOGD("[PostProcessChain] Build begin");
    Cleanup();
    if (m_Passes.empty()) return false;
    m_FinalRenderPass = finalRenderPass;
    m_Width = w;
    m_Height = h;

    const size_t n = m_Passes.size();
    m_Runtime.resize(n);

    for (size_t i = 0; i < n; i++) {
        PassRuntime& rt = m_Runtime[i];
        rt.passIndex = i;   // 不复制 PassDef（本环境 string/vector 拷贝入 PassRuntime 崩溃，已定位）
        const PassDef& def = m_Passes[i];

        if (!def.enabled) continue;

        // 末 pass 判定：最后一个启用的 pass 用 finalRenderPass（显示附件）——enable=false 的尾部 pass 不影响
        bool isLast = true;
        for (size_t j = i + 1; j < n; j++) {
            if (m_Passes[j].enabled) { isLast = false; break; }
        }

        if (!isLast) {
            // 中间附件（per-pass format：默认 R16G16B16A16_SFLOAT 线性 HDR；LDR 效果 pass 可在 JSON 声明 r8 等省带宽）
            const VkFormat outFmt = def.outputFormat;
            rt.width = (uint32_t)glm::max(1, (int)(w * def.scale));
            rt.height = (uint32_t)glm::max(1, (int)(h * def.scale));
            const uint32_t pw = rt.width;
            const uint32_t ph = rt.height;
            VkImageCreateInfo imageInfo = {};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.extent.width = pw;
            imageInfo.extent.height = ph;
            imageInfo.extent.depth = 1;
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = outFmt;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            // 链内的中间附件会被 TAA/GTAO/SSGI/Cloud 等历史机制在 pass 后
            // 拷贝到常驻纹理；显式声明 TRANSFER_SRC，避免在未开启验证层时
            // 看似可用、换驱动后却因 usage 不匹配而读到未定义结果。
            imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check_vk_result(vkCreateImage(g_Device, &imageInfo, g_Allocator, &rt.image));

            VkMemoryRequirements memReq;
            vkGetImageMemoryRequirements(g_Device, rt.image, &memReq);
            VkMemoryAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memReq.size;
            allocInfo.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check_vk_result(vkAllocateMemory(g_Device, &allocInfo, g_Allocator, &rt.memory));
            vkBindImageMemory(g_Device, rt.image, rt.memory, 0);

            VkImageViewCreateInfo viewInfo = {};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = rt.image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = outFmt;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            check_vk_result(vkCreateImageView(g_Device, &viewInfo, g_Allocator, &rt.view));

            // 中间 pass render pass：输出中间附件
            VkAttachmentDescription att = {};
            att.format = outFmt;
            att.samples = VK_SAMPLE_COUNT_1_BIT;
            // AMD 等分块架构可能暴露未定义 tile 内容；后续 pass 会采样整个附件，必须先确定性清零。
            att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            VkSubpassDescription subpass = {};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorRef;
            VkSubpassDependency dep = {};
            dep.srcSubpass = VK_SUBPASS_EXTERNAL;
            dep.dstSubpass = 0;
            dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            VkRenderPassCreateInfo rpInfo = {};
            rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            rpInfo.attachmentCount = 1;
            rpInfo.pAttachments = &att;
            rpInfo.subpassCount = 1;
            rpInfo.pSubpasses = &subpass;
            rpInfo.dependencyCount = 1;
            rpInfo.pDependencies = &dep;
            check_vk_result(vkCreateRenderPass(g_Device, &rpInfo, g_Allocator, &rt.renderPass));

            VkFramebufferCreateInfo fbInfo = {};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = rt.renderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments = &rt.view;
            fbInfo.width = pw;
            fbInfo.height = ph;
            fbInfo.layers = 1;
            check_vk_result(vkCreateFramebuffer(g_Device, &fbInfo, g_Allocator, &rt.framebuffer));

            uint32_t maxInputs = 8;
            for (const PassInput& input : def.inputs) {
                if (input.slot >= 0) {
                    maxInputs = std::max(maxInputs, static_cast<uint32_t>(input.slot) + 1u);
                }
            }
            rt.quad.Init(rt.renderPass, 0, def.shader.c_str(), maxInputs);
        } else {
            // 末 pass：输出到 final render pass（Execute 每帧传 framebuffer）；忽略 scale 恒全尺寸
            rt.width = (uint32_t)w;
            rt.height = (uint32_t)h;
            uint32_t maxInputs = 8;
            for (const PassInput& input : def.inputs) {
                if (input.slot >= 0) {
                    maxInputs = std::max(maxInputs, static_cast<uint32_t>(input.slot) + 1u);
                }
            }
            rt.quad.Init(m_FinalRenderPass, 0, def.shader.c_str(), maxInputs);
        }
    }

    m_Built = true;
    LOGD("[PostProcessChain] built %zu passes (%ux%u)", n, w, h);
    return true;
}

void PostProcessChain::Cleanup()
{
    if (g_Device == VK_NULL_HANDLE) return;
    for (PassRuntime& rt : m_Runtime) {
        rt.quad.Cleanup();
        if (rt.framebuffer) vkDestroyFramebuffer(g_Device, rt.framebuffer, g_Allocator);
        if (rt.renderPass) vkDestroyRenderPass(g_Device, rt.renderPass, g_Allocator);
        if (rt.view) vkDestroyImageView(g_Device, rt.view, g_Allocator);
        if (rt.image) vkDestroyImage(g_Device, rt.image, g_Allocator);
        if (rt.memory) vkFreeMemory(g_Device, rt.memory, g_Allocator);
        rt = PassRuntime{};
    }
    m_Runtime.clear();
    m_FinalRenderPass = VK_NULL_HANDLE;
    m_Built = false;
}

void PostProcessChain::Resize(uint32_t w, uint32_t h)
{
    if (!m_Built || (w == m_Width && h == m_Height)) return;
    // render pass 与尺寸无关；重建中间附件 + framebuffer；quad pipeline 用动态 viewport 无需重建
    VkRenderPass finalRP = m_FinalRenderPass;
    for (PassRuntime& rt : m_Runtime) {
        rt.quad.Cleanup();
        if (rt.framebuffer) vkDestroyFramebuffer(g_Device, rt.framebuffer, g_Allocator);
        if (rt.renderPass) vkDestroyRenderPass(g_Device, rt.renderPass, g_Allocator);
        if (rt.view) vkDestroyImageView(g_Device, rt.view, g_Allocator);
        if (rt.image) vkDestroyImage(g_Device, rt.image, g_Allocator);
        if (rt.memory) vkFreeMemory(g_Device, rt.memory, g_Allocator);
        rt = PassRuntime{};
    }
    m_Built = false;
    Build(w, h, finalRP);
}

VkSampler PostProcessChain::SamplerFor(const PassInput& in)
{
    // 用第一个已初始化 pass 的 quad 的采样器缓存（m_Runtime 现在还保留
    // enable=false 的定义，它们没有初始化 quad，不能再固定取槽 0）。
    for (PassRuntime& rt : m_Runtime) {
        if (rt.quad.GetPipeline() != VK_NULL_HANDLE) {
            return rt.quad.GetOrCreateSampler(ParseFilter(in.filter), ParseWrap(in.wrap));
        }
    }
    return VK_NULL_HANDLE;
}

bool PostProcessChain::ResolveSource(const PassInput& in, const ExternalInputs& ext, PostProcessQuad::InputBinding& out,
                                     size_t currentPassIndex)
{
    out.slot = (uint32_t)in.slot;
    const std::string& s = in.source;
    if (s == "pass:before") {
        // 运行时关闭中间 pass 后，向前寻找最近的有效输出；不能把已禁用
        // pass 的旧/空附件当作输入，否则切换后会出现一帧黑图或残留内容。
        for (size_t p = currentPassIndex; p > 0; --p) {
            const PassRuntime& prev = m_Runtime[p - 1];
            if (prev.passIndex >= m_Passes.size() || !m_Passes[prev.passIndex].enabled) continue;
            if (prev.view != VK_NULL_HANDLE) {
                out.view = prev.view;
                out.sampler = SamplerFor(in);
                return true;
            }
        }
        // 首个启用 pass 在前置 pass 被临时禁用时合法回退到 composite；只提示一次，避免每帧刷屏。
        static bool s_loggedBeforeFallback = false;
        if (!s_loggedBeforeFallback) {
            s_loggedBeforeFallback = true;
            LOGW("[PostProcessChain] 'pass:before' 无前序 pass（index=%zu），回退 composite", currentPassIndex);
        }
        out.view = ext.compositeView;
        out.sampler = SamplerFor(in);
        out.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "composite") {
        out.view = ext.compositeView;
        out.sampler = m_Runtime.empty() ? VK_NULL_HANDLE : SamplerFor(in);
        // composite 附件保持 COLOR_ATTACHMENT_OPTIMAL（finalLayout 不做隐式转换）——采样布局必须与实际一致
        out.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "gbuffer0" || s == "gbuffer") {
        out.view = ext.gbufferView;
        out.sampler = SamplerFor(in);
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "gbuffer1") {
        out.view = ext.gbuffer1View;
        out.sampler = SamplerFor(in);
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "gbuffer2") {
        out.view = ext.gbuffer2View;
        out.sampler = SamplerFor(in);
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "emissive") {
        out.view = ext.gbuffer2View;
        out.sampler = SamplerFor(in);
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "depth") {
        out.view = ext.depthView;
        out.sampler = SamplerFor(in);
        // 几何 render pass 的 depth attachment finalLayout 与 descriptor 必须一致。
        // 不能沿用颜色/普通纹理默认的 SHADER_READ_ONLY_OPTIMAL；否则 GTAO、cloud_view、TAA
        // 采样同一张 D24/D32 深度图时会读到未定义结果，而合成 subpass 仍可能正常。
        out.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "csm") {   // 2026-：CSM 阴影 2D array（gtao 半分辨率体积光）——自定义 shadow sampler
        out.view = ext.csmShadowView;
        out.sampler = ext.csmShadowSampler;
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "ssgi_history") {
        out.view = ext.ssgiHistoryView;
        out.sampler = ext.ssgiHistorySampler;
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "cloud_history") {
        out.view = ext.cloudHistoryView;
        out.sampler = ext.cloudHistorySampler;
        out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return out.view != VK_NULL_HANDLE && out.sampler != VK_NULL_HANDLE;
    }
    if (s == "history") {
        out.view = ext.historyView;
        out.sampler = ext.historySampler;
        return out.view != VK_NULL_HANDLE && out.sampler != VK_NULL_HANDLE;
    }
    if (s == "bluenoise") {
        out.view = BluenoiseView();
        out.sampler = BluenoiseSampler();
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "cloud_noise" || s == "cloud_base_shape") {
        // 兼容旧 source 名称；Nubis base shape 的 128³ RGBA 纹理。
        out.view = CloudNoiseView();
        out.sampler = CloudNoiseSampler();
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "cloud_details") {
        out.view = CloudDetailNoiseView();
        out.sampler = CloudDetailNoiseSampler();
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "cloud_motion") {
        out.view = CloudMotionNoiseView();
        out.sampler = CloudMotionNoiseSampler();
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "cloud_high") {
        out.view = CloudHighNoiseView();
        out.sampler = CloudHighNoiseSampler();
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "cloud_high_map") {
        out.view = CloudHighMapView();
        out.sampler = CloudHighMapSampler();
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "atmo_transmittance") {
        out.view = ext.atmoTransmittanceView;
        out.sampler = ext.atmoTransmittanceSampler;
        out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return out.view != VK_NULL_HANDLE && out.sampler != VK_NULL_HANDLE;
    }
    if (s == "atmo_scattering") {
        out.view = ext.atmoScatteringView;
        out.sampler = ext.atmoScatteringSampler;
        out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return out.view != VK_NULL_HANDLE && out.sampler != VK_NULL_HANDLE;
    }
    if (s == "skyrt") {
        out.view = ext.skyView;
        out.sampler = ext.skySampler;
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "area_tex") {
        EnsureSmaaTextures();
        out.view = s_SmaaAreaView;
        out.sampler = s_SmaaAreaSampler;
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "search_tex") {
        EnsureSmaaTextures();
        out.view = s_SmaaSearchView;
        out.sampler = s_SmaaSearchSampler;
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "cmaa_weight") {
        out.view = ext.cmaaWeightView;
        out.sampler = ext.cmaaWeightSampler;
        out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "gbuffer_motion") {
        out.view = ext.gbufferMotionView;
        out.sampler = SamplerFor(in);
        out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "taa_history") {
        out.view = ext.taaHistoryView;
        out.sampler = ext.taaHistorySampler;
        out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return out.view != VK_NULL_HANDLE;
    }
    if (s == "cmaa_result") {
        out.view = ext.cmaaWeightView;
        out.sampler = ext.cmaaWeightSampler;
        out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return out.view != VK_NULL_HANDLE;
    }
    if (s.rfind("pass:", 0) == 0) {
        std::string target = s.substr(5);
        for (const PassRuntime& rt : m_Runtime) {
            if (rt.passIndex < m_Passes.size() &&
                m_Passes[rt.passIndex].name == target &&
                m_Passes[rt.passIndex].enabled &&
                rt.view != VK_NULL_HANDLE) {
                out.view = rt.view;   // 前方 pass 的输出（中间附件 view）；末 pass 无输出不可引用
                out.sampler = SamplerFor(in);
                return true;
            }
        }
        LOGW("[PostProcessChain] pass reference '%s' 未找到（被禁用/删除？）→ 回退 composite", s.c_str());
        out.view = ext.compositeView;
        out.sampler = SamplerFor(in);
        out.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        return out.view != VK_NULL_HANDLE;
    }
    LOGE("[PostProcessChain] unknown source '%s'", s.c_str());
    return false;
}

void PostProcessChain::Execute(VkCommandBuffer cmd, int w, int h, ExternalInputs& ext, VkFramebuffer finalFB)
{
    if (!m_Built) return;
    const float temporalFrame = static_cast<float>(s_PostProcessTemporalFrame++ % 65536u);

    bool usesCloudNoise = false;
    for (const PassDef& pass : m_Passes) {
        if (!pass.enabled) continue;
        for (const PassInput& input : pass.inputs) {
            if (input.source == "cloud_noise" ||
                input.source == "cloud_base_shape" ||
                input.source == "cloud_details" ||
                input.source == "cloud_motion" ||
                input.source == "cloud_high" ||
                input.source == "cloud_high_map") {
                usesCloudNoise = true;
                break;
            }
        }
        if (usesCloudNoise) break;
    }
    if (usesCloudNoise) EnsureCloudNoise();

    for (size_t i = 0; i < m_Runtime.size(); i++) {
        PassRuntime& rt = m_Runtime[i];
        const PassDef& def = m_Passes[rt.passIndex];

        if (!def.enabled) continue;

        // 1) 输入 barrier：本 pass 的所有输入（外部源 + 前方 pass 输出）从写入转片元采样可见
        //    已知输入 image 的来源：
        //      - "composite" → ext.compositeView（合成 pass 已 barrier，但重复 barrier 无害）
        //      - "pass:<name>" → 前方 pass 的 rt.image
        //      - "gbuffer0"/"depth" → ext.gbufferView/ext.depthView（几何 pass 已 barrier）
        //      - "skyrt" → ext.skyView（RenderSkyRT 已 barrier）
        std::vector<VkImage> barrierImages;
        for (const PassInput& in : def.inputs) {
            if (in.source == "composite") {
                // 外部 composite：由调用方在合成 pass 后 barrier（此处跳过）
            } else if (in.source.rfind("pass:", 0) == 0) {
                std::string target = in.source.substr(5);
                for (PassRuntime& prev : m_Runtime) {
                    if (prev.passIndex < m_Passes.size() &&
                        m_Passes[prev.passIndex].enabled &&
                        m_Passes[prev.passIndex].name == target &&
                        prev.image != VK_NULL_HANDLE) {
                        barrierImages.push_back(prev.image);
                        break;
                    }
                }
            }
        }
        if (!barrierImages.empty()) {
            std::vector<VkImageMemoryBarrier> barriers;
            for (VkImage img : barrierImages) {
                VkImageMemoryBarrier b = {};
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = img;
                b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barriers.push_back(b);
            }
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, (uint32_t)barriers.size(), barriers.data());
        }

        // 2) 解析输入 → descriptor
        std::vector<PostProcessQuad::InputBinding> inputs;
        for (const PassInput& in : def.inputs) {
            PostProcessQuad::InputBinding b;
            if (ResolveSource(in, ext, b, i) && b.view != VK_NULL_HANDLE) {
                if (b.sampler == VK_NULL_HANDLE) b.sampler = SamplerFor(in);
                inputs.push_back(b);
            } else {
                static std::set<std::string> s_reportedUnresolved;
                std::string key = std::string(def.name) + "|" + std::to_string(in.slot) + "|" + in.source;
                if (s_reportedUnresolved.insert(key).second) {
                    LOGE("[PostProcessChain] pass '%s' input slot %d source '%s' unresolved",
                        def.name.c_str(), in.slot, in.source.c_str());
                }
            }
        }

        // 3) render pass + 绘制
        VkRenderPass rp = rt.renderPass ? rt.renderPass : m_FinalRenderPass;
        VkFramebuffer fb = rt.framebuffer ? rt.framebuffer : finalFB;
        if (rp == VK_NULL_HANDLE || fb == VK_NULL_HANDLE) {
            LOGW("[PostProcessChain] pass '%s' SKIPPED (rp=%p fb=%p)", def.name.c_str(), (void*)rp, (void*)fb);
            continue;
        }

        VkRenderPassBeginInfo rpBegin = {};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = rp;
        rpBegin.framebuffer = fb;
        const uint32_t pw = rt.width ? rt.width : (uint32_t)w;
        const uint32_t ph = rt.height ? rt.height : (uint32_t)h;
        rpBegin.renderArea.offset = { 0, 0 };
        rpBegin.renderArea.extent = { pw, ph };
        rpBegin.clearValueCount = 1;
        VkClearValue clear = {};
        clear.color = { 0.0f, 0.0f, 0.0f, 1.0f };
        rpBegin.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        rt.quad.SetInputs(inputs);
        // 写 Camera UBO（每帧每 pass 更新一次）
        rt.quad.UpdateCameraUBO(ext.cameraUBO);
        // push 只传 frameInfo
        ext.pushData.frameInfo.x = temporalFrame;
        rt.quad.Render(cmd, (int)pw, (int)ph, &ext.pushData);
        vkCmdEndRenderPass(cmd);

        //   edges 先在 SHADER_READ_ONLY（render pass finalLayout）采样 → 转 GENERAL（process/apply）→ 转回 READ_ONLY
        if (m_PassHook && def.name == m_HookAfterPass && rt.image != VK_NULL_HANDLE && rt.view != VK_NULL_HANDLE) {
            m_PassHook(cmd, rt.view, rt.image);
        }
    }
}
