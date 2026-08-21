#pragma once
// TilemapSystem.h - 瓦片地图系统(TMX 导入 + 渲染 + Box2D 碰撞)
// 场景树中带 TilemapComponent 的实体(Transform.position = 地图左上角世界原点):
//   - LoadTilemap: 解析 .tmx(TmxLoader)→ 加载图集纹理 → 按 collision 属性生成 Box2D 静态体
//   - Render: 由 Canvas2D::RenderECSNodes(世界层)调用, 与场景树 2D 实体同相机合批
//   - Clear/ClearAll: 销毁碰撞体(场景重建/停止)
#include "Platform/Export.h"
#include "ECS/Types.h"
#include <string>

class Renderer2D;

class MIKAN_API TilemapSystem {
public:
    static TilemapSystem& GetInstance();

    // 加载实体携带的地图(tmx 路径 → 解析 + 纹理 + 碰撞体); 失败打印错误返回 false
    bool LoadTilemap(ECS::Entity e);

    // 渲染实体地图(世界坐标, 同世界相机; Canvas2D 世界层调用)
    void Render(Renderer2D& r2d, ECS::Entity e);

    // 调试绘制: 画瓦片地图生成的 Box2D 碰撞体线框(物理排错)
    void RenderDebugColliders(Renderer2D& r2d);

    // 遍历场景加载全部瓦片地图实体(先 ClearAll; 启动/场景切换共用)
    void LoadAllFromScene();

    // 销毁实体地图的 Box2D 碰撞体(场景重建/实体销毁时)
    void ClearColliders(ECS::Entity e);
    // 遍历场景清理全部瓦片碰撞体(引擎停止/场景重建)
    void ClearAll();

private:
    TilemapSystem() = default;
    ~TilemapSystem() = default;
    TilemapSystem(const TilemapSystem&) = delete;
    TilemapSystem& operator=(const TilemapSystem&) = delete;

    void GenerateColliders(ECS::Entity e);
};
