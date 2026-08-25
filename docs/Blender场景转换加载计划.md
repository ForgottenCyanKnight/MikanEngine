# Blender 场景转换与加载计划

## 1. 目标

将 Blender 作为场景创作工具，将 `.blend` 场景离线转换为 Mikan Engine 可直接加载的运行时资源。

目标流程：

```text
scene.blend
    ↓ Blender Python 无头导出
scene.glb + scene.mikan.json + textures/
    ↓ Mikan Engine 场景加载
ECS Entity + Mesh/Material/Camera/Light/Water/Collider
```

第一阶段面向小到中型、精致布置的场景，不优先实现开放世界流式加载、无限地形和复杂流体模拟。

## 2. 总体架构

不在 C++ 引擎中直接解析 `.blend`。Blender 文件属于创作工程格式，使用 Blender 自己的 Python API 读取最可靠。

建议分成三层：

1. Blender 导出器：读取 `.blend`，导出 GLB、纹理和场景清单。
2. Mikan 转换层：将场景清单映射为 Mikan 的场景 JSON 和 ECS 组件。
3. Mikan 运行时：读取 GLB 和 Mikan 场景 JSON，不依赖 Blender。

现有 GLB/GLTF 模型加载器、场景序列化系统、`WaterComponent` 和 `RigidBodyComponent` 应优先复用。

## 3. 输出目录

建议每个 Blender 场景生成独立的构建目录：

```text
build/scenes/<scene-name>/
├── scene.glb
├── scene.mikan.json
├── manifest.json
├── textures/
├── materials/
└── collisions/
```

运行时只依赖该目录，不需要访问原始 `.blend` 文件。

## 4. Blender 标记规范

使用 Collection 名称和自定义属性标记引擎语义。

### 4.1 Collection 约定

```text
SCENE_RENDER       可见渲染模型
SCENE_COLLISION    碰撞模型
SCENE_WATER        水面和水体边界
SCENE_MARKERS      出生点、触发器、导航点
SCENE_LIGHTS       灯光
```

### 4.2 通用自定义属性

```text
mikan_type
mikan_entity_name
mikan_static
mikan_visible
mikan_cast_shadow
mikan_receive_shadow
```

### 4.3 碰撞属性

```text
collision_enabled = true
collision_shape = "box" | "capsule" | "mesh" | "none"
collision_source = "object" | "separate_collection"
collision_is_trigger = false
collision_use_convex_hull = true
```

### 4.4 水体属性

```text
mikan_type = "water"
water_enabled = true
water_depth = 20.0
water_buoyancy = 1.15
water_drag = 2.0
water_mask = "lake_mask.png"
water_affect_players_only = true
```

水面高度由 Blender 对象的世界坐标 `Y` 决定，水面 Mask 只描述水体覆盖范围，不负责高度。

## 5. 坐标、单位和资源规则

在第一阶段固定以下规则，避免不同工具之间出现隐式转换：

- Mikan 世界坐标使用 Y 轴向上。
- 场景单位使用米。
- Blender 导出时统一应用对象 Transform 和必要的坐标系转换。
- 不允许输出非均匀缩放下未处理的法线和切线。
- Mesh 顶点、法线、切线、UV 必须经过有限值检查。
- 纹理路径统一转换为场景目录下的相对路径。
- 外部纹理复制到 `textures/`；Packed Image 先解包到输出目录。
- 输出路径不能依赖 Blender 当前工作目录。

## 6. 第一阶段功能范围

### 必须支持

- 场景层级和父子关系
- Empty、Mesh、Camera、Light
- 静态 Mesh 的 GLB 导出
- Principled BSDF 基础材质
- Base Color、Normal、Roughness、Metallic、AO、Emissive 纹理
- 外部纹理复制和路径重写
- 对象 Transform、可见性和阴影开关
- 碰撞体标记和基础碰撞参数
- 水体实体、水平高度、范围、深度和 Mask 引用
- 场景清单和资源 Manifest

### 暂不支持

- Blender Geometry Nodes 的完整运行时转换
- Blender 特殊 Shader Node 网络
- 粒子系统和流体模拟
- 未烘焙的复杂约束
- Blender Compositor 后处理节点
- 直接在运行时读取 `.blend`
- 开放世界流式加载

## 7. Blender 导出器

新增脚本：

```text
tools/blender/export_mikan.py
```

建议命令：

```powershell
blender -b scene.blend --python tools/blender/export_mikan.py -- `
  --output build/scenes/example
```

导出器职责：

1. 打开并校验当前 Blender 场景。
2. 遍历 Collection、Object 和实例。
3. 识别自定义属性和特殊标记。
4. 导出 GLB 和纹理。
5. 生成场景清单。
6. 生成资源 hash 和导出日志。
7. 对输出 Transform、Mesh、材质和路径执行校验。

建议支持增量导出：输入 `.blend`、对象数据和纹理发生变化时才重新导出对应资源。

## 8. 场景清单格式

建议使用独立的 `scene.mikan.json`，不要把所有 Blender 原始信息直接暴露给运行时。

示例结构：

```json
{
  "version": 1,
  "scene": "example",
  "assetRoot": ".",
  "glb": "scene.glb",
  "entities": [
    {
      "id": 0,
      "name": "Main Camera",
      "parent": 4294967295,
      "transform": {
        "position": [0, 3, 8],
        "rotation": [1, 0, 0, 0],
        "scale": [1, 1, 1]
      },
      "components": {
        "camera": {},
        "render": {}
      }
    }
  ]
}
```

运行时实体组件字段应尽量与现有 Mikan 场景 JSON 保持一致，转换器只负责生成，不重复定义 ECS 数据结构。

## 9. 组件映射

| Blender 对象/标记 | Mikan 组件 |
|---|---|
| Mesh Object | `MeshComponent` + `RenderComponent` |
| Material | `MaterialComponent` |
| Camera | `CameraComponent` |
| Light | `LightComponent` |
| Collision Object | `RigidBodyComponent` / `ColliderComponent` |
| `mikan_type=water` | `WaterComponent` |
| Spawn Marker | `TransformComponent` + Spawn 标记组件或名称约定 |
| Trigger Marker | `ColliderComponent.isTrigger` |

## 10. 水体转换规则

水体实体不要求导出 Blender Mesh，优先由 Mikan 的 `WaterRenderer` 生成共享水面网格。

转换规则：

```text
Blender Object Transform.Y       → 水面世界高度
Object/Mask 边界                 → 水面范围或 maskPath
water_depth                      → WaterComponent.depth
water_buoyancy                   → WaterComponent.buoyancy
water_drag                       → WaterComponent.drag
water_affect_players_only        → WaterComponent.affectPlayersOnly
```

水体 Mask 应由渲染和物理共同使用，避免画面水面边界与玩家入水范围不一致。

当前第一版可以先支持矩形水体；后续再加入：

- Mask Texture
- Polygon 水面
- 水面孔洞
- 多个水体拼接
- 屏幕空间 Water Mask 后处理

## 11. 碰撞转换规则

渲染 Mesh 和碰撞 Mesh 默认分离，避免高模直接用于动态碰撞。

推荐优先级：

1. 显式碰撞体对象
2. `SCENE_COLLISION` Collection 中的碰撞体
3. Blender 对象自定义属性指定的简单碰撞形状
4. 没有标记时不自动生成动态碰撞

静态建筑可选择 Mesh 碰撞；动态角色和物体优先使用 Box、Capsule 或 Convex Hull。

## 12. 实施阶段

### 阶段 A：基础静态场景

- 建立 Blender 导出脚本目录。
- 导出静态 Mesh、层级和 Transform。
- 生成 GLB 和最小 `scene.mikan.json`。
- 使用一个简单房间场景完成加载验证。

### 阶段 B：材质和纹理

- 导出 Principled BSDF 材质。
- 复制和重写纹理路径。
- 支持法线、粗糙度、金属度、AO 和自发光。
- 增加纹理缺失和路径错误报告。

### 阶段 C：摄像机、灯光和实体属性

- 导出主摄像机。
- 导出方向光、点光源和聚光灯。
- 支持可见性、阴影和静态标志。
- 支持 Blender 自定义属性覆盖默认 ECS 参数。

### 阶段 D：碰撞和水体

- 导出简单碰撞体。
- 支持独立碰撞 Collection。
- 支持 `WaterComponent` 和水面高度。
- 接入水面 Mask 的资源引用。

### 阶段 E：角色和动画

- 导出骨骼、蒙皮和动画。
- 保留动画 Clip 名称。
- 绑定角色碰撞体和玩家控制器。
- 验证 Blender 动画与现有动画系统的兼容性。

### 阶段 F：工程化和优化

- 增量导出和资源 hash。
- 导出日志和错误摘要。
- CI 或无头自动化验证。
- 静态合批、实例化和 BVH 优化。
- 大场景按 Collection 分块导出。

## 13. 测试场景

准备一个最小 `blender_import_fixture.blend`，包含：

- 一个静态房间
- 一个嵌套父子模型
- 一个带完整 PBR 纹理的模型
- 一个摄像机
- 一个方向光和点光源
- 一个 Box 碰撞体
- 一个 Capsule 碰撞体
- 一个水体和水面 Mask
- 一个出生点
- 一个简单动画角色

## 14. 验收标准

### 导出阶段

- Blender 无头命令可重复执行。
- 输出目录只包含运行时需要的资源。
- 所有资源路径均为相对路径。
- 输出 JSON 可以通过 schema 校验。
- Mesh、Transform 和材质没有 NaN 或无效值。

### 引擎阶段

- Mikan 可以加载转换后的场景。
- 层级、位置、旋转和缩放与 Blender 一致。
- 材质纹理正确绑定。
- 摄像机和灯光行为正确。
- 碰撞体生成正确。
- 玩家可以进入水体并触发浮力。
- 无头玩法测试可以稳定运行。

### 迭代阶段

- 修改 Blender 中单个对象不会导致所有资源全部重导出。
- 缺失纹理、非法属性和不支持节点会产生明确警告。
- 同一输入场景重复导出得到稳定结果。

## 15. 推荐优先级

```text
静态 Mesh / Transform / 层级
        ↓
材质 / 纹理
        ↓
摄像机 / 灯光
        ↓
碰撞体
        ↓
水体 / Mask
        ↓
动画角色
        ↓
增量导出 / 合批 / BVH / 分块
```

第一版完成后，Blender 将成为场景创作入口，Mikan 负责运行时渲染、物理、玩法和后处理，不再需要为每个场景重复手工构建 ECS JSON。
