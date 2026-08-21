#pragma once
#include "Platform/Export.h"

#include "ECS/ECS.h"
#include "ECS/SceneECS.h"
#include "ECS/ComponentRegistry.h"
#include <string>
#include <vector>
#include <map>
#include <regex>

namespace ECS {

// 场景序列化器
class MIKAN_API SceneSerializer {
public:
    // 保存场景到文件
    bool SaveScene(const std::string& filepath);
    
    // 从文件加载场景
    bool LoadScene(const std::string& filepath);
    
    std::string OpenFileDialog();
    std::string OpenTextureDialog();   // 2026-08 材质面板：过滤 png/jpg/ktx2
    std::string SaveFileDialog();
    
    // 保存场景到字符串
    std::string SerializeScene();
    
    // 从字符串加载场景
    bool DeserializeScene(const std::string& jsonString);

    // ===== 预制体（Unity 式可复用实体模板）=====
    // 保存实体子树（含全部子实体与组件）为 prefab 文件：{"name":..., "entities":[...]}，
    // 实体 id 归一化为 0..N（0=根），hierarchy 内部引用重映射。
    bool SavePrefab(Entity rootEntity, const std::string& filepath);
    // 从 prefab 文件实例化实体树到场景（parent 指定父实体，INVALID=场景根）；返回新根实体，失败返回 INVALID_ENTITY。
    Entity InstantiatePrefab(const std::string& filepath, Entity parent = INVALID_ENTITY);
    
private:
    // 序列化实体
    std::string SerializeEntity(Entity entity);
    
    // 序列化组件
    std::string SerializeNameComponent(Entity entity);
    std::string SerializeTransformComponent(Entity entity);
    std::string SerializeHierarchyComponent(Entity entity);
    std::string SerializeMeshComponent(Entity entity);
    std::string SerializeRenderComponent(Entity entity);
    std::string SerializeCameraComponent(Entity entity);
    std::string SerializeLightComponent(Entity entity);
    std::string SerializeMaterialComponent(Entity entity);
    std::string SerializeRigidBodyComponent(Entity entity);
    std::string SerializeColliderComponent(Entity entity);
    std::string SerializeVoxModelComponent(Entity entity);
    std::string SerializeScriptComponent(Entity entity);
    // 2D 组件
    std::string SerializeCanvas2DComponent(Entity entity);
    std::string SerializeSprite2DComponent(Entity entity);
    std::string SerializeTextComponent(Entity entity);
    std::string SerializeButtonComponent(Entity entity);
    std::string SerializeSlice9Component(Entity entity);
    std::string SerializeTweenComponent(Entity entity);
    
    // 反序列化实体
    Entity DeserializeEntity(const std::string& jsonString, std::map<int, Entity>& entityMap);
    
    // 反序列化组件
    void DeserializeNameComponent(Entity entity, const std::string& jsonString);
    void DeserializeTransformComponent(Entity entity, const std::string& jsonString);
    void DeserializeHierarchyComponent(Entity entity, const std::string& jsonString, std::map<int, Entity>& entityMap);
    void DeserializeMeshComponent(Entity entity, const std::string& jsonString);
    void DeserializeRenderComponent(Entity entity, const std::string& jsonString);
    void DeserializeCameraComponent(Entity entity, const std::string& jsonString);
    void DeserializeLightComponent(Entity entity, const std::string& jsonString);
    void DeserializeMaterialComponent(Entity entity, const std::string& jsonString);
    void DeserializeRigidBodyComponent(Entity entity, const std::string& jsonString);
    void DeserializeColliderComponent(Entity entity, const std::string& jsonString);
    void DeserializeVoxModelComponent(Entity entity, const std::string& jsonString);
    void DeserializeScriptComponent(Entity entity, const std::string& jsonString);
    // 2D 组件
    void DeserializeCanvas2DComponent(Entity entity, const std::string& jsonString);
    void DeserializeSprite2DComponent(Entity entity, const std::string& jsonString);
    void DeserializeTextComponent(Entity entity, const std::string& jsonString);
    void DeserializeButtonComponent(Entity entity, const std::string& jsonString);
    void DeserializeSlice9Component(Entity entity, const std::string& jsonString);
    void DeserializeTweenComponent(Entity entity, const std::string& jsonString);
    
    // 辅助函数
    std::string EscapeString(const std::string& str);
    std::string UnescapeString(const std::string& str);
    std::string ExtractValue(const std::string& json, const std::string& key);
    std::vector<std::string> ExtractArray(const std::string& json, const std::string& key);
    std::vector<float> ParseFloatArray(const std::string& arrayStr);
    bool ExtractBoolValue(const std::string& json, const std::string& key);
    std::string NormalizePath(const std::string& path);
    std::string ConvertToRelativePath(const std::string& absolutePath);

    // ===== 通用反射序列化(字段反射 1b)=====
    // 按 ComponentMeta 字段表序列化组件字段(返回字段 JSON 行,不带组件 key 包裹)
    std::string SerializeComponentByMeta(const ComponentMeta& meta, const void* comp);
    // 按 ComponentMeta 字段表从 JSON 反序列化组件字段(comp 指向已存在的组件实例)
    void DeserializeComponentByMeta(const ComponentMeta& meta, void* comp, const std::string& jsonString);
    
    // 辅助函数清理场景
    void ClearScene();
};

} // namespace ECS
