#include "ShaderManager.h"
#include "EngineGlobal.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

ShaderManager& ShaderManager::GetInstance() {
    static ShaderManager instance;
    return instance;
}

ShaderManager::~ShaderManager() {
    Cleanup();
}

void ShaderManager::Init(const std::string& shaderDirectory) {
    m_ShaderDirectory = shaderDirectory;
    std::cout << "[ShaderManager] Initialized with directory: " << shaderDirectory << std::endl;
}

void ShaderManager::Cleanup() {
    for (auto& [name, module] : m_ShaderModules) {
        if (module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(g_Device, module, nullptr);
        }
    }
    m_ShaderModules.clear();
}

VkShaderModule ShaderManager::LoadShaderModule(const std::string& shaderPath) {
    std::ifstream file(shaderPath, std::ios::ate | std::ios::binary);
    
    if (!file.is_open()) {
        std::cerr << "[ShaderManager] Failed to open shader file: " << shaderPath << std::endl;
        return VK_NULL_HANDLE;
    }
    
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();
    
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = buffer.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());
    
    VkShaderModule shaderModule;
    if (vkCreateShaderModule(g_Device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        std::cerr << "[ShaderManager] Failed to create shader module: " << shaderPath << std::endl;
        return VK_NULL_HANDLE;
    }
    
    return shaderModule;
}

VkShaderModule ShaderManager::GetShaderModule(const std::string& shaderName) {
    auto it = m_ShaderModules.find(shaderName);
    if (it != m_ShaderModules.end()) {
        return it->second;
    }
    
    // 尝试加载
    std::string shaderPath = GetCompiledShaderPath(shaderName);
    VkShaderModule module = LoadShaderModule(shaderPath);
    
    if (module != VK_NULL_HANDLE) {
        m_ShaderModules[shaderName] = module;
    }
    
    return module;
}

std::string ShaderManager::GetShaderPath(const std::string& shaderName) const {
    return m_ShaderDirectory + "/" + shaderName;
}

std::string ShaderManager::GetCompiledShaderPath(const std::string& shaderName) const {
    // 假设编译后的着色器在 compiled 子目录
    std::filesystem::path path(shaderName);
    std::string baseName = path.stem().string();
    return m_ShaderDirectory + "/compiled/" + baseName + ".spv";
}

bool ShaderManager::CompileShader(const std::string& shaderPath, const std::string& outputPath) {
    // 使用 glslc 编译器
    std::string command = "glslc \"" + shaderPath + "\" -o \"" + outputPath + "\"";
    
    int result = std::system(command.c_str());
    
    if (result != 0) {
        std::cerr << "[ShaderManager] Compilation failed!" << std::endl;
        return false;
    }
    
    return true;
}

bool ShaderManager::CompileAllShaders() {
    std::vector<std::pair<std::string, std::string>> shaders = {
        {"model.vert", "model.vert.spv"},
        {"model.frag", "model.frag.spv"},
        {"model_array.vert", "model_array.vert.spv"},
        {"model_array.frag", "model_array.frag.spv"},
        {"skybox.vert", "skybox.vert.spv"},
        {"skybox.frag", "skybox.frag.spv"},
        {"wireframe.vert", "wireframe.vert.spv"},
        {"wireframe.frag", "wireframe.frag.spv"},
        {"fullscreen.vert", "fullscreen.vert.spv"},
    };
    
    bool allSuccess = true;
    
    for (const auto& [source, target] : shaders) {
        std::string sourcePath = m_ShaderDirectory + "/" + source;
        std::string targetPath = m_ShaderDirectory + "/compiled/" + target;
        
        // 确保输出目录存在
        std::filesystem::create_directories(std::filesystem::path(targetPath).parent_path());
        
        if (!CompileShader(sourcePath, targetPath)) {
            allSuccess = false;
        }
    }
    
    return allSuccess;
}

bool ShaderManager::RecompileShader(const std::string& shaderName) {
    // 销毁旧模块
    auto it = m_ShaderModules.find(shaderName);
    if (it != m_ShaderModules.end()) {
        vkDestroyShaderModule(g_Device, it->second, nullptr);
        m_ShaderModules.erase(it);
    }
    
    // 重新编译
    std::string sourcePath = GetShaderPath(shaderName);
    std::string targetPath = GetCompiledShaderPath(shaderName);
    
    return CompileShader(sourcePath, targetPath);
}

std::string ShaderManager::ShaderStageToString(VkShaderStageFlagBits stage) {
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT: return "vert";
        case VK_SHADER_STAGE_FRAGMENT_BIT: return "frag";
        case VK_SHADER_STAGE_COMPUTE_BIT: return "comp";
        default: return "unknown";
    }
}

VkShaderStageFlagBits ShaderManager::GetShaderStage(const std::string& fileName) {
    if (fileName.find(".vert") != std::string::npos) return VK_SHADER_STAGE_VERTEX_BIT;
    if (fileName.find(".frag") != std::string::npos) return VK_SHADER_STAGE_FRAGMENT_BIT;
    if (fileName.find(".comp") != std::string::npos) return VK_SHADER_STAGE_COMPUTE_BIT;
    return VK_SHADER_STAGE_ALL;
}
