# 来源与许可

原作 Pixel Hop Twins 的作者为 stm1978，固定版本为 `9835295888c7cb8afa795ca2a31707a65167c2ea`。原始代码采用 MIT，完整文本见本目录 `MIT_LICENSE.txt`。作者对角色、画面、关卡和音效的 CC0 声明见 `CC0_DECLARATION.md`，原始 README 全文见 `UPSTREAM_README.md`。[原作仓库](https://github.com/stm1978/retro-platformer/tree/9835295888c7cb8afa795ca2a31707a65167c2ea)

本模板将原作逻辑转换为 C++，通过 MikanEngine 的 Vulkan 渲染入口运行。两份 JSON 关卡保留原始字节。精灵矩阵和调色板取自原作，PCM 音效依据原作 Web Audio 的振荡器、包络和延迟音符生成。移动端使用安全区域内的触控按钮。

字体图集固定为参考浏览器所用的 DejaVu Sans Mono Bold。完整字体版权与许可保存在 `FONT_LICENSE.txt`。运行时读取图集、字形元数据和二进制遮罩。

几何绘制兼容实现引用了 Skia 的 `SkScan_Antihair.cpp`、`SkScan_AAAPath.cpp`、`SkAnalyticEdge.cpp`、`SkGeometry.cpp`、`SkEdgeClipper.cpp`、`SkLineClipper.cpp` 及相关定点运算。代码保留 Android Open Source Project 和 Google 的原始版权声明，完整 BSD 许可保存在 `SKIA_LICENSE.txt`。[Skia 源码](https://github.com/google/skia)

`project.json` 将本目录的许可和来源记录列为构建资源。复制模板创建项目时，运行资源与许可文件一并复制，并保持项目内的相对路径。
