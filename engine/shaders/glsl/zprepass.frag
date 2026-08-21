#version 450

// z-prepass 片段着色器（subpass 0，depth-only）——无颜色输出，仅深度。
// 无 discard（若几何有 alpha test 需在 z-prepass 复现，当前场景模型无 alpha 纹理；见 model.frag discard 限制注释）

void main() {
}
