#version 450

// 全屏三角形顶点着色器
// 使用单个超大三角形覆盖整个屏幕，避免两个三角形共享边上的辅助片段调用

layout(location = 0) out vec2 fragTexCoord;

void main() {
    // 使用单个超大三角形覆盖全屏（3 个顶点）
    // 顶点坐标超出 [-1, 1] 范围，利用 guard band 避免裁剪
    // 顶点顺序：0 = 左上 (-1, 3), 1 = 左下 (-1, -1), 2 = 右下 (3, -1)
    // 这个三角形完全覆盖屏幕，且没有共享边
    
    vec2 positions[3] = vec2[](
        vec2(-1.0,  3.0),  // 顶点 0: 左上（超出屏幕）
        vec2(-1.0, -1.0),  // 顶点 1: 左下
        vec2( 3.0, -1.0)   // 顶点 2: 右下（超出屏幕）
    );
    
    // 计算对应的纹理坐标
    // 需要将三角形顶点映射到纹理空间 [0, 1]
    vec2 texCoords[3] = vec2[](
        vec2(0.0, 2.0),  // 顶点 0: 左上
        vec2(0.0, 0.0),  // 顶点 1: 左下
        vec2(2.0, 0.0)   // 顶点 2: 右下
    );
    
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    fragTexCoord = texCoords[gl_VertexIndex];
}
