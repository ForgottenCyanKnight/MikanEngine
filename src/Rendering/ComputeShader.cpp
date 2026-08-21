#include "ComputeShader.h"
#include <fstream>
#include <vector>
#include <iostream>
#include <filesystem>
#include "RendererBase.h"


#include <SDL3/SDL.h>
#include <glm/mat4x4.hpp>

bool ComputeShader::Init(VkDevice device, VkPhysicalDevice physicalDevice, const std::string& shaderPath, uint32_t width, uint32_t height) {
    printf("ComputeShader::Init: Starting initialization...");
    
    m_Device = device;
    m_Width = width;
    m_Height = height;
    m_ShaderPath = shaderPath;

    if (m_Device == VK_NULL_HANDLE) {
        printf("ComputeShader::Init: Device is null");
        return false;
    }

    printf("ComputeShader::Init: Loading shader from %s", shaderPath.c_str());
    
    if (!LoadShaderModule(shaderPath, m_ShaderModule)) {
        printf("[ERROR] Failed to load compute shader");
        return false;
    }

    printf("ComputeShader::Init: Shader module loaded successfully");

    VkPipelineShaderStageCreateInfo shaderStageInfo{};
    shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStageInfo.module = m_ShaderModule;
    shaderStageInfo.pName = "main";

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    
    // 检查是否是光线追踪着色器
    m_IsRaytracingShader = (shaderPath.find("raytracing") != std::string::npos);
    
    if (m_IsRaytracingShader) {
        // 光线追踪着色器的描述符集布局
        // 存储图像绑定 (outputImage)
        VkDescriptorSetLayoutBinding storageImageBinding;
        storageImageBinding.binding = 0;
        storageImageBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        storageImageBinding.descriptorCount = 1;
        storageImageBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        storageImageBinding.pImmutableSamplers = nullptr;
        bindings.push_back(storageImageBinding);
        
        // SSBO绑定 - 模型变换
        VkDescriptorSetLayoutBinding transformsBinding;
        transformsBinding.binding = 1;
        transformsBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        transformsBinding.descriptorCount = 1;
        transformsBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        transformsBinding.pImmutableSamplers = nullptr;
        bindings.push_back(transformsBinding);
        
        // SSBO绑定 - 模型元数据
        VkDescriptorSetLayoutBinding metadataBinding;
        metadataBinding.binding = 2;
        metadataBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        metadataBinding.descriptorCount = 1;
        metadataBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        metadataBinding.pImmutableSamplers = nullptr;
        bindings.push_back(metadataBinding);
        
        // SSBO绑定 - 模型偏移量
        VkDescriptorSetLayoutBinding offsetsBinding;
        offsetsBinding.binding = 3;
        offsetsBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        offsetsBinding.descriptorCount = 1;
        offsetsBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        offsetsBinding.pImmutableSamplers = nullptr;
        bindings.push_back(offsetsBinding);
        
        // SSBO绑定 - 顶点
        VkDescriptorSetLayoutBinding verticesBinding;
        verticesBinding.binding = 4;
        verticesBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        verticesBinding.descriptorCount = 1;
        verticesBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        verticesBinding.pImmutableSamplers = nullptr;
        bindings.push_back(verticesBinding);
        
        // SSBO绑定 - 三角形
        VkDescriptorSetLayoutBinding trianglesBinding;
        trianglesBinding.binding = 5;
        trianglesBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        trianglesBinding.descriptorCount = 1;
        trianglesBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        trianglesBinding.pImmutableSamplers = nullptr;
        bindings.push_back(trianglesBinding);
        
        // SSBO绑定 - BVH节点
        VkDescriptorSetLayoutBinding bvhNodesBinding;
        bvhNodesBinding.binding = 6;
        bvhNodesBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bvhNodesBinding.descriptorCount = 1;
        bvhNodesBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bvhNodesBinding.pImmutableSamplers = nullptr;
        bindings.push_back(bvhNodesBinding);
        
        // SSBO绑定 - 模型纹理数据
        VkDescriptorSetLayoutBinding textureDataBinding;
        textureDataBinding.binding = 7;
        textureDataBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        textureDataBinding.descriptorCount = 1;
        textureDataBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        textureDataBinding.pImmutableSamplers = nullptr;
        bindings.push_back(textureDataBinding);
        
        // 纹理绑定 - ForwardTexture0
        VkDescriptorSetLayoutBinding forwardTexture0Binding;
        forwardTexture0Binding.binding = 8;
        forwardTexture0Binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        forwardTexture0Binding.descriptorCount = 1;
        forwardTexture0Binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        forwardTexture0Binding.pImmutableSamplers = nullptr;
        bindings.push_back(forwardTexture0Binding);
        
        // 纹理绑定 - ForwardTexture1
        VkDescriptorSetLayoutBinding forwardTexture1Binding;
        forwardTexture1Binding.binding = 9;
        forwardTexture1Binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        forwardTexture1Binding.descriptorCount = 1;
        forwardTexture1Binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        forwardTexture1Binding.pImmutableSamplers = nullptr;
        bindings.push_back(forwardTexture1Binding);
        
        // 纹理绑定 - ForwardTexture2
        VkDescriptorSetLayoutBinding forwardTexture2Binding;
        forwardTexture2Binding.binding = 10;
        forwardTexture2Binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        forwardTexture2Binding.descriptorCount = 1;
        forwardTexture2Binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        forwardTexture2Binding.pImmutableSamplers = nullptr;
        bindings.push_back(forwardTexture2Binding);
        
        // 纹理绑定 - ForwardTexture3
        VkDescriptorSetLayoutBinding forwardTexture3Binding;
        forwardTexture3Binding.binding = 11;
        forwardTexture3Binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        forwardTexture3Binding.descriptorCount = 1;
        forwardTexture3Binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        forwardTexture3Binding.pImmutableSamplers = nullptr;
        bindings.push_back(forwardTexture3Binding);
        
        // 纹理绑定 - 天空盒
        VkDescriptorSetLayoutBinding skyboxBinding;
        skyboxBinding.binding = 12;
        skyboxBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        skyboxBinding.descriptorCount = 1;
        skyboxBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        skyboxBinding.pImmutableSamplers = nullptr;
        bindings.push_back(skyboxBinding);
        
        // 纹理绑定 - 蓝噪声
        VkDescriptorSetLayoutBinding blueNoiseBinding;
        blueNoiseBinding.binding = 13;
        blueNoiseBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        blueNoiseBinding.descriptorCount = 1;
        blueNoiseBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        blueNoiseBinding.pImmutableSamplers = nullptr;
        bindings.push_back(blueNoiseBinding);
        
        // 纹理绑定 - 深度纹理
        VkDescriptorSetLayoutBinding depthTextureBinding;
        depthTextureBinding.binding = 14;
        depthTextureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        depthTextureBinding.descriptorCount = 1;
        depthTextureBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        depthTextureBinding.pImmutableSamplers = nullptr;
        bindings.push_back(depthTextureBinding);
        
        // 纹理绑定 - 反照率纹理
        VkDescriptorSetLayoutBinding albedoTextureBinding;
        albedoTextureBinding.binding = 15;
        albedoTextureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        albedoTextureBinding.descriptorCount = 1;
        albedoTextureBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        albedoTextureBinding.pImmutableSamplers = nullptr;
        bindings.push_back(albedoTextureBinding);
        
        // 纹理绑定 - 模型纹理数组
        VkDescriptorSetLayoutBinding modelTexturesBinding;
        modelTexturesBinding.binding = 16;
        modelTexturesBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        modelTexturesBinding.descriptorCount = 1;
        modelTexturesBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        modelTexturesBinding.pImmutableSamplers = nullptr;
        bindings.push_back(modelTexturesBinding);
        
        // 纹理绑定 - 历史帧纹理
        VkDescriptorSetLayoutBinding historyTextureBinding;
        historyTextureBinding.binding = 17;
        historyTextureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        historyTextureBinding.descriptorCount = 1;
        historyTextureBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        historyTextureBinding.pImmutableSamplers = nullptr;
        bindings.push_back(historyTextureBinding);
    } else {
        if (shaderPath.find("taa") != std::string::npos) {
            // TAA 计算着色器的描述符集布局 - 统一 Binding 布局
            // Binding 0: 输出图像
            VkDescriptorSetLayoutBinding outputImageBinding;
            outputImageBinding.binding = 0;
            outputImageBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            outputImageBinding.descriptorCount = 1;
            outputImageBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            outputImageBinding.pImmutableSamplers = nullptr;
            bindings.push_back(outputImageBinding);
            
            // Binding 1: 深度纹理
            VkDescriptorSetLayoutBinding depthTextureBinding;
            depthTextureBinding.binding = 1;
            depthTextureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            depthTextureBinding.descriptorCount = 1;
            depthTextureBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            depthTextureBinding.pImmutableSamplers = nullptr;
            bindings.push_back(depthTextureBinding);
            
            // Binding 2-5: MRT 0-3
            // MRT 0 - 当前帧颜色
            VkDescriptorSetLayoutBinding forwardTexture0Binding;
            forwardTexture0Binding.binding = 2;
            forwardTexture0Binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            forwardTexture0Binding.descriptorCount = 1;
            forwardTexture0Binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            forwardTexture0Binding.pImmutableSamplers = nullptr;
            bindings.push_back(forwardTexture0Binding);
            
            // MRT 1 - 保留
            VkDescriptorSetLayoutBinding forwardTexture1Binding;
            forwardTexture1Binding.binding = 3;
            forwardTexture1Binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            forwardTexture1Binding.descriptorCount = 1;
            forwardTexture1Binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            forwardTexture1Binding.pImmutableSamplers = nullptr;
            bindings.push_back(forwardTexture1Binding);
            
            // MRT 2 - 运动矢量
            VkDescriptorSetLayoutBinding forwardTexture2Binding;
            forwardTexture2Binding.binding = 4;
            forwardTexture2Binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            forwardTexture2Binding.descriptorCount = 1;
            forwardTexture2Binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            forwardTexture2Binding.pImmutableSamplers = nullptr;
            bindings.push_back(forwardTexture2Binding);
            
            // MRT 3 - 保留
            VkDescriptorSetLayoutBinding forwardTexture3Binding;
            forwardTexture3Binding.binding = 5;
            forwardTexture3Binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            forwardTexture3Binding.descriptorCount = 1;
            forwardTexture3Binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            forwardTexture3Binding.pImmutableSamplers = nullptr;
            bindings.push_back(forwardTexture3Binding);
            
            // Binding 6: 历史帧
            VkDescriptorSetLayoutBinding historyTextureBinding;
            historyTextureBinding.binding = 6;
            historyTextureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            historyTextureBinding.descriptorCount = 1;
            historyTextureBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            historyTextureBinding.pImmutableSamplers = nullptr;
            bindings.push_back(historyTextureBinding);
        } else if (shaderPath.find("copy_vertex_data") != std::string::npos) {
            // 顶点数据复制着色器的描述符集布局
            // Binding 0: 复制命令缓冲区
            VkDescriptorSetLayoutBinding copyCommandsBinding;
            copyCommandsBinding.binding = 0;
            copyCommandsBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            copyCommandsBinding.descriptorCount = 1;
            copyCommandsBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            copyCommandsBinding.pImmutableSamplers = nullptr;
            bindings.push_back(copyCommandsBinding);
            
            // Binding 1: 源顶点缓冲区
            VkDescriptorSetLayoutBinding sourceVerticesBinding;
            sourceVerticesBinding.binding = 1;
            sourceVerticesBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            sourceVerticesBinding.descriptorCount = 1;
            sourceVerticesBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            sourceVerticesBinding.pImmutableSamplers = nullptr;
            bindings.push_back(sourceVerticesBinding);
            
            // Binding 2: 目标顶点缓冲区
            VkDescriptorSetLayoutBinding destinationVerticesBinding;
            destinationVerticesBinding.binding = 2;
            destinationVerticesBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            destinationVerticesBinding.descriptorCount = 1;
            destinationVerticesBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            destinationVerticesBinding.pImmutableSamplers = nullptr;
            bindings.push_back(destinationVerticesBinding);
            
            // Binding 3: 源索引缓冲区
            VkDescriptorSetLayoutBinding sourceIndicesBinding;
            sourceIndicesBinding.binding = 3;
            sourceIndicesBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            sourceIndicesBinding.descriptorCount = 1;
            sourceIndicesBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            sourceIndicesBinding.pImmutableSamplers = nullptr;
            bindings.push_back(sourceIndicesBinding);
            
            // Binding 4: 目标索引缓冲区
            VkDescriptorSetLayoutBinding destinationIndicesBinding;
            destinationIndicesBinding.binding = 4;
            destinationIndicesBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            destinationIndicesBinding.descriptorCount = 1;
            destinationIndicesBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            destinationIndicesBinding.pImmutableSamplers = nullptr;
            bindings.push_back(destinationIndicesBinding);
        } else {
            // 原始计算着色器的描述符集布局
            // 输入纹理绑定
            VkDescriptorSetLayoutBinding inputTextureBinding;
            inputTextureBinding.binding = 0;
            inputTextureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            inputTextureBinding.descriptorCount = 1;
            inputTextureBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            inputTextureBinding.pImmutableSamplers = nullptr;
            bindings.push_back(inputTextureBinding);
            
            // 输出图像绑定
            VkDescriptorSetLayoutBinding outputImageBinding;
            outputImageBinding.binding = 1;
            outputImageBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            outputImageBinding.descriptorCount = 1;
            outputImageBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            outputImageBinding.pImmutableSamplers = nullptr;
            bindings.push_back(outputImageBinding);
        }
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &m_DescriptorSetLayout) != VK_SUCCESS) {
        printf("[ERROR] Failed to create descriptor set layout");
        return false;
    }

    printf("ComputeShader::Init: Descriptor set layout created");

    // 推常量布局
    std::vector<VkPushConstantRange> pushConstantRanges;
    
    std::cout << "[ComputeShader::Init] Setting up push constant ranges for " << shaderPath << std::endl;
    
    // 相机参数
    VkPushConstantRange cameraPushConstant;
    cameraPushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    cameraPushConstant.offset = 0;
    cameraPushConstant.size = sizeof(float) * 3 * 4 + sizeof(glm::mat4) + sizeof(float) * 4 + sizeof(int) * 2;
    pushConstantRanges.push_back(cameraPushConstant);
    
    std::cout << "[ComputeShader::Init] Camera push constant size: " << cameraPushConstant.size << std::endl;
    
    // 光照参数
    VkPushConstantRange lightPushConstant;
    lightPushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    lightPushConstant.offset = cameraPushConstant.offset + cameraPushConstant.size;
    lightPushConstant.size = sizeof(float) * 3 * 2 + sizeof(float) * 2;
    pushConstantRanges.push_back(lightPushConstant);
    
    std::cout << "[ComputeShader::Init] Light push constant size: " << lightPushConstant.size << ", offset: " << lightPushConstant.offset << std::endl;
    
    // 场景参数
    VkPushConstantRange scenePushConstant;
    scenePushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    scenePushConstant.offset = lightPushConstant.offset + lightPushConstant.size;
    scenePushConstant.size = sizeof(int) + sizeof(float) * 2;
    pushConstantRanges.push_back(scenePushConstant);
    
    std::cout << "[ComputeShader::Init] Scene push constant size: " << scenePushConstant.size << ", offset: " << scenePushConstant.offset << std::endl;

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_DescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstantRanges.size());
    pipelineLayoutInfo.pPushConstantRanges = pushConstantRanges.data();

    std::cout << "[ComputeShader::Init] Creating pipeline layout with " << pushConstantRanges.size() << " push constant ranges" << std::endl;
    if (vkCreatePipelineLayout(m_Device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout) != VK_SUCCESS) {
        printf("[ERROR] Failed to create pipeline layout");
        std::cout << "[ComputeShader::Init] Failed to create pipeline layout" << std::endl;
        return false;
    }

    printf("ComputeShader::Init: Pipeline layout created");
    std::cout << "[ComputeShader::Init] Pipeline layout created" << std::endl;

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shaderStageInfo;
    pipelineInfo.layout = m_PipelineLayout;

    printf("ComputeShader::Init: Creating compute pipeline...");
    std::cout << "[ComputeShader::Init] Creating compute pipeline..." << std::endl;
    VkResult result = vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_ComputePipeline);
    if (result != VK_SUCCESS) {
        printf("[ERROR] Failed to create compute pipeline, error code: %d", result);
        std::cout << "[ComputeShader::Init] Failed to create compute pipeline, error code: " << result << std::endl;
        return false;
    }

    printf("ComputeShader::Init: Compute pipeline created successfully");
    std::cout << "[ComputeShader::Init] Compute pipeline created" << std::endl;

    if (!CreateDescriptorSet()) {
        printf("[ERROR] Failed to create descriptor set");
        std::cout << "[ComputeShader::Init] Failed to create descriptor set" << std::endl;
        return false;
    }

    printf("ComputeShader::Init: Descriptor set created");
    std::cout << "[ComputeShader::Init] Descriptor set created" << std::endl;

    std::cout << "[ComputeShader::Init] Creating output image..." << std::endl;
    if (!CreateOutputImage(physicalDevice)) {
        printf("[ERROR] Failed to create output image");
        std::cout << "[ComputeShader::Init] Failed to create output image" << std::endl;
        return false;
    }

    printf("ComputeShader::Init: Output image created");
    std::cout << "[ComputeShader::Init] Output image created" << std::endl;

    // 更新存储图像描述符
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = m_OutputImageView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = m_DescriptorSet;
    // 光线追踪着色器和TAA着色器都使用绑定0作为输出图像，原始计算着色器使用绑定1
    bool isTAAShader = (m_ShaderPath.find("taa") != std::string::npos);
    descriptorWrite.dstBinding = (m_IsRaytracingShader || isTAAShader) ? 0 : 1;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(m_Device, 1, &descriptorWrite, 0, nullptr);

    printf("ComputeShader::Init: Initialization completed successfully");
    return true;
}

void ComputeShader::Cleanup() {
    CleanupOutputImage();
    
    if (m_DescriptorSet != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(m_Device, m_DescriptorPool, 1, &m_DescriptorSet);
    }

    if (m_DescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
    }

    if (m_DescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_Device, m_DescriptorSetLayout, nullptr);
    }

    if (m_ComputePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_Device, m_ComputePipeline, nullptr);
    }

    if (m_PipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
    }

    if (m_ShaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_Device, m_ShaderModule, nullptr);
    }
}

void ComputeShader::Bind(VkCommandBuffer commandBuffer) {
    if (commandBuffer == VK_NULL_HANDLE) {
        printf("ComputeShader::Bind: commandBuffer is null");
        return;
    }
    
    if (m_ComputePipeline == VK_NULL_HANDLE) {
        printf("ComputeShader::Bind: ComputePipeline is null");
        return;
    }
    
    if (m_PipelineLayout == VK_NULL_HANDLE) {
        printf("ComputeShader::Bind: PipelineLayout is null");
        return;
    }
    
    if (m_DescriptorSet == VK_NULL_HANDLE) {
        printf("ComputeShader::Bind: DescriptorSet is null");
        return;
    }
    
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_ComputePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &m_DescriptorSet, 0, nullptr);
}

void ComputeShader::Dispatch(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    vkCmdDispatch(commandBuffer, groupCountX, groupCountY, groupCountZ);
}

void ComputeShader::Execute(VkCommandBuffer commandBuffer) {
    if (commandBuffer == VK_NULL_HANDLE) {
        printf("ComputeShader::Execute: commandBuffer is null");
        return;
    }
    
    if (m_ComputePipeline == VK_NULL_HANDLE) {
        printf("ComputeShader::Execute: ComputePipeline is null, skipping compute shader execution");
        return;
    }
    
    if (m_Width == 0 || m_Height == 0) {
        printf("ComputeShader::Execute: Invalid dimensions (%ux%u)", m_Width, m_Height);
        return;
    }
    
    // 输出图像布局转换：如果需要，转换到 GENERAL
    if (m_OutputImage != VK_NULL_HANDLE && m_OutputImageLayout != VK_IMAGE_LAYOUT_GENERAL) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = m_OutputImageLayout;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_OutputImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = (m_OutputImageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ? VK_ACCESS_SHADER_READ_BIT : 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        
        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &barrier
        );
        
        m_OutputImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    
    // 深度图像布局转换
    if (m_DepthImage != VK_NULL_HANDLE) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_DepthImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        
        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &barrier
        );
    }
    
    Bind(commandBuffer);
    
    // 计算工作组数量 - 根据着色器的local_size调整
    // TAA着色器使用8x8的工作组大小
    bool isTAAShader = (m_ShaderPath.find("taa") != std::string::npos);
    uint32_t localSize = isTAAShader ? 8 : 16;
    
    uint32_t groupCountX = (m_Width + localSize - 1) / localSize;
    uint32_t groupCountY = (m_Height + localSize - 1) / localSize;
    
    Dispatch(commandBuffer, groupCountX, groupCountY);
}

void ComputeShader::SetInputTexture(VkImageView imageView, VkSampler sampler, uint32_t binding) {
    // 添加错误检查
    if (m_Device == VK_NULL_HANDLE) {
        return;
    }
    
    if (m_DescriptorSet == VK_NULL_HANDLE) {
        return;
    }
    
    if (imageView == VK_NULL_HANDLE) {
        return;
    }
    
    if (sampler == VK_NULL_HANDLE) {
        return;
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = imageView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.sampler = sampler;

    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = m_DescriptorSet;
    // 如果绑定是默认值0，根据着色器类型自动选择绑定位置
    uint32_t actualBinding = binding;
    descriptorWrite.dstBinding = actualBinding;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &imageInfo;// 更新描述符
    vkUpdateDescriptorSets(m_Device, 1, &descriptorWrite, 0, nullptr);
    
    // 移除过多的日志输出
    // printf("ComputeShader::SetInputTexture: Input texture set at binding %u", actualBinding);
}

void ComputeShader::UpdateSize(uint32_t width, uint32_t height, VkPhysicalDevice physicalDevice) {
    // 如果大小没有变化，直接返回
    if (m_Width == width && m_Height == height) {
        return;
    }
    
    // 保存新的大小
    m_Width = width;
    m_Height = height;
    
    // 重新创建输出图像，以适应新的大小
    CleanupOutputImage();
    CreateOutputImage(physicalDevice);
}

bool ComputeShader::LoadShaderModule(const std::string& filePath, VkShaderModule& shaderModule) {
    std::vector<char> buffer;
    
#ifdef __ANDROID__
    // Android: 使用SDL_IOStream加载assets中的文件
    SDL_IOStream* io = SDL_IOFromFile(filePath.c_str(), "rb");
    if (io == nullptr) {
        printf("[ERROR] Failed to open shader file: %s (SDL Error: %s)", filePath.c_str(), SDL_GetError());
        return false;
    }
    
    Sint64 fileSize = SDL_GetIOSize(io);
    if (fileSize <= 0) {
        printf("Shader file is empty or invalid: %s", filePath.c_str());
        SDL_CloseIO(io);
        return false;
    }
    
    buffer.resize((size_t)fileSize);
    size_t bytesRead = SDL_ReadIO(io, buffer.data(), buffer.size());
    if (bytesRead != buffer.size()) {
        printf("[ERROR] Failed to read shader file: %s (Read %zu bytes, expected %zu)", filePath.c_str(), bytesRead, buffer.size());
        SDL_CloseIO(io);
        return false;
    }
    
    SDL_CloseIO(io);
#else
    // 非Android平台: 使用标准文件流
    std::ifstream file(filePath, std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
        printf("[ERROR] Failed to open shader file: %s", filePath.c_str());
        printf("Current working directory: %s", std::filesystem::current_path().string().c_str());
        return false;
    }

    size_t fileSize = (size_t)file.tellg();
    printf("Shader file size: %zu bytes", fileSize);
    
    if (fileSize == 0) {
        printf("Shader file is empty");
        file.close();
        return false;
    }

    buffer.resize(fileSize);

    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();
#endif

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = buffer.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());

    VkResult result = vkCreateShaderModule(m_Device, &createInfo, nullptr, &shaderModule);
    if (result != VK_SUCCESS) {
        printf("[ERROR] Failed to create shader module, error code: %d", result);
        return false;
    }

    printf("Shader module created successfully");
    return true;
}

bool ComputeShader::CreateDescriptorSet() {
    std::cout << "[ComputeShader::CreateDescriptorSet] Starting..." << std::endl;
    // 创建描述符池
    std::vector<VkDescriptorPoolSize> poolSizes;
    
    // 存储图像描述符池大小
    VkDescriptorPoolSize storageImagePoolSize;
    storageImagePoolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    storageImagePoolSize.descriptorCount = 1;
    poolSizes.push_back(storageImagePoolSize);
    
    // 存储缓冲区描述符池大小
    VkDescriptorPoolSize storageBufferPoolSize;
    storageBufferPoolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    storageBufferPoolSize.descriptorCount = 7; // 为模型变换、元数据、偏移量、顶点、三角形、BVH节点和纹理数据预留
    poolSizes.push_back(storageBufferPoolSize);
    
    // 纹理池 - 增加到18个（支持 binding 0-17）
    VkDescriptorPoolSize texturePoolSize;
    texturePoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    texturePoolSize.descriptorCount = 18;
    poolSizes.push_back(texturePoolSize);
    
    // 组合图像采样器描述符池大小
    VkDescriptorPoolSize combinedImageSamplerPoolSize;
    combinedImageSamplerPoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    combinedImageSamplerPoolSize.descriptorCount = 6; // 为模型纹理、压缩纹理数组、未压缩纹理数组、天空盒和蓝噪声预留
    poolSizes.push_back(combinedImageSamplerPoolSize);

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 1;

    if (vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescriptorPool) != VK_SUCCESS) {
        printf("[ERROR] Failed to create descriptor pool");
        std::cout << "[ComputeShader::CreateDescriptorSet] Failed to create descriptor pool" << std::endl;
        return false;
    }

    std::cout << "[ComputeShader::CreateDescriptorSet] Descriptor pool created" << std::endl;

    // 分配描述符集
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_DescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_DescriptorSetLayout;

    if (vkAllocateDescriptorSets(m_Device, &allocInfo, &m_DescriptorSet) != VK_SUCCESS) {
        printf("[ERROR] Failed to allocate descriptor set");
        std::cout << "[ComputeShader::CreateDescriptorSet] Failed to allocate descriptor set" << std::endl;
        return false;
    }

    std::cout << "[ComputeShader::CreateDescriptorSet] Descriptor set allocated" << std::endl;
    return true;
}

void ComputeShader::SetStorageBuffer(VkBuffer buffer, VkDeviceSize size, uint32_t binding) {
    if (m_Device == VK_NULL_HANDLE || m_DescriptorSet == VK_NULL_HANDLE) {
        printf("ComputeShader::SetStorageBuffer: Device or DescriptorSet is null");
        return;
    }

    if (buffer == VK_NULL_HANDLE) {
        printf("ComputeShader::SetStorageBuffer: Buffer is null at binding %u", binding);
        return;
    }

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = buffer;
    bufferInfo.offset = 0;
    // 确保 size 不为 0，Vulkan 不允许 0 大小的缓冲区
    bufferInfo.range = size > 0 ? size : sizeof(uint32_t);

    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = m_DescriptorSet;
    descriptorWrite.dstBinding = binding;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pBufferInfo = &bufferInfo;
// 更新描述符
    vkUpdateDescriptorSets(m_Device, 1, &descriptorWrite, 0, nullptr);
    
    // 移除过多的日志输出
    // printf("ComputeShader::SetStorageBuffer: Storage buffer set at binding %u (size: %zu)", binding, size);
}

// 光线追踪相关方法实现
void ComputeShader::SetModelTransforms(VkBuffer buffer, VkDeviceSize size) {
    SetStorageBuffer(buffer, size, 1);
}

void ComputeShader::SetModelMetadata(VkBuffer buffer, VkDeviceSize size) {
    SetStorageBuffer(buffer, size, 2);
}

void ComputeShader::SetModelOffsets(VkBuffer buffer, VkDeviceSize size) {
    SetStorageBuffer(buffer, size, 3);
}

void ComputeShader::SetVertices(VkBuffer buffer, VkDeviceSize size) {
    SetStorageBuffer(buffer, size, 4);
}

void ComputeShader::SetTriangles(VkBuffer buffer, VkDeviceSize size) {
    SetStorageBuffer(buffer, size, 5);
}

void ComputeShader::SetBVHNodes(VkBuffer buffer, VkDeviceSize size) {
    SetStorageBuffer(buffer, size, 6);
}

void ComputeShader::SetModelTextureData(VkBuffer buffer, VkDeviceSize size) {
    SetStorageBuffer(buffer, size, 7);
}

void ComputeShader::SetForwardTexture0(VkImageView imageView, VkSampler sampler) {
    uint32_t binding = m_IsRaytracingShader ? 8 : 2;
    SetInputTexture(imageView, sampler, binding);
}

void ComputeShader::SetForwardTexture1(VkImageView imageView, VkSampler sampler) {
    uint32_t binding = m_IsRaytracingShader ? 9 : 3;
    SetInputTexture(imageView, sampler, binding);
}

void ComputeShader::SetForwardTexture2(VkImageView imageView, VkSampler sampler) {
    uint32_t binding = m_IsRaytracingShader ? 10 : 4;
    SetInputTexture(imageView, sampler, binding);
}

void ComputeShader::SetForwardTexture3(VkImageView imageView, VkSampler sampler) {
    uint32_t binding = m_IsRaytracingShader ? 11 : 5;
    SetInputTexture(imageView, sampler, binding);
}

void ComputeShader::SetDepthTexture(VkImageView imageView, VkSampler sampler) {
    uint32_t binding = m_IsRaytracingShader ? 14 : 1;
    SetInputTexture(imageView, sampler, binding);
}

void ComputeShader::SetHistoryTexture(VkImageView imageView, VkSampler sampler) {
    uint32_t binding = m_IsRaytracingShader ? 15 : 6;
    SetInputTexture(imageView, sampler, binding);
}

void ComputeShader::SetRaytracingOutput(VkImageView imageView, VkSampler sampler) {
    uint32_t binding = m_IsRaytracingShader ? 7 : 7;
    SetInputTexture(imageView, sampler, binding);
}

void ComputeShader::SetSkybox(VkImageView imageView, VkSampler sampler) {
    uint32_t binding = m_IsRaytracingShader ? 12 : 9;
    SetInputTexture(imageView, sampler, binding);
}

void ComputeShader::SetBlueNoise(VkImageView imageView, VkSampler sampler) {
    uint32_t binding = m_IsRaytracingShader ? 13 : 10;
    SetInputTexture(imageView, sampler, binding);
}

void ComputeShader::SetAlbedoTexture(VkImageView imageView, VkSampler sampler) {
    uint32_t binding = m_IsRaytracingShader ? 15 : 8;
    SetInputTexture(imageView, sampler, binding);
}

void ComputeShader::SetModelTextures(VkImageView imageView, VkSampler sampler) {
    uint32_t binding = m_IsRaytracingShader ? 16 : 7;
    SetInputTexture(imageView, sampler, binding);
}

void ComputeShader::SetHistoryTexture0(VkImageView imageView, VkSampler sampler) {
    // 与 SetHistoryTexture 相同，binding 6 用于光照/TAA shader
    SetHistoryTexture(imageView, sampler);
}

void ComputeShader::SetDepthImage(VkImage image) {
    m_DepthImage = image;
}

void ComputeShader::SetTextureArray(const std::vector<VkDescriptorSet>& textureDescriptorSets, uint32_t binding) {
    if (m_Device == VK_NULL_HANDLE || m_DescriptorSet == VK_NULL_HANDLE || textureDescriptorSets.empty()) {
        return;
    }
    
    // 使用 vkCmdBindDescriptorSets 在命令缓冲级别绑定纹理数组
    // 这需要在 Execute 方法中处理
    // 这里我们只是存储纹理描述符集供后续使用
    printf("ComputeShader::SetTextureArray: Binding %zu textures at binding %u", textureDescriptorSets.size(), binding);
}

void ComputeShader::SetPushConstants(VkCommandBuffer commandBuffer, const void* data, size_t dataSize, const void* lightData, size_t lightDataSize, const void* sceneData, size_t sceneDataSize) {
    if (m_Device == VK_NULL_HANDLE || m_PipelineLayout == VK_NULL_HANDLE || commandBuffer == VK_NULL_HANDLE) {
        return;
    }

    // 一次性设置所有push constants数据
    if (data && dataSize > 0) {
        vkCmdPushConstants(commandBuffer, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, static_cast<uint32_t>(dataSize), data);
    }
}

bool ComputeShader::CreateOutputImage(VkPhysicalDevice physicalDevice) {
    std::cout << "[ComputeShader::CreateOutputImage] Starting... (width=" << m_Width << ", height=" << m_Height << ")" << std::endl;
    // 图像创建信息
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = m_Width;
    imageInfo.extent.height = m_Height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // 创建图像
    std::cout << "[ComputeShader::CreateOutputImage] Creating image..." << std::endl;
    if (vkCreateImage(m_Device, &imageInfo, nullptr, &m_OutputImage) != VK_SUCCESS) {
        printf("[ERROR] Failed to create output image");
        std::cout << "[ComputeShader::CreateOutputImage] Failed to create image" << std::endl;
        return false;
    }

    std::cout << "[ComputeShader::CreateOutputImage] Image created" << std::endl;

    // 获取内存需求
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(m_Device, m_OutputImage, &memRequirements);

    // 内存分配信息
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = RendererUtils::FindMemoryType(memRequirements.memoryTypeBits, 
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    std::cout << "[ComputeShader::CreateOutputImage] Allocating memory (size=" << allocInfo.allocationSize << ", type=" << allocInfo.memoryTypeIndex << ")" << std::endl;
    // 分配内存
    if (vkAllocateMemory(m_Device, &allocInfo, nullptr, &m_OutputImageMemory) != VK_SUCCESS) {
        printf("[ERROR] Failed to allocate output image memory");
        std::cout << "[ComputeShader::CreateOutputImage] Failed to allocate memory" << std::endl;
        vkDestroyImage(m_Device, m_OutputImage, nullptr);
        m_OutputImage = VK_NULL_HANDLE;
        return false;
    }

    std::cout << "[ComputeShader::CreateOutputImage] Memory allocated" << std::endl;

    // 绑定内存
    std::cout << "[ComputeShader::CreateOutputImage] Binding memory..." << std::endl;
    if (vkBindImageMemory(m_Device, m_OutputImage, m_OutputImageMemory, 0) != VK_SUCCESS) {
        printf("[ERROR] Failed to bind output image memory");
        std::cout << "[ComputeShader::CreateOutputImage] Failed to bind memory" << std::endl;
        vkFreeMemory(m_Device, m_OutputImageMemory, nullptr);
        m_OutputImageMemory = VK_NULL_HANDLE;
        vkDestroyImage(m_Device, m_OutputImage, nullptr);
        m_OutputImage = VK_NULL_HANDLE;
        return false;
    }

    std::cout << "[ComputeShader::CreateOutputImage] Memory bound" << std::endl;

    // 图像视图创建信息
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_OutputImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    // 创建图像视图
    if (vkCreateImageView(m_Device, &viewInfo, nullptr, &m_OutputImageView) != VK_SUCCESS) {
        printf("[ERROR] Failed to create output image view");
        vkFreeMemory(m_Device, m_OutputImageMemory, nullptr);
        m_OutputImageMemory = VK_NULL_HANDLE;
        vkDestroyImage(m_Device, m_OutputImage, nullptr);
        m_OutputImage = VK_NULL_HANDLE;
        return false;
    }

    // 更新存储图像描述符
    if (m_DescriptorSet != VK_NULL_HANDLE) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = m_OutputImageView;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_DescriptorSet;
        // 光线追踪着色器和TAA着色器都使用绑定0作为输出图像，原始计算着色器使用绑定1
        bool isTAAShader = (m_ShaderPath.find("taa") != std::string::npos);
        descriptorWrite.dstBinding = (m_IsRaytracingShader || isTAAShader) ? 0 : 1;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_Device, 1, &descriptorWrite, 0, nullptr);
    }

    return true;
}

void ComputeShader::CleanupOutputImage() {
    if (m_OutputImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_Device, m_OutputImageView, nullptr);
        m_OutputImageView = VK_NULL_HANDLE;
    }
    if (m_OutputImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_Device, m_OutputImage, nullptr);
        m_OutputImage = VK_NULL_HANDLE;
    }
    if (m_OutputImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_Device, m_OutputImageMemory, nullptr);
        m_OutputImageMemory = VK_NULL_HANDLE;
    }
}