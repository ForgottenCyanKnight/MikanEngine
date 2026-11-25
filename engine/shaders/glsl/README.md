# 引擎着色器

| 目录 | 用途 |
| --- | --- |
| `atmosphere` | 物理大气 LUT、云和天空盒 |
| `gbuffer` | 几何写入和 G-Buffer 合成，含 `depth` 深度预通道和 `voxel` 体素光栅化 |
| `lighting` | 光源剔除、IBL 和反射探针 |
| `postprocess` | AO、SSGI、抗锯齿、Bloom、色调映射、升频和水面后处理 |
| `raytracing` | 光追输出、体素光追、可见性缓冲和降噪 |
| `shadow` / `water` | 阴影深度绘制和水面几何 |
| `common` / `debug` / `simulation` / `ui` | 共享代码、调试和预览、SPH 模拟、2D 界面 |

`CompileShaders` 会递归发现 `.vert`、`.frag` 和 `.comp`，并将产物写入
`engine/shaders/spv` 下对应的分类目录。例如 `gbuffer/world.frag` 编译为
`gbuffer/world.frag.spv`。跨目录引用使用相对于当前文件的 `#include` 路径。

运行时可使用分类路径，也可通过构建生成的 `shader-build-plan.txt` 解析旧的短文件名。
请保持 SPIR-V 文件名唯一。移动或新增着色器后，重新运行 `CompileShaders` 生成清单。
