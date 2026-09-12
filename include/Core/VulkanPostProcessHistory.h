#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <cstdint>

extern VkImage g_SceneTAAHistory;
extern VkDeviceMemory g_SceneTAAHistoryMem;
extern VkImageView g_SceneTAAHistoryView;
extern VkImage g_GameTAAHistory;
extern VkDeviceMemory g_GameTAAHistoryMem;
extern VkImageView g_GameTAAHistoryView;
extern VkSampler g_TAAHistorySampler;
extern uint32_t g_TAAHistoryW;
extern uint32_t g_TAAHistoryH;
extern bool g_TAAHistoryNeedsClear;

extern VkImage g_SceneAOHistory;
extern VkDeviceMemory g_SceneAOHistoryMem;
extern VkImageView g_SceneAOHistoryView;
extern VkImage g_GameAOHistory;
extern VkDeviceMemory g_GameAOHistoryMem;
extern VkImageView g_GameAOHistoryView;
extern VkSampler g_AOHistorySampler;
extern uint32_t g_SceneAOHistoryW;
extern uint32_t g_SceneAOHistoryH;
extern uint32_t g_GameAOHistoryW;
extern uint32_t g_GameAOHistoryH;
extern bool g_SceneAOHistoryNeedsClear;
extern bool g_GameAOHistoryNeedsClear;

extern VkImage g_SceneSSGIHistory;
extern VkDeviceMemory g_SceneSSGIHistoryMem;
extern VkImageView g_SceneSSGIHistoryView;
extern VkImage g_GameSSGIHistory;
extern VkDeviceMemory g_GameSSGIHistoryMem;
extern VkImageView g_GameSSGIHistoryView;
extern VkSampler g_SSGIHistorySampler;
extern uint32_t g_SceneSSGIHistoryW;
extern uint32_t g_SceneSSGIHistoryH;
extern uint32_t g_GameSSGIHistoryW;
extern uint32_t g_GameSSGIHistoryH;
extern bool g_SceneSSGIHistoryNeedsClear;
extern bool g_GameSSGIHistoryNeedsClear;

extern VkImage g_SceneCloudHistory;
extern VkDeviceMemory g_SceneCloudHistoryMem;
extern VkImageView g_SceneCloudHistoryView;
extern VkImage g_GameCloudHistory;
extern VkDeviceMemory g_GameCloudHistoryMem;
extern VkImageView g_GameCloudHistoryView;
extern VkSampler g_CloudHistorySampler;
extern uint32_t g_SceneCloudHistoryW;
extern uint32_t g_SceneCloudHistoryH;
extern uint32_t g_GameCloudHistoryW;
extern uint32_t g_GameCloudHistoryH;
extern bool g_SceneCloudHistoryNeedsClear;
extern bool g_GameCloudHistoryNeedsClear;

void CleanupAOAndSSGIHistoryTextures();

void EnsureAOHistoryTexture(bool sceneHistory, uint32_t w, uint32_t h);
void PrepareAOHistoryForRead(VkCommandBuffer cmd, VkImage history, bool& needsClear);
void CopyAOHistory(VkCommandBuffer cmd, VkImage gtaoImg, VkImage history,
                   uint32_t width, uint32_t height);

void EnsureSSGIHistoryTexture(bool sceneHistory, uint32_t w, uint32_t h);
void PrepareSSGIHistoryForRead(VkCommandBuffer cmd, VkImage history, bool& needsClear);
void CopySSGIHistory(VkCommandBuffer cmd, VkImage ssgiImg, VkImage history,
                     uint32_t width, uint32_t height);

void EnsureCloudHistoryTexture(bool sceneHistory, uint32_t w, uint32_t h);
void PrepareCloudHistoryForRead(VkCommandBuffer cmd, VkImage history, bool& needsClear);
void CopyCloudHistory(VkCommandBuffer cmd, VkImage cloudImg, VkImage history,
                      uint32_t width, uint32_t height);

void EnsureTAAHistoryTexture(uint32_t w, uint32_t h);
void PrepareTAAHistoryForRead(VkCommandBuffer cmd, VkImage history,
                              VkImage prevTaaOutput);
void CopyTAAHistory(VkCommandBuffer cmd, VkImage taaImg, VkImage history);

