#version 450

// 全屏三角形（无顶点缓冲，gl_VertexIndex 生成坐标）
// 物理天空计算 pass 使用：覆盖整个屏幕，输出 UV 供片段重建视线方向

layout(location = 0) out vec2 fragUV;

void main() {
    vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    vec2 uvs[3] = vec2[](
        vec2(0.0, 0.0),
        vec2(2.0, 0.0),
        vec2(0.0, 2.0)
    );
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    fragUV = uvs[gl_VertexIndex];
}
