#pragma once
#include "Platform/Export.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <vulkan/vulkan.h>
#include "Types.h"
#include "World/WorldTypes.h"
#include "Core/TmxLoader.h"

namespace ECS {

// 名称组件
struct MIKAN_API NameComponent {
    std::string name = "Entity";
};

// 可被第三人称相机自动锁定的目标(通常挂在敌人/可交互角色上)。
// 相机跟随目标仍由 CameraComponent.thirdPersonTargetName 指定，二者职责分离。
struct MIKAN_API LockOnTargetComponent {
    bool enabled = true;
    glm::vec3 aimOffset = glm::vec3(0.0f, 1.0f, 0.0f);
    float priority = 0.0f;
};

// 变换组件
struct MIKAN_API TransformComponent {
    glm::vec3 position = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 scale = glm::vec3(1.0f);

    // ===== 世界矩阵版本缓存(由 SceneECS::GetWorldMatrix 维护) =====
    // 任何本地变换字段被直接修改的代码都必须调用 MarkDirty()(或走 SceneECS::SetPosition 等 setter),
    // 否则渲染读取的世界矩阵缓存不会失效。子实体无需传播:GetWorldMatrix 比较父的 worldVersion 惰性重算。
    uint32_t localVersion = 0;                        // 本地修改版本:每次 MarkDirty 递增
    uint64_t worldVersion = 0;                        // 本节点世界版本:缓存重算时写入,供子节点判断父是否变化
    uint64_t cachedWorldVersion = 0;                  // 计算缓存时的"父worldVersion + localVersion"组合
    Entity cachedParentEntity = INVALID_ENTITY;       // 计算缓存时的父实体
    glm::mat4 cachedWorldMatrix = glm::mat4(1.0f);    // 世界矩阵缓存
    bool worldCacheValid = false;                     // 缓存是否已建立

    // 本地变换被直接修改后调用(物理同步、编辑器、输入、Tween 等写点)
    void MarkDirty() { ++localVersion; }

    glm::mat4 GetModelMatrix() const {
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, position);
        model = model * glm::mat4_cast(rotation);
        model = glm::scale(model, scale);
        return model;
    }

    glm::vec3 GetEulerAngles() const {
        return glm::degrees(glm::eulerAngles(rotation));
    }

    void SetEulerAngles(const glm::vec3& euler) {
        rotation = glm::quat(glm::radians(euler));
        MarkDirty();
    }
};

// 层级组件 - 支持父子关系
struct MIKAN_API HierarchyComponent {
    Entity parent = INVALID_ENTITY;
    std::vector<Entity> children;
};

// 网格类型
enum class MeshType {
    None,
    Cube,
    Sphere,
    Plane,
    Model,
    Cylinder,
    Cone,
    Capsule,
    Torus,
    Pyramid
};

// 网格组件
struct MIKAN_API MeshComponent {
    MeshType type = MeshType::None;
    std::string modelPath = "";
    // 后续可以添加材质、模型资源等
};

// 体素模型组件
struct MIKAN_API VoxModelComponent {
    std::string voxPath = "";  // .vox 文件路径
    bool loaded = false;       // 是否已加载
    bool isStatic = true;      // 是否为静态（静态使用 mesh 渲染，动态使用实例化面渲染）
};

// 渲染组件
struct MIKAN_API RenderComponent {
    bool visible = true;
    bool castShadow = true;
    bool receiveShadow = true;
    bool showAABB = false;  // 是否显示 AABB 包围盒线框（轴向）
    bool showOBB = false;   // 是否显示 OBB 包围盒线框（旋转）
    bool doubleSided = false;  // 是否双面渲染
    bool wireframe = false;  // 是否线框渲染
};

// 激活状态组件(已移除:组件存在与否即"激活",无需显式开关)

// 碰撞体组件（基础）
struct MIKAN_API ColliderComponent {
    enum class Type {
        Box,
        Sphere,
        Capsule
    };
    Type type = Type::Box;
    glm::vec3 offset = glm::vec3(0.0f);
    glm::vec3 size = glm::vec3(1.0f);
    bool isTrigger = false;
    
    // OBB 包围盒选项
    bool useOBB = false;  // 使用 OBB（旋转包围盒）而非 AABB
    bool syncWithModel = true;  // 实时同步模型位置/旋转；缩放始终自动同步到碰撞形状
    bool autoFitToModel = false;  // 使用模型静态 AABB 自动计算尺寸/局部中心
    std::string modelPath;  // 关联的模型路径（用于从模型生成碰撞体）
};

// 相机组件
struct MIKAN_API CameraComponent {
    float fov = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    bool isMainCamera = false;
    bool isOrthographic = false;
    float orthographicSize = 5.0f;
    
    bool enableFrustumCulling = false;
    bool showFrustumWireframe = true;
    bool useSubMeshCulling = true;
    bool showBVHWireframe = false;
    bool showCollisionWireframe = false; // 场景视图显示全局三维碰撞体线框
    bool useBVHCulling = false;

    // 基础 3D 第三人称相机配置。运行时状态(当前轨道角度/距离/阻尼位置)
    // 由 ThirdPersonCameraSystem 持有，避免把帧间状态写回场景资源。
    bool thirdPersonEnabled = false;
    std::string thirdPersonTargetName;
    glm::vec3 thirdPersonTargetOffset = glm::vec3(0.0f, 1.5f, 0.0f);
    float thirdPersonDistance = 5.0f;
    float thirdPersonMinDistance = 2.0f;
    float thirdPersonMaxDistance = 12.0f;
    float thirdPersonYaw = 180.0f;
    float thirdPersonPitch = 15.0f;
    float thirdPersonMinPitch = -25.0f;
    float thirdPersonMaxPitch = 70.0f;
    float thirdPersonOrbitSensitivity = 0.12f;
    float thirdPersonZoomSensitivity = 1.0f;
    float thirdPersonPositionDamping = 12.0f;
    float thirdPersonRotationDamping = 16.0f;
    bool thirdPersonCaptureMouse = false; // false: 按住鼠标右键环绕; true: 始终捕获鼠标

    // 第三人称相机遮挡策略：默认保持用户选择的轨道距离与角度。
    // 玩家与相机之间被场景物体遮挡时暂不缩距，后续由遮挡模型淡化/虚化处理。
    bool thirdPersonPreserveDistanceWhenOccluded = true;

    // 可选的真实相机碰撞修正：优先使用场景 Collider/模型 AABB，体素世界仅作可选兜底。
    // 仅当上面的保持距离开关关闭时，命中阻挡物才会缩短相机距离。
    bool thirdPersonCollisionEnabled = true;
    float thirdPersonCollisionRadius = 0.2f;
    float thirdPersonCollisionBuffer = 0.08f;
    float thirdPersonCollisionMinDistance = 0.75f;
    float thirdPersonCollisionDampingIn = 24.0f;
    float thirdPersonCollisionDampingOut = 6.0f;
    float thirdPersonCollisionSmoothingTime = 0.12f;

    // 瞄准模式：默认按住左/右 Shift 进入。瞄准时相机向屏幕右侧肩位偏移，
    // 仍看向 targetOffset 指定的瞄准点，并平滑收窄 FOV。
    bool thirdPersonAimEnabled = false;
    float thirdPersonAimShoulderOffset = 0.75f;
    float thirdPersonAimFov = 50.0f;
    float thirdPersonAimSensitivity = 0.08f;
    float thirdPersonAimPositionDamping = 18.0f;
    float thirdPersonAimRotationDamping = 20.0f;

    // Zelda 风格锁敌：按 Q 切换，自动从带 LockOnTargetComponent 的实体中选目标。
    bool thirdPersonLockOnEnabled = false;
    std::string thirdPersonLockTargetName;
    float thirdPersonLockOnMaxDistance = 25.0f;
    float thirdPersonLockOnLookAtBlend = 0.5f; // 0=只看玩家, 1=只看敌人

    glm::mat4 GetProjectionMatrix(float aspectRatio) const {
        if (isOrthographic) {
            return glm::ortho(-orthographicSize * aspectRatio, orthographicSize * aspectRatio,
                             -orthographicSize, orthographicSize, nearPlane, farPlane);
        }
        return glm::perspective(glm::radians(fov), aspectRatio, nearPlane, farPlane);
    }

    glm::mat4 GetViewMatrix(const glm::vec3& position, const glm::quat& rotation) const {
        glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, -1.0f);
        glm::vec3 up = rotation * glm::vec3(0.0f, 1.0f, 0.0f);
        return glm::lookAt(position, position + forward, up);
    }
};

// 灯光组件
struct MIKAN_API LightComponent {
    enum class Type {
        Directional,
        Point,
        Spot
    };
    Type type = Type::Directional;
    glm::vec3 color = glm::vec3(1.0f);
    float intensity = 1.0f;
    float range = 10.0f;
    float spotAngle = 45.0f;
    bool castShadow = true;
};

// 天空盒组件（场景树标准化控制；实体带此组件即接管天空盒渲染）
// 渲染时取场景中第一个带 SkyboxComponent 的实体（按 root 顺序遍历）；
// 场景中无此组件时**不渲染**天空盒，背景显示渲染目标清屏颜色。
struct MIKAN_API SkyboxComponent {
    bool enabled = true;                 // 是否渲染天空盒
    std::string textureName = "skybox";  // TexturePool 中 cubemap 注册名（引擎默认资产）
    glm::vec3 tint = glm::vec3(1.0f);    // 颜色调制（乘到采样色）
    float intensity = 1.0f;              // 亮度倍数
};

// 体积云控制组件：实体存在即把云参数接入后处理 Camera UBO。
// 组件由渲染器每帧读取，属性面板的修改无需重建场景或后处理链即可实时预览。
// 当前原型只支持一个有效 CloudVolumeComponent；场景树遍历到的第一个组件生效。
struct MIKAN_API CloudVolumeComponent {
    bool enabled = true;                 // 是否执行体积云输出
    float coverage = 0.45f;              // 基础覆盖率（越高云越密）
    float density = 0.55f;               // 体积消光系数（1/km）
    float baseAltitudeKm = 7.5f;         // 云底高度（km）
    float thicknessKm = 12.5f;           // 云层厚度（km）
    float noiseScale = 0.01f;            // 世界公里坐标到 3D 噪声 UVW 的尺度
    float detailErosion = 0.24f;         // 高频细节侵蚀强度
    float detailScale = 4.0f;             // 高频 32^3 纹理相对基础形状的采样倍率
    float lightAbsorption = 1.0f;        // 兼容旧字段名；单次散射反照率（0..1）
    float multipleScattering = 0.22f;   // 有界多阶散射强度
    float multipleScatteringBuild = 0.15f; // PhiFwd 各向同性建立率（0..1）
    float multipleScatteringBoundary = 0.65f; // 真实光路边界项混合
    float multipleScatteringCompress = 0.35f; // 高阶散射衰减
    glm::vec3 noiseOffsetKm = glm::vec3(37.0f, 13.0f, -61.0f); // 周期噪声偏移（km）
    float windSpeedKmPerSecond = 0.01f;  // 体积云平移速度（km/s）
    glm::vec2 windDirectionXZ = glm::vec2(1.0f, 0.0f); // 世界 XZ 平面风向
    // 高层云按 Nubis 的独立 2D 滚动层处理，不参与低层 3D 密度场。
    bool highCloudEnabled = true;
    float highCloudCoverage = 0.28f;     // 2D 覆盖率
    float highCloudDensity = 0.35f;       // 2D 层消光系数（1/km）
    float highCloudAltitudeKm = 18.0f;    // 高层云底海拔（km）
    float highCloudThicknessKm = 1.0f;    // 2D 云层厚度（km）
    float highCloudScale = 0.0045f;       // 球面展开图尺度（cycles/km）
    float highCloudDetail = 0.45f;        // 2D 多频细节权重
    float highCloudBrightness = 0.32f;    // 高层云相对直接光照亮度
    float highCloudWindSpeedKmPerSecond = 0.006f; // 高层云风速（km/s）
    glm::vec2 highCloudWindDirectionXZ = glm::vec2(0.35f, 1.0f); // 高层云风向
};

// 2D 正交相机组件(驱动 Canvas2D 世界玩法层：center=世界坐标中心, zoom=缩放)
// 场景中第一个带此组件且 enabled 的实体控制 2D 玩法相机；无则默认 (0,0,1)。
// ===== Cinemachine 风格智能相机(序5): followTargetName/damp*/lookahead/useBounds 全部可选,
// 默认值保持"无目标 = 静态相机", 不改变现有场景行为。由 Camera2DSystem 每帧计算并写回 center。=====
struct MIKAN_API Camera2DComponent {
    bool enabled = true;
    glm::vec2 center = glm::vec2(0.0f);
    float zoom = 1.0f;

    // ===== 跟随(序列化) =====
    std::string followTargetName = "";      // 跟随目标实体名(空 = 不跟随; 加载后解析为 followTarget)
    glm::vec2 followOffset = glm::vec2(0.0f); // 相对目标中心的常驻偏移(世界坐标)
    float dampX = 8.0f;                     // x 轴阻尼速度(帧率无关指数平滑; 0 = 瞬移)
    float dampY = 8.0f;                     // y 轴阻尼速度
    glm::vec2 lookahead = glm::vec2(0.0f);  // 速度前视系数(目标速度 × 系数 加到目标位置; 0 = 关)

    // ===== 边界限制 confiner(序列化) =====
    bool useBounds = false;                 // false = 不限制
    glm::vec2 boundMin = glm::vec2(0.0f);   // 相机中心允许范围(视野边缘由系统按视口/zoom 收窄)
    glm::vec2 boundMax = glm::vec2(0.0f);

    // ===== 屏幕震动(运行时, 不序列化; ShakeCamera 触发, 系统衰减) =====
    float shakeStrength = 0.0f;             // 当前强度(像素; 0 = 不震)
    float shakeTotalDuration = 0.0f;        // 总时长(衰减比例基准)
    float shakeDuration = 0.0f;             // 剩余时长(秒)
    float shakeFrequency = 24.0f;           // 震动频率(Hz)
    float shakeElapsed = 0.0f;              // 已震动时间(噪声相位)

    // ===== 运行时(系统维护, 不序列化) =====
    ECS::Entity followTarget = ECS::INVALID_ENTITY; // 解析后的目标实体(按名字懒解析)
    glm::vec2 currentCenter = glm::vec2(0.0f);      // 阻尼后当前位置(初始 = center)
    glm::vec2 prevTargetPos = glm::vec2(0.0f);      // 目标上一帧位置(速度估计)
    bool initialized = false;                       // currentCenter 是否已初始化(首帧直接定住)
};

// 2D 刚体组件(Box2D 物理,画布坐标 y 向下,像素单位)
// body 为运行时句柄(指向 b2Body), 不序列化。
struct MIKAN_API RigidBody2DComponent {
    enum class Type { Static = 0, Dynamic = 1, Kinematic = 2 };
    Type type = Type::Dynamic;
    float mass = 1.0f;          // 质量(Box2D 由密度+面积推导, 此处仅记录)
    bool fixedRotation = false;
    float gravityScale = 1.0f;
    void* body = nullptr;       // 运行时 b2Body*(Physics2DSystem 管理)
};

// 2D 碰撞体组件(配合 RigidBody2D;盒/圆, offset 相对实体中心)
struct MIKAN_API Collider2DComponent {
    enum class Shape { Box = 0, Circle = 1 };
    Shape shape = Shape::Box;
    glm::vec2 size = glm::vec2(1.0f);    // Box: 全宽高
    float radius = 0.5f;                 // Circle: 半径
    glm::vec2 offset = glm::vec2(0.0f);  // 相对实体中心的偏移
    float density = 1.0f;
    float friction = 0.5f;
    float restitution = 0.0f;            // 弹性
    bool isTrigger = false;              // sensor(只检测不碰撞)
};

// 2D 精灵帧动画组件(序2): 驱动同实体 Sprite2DComponent 的 uv0/uv1 逐帧切换// 支持两种帧布局:
//   1. 规则网格: frameCols×frameRows 均匀切分纹理(如 4x4 精灵表), 字段可序列化
//   2. 显式帧矩形表: frames 每项 = 纹理像素矩形 {x,y,w,h}(Aseprite/TexturePacker 图集),
//      运行时设置(不序列化); 非空时优先于规则网格
// 由 SpriteAnimatorSystem 每帧推进 currentFrame 并写回 Sprite2DComponent.uv0/uv1(渲染器无需改动)。
struct MIKAN_API SpriteAnimationComponent {
    std::string texture = "";       // 动画纹理名(Renderer2D 已注册; 通常与 Sprite2D.texture 相同)
    int frameCols = 1;              // 规则网格列数
    int frameRows = 1;              // 规则网格行数
    int frameCount = 0;             // 总帧数(0 = 自动 cols*rows; 可小于 cols*rows 截断)
    float fps = 12.0f;              // 播放速度(帧/秒)
    bool loop = true;               // true 循环 / false 播完停末帧

    // ===== 显式帧矩形表(可选; 非空优先; 纹理像素坐标, 0=顶部) =====
    std::vector<glm::ivec4> frames; // {x, y, w, h}; 运行时设置, 不序列化
    int texWidth = 0;               // 纹理像素宽(帧表归一化用; 运行时设置)
    int texHeight = 0;              // 纹理像素高

    // ===== 运行时(系统推进, 不序列化) =====
    int currentFrame = 0;
    float elapsed = 0.0f;           // 帧内累计时间
    bool playing = true;            // false = 冻结在当前帧
};

// 瓦片地图组件(用户优先级: TMX 导入 + Box2D 碰撞)
// tmxPath = Tiled 地图文件(相对项目资产根, 序列化); 加载后由 TilemapSystem 解析并
// 生成渲染数据与 Box2D 静态碰撞体; 世界坐标 = 实体 Transform.position 为地图左上角原点。
struct MIKAN_API TilemapComponent {
    std::string tmxPath = "";       // 相对资产根的 .tmx 路径(序列化; 二选一: tmxPath 或 tilemapFile)
    std::string tilemapFile = "";   // 相对资产根的引擎自产地图 .tmap.json(序列化; 编辑器瓦片绘制产物)
    float layer = 0.0f;             // 渲染层(数值越大越在上层)
    bool generateColliders = true;  // 是否按瓦片 collision 属性生成 Box2D 静态体(序列化)
    std::string textureOverride = ""; // 图集纹理覆盖名(调色板变体等; 空 = 用 tileset 图片, 序列化)

    // ===== 运行时(TilemapSystem 填充, 不序列化) =====
    bool loaded = false;
    Tmx::Map map;
    std::string textureName = "";   // 已注册的图集纹理名
    std::vector<void*> colliderBodies; // 生成的 b2BodyId 存储(void*, 与 Physics2D 约定一致)
};

// 2D 接触事件组件(运行时绑定回调, 不序列化)
// 挂在带 RigidBody2D + Collider2D 的实体上;Physics2DSystem 每帧轮询 Box2D 接触事件后触发。
// 覆盖两类事件: 普通碰撞(两个非传感器形状 begin/end touch)与触发器重叠(isTrigger 传感器进入/离开)。
// 事件双方都会收到回调(参数为对方实体 id);回调在主线程物理结算后调用, 内部可安全增删组件/实体。
struct MIKAN_API Contact2DComponent {
    bool enabled = true;                            // 总开关(false 跳过派发)
    std::function<void(ECS::Entity other)> onEnter; // 开始接触/重叠
    std::function<void(ECS::Entity other)> onExit;  // 接触/重叠结束
};

// 刚体组件
struct MIKAN_API RigidBodyComponent {
    enum class Type {
        Static,
        Dynamic,
        Kinematic
    };
    
    Type type = Type::Dynamic;
    float mass = 1.0f;
    bool useGravity = true;
    bool isTrigger = false;
    float restitution = 0.5f; // 弹性系数（0-1，0=完全非弹性，1=完全弹性）
    
    // 碰撞体类型
    enum class ShapeType {
        Box,
        Sphere,
        Capsule,
        OBB,  // 有向包围盒
        Mesh  // 从模型生成的碰撞体
    };
    
    ShapeType shapeType = ShapeType::Box;
    glm::vec3 size = glm::vec3(1.0f);
    glm::vec3 offset = glm::vec3(0.0f);
    
    // OBB 和同步选项
    bool useOBB = false;  // 使用 OBB（旋转包围盒）
    bool syncWithModel = false;  // 同步模型位置/旋转（动态刚体默认关闭）；模型缩放始终自动同步
    bool autoFitToModel = false;  // 根据模型静态 AABB 自动计算碰撞尺寸/局部中心
    
    // 精密碰撞体生成选项
    std::string collisionModelPath = "";  // 用于生成碰撞体的模型路径
    float collisionPrecision = 0.01f;  // 碰撞体生成精度，值越小精度越高
    // 静态 Mesh 关闭后使用原始三角面，保留桥洞/门洞等凹形空间；动态/运动学 Mesh 会自动回退为凸包。
    bool useConvexHull = true;  // 是否使用凸包生成碰撞体
    int maxConvexHullVertices = 256;  // 凸包最大顶点数
    bool generatePerSubmesh = false;  // 凸包路径下是否为每个子网格生成独立的碰撞体
};

// 基于动态刚体的最小可用 3D 玩家控制器。
// 位置/重力/地形接触由 RigidBodyComponent + PhysicsSystem 负责；本组件只保存
// 输入映射无关的移动参数，因此后续可以被脚本或编辑器直接复用。
struct MIKAN_API PlayerControllerComponent {
    bool enabled = true;
    float moveSpeed = 12.0f;       // 水平移动速度（世界单位/秒）
    float acceleration = 45.0f;    // 水平速度响应
    float airControl = 0.35f;      // 空中水平控制比例
    float jumpSpeed = 10.0f;       // 跳跃初速度
    bool faceMoveDirection = true; // 移动时让模型朝向移动方向
    std::string cameraName;        // 空字符串=使用主相机；用于相机相对移动
};

// 材质组件
struct MIKAN_API MaterialComponent {
    // 纹理路径
    std::string albedoPath = "";
    std::string normalPath = "";
    std::string roughnessPath = "";
    std::string metallicPath = "";
    std::string aoPath = "";
    std::string emissivePath = "";
    
    // 纹理采样方式
    int albedoSamplerType = 0;     // 0: Linear, 1: Nearest, 2: LinearClamp, 3: NearestClamp
    int normalSamplerType = 0;
    int roughnessSamplerType = 0;
    int metallicSamplerType = 0;
    int aoSamplerType = 0;
    int emissiveSamplerType = 0;
    
    // 材质参数
    glm::vec3 albedoColor = glm::vec3(1.0f);
    float metallic = 0.0f;
    float roughness = 0.5f;
    float ao = 1.0f;
    float emissiveIntensity = 0.0f;
    
    // 是否使用纹理
    bool useAlbedoTexture = false;
    bool useNormalTexture = false;
    bool useRoughnessTexture = false;
    bool useMetallicTexture = false;
    bool useAOTexture = false;
    bool useEmissiveTexture = false;
};

// 高度图地形组件。
// 高度图使用 16-bit 灰度 PNG 等常见外部工具格式；图层纹理和可选 control map
// 只保存路径，由 TerrainRenderer 统一加载、缓存和绑定，避免在 ECS 中存 GPU 句柄。
struct MIKAN_API TerrainComponent {
    bool enabled = true;
    std::string heightmapPath = "";

    glm::vec2 worldSize = glm::vec2(256.0f, 256.0f); // X/Z 世界尺寸
    float heightScale = 64.0f;
    float heightOffset = 0.0f;

    int chunkCount = 8;
    int patchResolution = 33; // 近处 patch 顶点数；中/远 LOD 为 17/9
    float viewDistance = 2000.0f;
    float lod0Distance = 200.0f;
    float lod1Distance = 600.0f;
    int maxLod = 2;

    // 编辑器辅助：线框模式（VK_POLYGON_MODE_LINE 渲染地形网格，便于观察 patch/LOD 结构）
    bool wireframe = false;

    // 碰撞与渲染 LOD 解耦：只生成一份稳定的静态低分辨率网格，避免
    // 相机远近变化时角色突然失去支撑。collisionResolution 是每个轴的
    // 最大采样数，默认 257x257；大型世界可进一步拆分为流式碰撞块。
    bool collisionEnabled = true;
    int collisionResolution = 257;

    float materialTiling = 8.0f;
    float blendSharpness = 1.0f;
    std::string layer0Path = "";
    std::string layer1Path = "";
    std::string layer2Path = "";
    std::string layer3Path = "";
    std::string controlMapPath = ""; // RGBA 权重图；为空时使用高度/坡度规则混合
};

// 水体组件：第一阶段只负责水平水面网格和玩法层浮力。
// 水面默认位于实体 Transform 的世界位置，surfaceOffset 可用于在实体局部
// 空间抬高/降低水面；size 是水面完整 X/Z 尺寸，depth 是向下的可浮力范围。
// 当前阶段按不透明 G-buffer 几何绘制，不启用透明混合；后续由 mask 后处理接管。
struct MIKAN_API WaterComponent {
    bool enabled = true;
    glm::vec2 size = glm::vec2(32.0f, 32.0f);
    float surfaceOffset = 0.0f;
    float depth = 20.0f;
    float buoyancy = 1.15f; // 1.0 约等于抵消重力；大于 1 会把物体托向水面
    float drag = 2.0f;      // 速度阻尼系数(1/s)，按浸入比例施加
    glm::vec3 color = glm::vec3(0.035f, 0.22f, 0.32f);
    float roughness = 0.12f;
    bool affectPlayersOnly = true;
};

// 体素世界组件（从 OpenGL 版迁移的无限体素世界）
// 挂到任意实体上表示"场景启用体素世界"；世界随主相机自动生成/渲染
struct MIKAN_API WorldComponent {
    bool enabled = true;                 // 是否启用世界
    int renderRadius = 16;               // 渲染半径（chunk 数）
    ConfigTerrainType terrainType = ADVANCED; // FLAT / PERLIN / ADVANCED
    glm::vec3 spawnPosition = glm::vec3(8.0f, 150.0f, 8.0f); // 出生点（抬高避免在地下）
};

// 2D 画布组件（Unity Canvas 模型：UI 实体作为画布子节点，位置为相对画布的屏幕坐标，左下原点）
// 场景树结构: Canvas(带本组件) ── 子级 UI 实体(带 Sprite2DComponent, isUI=true)
struct MIKAN_API Canvas2DComponent {
    float width = 1920.0f;            // 画布逻辑宽度(像素)
    float height = 1080.0f;           // 画布逻辑高度(像素)
    bool stretchToViewport = true;    // 画布拉伸到视口(忽略 width/height)
    float layer = 0.0f;               // 画布渲染层
};

// 2D 精灵组件（Canvas2D 渲染：isUI=false 世界层/玩法，isUI=true 屏幕层/UI）
// 变换用 TransformComponent（position 为左下角坐标, scale 缩放尺寸）
// 锚点：anchorMin/anchorMax（0-1 归一化父容器坐标）实现拉伸布局：
//   - 同轴 anchorMin == anchorMax → 位置停靠（position 为像素偏移）
//   - 同轴 anchorMin != anchorMax → 拉伸填充（position 为该轴起点像素偏移，size 由锚点差计算）
struct MIKAN_API Sprite2DComponent {
    enum class Type { Rect, Sprite, Button };
    Type type = Type::Rect;
    bool visible = true;          // 运行时可见性（SceneECS::SetVisible 操作；不序列化）
    bool isUI = false;            // true=屏幕坐标(UI 层), false=世界坐标(玩法层)
    float width = 64.0f;
    float height = 64.0f;
    glm::vec2 uv0 = glm::vec2(0.0f);
    glm::vec2 uv1 = glm::vec2(1.0f);
    glm::vec4 color = glm::vec4(1.0f);
    std::string texture = "";     // Renderer2D 已注册的纹理名（空 = 纯色）
    int layer = 0;                // 渲染层（整数；数字越大越在上层）
    // ===== 锚点拉伸布局（0-1 归一化相对父容器） =====
    glm::vec2 anchorMin = glm::vec2(0.0f);  // 左下锚点（默认左下角）
    glm::vec2 anchorMax = glm::vec2(0.0f);  // 右上锚点（默认左下角=固定尺寸）
    // 按钮状态（由 Canvas2D::Update 每帧更新；仅 type==Button 有效）
    bool hovered = false;
    // ===== Button 标签与事件（type==Button） =====
    std::string label = "";       // 按钮文字（空 = 不显示）
    float labelFontSize = 0.0f;   // 0 = 自动（按钮高度的 30%）
    glm::vec4 labelColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    std::function<void()> onClick;  // 点击回调（按下瞬间触发；运行时绑定，不序列化）
};

// 2D 按钮组件（独立 ECS 组件：填充矩形 + 文字 + 点击回调）
// 与 Sprite2DComponent.Button 兼容并存；新代码推荐用本组件
// 变换用 TransformComponent（position 为左下角坐标, scale 缩放尺寸）
struct MIKAN_API ButtonComponent {
    bool visible = true;          // 运行时可见性（SceneECS::SetVisible 操作；不序列化）
    bool isUI = false;            // true=屏幕坐标(UI 层), false=世界坐标(玩法层)
    float width = 160.0f;         // 按钮尺寸（像素）
    float height = 48.0f;
    glm::vec4 fillColor = glm::vec4(0.15f, 0.15f, 0.18f, 1.0f);   // 填充色（可编辑）
    glm::vec4 hoverColor = glm::vec4(0.25f, 0.30f, 0.45f, 1.0f);   // 悬停色
    glm::vec4 textColor = glm::vec4(1.0f);                         // 文字颜色（可编辑）
    std::string text = "";        // 按钮文字（空 = 无文字；可编辑）
    float fontSize = 0.0f;        // 0 = 自动（按钮高度的 30%）
    int layer = 0;                // 渲染层（整数；数字越大越在上层）
    // 运行时状态（Canvas2D::Update 每帧更新）
    bool hovered = false;
    // 点击回调（按下瞬间触发；运行时绑定，不序列化）
    std::function<void()> onClick;
};

// 2D 文本组件（Canvas2D 渲染：TextRenderer，位图/SDF/MSDF 三模式）
// 变换用 TransformComponent（position 为文本左下角/baseline 起点，左下原点坐标系；scale.x 缩放字号）
struct MIKAN_API TextComponent {
    enum class RenderMode { Bitmap, Sdf, Msdf };   // 渲染模式：位图(低分辨率回退) / SDF / MSDF(默认,角点锐利)
    std::string text = "Text";    // UTF-8 文本内容
    bool visible = true;          // 运行时可见性（SceneECS::SetVisible 操作；不序列化）
    bool isUI = false;            // true=屏幕坐标(UI 层), false=世界坐标(玩法层)
    float fontSize = 24.0f;       // 字号（像素）
    glm::vec4 color = glm::vec4(1.0f);
    int layer = 0;                // 渲染层（整数；数字越大越在上层）
    RenderMode renderMode = RenderMode::Msdf;   // 默认 MSDF（效果最好）
    // 运行时测量（渲染时更新，供命中测试/包围盒；非序列化字段）
    float measuredWidth = 0.0f;
    float measuredHeight = 0.0f;
};

// ===== 九宫格精灵组件（9-Slice：边框固定 + 内容拉伸，适合 UI 面板/弹窗/按钮背景） =====
// 将纹理按 border(左/右/上/下 像素) 切为 9 格，渲染时边框四角不变形、四边单向拉伸、中心双向拉伸。
// 变换用 TransformComponent（position 为左下角坐标, scale 缩放最终尺寸）
struct MIKAN_API Slice9Component {
    std::string texture = "";         // 纹理名（Renderer2D 已注册）
    bool visible = true;              // 运行时可见性（SceneECS::SetVisible 操作；不序列化）
    bool isUI = true;                 // true=屏幕坐标(UI 层), false=世界坐标(玩法层)
    float width = 128.0f;             // 目标宽度（像素）
    float height = 128.0f;            // 目标高度（像素）
    glm::vec4 border = glm::vec4(8.0f); // 边框像素: x=左, y=右, z=上, w=下
    // 源纹理 UV 范围（左下/右上；默认全纹理）
    glm::vec2 uv0 = glm::vec2(0.0f);
    glm::vec2 uv1 = glm::vec2(1.0f);
    glm::vec4 color = glm::vec4(1.0f);
    int layer = 0;                    // 渲染层
    // 运行时纹理尺寸缓存（LoadTexture 后由渲染器更新；-1 表示未设置，RenderSlice9 容错）
    float texWidth = -1.0f;
    float texHeight = -1.0f;
};

// ===== 音频源组件（类似 Unity AudioSource）：场景树实体挂载，AudioSourceSystem 每帧驱动 =====
// 可序列化字段（clip/volume/loop/playOnAwake）由 AudioSourceSystem 从场景加载后自动播放；
// 运行时字段不序列化（Hidden）：插件通过 playRequested/stopRequested 触发播放/停止。
struct MIKAN_API AudioSourceComponent {
    // ===== 可序列化 =====
    std::string clip = "";       // 音频文件名（assets/audio/ 下，如 "bgm.wav"；加载名 = 去扩展名）
    float volume = 1.0f;         // 0-1（实时生效）
    bool loop = false;           // 无缝循环
    bool playOnAwake = true;     // 场景加载后自动播放一次（首帧生效）
    // ===== 运行时（不序列化）=====
    bool playing = false;        // 引擎维护：当前是否播放中
    bool playRequested = false;  // 插件置 true → 触发播放一次（系统自动复位）
    bool stopRequested = false;  // 插件置 true → 停止播放（系统自动复位）
    bool awakeStarted = false;   // playOnAwake 已处理标记（防重复自动播放）
    std::string lastClip = "";   // 已加载的 clip（检测切换后重载）
    float lastVolume = 1.0f;     // 已应用的音量（检测变化）
    bool lastLoop = false;       // 已应用的循环（检测变化）
};

// ===== UI 补间动画组件（Tween：对实体的 Transform + 颜色属性做时间插值） =====
// 由 TweenSystem 每帧驱动；多个 Tween 可通过添加多个组件共存（依次处理）。
// 变换用 TransformComponent（position/scale/rotation 驱动目标），颜色驱动 Sprite2DComponent.color
struct MIKAN_API TweenComponent {
    enum class Easing {
        Linear,          // 线性
        EaseInQuad,      // 二次缓入
        EaseOutQuad,     // 二次缓出
        EaseInOutQuad,   // 二次缓入缓出
        EaseInCubic,     // 三次缓入
        EaseOutCubic,    // 三次缓出
        EaseInOutCubic,  // 三次缓入缓出
        EaseOutBounce,   // 弹跳出
        EaseOutElastic,  // 弹性出
    };
    enum class Property {
        PositionX,       // Transform.position.x
        PositionY,       // Transform.position.y
        ScaleX,          // Transform.scale.x
        ScaleY,          // Transform.scale.y
        RotationZ,       // Transform 欧拉角 z（度）
        ColorAlpha,      // Sprite2DComponent.color.a（需实体带 Sprite2DComponent）
        Width,           // Sprite2DComponent.width
        Height,          // Sprite2DComponent.height
    };

    Property property = Property::PositionX;
    Easing easing = Easing::EaseOutQuad;
    float from = 0.0f;           // 起始值
    float to = 0.0f;             // 目标值
    float duration = 0.5f;       // 持续时间（秒）
    float delay = 0.0f;          // 延迟（秒）
    float elapsed = 0.0f;        // 已过时间（运行时，从 0 开始计数）
    bool running = true;         // 是否播放中（false 则跳过; 完成后自动设 false）
    bool loop = false;           // 完成后是否循环重播
    bool pingPong = false;       // 完成后是否反向（来回：to→from→to…）
    bool pingPongReverse = false;// 内部：当前是否处于反向阶段
};

// ===== 脚本组件（Unity 式 C++ 玩法逻辑挂载）=====
// 挂载关系直接进入场景数据：{"script":{"scriptName":"ExampleScript","params":{"speedDegPerSec":45}}}
// 由 ScriptSystem 反序列化时按 scriptName 创建实例、每帧调 OnUpdate、场景卸载时 OnDestroy。
class IScriptBehaviour; // 前向声明（完整接口见 ECS/ScriptSystem.h）
struct MIKAN_API ScriptComponent {
    std::string scriptName = "";   // 注册的脚本类名（REGISTER_SCRIPT 注册），序列化
    std::string paramsJson = "";   // 脚本参数字段表序列化的 JSON 对象原文，序列化（手写处理）
    IScriptBehaviour* runtime = nullptr; // 运行时实例，Hidden 不序列化，由 ScriptSystem 管理
};

// ===== 动画控制器组件（Animator：驱动挂载实体的模型动画播放）=====
// 由 SceneRenderer::UpdateModelAnimations 每帧同步到实体 mesh.modelPath 对应的 ModelRenderer。
// time 为运行时回写（每帧从渲染器读取，属性面板可查看当前动画帧/时间）。
struct MIKAN_API AnimatorComponent {
    int clipIndex = 0;       // 当前播放 clip 索引（MeshData::animations）
    float speed = 1.0f;      // 播放速度倍率（0 = 暂停播放）
    bool loop = true;        // 循环播放
    bool playing = true;     // 播放开关
    float time = 0.0f;       // 当前播放时间（秒；运行时由渲染器回写，用于查看）
};

// ===== VMD 播放器组件 =====
// 组件可挂在带骨骼 MeshComponent 的 PMX/PMD 实体，或挂在 CameraComponent 实体。
// Auto 模式下优先选择相机，否则选择模型；ModelRenderer 只接收通用局部骨骼姿态，
// VMD 文件解析和播放状态由独立的 VmdSystem 管理。
enum class VmdTarget {
    Auto,
    Model,
    Camera
};

struct MIKAN_API VmdPlayerComponent {
    std::string motionPath = ""; // 相对 assets/ 的 .vmd 路径，也接受绝对路径
    VmdTarget target = VmdTarget::Auto;
    float speed = 1.0f;           // 播放速度倍率；VMD 时间基准为 30 FPS
    bool loop = true;
    bool playing = true;
    bool enabled = true;
    float startFrame = 0.0f;      // 循环/播放起点
    float currentFrame = 0.0f;    // 运行时游标，不参与场景序列化
};

} // namespace ECS
