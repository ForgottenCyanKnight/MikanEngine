#include "WireframeRenderer.h"
#include "EngineGlobal.h"
#include "RendererBase.h"
#include <iostream>

// 单位立方体的线框顶点（8个顶点）
// 线框由12条边组成，每条边需要2个顶点
static const float cubeVertices[] = {
    // 底面四条边
    -0.5f, -0.5f, -0.5f,  0.5f, -0.5f, -0.5f,  // 后边缘
     0.5f, -0.5f, -0.5f,  0.5f, -0.5f,  0.5f,  // 右边缘
     0.5f, -0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  // 前边缘
    -0.5f, -0.5f,  0.5f, -0.5f, -0.5f, -0.5f,  // 左边缘
    // 顶面四条边
    -0.5f,  0.5f, -0.5f,  0.5f,  0.5f, -0.5f,  // 后边缘
     0.5f,  0.5f, -0.5f,  0.5f,  0.5f,  0.5f,  // 右边缘
     0.5f,  0.5f,  0.5f, -0.5f,  0.5f,  0.5f,  // 前边缘
    -0.5f,  0.5f,  0.5f, -0.5f,  0.5f, -0.5f,  // 左边缘
    // 连接底面和顶面的四条边
    -0.5f, -0.5f, -0.5f, -0.5f,  0.5f, -0.5f,  // 后左
     0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f,  // 后右
     0.5f, -0.5f,  0.5f,  0.5f,  0.5f,  0.5f,  // 前右
    -0.5f, -0.5f,  0.5f, -0.5f,  0.5f,  0.5f,  // 前左
};

WireframeRenderer::WireframeRenderer() {
}

WireframeRenderer::~WireframeRenderer() {
    Cleanup();
}

void WireframeRenderer::EnsureInit(VkRenderPass uiPass) {
    if (m_Initialized && m_UIPass == uiPass) return;
    Cleanup();
    CreateCubeVertices();
    CreatePipeline(uiPass);
    CreateFrustumPipeline(uiPass);
    m_UIPass = uiPass;
    m_Initialized = true;
}

void WireframeRenderer::Cleanup() {
    m_Pipeline.Cleanup();
    m_FrustumPipeline.Cleanup();
    m_VertexBuffer.Cleanup();
    m_InstanceBuffer.Cleanup();
    m_FrustumVertexBuffer.Cleanup();
    m_FrustumColorBuffer.Cleanup();
    
    m_Instances.clear();
    m_FrustumVertices.clear();
    m_FrustumColors.clear();
    m_UIPass = VK_NULL_HANDLE;
    m_Initialized = false;
}

void WireframeRenderer::CreateCubeVertices() {
    m_VertexCount = 24; // 12条边 * 2个顶点
    
    VkDeviceSize bufferSize = sizeof(cubeVertices);
    
    // 创建暂存缓冲区
    VulkanBuffer stagingBuffer;
    stagingBuffer.Create(bufferSize,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    // 复制数据到暂存缓冲区
    void* data;
    vkMapMemory(g_Device, stagingBuffer.GetMemory(), 0, bufferSize, 0, &data);
    memcpy(data, cubeVertices, (size_t)bufferSize);
    vkUnmapMemory(g_Device, stagingBuffer.GetMemory());
    
    // 创建顶点缓冲区
    m_VertexBuffer.Create(bufferSize,
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    // 复制数据到顶点缓冲区
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = g_CommandPool;
    allocInfo.commandBufferCount = 1;
    
    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(g_Device, &allocInfo, &commandBuffer);
    
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    
    VkBufferCopy copyRegion = {};
    copyRegion.size = bufferSize;
    vkCmdCopyBuffer(commandBuffer, stagingBuffer.GetBuffer(), m_VertexBuffer.GetBuffer(), 1, &copyRegion);
    
    vkEndCommandBuffer(commandBuffer);
    
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    vkQueueSubmit(g_Queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_Queue);
    
    vkFreeCommandBuffers(g_Device, g_CommandPool, 1, &commandBuffer);
    
    // 清理暂存缓冲区
    stagingBuffer.Cleanup();
}

void WireframeRenderer::CreatePipeline(VkRenderPass renderPass) {
    // 创建管线配置
    PipelineConfig config;
    config.vertShader = "wireframe.vert.spv";
    config.fragShader = "wireframe.frag.spv";
    config.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    config.cullMode = VK_CULL_MODE_NONE;
    config.subpass = 0;               // UI overlay pass（链末叠加，无深度附件）
    config.depthTest = false;         // 2026-08-10：线框移出 G-Buffer（不再写深度/albedo 污染合成）
    config.depthWrite = false;
    config.blending = true;           // 叠加模式（alpha=1 覆盖，为将来半透明预留）
    config.usePushConstants = true;
    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    config.pushConstantRange.offset = 0;
    config.pushConstantRange.size = sizeof(UniformBufferObject);
    
    // 顶点输入绑定
    // 绑定0: 顶点位置 (vec3)
    VkVertexInputBindingDescription vertexBinding = {};
    vertexBinding.binding = 0;
    vertexBinding.stride = sizeof(glm::vec3);
    vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    config.vertexBindings.push_back(vertexBinding);
    
    // 绑定1: 实例数据 (position + size + rotation + color)
    VkVertexInputBindingDescription instanceBinding = {};
    instanceBinding.binding = 1;
    instanceBinding.stride = sizeof(WireframeInstance);
    instanceBinding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    config.vertexBindings.push_back(instanceBinding);
    
    // 顶点属性
    // 位置属性
    VkVertexInputAttributeDescription posAttr = {};
    posAttr.binding = 0;
    posAttr.location = 0;
    posAttr.format = VK_FORMAT_R32G32B32_SFLOAT;
    posAttr.offset = 0;
    config.vertexAttributes.push_back(posAttr);
    
    // 实例位置
    VkVertexInputAttributeDescription instancePosAttr = {};
    instancePosAttr.binding = 1;
    instancePosAttr.location = 1;
    instancePosAttr.format = VK_FORMAT_R32G32B32_SFLOAT;
    instancePosAttr.offset = offsetof(WireframeInstance, position);
    config.vertexAttributes.push_back(instancePosAttr);
    
    // 实例尺寸
    VkVertexInputAttributeDescription instanceSizeAttr = {};
    instanceSizeAttr.binding = 1;
    instanceSizeAttr.location = 2;
    instanceSizeAttr.format = VK_FORMAT_R32G32B32_SFLOAT;
    instanceSizeAttr.offset = offsetof(WireframeInstance, size);
    config.vertexAttributes.push_back(instanceSizeAttr);
    
    // 实例旋转
    VkVertexInputAttributeDescription instanceRotAttr = {};
    instanceRotAttr.binding = 1;
    instanceRotAttr.location = 3;
    instanceRotAttr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    instanceRotAttr.offset = offsetof(WireframeInstance, rotation);
    config.vertexAttributes.push_back(instanceRotAttr);
    
    // 实例颜色
    VkVertexInputAttributeDescription instanceColorAttr = {};
    instanceColorAttr.binding = 1;
    instanceColorAttr.location = 4;
    instanceColorAttr.format = VK_FORMAT_R32G32B32_SFLOAT;
    instanceColorAttr.offset = offsetof(WireframeInstance, color);
    config.vertexAttributes.push_back(instanceColorAttr);
    
    // 创建管线（传入nullptr作为descriptor set layout，因为使用push constants）
    m_Pipeline.Create(renderPass, VK_NULL_HANDLE, config);
}

void WireframeRenderer::CreateFrustumPipeline(VkRenderPass renderPass) {
    // 创建管线配置（用于视锥体渲染，使用简单的位置+颜色顶点格式）
    PipelineConfig config;
    config.vertShader = "wireframe_simple.vert.spv";
    config.fragShader = "wireframe_simple.frag.spv";
    config.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    config.cullMode = VK_CULL_MODE_NONE;
    config.subpass = 0;               // UI overlay pass（链末叠加，无深度附件）
    config.depthTest = false;         // 2026-08-10：视锥线框移出 G-Buffer
    config.depthWrite = false;
    config.blending = true;
    config.usePushConstants = true;
    config.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    config.pushConstantRange.offset = 0;
    config.pushConstantRange.size = sizeof(UniformBufferObject);
    
    // 顶点输入绑定
    // 绑定0: 顶点位置 (vec3)
    VkVertexInputBindingDescription vertexBinding = {};
    vertexBinding.binding = 0;
    vertexBinding.stride = sizeof(glm::vec3);
    vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    config.vertexBindings.push_back(vertexBinding);
    
    // 绑定1: 顶点颜色 (vec3)
    VkVertexInputBindingDescription colorBinding = {};
    colorBinding.binding = 1;
    colorBinding.stride = sizeof(glm::vec3);
    colorBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    config.vertexBindings.push_back(colorBinding);
    
    // 顶点属性
    // 位置属性
    VkVertexInputAttributeDescription posAttr = {};
    posAttr.binding = 0;
    posAttr.location = 0;
    posAttr.format = VK_FORMAT_R32G32B32_SFLOAT;
    posAttr.offset = 0;
    config.vertexAttributes.push_back(posAttr);
    
    // 颜色属性
    VkVertexInputAttributeDescription colorAttr = {};
    colorAttr.binding = 1;
    colorAttr.location = 1;
    colorAttr.format = VK_FORMAT_R32G32B32_SFLOAT;
    colorAttr.offset = 0;
    config.vertexAttributes.push_back(colorAttr);
    
    // 创建管线
    m_FrustumPipeline.Create(renderPass, VK_NULL_HANDLE, config);
}

void WireframeRenderer::AddAABB(const AABB& aabb, const glm::vec3& color) {
    WireframeInstance instance;
    instance.position = aabb.GetCenter();
    instance.size = aabb.GetSize(); // 全尺寸
    instance.rotation = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f); // 无旋转
    instance.color = color;
    m_Instances.push_back(instance);
    m_InstanceBufferDirty = true;
}

void WireframeRenderer::AddOBB(const glm::vec3& center, const glm::vec3& halfExtents, 
                                const glm::vec4& rotation, const glm::vec3& color) {
    WireframeInstance instance;
    instance.position = center;
    instance.size = halfExtents * 2.0f; // 转换为全尺寸
    instance.rotation = rotation;
    instance.color = color;
    m_Instances.push_back(instance);
    m_InstanceBufferDirty = true;
}

void WireframeRenderer::AddOBBFromMatrix(const AABB& localAABB, const glm::mat4& modelMatrix, 
                                          const glm::vec3& color) {
    // 从模型矩阵提取平移、旋转和缩放
    glm::vec3 translation = glm::vec3(modelMatrix[3]);
    glm::vec3 scale = glm::vec3(
        glm::length(glm::vec3(modelMatrix[0])),
        glm::length(glm::vec3(modelMatrix[1])),
        glm::length(glm::vec3(modelMatrix[2]))
    );
    
    // 计算旋转矩阵（去除缩放）
    glm::mat3 rotationMatrix = glm::mat3(modelMatrix);
    rotationMatrix[0] = glm::normalize(rotationMatrix[0]);
    rotationMatrix[1] = glm::normalize(rotationMatrix[1]);
    rotationMatrix[2] = glm::normalize(rotationMatrix[2]);
    
    // 使用 glm 的标准方法将旋转矩阵转换为四元数
    glm::quat rotation = glm::quat_cast(rotationMatrix);
    
    // 计算局部空间的半尺寸
    glm::vec3 localHalfExtents = (localAABB.max - localAABB.min) * 0.5f;
    
    // 应用缩放
    glm::vec3 scaledHalfExtents = localHalfExtents * scale;
    
    // 计算世界空间中心
    glm::vec3 localCenter = (localAABB.min + localAABB.max) * 0.5f;
    glm::vec3 worldCenter = glm::vec3(modelMatrix * glm::vec4(localCenter, 1.0f));
    
    // 添加 OBB（转换为 vec4）
    AddOBB(worldCenter, scaledHalfExtents, glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w), color);
}

void WireframeRenderer::AddAABB(const AABB& aabb, const glm::vec3& position, 
                                 const glm::vec4& rotation, const glm::vec3& color) {
    WireframeInstance instance;
    instance.position = position;
    instance.size = aabb.GetSize(); // 全尺寸
    instance.rotation = rotation;
    instance.color = color;
    m_Instances.push_back(instance);
    m_InstanceBufferDirty = true;
}

void WireframeRenderer::AddCenteredCube(const glm::vec3& center, const glm::vec3& halfSize, const glm::vec3& color) {
    WireframeInstance instance;
    instance.position = center;
    instance.size = halfSize * 2.0f; // 转换为全尺寸
    instance.rotation = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    instance.color = color;
    m_Instances.push_back(instance);
    m_InstanceBufferDirty = true;
}

void WireframeRenderer::AddCrossGrid(const glm::vec3& position, float length, float thickness, 
                                      const glm::vec3& color) {
    // X轴方向的细长长方体
    WireframeInstance instanceX;
    instanceX.position = position;
    instanceX.size = glm::vec3(length, thickness, thickness);
    instanceX.rotation = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    instanceX.color = color;
    m_Instances.push_back(instanceX);
    
    // Z轴方向的细长长方体
    WireframeInstance instanceZ;
    instanceZ.position = position;
    instanceZ.size = glm::vec3(thickness, thickness, length);
    instanceZ.rotation = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    instanceZ.color = color;
    m_Instances.push_back(instanceZ);
    
    m_InstanceBufferDirty = true;
}

void WireframeRenderer::AddLine(const glm::vec3& start, const glm::vec3& end, const glm::vec3& color) {
    m_FrustumVertices.push_back(start);
    m_FrustumVertices.push_back(end);
    m_FrustumColors.push_back(color);
    m_FrustumColors.push_back(color);
    m_FrustumBufferDirty = true;
}

void WireframeRenderer::ClearInstances() {
    m_Instances.clear();
    m_InstanceBufferDirty = true;
}

void WireframeRenderer::AddFrustum(const Frustum& frustum, const glm::vec3& color) {
    AddFrustum(frustum.corners, color);
}

void WireframeRenderer::AddFrustum(const std::array<glm::vec3, 8>& corners, const glm::vec3& color) {
    // 添加12条边的顶点（每条边2个顶点）
    // 近平面的4条边
    m_FrustumVertices.push_back(corners[0]); m_FrustumVertices.push_back(corners[1]);
    m_FrustumVertices.push_back(corners[1]); m_FrustumVertices.push_back(corners[2]);
    m_FrustumVertices.push_back(corners[2]); m_FrustumVertices.push_back(corners[3]);
    m_FrustumVertices.push_back(corners[3]); m_FrustumVertices.push_back(corners[0]);
    
    // 远平面的4条边
    m_FrustumVertices.push_back(corners[4]); m_FrustumVertices.push_back(corners[5]);
    m_FrustumVertices.push_back(corners[5]); m_FrustumVertices.push_back(corners[6]);
    m_FrustumVertices.push_back(corners[6]); m_FrustumVertices.push_back(corners[7]);
    m_FrustumVertices.push_back(corners[7]); m_FrustumVertices.push_back(corners[4]);
    
    // 连接近平面和远平面的4条边
    m_FrustumVertices.push_back(corners[0]); m_FrustumVertices.push_back(corners[4]);
    m_FrustumVertices.push_back(corners[1]); m_FrustumVertices.push_back(corners[5]);
    m_FrustumVertices.push_back(corners[2]); m_FrustumVertices.push_back(corners[6]);
    m_FrustumVertices.push_back(corners[3]); m_FrustumVertices.push_back(corners[7]);
    
    // 为每个顶点添加颜色（24个顶点）
    for (int i = 0; i < 24; i++) {
        m_FrustumColors.push_back(color);
    }
    
    m_FrustumBufferDirty = true;
}

void WireframeRenderer::ClearFrustums() {
    m_FrustumVertices.clear();
    m_FrustumColors.clear();
    m_FrustumBufferDirty = true;
}

void WireframeRenderer::UpdateInstanceBuffer() {
    if (!m_InstanceBufferDirty || m_Instances.empty()) return;
    
    // 2026-08-10 性能修复：容量预分配（当前数量 +50% + 余量），缓冲"只增不缩"——
    // 实例数每帧变化（BVH 可视化节点数随相机移动变化）时不再重建缓冲/waitIdle，
    // 否则每帧 vkDeviceWaitIdle 清空 GPU 流水线 → 帧率骤降。
    // 重建仅在超出容量时发生（一次性），draw 仍用 m_Instances.size()（实际数量）。
    const size_t capacity = m_Instances.size() + m_Instances.size() / 2 + 64;
    const VkDeviceSize bufferSize = sizeof(WireframeInstance) * capacity;
    
    // 如果缓冲区容量不足，重新创建（只增长，不收缩）
    if (m_InstanceBuffer.GetBuffer() == VK_NULL_HANDLE || 
        m_InstanceBuffer.GetSize() < bufferSize) {
        // 等待GPU完成之前的渲染操作
        vkDeviceWaitIdle(g_Device);
        
        m_InstanceBuffer.Cleanup();
        if (!m_InstanceBuffer.Create(bufferSize,
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            // 创建失败，标记为脏以便下次重试
            return;
        }
    }
    
    // 复制数据（只拷贝实际实例数）
    const VkDeviceSize dataSize = sizeof(WireframeInstance) * m_Instances.size();
    void* data;
    VkResult result = vkMapMemory(g_Device, m_InstanceBuffer.GetMemory(), 0, dataSize, 0, &data);
    if (result == VK_SUCCESS) {
        memcpy(data, m_Instances.data(), (size_t)dataSize);
        vkUnmapMemory(g_Device, m_InstanceBuffer.GetMemory());
        m_InstanceBufferDirty = false;
    }
}

void WireframeRenderer::Render(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj) {
    if (m_Instances.empty()) return;
    
    // 更新实例缓冲区
    UpdateInstanceBuffer();
    
    // 检查缓冲区是否有效
    if (m_InstanceBuffer.GetBuffer() == VK_NULL_HANDLE) return;
    
    // 绑定管线
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline.GetPipeline());
    
    // 设置线宽（加粗线条，BVH 可视化使用更粗的线）
    vkCmdSetLineWidth(commandBuffer, 3.0f);
    
    // 绑定顶点缓冲区
    VkBuffer vertexBuffers[] = {m_VertexBuffer.GetBuffer(), m_InstanceBuffer.GetBuffer()};
    VkDeviceSize offsets[] = {0, 0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
    
    // 推送常量
    UniformBufferObject ubo = {};
    ubo.view = view;
    ubo.proj = proj;
    
    vkCmdPushConstants(commandBuffer, m_Pipeline.GetLayout(), 
                      VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ubo), &ubo);
    
    // 绘制
    vkCmdDraw(commandBuffer, m_VertexCount, static_cast<uint32_t>(m_Instances.size()), 0, 0);
}

void WireframeRenderer::UpdateFrustumBuffer() {
    if (!m_FrustumBufferDirty || m_FrustumVertices.empty()) return;
    
    VkDeviceSize vertexBufferSize = sizeof(glm::vec3) * m_FrustumVertices.size();
    VkDeviceSize colorBufferSize = sizeof(glm::vec3) * m_FrustumColors.size();
    
    // 如果缓冲区太小，重新创建
    if (m_FrustumVertexBuffer.GetBuffer() == VK_NULL_HANDLE || 
        m_FrustumVertexBuffer.GetSize() < vertexBufferSize) {
        // 等待GPU完成之前的渲染操作
        vkDeviceWaitIdle(g_Device);
        
        m_FrustumVertexBuffer.Cleanup();
        m_FrustumVertexBuffer.Create(vertexBufferSize,
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    
    if (m_FrustumColorBuffer.GetBuffer() == VK_NULL_HANDLE || 
        m_FrustumColorBuffer.GetSize() < colorBufferSize) {
        // 等待GPU完成之前的渲染操作
        vkDeviceWaitIdle(g_Device);
        
        m_FrustumColorBuffer.Cleanup();
        m_FrustumColorBuffer.Create(colorBufferSize,
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    
    // 复制顶点数据
    void* data;
    vkMapMemory(g_Device, m_FrustumVertexBuffer.GetMemory(), 0, vertexBufferSize, 0, &data);
    memcpy(data, m_FrustumVertices.data(), (size_t)vertexBufferSize);
    vkUnmapMemory(g_Device, m_FrustumVertexBuffer.GetMemory());
    
    // 复制颜色数据
    vkMapMemory(g_Device, m_FrustumColorBuffer.GetMemory(), 0, colorBufferSize, 0, &data);
    memcpy(data, m_FrustumColors.data(), (size_t)colorBufferSize);
    vkUnmapMemory(g_Device, m_FrustumColorBuffer.GetMemory());
    
    m_FrustumBufferDirty = false;
}

void WireframeRenderer::RenderFrustums(VkCommandBuffer commandBuffer, const glm::mat4& view, const glm::mat4& proj) {
    if (m_FrustumVertices.empty()) return;
    
    // 更新视锥体顶点缓冲区
    UpdateFrustumBuffer();
    
    // 绑定管线（使用专门的视锥体管线）
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_FrustumPipeline.GetPipeline());
    
    // 设置线宽（加粗线条）
    vkCmdSetLineWidth(commandBuffer, 3.0f);
    
    // 绑定顶点缓冲区（位置和颜色）
    VkBuffer vertexBuffers[] = {m_FrustumVertexBuffer.GetBuffer(), m_FrustumColorBuffer.GetBuffer()};
    VkDeviceSize offsets[] = {0, 0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
    
    // 推送常量
    UniformBufferObject ubo = {};
    ubo.view = view;
    ubo.proj = proj;
    
    vkCmdPushConstants(commandBuffer, m_FrustumPipeline.GetLayout(), 
                      VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ubo), &ubo);
    
    // 绘制视锥体线条
    vkCmdDraw(commandBuffer, static_cast<uint32_t>(m_FrustumVertices.size()), 1, 0, 0);
}
