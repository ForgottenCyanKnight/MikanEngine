// ComponentRegistry.cpp - 组件元数据注册表实现(Game.dll)
// 单例定义与全部内置组件的元数据注册。Editor.dll 只读遍历/查询,并调用注册的
// add/remove 函数指针(模板在 Game.dll 内实例化,跨 DLL 安全)。
#include "ECS/ComponentRegistry.h"
#include "ECS/Components.h"
#include <cstddef>
#include <cstdio>

namespace ECS {

ComponentRegistry& ComponentRegistry::GetInstance() {
    static ComponentRegistry instance; // 定义在 Game.dll,Editor.dll 导入
    return instance;
}

void ComponentRegistry::Register(const ComponentMeta& meta) {
    assert(Find(meta.typeName) == nullptr && "Component meta registered more than once.");
    m_Components.push_back(meta);
}

const ComponentMeta* ComponentRegistry::Find(const std::string& typeName) const {
    for (const auto& c : m_Components) {
        if (c.typeName == typeName) {
            return &c;
        }
    }
    return nullptr;
}

// 字段表定义辅助
#define CountOf(arr) (sizeof(arr) / sizeof((arr)[0]))
#define FIELD(T, f, type, label)       { #f, label, ECS::FieldType::type, offsetof(T, f) }
#define PATH_FIELD(T, f, label)        { #f, label, ECS::FieldType::Path, offsetof(T, f) }
#define ENUM_FIELD(T, f, names, label) { #f, label, ECS::FieldType::Enum, offsetof(T, f), names }

// ===== 可通用编辑组件的字段表(反射渲染器遍历;Hidden = 运行时字段不显示) =====

// MeshComponent
static const char* const s_MeshTypeNames[] = {
    "无", "立方体", "球体", "平面", "模型", "圆柱", "圆锥", "胶囊", "圆环", "棱锥", nullptr
};
static const FieldMeta s_MeshFields[] = {
    ENUM_FIELD(MeshComponent, type, s_MeshTypeNames, "类型"),
    PATH_FIELD(MeshComponent, modelPath, "模型路径"),
};

// RenderComponent
static const FieldMeta s_RenderFields[] = {
    FIELD(RenderComponent, visible, Bool, "可见"),
    FIELD(RenderComponent, castShadow, Bool, "投射阴影"),
    FIELD(RenderComponent, receiveShadow, Bool, "接收阴影"),
    FIELD(RenderComponent, showAABB, Bool, "显示AABB"),
    FIELD(RenderComponent, showOBB, Bool, "显示OBB"),
    FIELD(RenderComponent, doubleSided, Bool, "双面渲染"),
    FIELD(RenderComponent, wireframe, Bool, "线框渲染"),
};

// VoxModelComponent
static const FieldMeta s_VoxModelFields[] = {
    PATH_FIELD(VoxModelComponent, voxPath, "体素文件"),
    FIELD(VoxModelComponent, isStatic, Bool, "静态体素"),
    FIELD(VoxModelComponent, loaded, Hidden, nullptr),
};

// LockOnTargetComponent
static const FieldMeta s_LockOnTargetFields[] = {
    FIELD(LockOnTargetComponent, enabled, Bool, "可锁定"),
    FIELD(LockOnTargetComponent, aimOffset, Vec3, "瞄准偏移"),
    FIELD(LockOnTargetComponent, priority, Float, "锁定优先级"),
};

// CameraComponent
static const FieldMeta s_CameraFields[] = {
    FIELD(CameraComponent, fov, Float, "视野"),
    FIELD(CameraComponent, nearPlane, Float, "近裁剪面"),
    FIELD(CameraComponent, farPlane, Float, "远裁剪面"),
    FIELD(CameraComponent, isMainCamera, Bool, "主相机"),
    FIELD(CameraComponent, isOrthographic, Bool, "正交"),
    FIELD(CameraComponent, orthographicSize, Float, "正交大小"),
    FIELD(CameraComponent, enableFrustumCulling, Bool, "启用视锥剔除"),
    FIELD(CameraComponent, showFrustumWireframe, Bool, "显示视锥线框"),
    FIELD(CameraComponent, useSubMeshCulling, Bool, "逐子模型剔除"),
    FIELD(CameraComponent, showBVHWireframe, Bool, "显示BVH线框"),
    FIELD(CameraComponent, showCollisionWireframe, Bool, "显示碰撞体线框"),
    FIELD(CameraComponent, useBVHCulling, Bool, "启用BVH剔除"),
    FIELD(CameraComponent, thirdPersonEnabled, Bool, "启用第三人称"),
    FIELD(CameraComponent, thirdPersonTargetName, String, "跟随目标"),
    FIELD(CameraComponent, thirdPersonTargetOffset, Vec3, "目标偏移"),
    FIELD(CameraComponent, thirdPersonDistance, Float, "跟随距离"),
    FIELD(CameraComponent, thirdPersonMinDistance, Float, "最小距离"),
    FIELD(CameraComponent, thirdPersonMaxDistance, Float, "最大距离"),
    FIELD(CameraComponent, thirdPersonYaw, Float, "初始水平角"),
    FIELD(CameraComponent, thirdPersonPitch, Float, "初始俯仰角"),
    FIELD(CameraComponent, thirdPersonMinPitch, Float, "最小俯仰角"),
    FIELD(CameraComponent, thirdPersonMaxPitch, Float, "最大俯仰角"),
    FIELD(CameraComponent, thirdPersonOrbitSensitivity, Float, "环绕灵敏度"),
    FIELD(CameraComponent, thirdPersonZoomSensitivity, Float, "缩放灵敏度"),
    FIELD(CameraComponent, thirdPersonPositionDamping, Float, "位置阻尼"),
    FIELD(CameraComponent, thirdPersonRotationDamping, Float, "旋转阻尼"),
    FIELD(CameraComponent, thirdPersonCaptureMouse, Bool, "持续捕获鼠标"),
    FIELD(CameraComponent, thirdPersonPreserveDistanceWhenOccluded, Bool, "遮挡保持距离"),
    FIELD(CameraComponent, thirdPersonCollisionEnabled, Bool, "启用相机碰撞"),
    FIELD(CameraComponent, thirdPersonCollisionRadius, Float, "相机碰撞半径"),
    FIELD(CameraComponent, thirdPersonCollisionBuffer, Float, "相机碰撞缓冲"),
    FIELD(CameraComponent, thirdPersonCollisionMinDistance, Float, "碰撞最小距离"),
    FIELD(CameraComponent, thirdPersonCollisionDampingIn, Float, "进入碰撞阻尼"),
    FIELD(CameraComponent, thirdPersonCollisionDampingOut, Float, "离开碰撞阻尼"),
    FIELD(CameraComponent, thirdPersonCollisionSmoothingTime, Float, "碰撞保持时间"),
    FIELD(CameraComponent, thirdPersonAimEnabled, Bool, "启用瞄准"),
    FIELD(CameraComponent, thirdPersonAimShoulderOffset, Float, "瞄准肩位偏移"),
    FIELD(CameraComponent, thirdPersonAimFov, Float, "瞄准视野"),
    FIELD(CameraComponent, thirdPersonAimSensitivity, Float, "瞄准灵敏度"),
    FIELD(CameraComponent, thirdPersonAimPositionDamping, Float, "瞄准位置阻尼"),
    FIELD(CameraComponent, thirdPersonAimRotationDamping, Float, "瞄准旋转阻尼"),
    FIELD(CameraComponent, thirdPersonLockOnEnabled, Bool, "启用锁敌"),
    FIELD(CameraComponent, thirdPersonLockTargetName, String, "指定锁敌目标"),
    FIELD(CameraComponent, thirdPersonLockOnMaxDistance, Float, "锁敌最大距离"),
    FIELD(CameraComponent, thirdPersonLockOnLookAtBlend, Float, "锁敌观察权重"),
};

// LightComponent
static const char* const s_LightTypeNames[] = { "方向光", "点光源", "聚光灯", nullptr };
static const FieldMeta s_LightFields[] = {
    ENUM_FIELD(LightComponent, type, s_LightTypeNames, "类型"),
    FIELD(LightComponent, color, Color3, "颜色"),
    FIELD(LightComponent, intensity, Float, "强度"),
    FIELD(LightComponent, range, Float, "范围"),
    FIELD(LightComponent, spotAngle, Float, "聚光角度"),
    FIELD(LightComponent, castShadow, Bool, "投射阴影"),
};

// SkyboxComponent
static const FieldMeta s_SkyboxFields[] = {
    FIELD(SkyboxComponent, enabled, Bool, "启用"),
    FIELD(SkyboxComponent, textureName, String, "纹理名"),
    FIELD(SkyboxComponent, tint, Color3, "颜色调制"),
    FIELD(SkyboxComponent, intensity, Float, "亮度"),
};

// CloudVolumeComponent
static const FieldMeta s_CloudVolumeFields[] = {
    FIELD(CloudVolumeComponent, enabled, Bool, "启用渲染"),
    FIELD(CloudVolumeComponent, coverage, Float, "云量覆盖率"),
    FIELD(CloudVolumeComponent, density, Float, "消光系数(1/km)"),
    FIELD(CloudVolumeComponent, baseAltitudeKm, Float, "云底高度(km)"),
    FIELD(CloudVolumeComponent, thicknessKm, Float, "云层厚度(km)"),
    FIELD(CloudVolumeComponent, noiseScale, Float, "噪声尺度"),
    FIELD(CloudVolumeComponent, detailErosion, Float, "细节侵蚀"),
    FIELD(CloudVolumeComponent, detailScale, Float, "细节采样倍率"),
    FIELD(CloudVolumeComponent, lightAbsorption, Float, "单次散射反照率(0..1)"),
    FIELD(CloudVolumeComponent, multipleScattering, Float, "各向同性多重散射"),
    FIELD(CloudVolumeComponent, multipleScatteringBuild, Float, "多重散射建立"),
    FIELD(CloudVolumeComponent, multipleScatteringBoundary, Float, "多重散射边界置信度"),
    FIELD(CloudVolumeComponent, multipleScatteringCompress, Float, "多重散射压缩"),
    FIELD(CloudVolumeComponent, noiseOffsetKm, Vec3, "噪声偏移(km)"),
    FIELD(CloudVolumeComponent, windSpeedKmPerSecond, Float, "云风速(km/s)"),
    FIELD(CloudVolumeComponent, windDirectionXZ, Vec2, "云风向(XZ)"),
    FIELD(CloudVolumeComponent, highCloudEnabled, Bool, "启用高层2D云"),
    FIELD(CloudVolumeComponent, highCloudCoverage, Float, "高层云覆盖率"),
    FIELD(CloudVolumeComponent, highCloudDensity, Float, "高层云消光系数(1/km)"),
    FIELD(CloudVolumeComponent, highCloudAltitudeKm, Float, "高层云底高度(km)"),
    FIELD(CloudVolumeComponent, highCloudThicknessKm, Float, "高层云厚度(km)"),
    FIELD(CloudVolumeComponent, highCloudScale, Float, "高层云尺度"),
    FIELD(CloudVolumeComponent, highCloudDetail, Float, "高层云细节"),
    FIELD(CloudVolumeComponent, highCloudBrightness, Float, "高层云亮度"),
    FIELD(CloudVolumeComponent, highCloudWindSpeedKmPerSecond, Float, "高层云风速(km/s)"),
    FIELD(CloudVolumeComponent, highCloudWindDirectionXZ, Vec2, "高层云风向(XZ)"),
};

// Camera2DComponent
static const FieldMeta s_Camera2DFields[] = {
    FIELD(Camera2DComponent, enabled, Bool, "启用"),
    FIELD(Camera2DComponent, center, Vec2, "中心"),
    FIELD(Camera2DComponent, zoom, Float, "缩放"),
    FIELD(Camera2DComponent, followTargetName, String, "跟随目标(实体名)"),
    FIELD(Camera2DComponent, followOffset, Vec2, "跟随偏移"),
    FIELD(Camera2DComponent, dampX, Float, "阻尼X"),
    FIELD(Camera2DComponent, dampY, Float, "阻尼Y"),
    FIELD(Camera2DComponent, lookahead, Vec2, "速度前视"),
    FIELD(Camera2DComponent, useBounds, Bool, "边界限制"),
    FIELD(Camera2DComponent, boundMin, Vec2, "边界Min"),
    FIELD(Camera2DComponent, boundMax, Vec2, "边界Max"),
};

// RigidBody2DComponent
static const char* const s_RB2DTypeNames[] = { "静态", "动态", "运动学", nullptr };
static const FieldMeta s_RigidBody2DFields[] = {
    ENUM_FIELD(RigidBody2DComponent, type, s_RB2DTypeNames, "类型"),
    FIELD(RigidBody2DComponent, mass, Float, "质量"),
    FIELD(RigidBody2DComponent, fixedRotation, Bool, "锁定旋转"),
    FIELD(RigidBody2DComponent, gravityScale, Float, "重力比例"),
};

// Collider2DComponent
static const char* const s_Col2DShapeNames[] = { "盒", "圆", nullptr };
static const FieldMeta s_Collider2DFields[] = {
    ENUM_FIELD(Collider2DComponent, shape, s_Col2DShapeNames, "形状"),
    FIELD(Collider2DComponent, size, Vec2, "尺寸"),
    FIELD(Collider2DComponent, radius, Float, "半径"),
    FIELD(Collider2DComponent, offset, Vec2, "偏移"),
    FIELD(Collider2DComponent, density, Float, "密度"),
    FIELD(Collider2DComponent, friction, Float, "摩擦"),
    FIELD(Collider2DComponent, restitution, Float, "弹性"),
    FIELD(Collider2DComponent, isTrigger, Bool, "触发器"),
};

// WorldComponent
static const char* const s_TerrainTypeNames[] = { "FLAT", "PERLIN", "ADVANCED", nullptr };
static const FieldMeta s_WorldFields[] = {
    FIELD(WorldComponent, enabled, Bool, "启用"),
    FIELD(WorldComponent, renderRadius, Int, "渲染半径"),
    ENUM_FIELD(WorldComponent, terrainType, s_TerrainTypeNames, "地形类型"),
    FIELD(WorldComponent, spawnPosition, Vec3, "出生点"),
};

// TerrainComponent
static const FieldMeta s_TerrainFields[] = {
    FIELD(TerrainComponent, enabled, Bool, "启用"),
    PATH_FIELD(TerrainComponent, heightmapPath, "16-bit 高度图"),
    FIELD(TerrainComponent, worldSize, Vec2, "世界尺寸"),
    FIELD(TerrainComponent, heightScale, Float, "高度缩放"),
    FIELD(TerrainComponent, heightOffset, Float, "高度偏移"),
    FIELD(TerrainComponent, chunkCount, Int, "Chunk 数量"),
    FIELD(TerrainComponent, patchResolution, Int, "Patch 分辨率"),
    FIELD(TerrainComponent, viewDistance, Float, "最大视距"),
    FIELD(TerrainComponent, lod0Distance, Float, "LOD0 距离"),
    FIELD(TerrainComponent, lod1Distance, Float, "LOD1 距离"),
    FIELD(TerrainComponent, maxLod, Int, "最大 LOD"),
    FIELD(TerrainComponent, wireframe, Bool, "线框模式"),
    FIELD(TerrainComponent, collisionEnabled, Bool, "启用碰撞"),
    FIELD(TerrainComponent, collisionResolution, Int, "碰撞采样分辨率"),
    FIELD(TerrainComponent, materialTiling, Float, "材质平铺"),
    FIELD(TerrainComponent, blendSharpness, Float, "混合锐度"),
    PATH_FIELD(TerrainComponent, layer0Path, "材质层 0"),
    PATH_FIELD(TerrainComponent, layer1Path, "材质层 1"),
    PATH_FIELD(TerrainComponent, layer2Path, "材质层 2"),
    PATH_FIELD(TerrainComponent, layer3Path, "材质层 3"),
    PATH_FIELD(TerrainComponent, controlMapPath, "RGBA 控制图"),
};

// WaterComponent
static const FieldMeta s_WaterFields[] = {
    FIELD(WaterComponent, enabled, Bool, "启用"),
    FIELD(WaterComponent, size, Vec2, "水面尺寸"),
    FIELD(WaterComponent, surfaceOffset, Float, "水面局部高度"),
    FIELD(WaterComponent, depth, Float, "浮力深度"),
    FIELD(WaterComponent, buoyancy, Float, "浮力倍率"),
    FIELD(WaterComponent, drag, Float, "水中阻尼"),
    FIELD(WaterComponent, color, Color3, "水面颜色"),
    FIELD(WaterComponent, roughness, Float, "粗糙度"),
    FIELD(WaterComponent, affectPlayersOnly, Bool, "仅影响玩家"),
};

// ColliderComponent
static const char* const s_ColliderTypeNames[] = { "立方体", "球体", "胶囊体", nullptr };
static const FieldMeta s_ColliderFields[] = {
    ENUM_FIELD(ColliderComponent, type, s_ColliderTypeNames, "类型"),
    FIELD(ColliderComponent, offset, Vec3, "偏移"),
    FIELD(ColliderComponent, size, Vec3, "大小"),
    FIELD(ColliderComponent, isTrigger, Bool, "触发器"),
    FIELD(ColliderComponent, useOBB, Bool, "使用OBB"),
    FIELD(ColliderComponent, syncWithModel, Bool, "同步模型"),
    FIELD(ColliderComponent, autoFitToModel, Bool, "自动适配模型"),
    PATH_FIELD(ColliderComponent, modelPath, "模型路径"),
};

// Canvas2DComponent
static const FieldMeta s_Canvas2DFields[] = {
    FIELD(Canvas2DComponent, width, Float, "宽度"),
    FIELD(Canvas2DComponent, height, Float, "高度"),
    FIELD(Canvas2DComponent, stretchToViewport, Bool, "拉伸到视口"),
    FIELD(Canvas2DComponent, layer, Float, "渲染层"),
};

// TextComponent
static const char* const s_TextModeNames[] = { "位图", "SDF", "MSDF", nullptr };
static const FieldMeta s_TextFields[] = {
    FIELD(TextComponent, text, String, "内容"),
    FIELD(TextComponent, isUI, Bool, "UI层"),
    FIELD(TextComponent, fontSize, Float, "字号"),
    FIELD(TextComponent, color, Color4, "颜色"),
    FIELD(TextComponent, layer, Int, "渲染层"),
    ENUM_FIELD(TextComponent, renderMode, s_TextModeNames, "渲染模式"),
    FIELD(TextComponent, measuredWidth, Hidden, nullptr),
    FIELD(TextComponent, measuredHeight, Hidden, nullptr),
};

// ButtonComponent
static const FieldMeta s_ButtonFields[] = {
    FIELD(ButtonComponent, isUI, Bool, "UI层"),
    FIELD(ButtonComponent, width, Float, "宽度"),
    FIELD(ButtonComponent, height, Float, "高度"),
    FIELD(ButtonComponent, fillColor, Color4, "填充色"),
    FIELD(ButtonComponent, hoverColor, Color4, "悬停色"),
    FIELD(ButtonComponent, textColor, Color4, "文字颜色"),
    FIELD(ButtonComponent, text, String, "文字"),
    FIELD(ButtonComponent, fontSize, Float, "字号"),
    FIELD(ButtonComponent, layer, Int, "渲染层"),
    FIELD(ButtonComponent, hovered, Hidden, nullptr),
    FIELD(ButtonComponent, onClick, Hidden, nullptr),
};

// Slice9Component
static const FieldMeta s_Slice9Fields[] = {
    FIELD(Slice9Component, texture, String, "纹理名"),
    FIELD(Slice9Component, isUI, Bool, "UI层"),
    FIELD(Slice9Component, width, Float, "宽度"),
    FIELD(Slice9Component, height, Float, "高度"),
    FIELD(Slice9Component, border, Vec4, "边框"),
    FIELD(Slice9Component, uv0, Vec2, "UV0"),
    FIELD(Slice9Component, uv1, Vec2, "UV1"),
    FIELD(Slice9Component, color, Color4, "颜色"),
    FIELD(Slice9Component, layer, Int, "渲染层"),
    FIELD(Slice9Component, texWidth, Hidden, nullptr),
    FIELD(Slice9Component, texHeight, Hidden, nullptr),
};

// SpriteAnimationComponent
static const FieldMeta s_SpriteAnimFields[] = {
    FIELD(SpriteAnimationComponent, texture, String, "纹理名"),
    FIELD(SpriteAnimationComponent, frameCols, Int, "列数"),
    FIELD(SpriteAnimationComponent, frameRows, Int, "行数"),
    FIELD(SpriteAnimationComponent, frameCount, Int, "帧数(0=自动)"),
    FIELD(SpriteAnimationComponent, fps, Float, "帧率"),
    FIELD(SpriteAnimationComponent, loop, Bool, "循环"),
};

// TilemapComponent
static const FieldMeta s_TilemapFields[] = {
    PATH_FIELD(TilemapComponent, tmxPath, "TMX 路径"),
    PATH_FIELD(TilemapComponent, tilemapFile, "自产地图(.tmap.json)"),
    FIELD(TilemapComponent, layer, Float, "渲染层"),
    FIELD(TilemapComponent, generateColliders, Bool, "生成碰撞"),
    FIELD(TilemapComponent, textureOverride, String, "纹理覆盖(变体名)"),
};

// RigidBodyComponent（3D 刚体；物理已封装为组件：反射字段渲染 + PhysicsSystem 自动同步 body）
static const char* const s_RBTypeNames[] = { "静态", "动态", "运动学", nullptr };
static const char* const s_RBShapeNames[] = { "盒体", "球体", "胶囊体", "OBB", "网格", nullptr };
static const FieldMeta s_RigidBodyFields[] = {
    ENUM_FIELD(RigidBodyComponent, type, s_RBTypeNames, "类型"),
    FIELD(RigidBodyComponent, mass, Float, "质量"),
    FIELD(RigidBodyComponent, useGravity, Bool, "使用重力"),
    FIELD(RigidBodyComponent, isTrigger, Bool, "触发器"),
    FIELD(RigidBodyComponent, restitution, Float, "弹性"),
    ENUM_FIELD(RigidBodyComponent, shapeType, s_RBShapeNames, "碰撞体类型"),
    FIELD(RigidBodyComponent, size, Vec3, "尺寸"),
    FIELD(RigidBodyComponent, offset, Vec3, "偏移"),
    FIELD(RigidBodyComponent, useOBB, Bool, "使用OBB"),
    FIELD(RigidBodyComponent, syncWithModel, Bool, "同步模型"),
    FIELD(RigidBodyComponent, autoFitToModel, Bool, "自动适配模型"),
    PATH_FIELD(RigidBodyComponent, collisionModelPath, "碰撞模型路径"),
    FIELD(RigidBodyComponent, collisionPrecision, Float, "碰撞精度"),
    FIELD(RigidBodyComponent, useConvexHull, Bool, "凸包碰撞（静态关闭可保留孔洞）"),
    FIELD(RigidBodyComponent, maxConvexHullVertices, Int, "凸包顶点上限"),
    FIELD(RigidBodyComponent, generatePerSubmesh, Bool, "每子网格独立凸包"),
};

// PlayerControllerComponent
static const FieldMeta s_PlayerControllerFields[] = {
    FIELD(PlayerControllerComponent, enabled, Bool, "启用"),
    FIELD(PlayerControllerComponent, moveSpeed, Float, "移动速度"),
    FIELD(PlayerControllerComponent, acceleration, Float, "加速度"),
    FIELD(PlayerControllerComponent, airControl, Float, "空中控制"),
    FIELD(PlayerControllerComponent, jumpSpeed, Float, "跳跃速度"),
    FIELD(PlayerControllerComponent, faceMoveDirection, Bool, "朝向移动方向"),
    FIELD(PlayerControllerComponent, cameraName, String, "相对相机"),
};

// AudioSourceComponent（类似 Unity AudioSource；Hidden = 运行时字段不序列化）
static const FieldMeta s_AudioSourceFields[] = {
    PATH_FIELD(AudioSourceComponent, clip, "音频文件"),
    FIELD(AudioSourceComponent, volume, Float, "音量"),
    FIELD(AudioSourceComponent, loop, Bool, "循环"),
    FIELD(AudioSourceComponent, playOnAwake, Bool, "自动播放"),
    FIELD(AudioSourceComponent, playing, Hidden, nullptr),
    FIELD(AudioSourceComponent, playRequested, Hidden, nullptr),
    FIELD(AudioSourceComponent, stopRequested, Hidden, nullptr),
    FIELD(AudioSourceComponent, awakeStarted, Hidden, nullptr),
    FIELD(AudioSourceComponent, lastClip, Hidden, nullptr),
    FIELD(AudioSourceComponent, lastVolume, Hidden, nullptr),
    FIELD(AudioSourceComponent, lastLoop, Hidden, nullptr),
};

// 注册全部内置组件元数据(SceneECS::Init 中调用)
// userAddable=false:内部/基础组件,不允许用户手动添加(由 CreateEmpty/系统自动挂载)
// userRemovable=false:基础组件,移除会破坏渲染/物理等依赖,禁止移除
void RegisterAllComponentMeta() {
    auto& reg = ComponentRegistry::GetInstance();

    // ---- 基础/自动管理(不可增删;序列化走手写/特殊) ----
    reg.RegisterComponent<NameComponent>("名称", "基础", false, false, nullptr, 0, "name");
    reg.RegisterComponent<LockOnTargetComponent>("锁敌目标", "玩法", true, true, s_LockOnTargetFields, CountOf(s_LockOnTargetFields), "lockOnTarget");
    reg.RegisterComponent<TransformComponent>("变换", "基础", false, false, nullptr, 0, "transform");
    reg.RegisterComponent<HierarchyComponent>("层级", "基础", false, false, nullptr, 0, "hierarchy");

    // ---- 渲染(可通用字段编辑 + 通用序列化) ----
    reg.RegisterComponent<MeshComponent>("网格", "渲染", true, true, s_MeshFields, CountOf(s_MeshFields), "mesh");
    reg.RegisterComponent<RenderComponent>("渲染", "渲染", true, true, s_RenderFields, CountOf(s_RenderFields), "render");
    reg.RegisterComponent<MaterialComponent>("材质", "渲染", true, true, nullptr, 0, "material"); // 定制编辑器(纹理加载)
    reg.RegisterComponent<TerrainComponent>("高度图地形", "渲染", true, true, s_TerrainFields, CountOf(s_TerrainFields), "terrain");
    reg.RegisterComponent<WaterComponent>("水体", "渲染", true, true, s_WaterFields, CountOf(s_WaterFields), "water");
    reg.RegisterComponent<VoxModelComponent>("体素模型", "渲染", true, true, s_VoxModelFields, CountOf(s_VoxModelFields), "voxModel");

    // ---- 相机 / 灯光 / 世界 ----
    reg.RegisterComponent<CameraComponent>("相机", "相机", true, true, s_CameraFields, CountOf(s_CameraFields), "camera");
    reg.RegisterComponent<LightComponent>("灯光", "灯光", true, true, s_LightFields, CountOf(s_LightFields), "light");
    reg.RegisterComponent<SkyboxComponent>("天空盒", "渲染", true, true, s_SkyboxFields, CountOf(s_SkyboxFields), "skybox");
    reg.RegisterComponent<CloudVolumeComponent>("体积云", "渲染", true, true, s_CloudVolumeFields, CountOf(s_CloudVolumeFields), "cloudVolume");
    reg.RegisterComponent<Camera2DComponent>("2D 相机", "2D", true, true, s_Camera2DFields, CountOf(s_Camera2DFields), "camera2d");
    reg.RegisterComponent<RigidBody2DComponent>("2D 刚体", "物理", true, true, s_RigidBody2DFields, CountOf(s_RigidBody2DFields), "rigidbody2d");
    reg.RegisterComponent<Collider2DComponent>("2D 碰撞体", "物理", true, true, s_Collider2DFields, CountOf(s_Collider2DFields), "collider2d");
    reg.RegisterComponent<Contact2DComponent>("2D 接触事件", "2D"); // 运行时组件(回调代码绑定),不入存档
    reg.RegisterComponent<WorldComponent>("体素世界", "世界"); // 运行时组件,不入存档
    // 音频源（场景树驱动播放；通用反射序列化 audioSource）
    reg.RegisterComponent<AudioSourceComponent>("音频源", "音频", true, true, s_AudioSourceFields, CountOf(s_AudioSourceFields), "audioSource");

    // ---- 物理 ----
    reg.RegisterComponent<ColliderComponent>("碰撞体", "物理", true, true, s_ColliderFields, CountOf(s_ColliderFields), "collider");
    reg.RegisterComponent<RigidBodyComponent>("刚体", "物理", true, true, s_RigidBodyFields, CountOf(s_RigidBodyFields), "rigidBody"); // 物理封装为组件：反射字段渲染
    reg.RegisterComponent<PlayerControllerComponent>("玩家控制器", "玩法", true, true, s_PlayerControllerFields, CountOf(s_PlayerControllerFields), "playerController");

    // ---- 2D / UI / 动画 ----
    reg.RegisterComponent<Canvas2DComponent>("2D 画布", "2D", true, true, s_Canvas2DFields, CountOf(s_Canvas2DFields), "canvas2d");
    reg.RegisterComponent<Sprite2DComponent>("2D 精灵", "2D", true, true, nullptr, 0, "sprite2d"); // 定制编辑器(按钮条件字段)
    reg.RegisterComponent<TextComponent>("2D 文本", "2D", true, true, s_TextFields, CountOf(s_TextFields), "textComp");
    reg.RegisterComponent<ButtonComponent>("2D 按钮", "2D", true, true, s_ButtonFields, CountOf(s_ButtonFields), "button");
    reg.RegisterComponent<Slice9Component>("九宫格", "2D", true, true, s_Slice9Fields, CountOf(s_Slice9Fields), "slice9");
    reg.RegisterComponent<TweenComponent>("补间动画", "动画", true, true, nullptr, 0, "tween"); // 定制编辑器(播放控制)
    reg.RegisterComponent<SpriteAnimationComponent>("帧动画", "2D", true, true, s_SpriteAnimFields, CountOf(s_SpriteAnimFields), "spriteAnim");
    reg.RegisterComponent<TilemapComponent>("瓦片地图", "2D", true, true, s_TilemapFields, CountOf(s_TilemapFields), "tilemap");

    // 动画控制器（Animator：驱动实体模型的动画播放；time 为运行时回写显示）
    static const FieldMeta s_AnimatorFields[] = {
        FIELD(AnimatorComponent, clipIndex, Int, "动画索引"),
        FIELD(AnimatorComponent, speed, Float, "播放速度"),
        FIELD(AnimatorComponent, loop, Bool, "循环"),
        FIELD(AnimatorComponent, playing, Bool, "播放"),
        FIELD(AnimatorComponent, time, Float, "当前时间"),
    };
    reg.RegisterComponent<AnimatorComponent>("动画控制器", "动画", true, true, s_AnimatorFields, CountOf(s_AnimatorFields), "animator");

    static const char* const s_VmdTargetNames[] = {
        "自动", "PMX/PMD 模型", "相机", nullptr
    };
    static const FieldMeta s_VmdFields[] = {
        PATH_FIELD(VmdPlayerComponent, motionPath, "VMD 动作文件"),
        ENUM_FIELD(VmdPlayerComponent, target, s_VmdTargetNames, "绑定目标"),
        FIELD(VmdPlayerComponent, speed, Float, "播放速度"),
        FIELD(VmdPlayerComponent, loop, Bool, "循环"),
        FIELD(VmdPlayerComponent, playing, Bool, "播放"),
        FIELD(VmdPlayerComponent, enabled, Bool, "启用"),
        FIELD(VmdPlayerComponent, startFrame, Float, "起始帧"),
        FIELD(VmdPlayerComponent, currentFrame, Hidden, nullptr),
    };
    reg.RegisterComponent<VmdPlayerComponent>("VMD 播放器", "动画", true, true,
                                               s_VmdFields, CountOf(s_VmdFields), "vmd");

    // 脚本组件（Unity 式玩法挂载）：scriptName + paramsJson(Hidden)；params 对象由 SceneSerializer 手写序列化
    static const FieldMeta s_ScriptFields[] = {
        FIELD(ScriptComponent, scriptName, String, "脚本类"),
        FIELD(ScriptComponent, paramsJson, Hidden, nullptr),
    };
    reg.RegisterComponent<ScriptComponent>("脚本", "脚本", true, true, s_ScriptFields, CountOf(s_ScriptFields), "script");

    printf("[ComponentRegistry] Registered %zu component meta entries\n", reg.GetAll().size());
}

} // namespace ECS
