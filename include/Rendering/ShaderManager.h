#pragma once
#include "Platform/Export.h"
#ifndef SHADER_MANAGER_H
#define SHADER_MANAGER_H

#include <vulkan/vulkan.h>
#include <string>
#include <unordered_map>
#include <filesystem>

// 着色器模块管理
class MIKAN_API ShaderManager {
public:
    static ShaderManager& GetInstance();
    
    // 初始化
    void Init(const std::string& shaderDirectory);
    void Cleanup();
    
    // 编译着色器
    bool CompileShader(const std::string& shaderPath, const std::string& outputPath);
    bool CompileAllShaders();
    
    // 加载 SPIR-V
    VkShaderModule LoadShaderModule(const std::string& shaderPath);
    VkShaderModule GetShaderModule(const std::string& shaderName);
    
    // 获取着色器路径
    std::string GetShaderPath(const std::string& shaderName) const;
    std::string GetCompiledShaderPath(const std::string& shaderName) const;
    
    // 重新编译（用于热重载）
    bool RecompileShader(const std::string& shaderName);
    
private:
    ShaderManager() = default;
    ~ShaderManager();
    
    std::string ShaderStageToString(VkShaderStageFlagBits stage);
    VkShaderStageFlagBits GetShaderStage(const std::string& fileName);
    
    std::string m_ShaderDirectory;
    std::unordered_map<std::string, VkShaderModule> m_ShaderModules;
    std::unordered_map<std::string, std::string> m_ShaderPaths;
};

#endif // SHADER_MANAGER_H
