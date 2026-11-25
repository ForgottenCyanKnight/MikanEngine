#version 450

// 草叶阴影深度片元：CSM 深度 pass 只写深度，无颜色输出。
// 顶点阶段复用 grass.vert——草的投影与主 pass 渲染走完全相同的几何、
// 风摆与距离 LOD 抽稀，草影和草严格同步。

void main() {
}
