# MMD 舞蹈原型

## 最新相机状态：恢复原始 VMD

用户要求保留原镜头设计语义，Main Camera 已恢复 animations/camera.vmd；原始目标点、距离、旋转、FOV 与插值直接加载，不使用居中改写轨道。Vepley 保持世界原点，时间轴同步及阴影覆盖修复保留。下方 camera-centered.vmd 的说明属于历史记录，该资源当前未绑定。


## 最新取景：角色原点与居中 VMD 相机（2026-10-01）

Vepley 主场景位置已设为 (0,0,0)。Main Camera 改用 animations/camera-centered.vmd，timelineSource 仍为 Vepley；原 animations/camera.vmd 保留。原始相机直接使用文件目标点，没有跟随角色实体位置；此前角色 X=-12.6465 导致偏离。

新相机目标统一为模型身体中心 (0,9.095376,0)，从 PMX 原始顶点 y 范围 [0,18.190752] 得出。保留264个相机键的时间、旋转、FOV、透视标志及插值控制点；目标点平移轨道被居中目标替代。相机距离为原距离×1.1与模型半高×1.15/sin(FOV/2)二者较大值，保留拉远变化并限制窄视角下的过近镜头，避免常规全身取景裁掉身体。距离约43.58–166.23；这份轨道按 Vepley 尺寸适配，更换角色时需要重新生成。生成脚本 out/create-centered-vmd-camera.py。角色旋转、缩放、物理、舞蹈/表情及太阳/云配置未修改。


## 最新状态：移除水面并对齐 test 天空（2026-10-01）

用户要求去掉开阔水面：主场景已删除水面地形实体、恢复 Ground 可见，总计6实体。笔刷图资源保留备用，当前场景不再引用。CloudVolume 与天空盒保留。太阳旋转与光强已对齐 test（约30度、强度1）；此前 MMD 光源约135度、强度3，这是复制同样云参数但天空照明不同的主要原因。云、天空盒、cloud_view 与天空合成参数经对比一致，后处理链差异主要在 AA。不同相机朝向、位置及风动时间仍会改变可见天空。下方水面配置为历史记录。


## 最新场景：体积云与开阔笔刷水面（2026-10-01 晚）

主场景新增 CloudVolume 和“开阔水面（地形笔刷）”，现在 7 实体；旧 Ground 隐藏。现有角色、灯光、相机及天空盒变换经逐项比对保持原值，动画与物理参数未修改。

体积云复制 projects/engine-samples/test/scenes/main.json 的 cloudVolume 参数：coverage=0.45、density=2、baseAltitudeKm=2、thicknessKm=2，包含高空云、噪声偏移与风速配置。使用现有引擎 cloud_view 与共享 PBR 合成，未改 albedo。

水面采用 TerrainComponent 的 paintedWaterPath，而非独立平面水实体。范围 2048×2048，中心跟随当前角色场景位置；513×513 的水深图与16位高度图保存在 scenes/terrain_paint/open_water_depth.png、open_water_height.png。生成采用 PaintTerrainWaterWorld 的圆形平顶/smoothstep 笔刷规则：半径1600、硬度0.85、目标深度0.25、涂抹强度1，并按新增水深挖低湖底。地形 heightScale=8、y=-8.04，水面约 y=-0.04，中央水深约2.00784；之后可在编辑器用水笔刷继续编辑并保存两张图。地形碰撞关闭，MMD 的角色局部物理世界仍保持原地面基线。

地形底部材质沿用引擎地形资源，运行时项目路径找不到会正常回退 engine/textures；离线校验可能提示对应路径警告。项目资产列表已包含 scenes/terrain_paint。

验证：out/test_runs/vepley-cloud-water-20261001/result.json 的600帧 CPU/Vulkan、场景与状态检查5项通过。out/vepley-cloud-water-preview.png 已检查，可见云层、水波和人物倒影；该截图采用 out 内独立固定相机副本，不覆盖主场景相机。备份：D:/Engine project/backup/vepley_cloud_water_20261001_213636/manifest.json，修改前已有文件的副本和 SHA256 已验证，新建图像记录 existed=false。


## 当前主场景：Vepley PBR（2026-10-01 晚）

用户改用 Vepley 作为混合材质与物理测试角色。主场景保持 5 实体，仅包含 Vepley 人物；相机 timelineSource 已改为 Vepley，角色位置和场景环境保留现值。dance/face/camera 动画继续从 0 开始、速度 1，物理启用；test-vepley 也已启用物理。下文 Miku 主场景描述属于此前阶段。

专有 PMX 前向材质阶段继续输出颜色、法线和 PBR 属性，共享后处理负责光照、阴影、环境反射。新增 `<模型>.pmx.import.json` 的 `pbrMaterials` 规则：以 materialName、diffuseTexture 或 sphereTexture 文件名匹配，按规则顺序覆盖 metallic/roughness；normalTexture 为相对模型目录路径，useSphereTexture=false 关闭对应烘焙球面高光。只影响明确配置的模型。匹配到的材质参数优先于人物整体 metallic/roughness，需改此文件调节。

Vepley 的布料、绒毛法线图已接入 PMX 专用管线，使用形变后的世界位置/UV 导数建立切线基，输出世界空间法线交给统一 PBR。法线图通过 UNORM 线性采样。布料、绒毛、头发、皮肤、眼睛使用不同粗糙度；数值是当前测试预设，并非资源中存在粗糙度贴图。PMX 实际没有引用 Metal/gold 球面图，也没有提供独立金属度贴图，因此没有把整张混合纹理错误设为金属。细分金属配件还需要可靠的区域蒙版。

已配置材质的 sphere map 关闭，避免重复叠加假高光；diffuse/贴图颜色不加入艺术亮度系数。阴影导入覆盖继续保留。法线纹理文件见 models/vepley/Textures/normalmap，材质配置见 GirlsFrontline VepleyDefault.pmx.import.json。


## 多模型动画对比（2026-10-01）

五模型的动作与表情一致性已由用户确认。默认 `scenes/main.json` 已恢复仅包含 Miku 人物，已完成本轮 Saba 物理语义对齐；其他模型资源和下列单模型测试场景保留。Miku 继续绑定 `dance.vmd` 和 `face.vmd`，startFrame=0、speed=1、playing=true、loop=true，物理已重新开启。相机、灯光、地面、天空盒及用户调整的位置保持现值，相机动画继续跟随 Miku 的时间轴。

可在资源浏览器打开以下单模型场景，使用同一个位置、相机和动画比较：

| 模型 | 场景 |
| --- | --- |
| 原 Miku | `scenes/test-miku.json` |
| Klee | `scenes/test-klee.json` |
| IA | `scenes/test-ia.json` |
| IA ettc 发型变体 | `scenes/test-ia-ettc.json` |
| IA tam 发型变体 | `scenes/test-ia-tam.json` |
| IA Children Record 变体 | `scenes/test-ia-children-record.json` |
| Vepley | `scenes/test-vepley.json` |
| PJSK Len | `scenes/test-pjsk-len.json` |
| PJSK Len Apose 变体 | `scenes/test-pjsk-len-apose.json` |

导入文件的来源、大小和 SHA256 记录在 `imported-model-sources.json`。贴图目录结构及原作者说明一并保留，未复制旧工程的 BVH/缩略图缓存。所有导入 PMX 均有绑定动画的测试场景。

验证：五模型主场景的 120 帧 CPU 播放测试通过，全部模型成功加载，人物与相机两秒后均为 VMD 第60帧；九个单模型场景通过场景与资源引用校验。证据在 `out/test_runs/mmd-model-comparison-20261001/`。尚未逐个进行 Vulkan 画面验收；相同时间轴不表示不同骨架、比例或 Morph 命名一定产生完全相同的外观。

当前实现、已知问题、测试证据与接手约束见 [MMD 系统交接现状](../../docs/MMD系统交接现状.md)（2026-10-01）。

在项目选择器打开「MMD舞蹈原型」，点击播放即可观看舞蹈。

- Miku：`dance.vmd` 驱动骨骼，`face.vmd` 叠加表情，已开启 Jolt 物理模拟（`physicsEnabled: true`），头发与裙摆参与物理模拟。
- Main Camera：`camera.vmd` 驱动镜头，通过 `timelineSource: Miku` 跟随人物时间轴。
- 以 VMD 的 30 fps 从第 0 帧开始，保留完整开头；循环、暂停和速度均跟随人物播放器。
- 三类 VMD 与 PMX、贴图直接提取自旧 OpenGL 项目，来源及 SHA256 见 `asset-sources.json`。
- 旧项目中的相机 VMD 来自 Star Night Snow，与人物/表情属于不同编舞；这是可运行的三轨原型，镜头并非该舞蹈的配套摄像机。
- 未附音乐；模型原作者说明保存在 `models/miku/read me.txt`。

`tests.json` 提供 600 帧玩法和 Vulkan 渲染回归。
验证：人物/表情叠加、相机独立 Bezier 曲线与切镜测试通过；600 帧 CPU 和 Vulkan 整场运行通过，相机状态一致。截图见 out/mmd-dance-prototype/frame-240.png 和 frame-600.png。

播放插值修复：骨骼 Bézier 曲线使用标准三次基函数，避免关键帧起点偏移和赶跳。速度仍为 1；30/60/120 FPS 下两秒均推进 60 个 VMD 帧，相机同步。状态 dump 的 vmd.frame/vmd.speed/vmd.playing 可用于核对时间轴。

PMX 保留专用管线、CPU 蒙皮、PMX 材质顺序、贴图 alpha、双面、球面贴图与描边处理。专用 `pmx` 管线在几何阶段输出原始材质颜色、世界法线、金属度/粗糙度/AO/自发光及运动矢量；环境和人物由同一个 fullscreen PBR 光照阶段统一处理太阳、CSM 阴影、环境反射和间接光。Toon/ambient/specular 不再作为预计算光照混入 albedo，避免重复照明。

场景 material 的 metallic、roughness、ao、emissiveIntensity 为 PBR 参数；没有 material 时，金属度为 0，粗糙度从 PMX 高光指数转换。乘法球面贴图和 UV1 子贴图保留为材质贴图；加法球面贴图与描边在光照后的 HDR 前向阶段叠加，然后统一进入后处理。透明度参与颜色混合，法线/材质采用最近表面，半透明叠层没有逐层独立 PBR 光照；不支持 independentBlend 的设备采用最近表面覆盖，关闭需要独立附件写掩码的描边/加法球面叠加。

投射阴影默认按 PMX CastSelfShadow、材质 opacity 和贴图 alpha 裁剪；Vepley 使用独立导入设置覆盖投影标记。统一 PBR 阶段接收阴影的规则与普通模型相同。材质 morph 已连接到专用渲染器，附加 UV morph 尚未连接；运动矢量尚不包含上一帧骨骼形变。主场景与 test-miku 已启用物理。物理调查与实施顺序见 [MMD 物理对齐分析](../../docs/MMD物理对齐分析.md)。

公共 Toon 资源保留于 `engine/textures/mmd/`，附 Saba MIT 许可证和来源 SHA256，统一 PBR 不再采样其光照斜坡。Android 渲染入口同步，尚未真机验证；当前机器缺少 Khronos validation layer。
验证：Release 编译无警告；600 帧 CPU/Vulkan 回归 5 项通过；编辑器场景视图与游戏播放截图正常。独立场景副本把人物材质改为 metallic=1、roughness=0.06，反射结果明显变化，确认材质参数进入统一 PBR。证据位于 out/pmx-pbr/，原场景未修改。

播放使用独立 120 Hz 时间轴，渲染超过 120 FPS 不会额外推进；低帧率合并时间推进，VMD 仍按文件标准 30 帧/秒（speed=1）采样。人物、表情和相机同步。原型已重新启用物理（physicsEnabled=true）；本轮对齐项与求解器剩余差异见物理分析文档。

## 本轮物理验证（2026-10-01 晚）

Saba 对齐后的 Miku 600 帧 CPU 动画加物理平均 2.4125 ms；完整构建与 CPU/Vulkan 600 帧回归 5 项通过。首次开启有渐入预热；Jolt/Bullet 关节和接触求解器仍有差异，尚未验收视觉等价。详细参数、证据与剩余工作见 [物理对齐分析](../../docs/MMD物理对齐分析.md)。

Vepley 本轮验证：完整构建无警告；out/test_runs/vepley-pbr-normal-20261001/result.json 的 600 帧 CPU/Vulkan 与场景/状态检查 5 项通过；角色与相机均为第 300 帧。已检查实际截图 out/vepley-pbr-preview.png，衣袖高光、服装纹理与姿势正常可见；这是一帧画面检查，不代表所有舞蹈帧已视觉验收。修改前 SHA256 校验备份：D:/Engine project/backup/vepley_pbr_20261001_211345/manifest.json。
