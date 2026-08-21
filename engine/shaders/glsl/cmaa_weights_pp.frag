#version 450
// CMAA2 纯后处理权重计算版（2026-08-16，与 compute 版对比用）——Intel CMAA2 算法移植到 pixel shader
// 与 compute 版（cmaa_edges.comp + cmaa_process.comp）的区别：
//   - 无候选列表/邻域写：每像素自包含（边缘检测 + Simple 权重 + Z 拐点自权重）
//   - 权重语义同 compute 版输出（RGBA8：R=左(-1,0) G=上(0,-1) B=右(+1,0) A=下(0,+1)）
//   - Z 形状只处理"拐点像素自身"（compute 版拐点写整条线——pixel shader 无法写邻域，此处近似）
// 输入：binding 0 = tonemap 输出（LDR）
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D colorTex;

// 官方常量（HIGH 预设）
const float CMAA2_EDGE_THRESHOLD = 0.07;
const float CMAA2_LCA_AMOUNT = 0.10;
const float CMAA2_SIMPLE_SHAPE_BLURINESS = 0.10;
const float CMAA2_MAX_LINE_LENGTH = 86.0;
const float CMAA2_SYMMETRY_CORRECTION_OFFSET = 0.22;
const float CMAA2_DAMPENING_EFFECT = 0.15;
const float CMAA2_CORNER_ROUNDING = 0.25;

float LumaAt(vec2 uv) {
    return dot(sqrt(texture(colorTex, uv).rgb), vec3(0.299, 0.587, 0.114));
}

// 4bit edges 编码（右=1 下=2 左=4 上=8）——用 3×3 luma + 3×3 LCA 近似（线长搜索用；主检测用 7×7 精确版）
uint LoadEdgeFast(ivec2 pixelPos) {
    ivec2 sz = textureSize(colorTex, 0);
    pixelPos = clamp(pixelPos, ivec2(0), sz - ivec2(1));
    vec2 uv = (vec2(pixelPos) + 0.5) / vec2(sz);
    vec2 texel = 1.0 / vec2(sz);
    float L[3][3];
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            L[dy + 1][dx + 1] = LumaAt(uv + vec2(dx, dy) * texel);
    float er = abs(L[1][1] - L[1][2]);
    float ed = abs(L[1][1] - L[2][1]);
    float el = abs(L[1][1] - L[1][0]);
    float eu = abs(L[1][1] - L[0][1]);
    // 简化 LCA：3×3 邻域差分 max × 0.10
    float maxD = max(max(max(abs(L[1][1] - L[0][0]), abs(L[1][1] - L[0][2])),
                         max(abs(L[1][1] - L[2][0]), abs(L[1][1] - L[2][2]))), max(er, max(ed, max(el, eu))));
    float lca = maxD * CMAA2_LCA_AMOUNT;
    uint v = 0u;
    if ((er - lca) > CMAA2_EDGE_THRESHOLD) v |= 1u;
    if ((ed - lca) > CMAA2_EDGE_THRESHOLD) v |= 2u;
    if ((el - lca) > CMAA2_EDGE_THRESHOLD) v |= 4u;
    if ((eu - lca) > CMAA2_EDGE_THRESHOLD) v |= 8u;
    return v;
}

vec4 UnpackEdgesFlt(uint v) {
    return vec4(
        float((v & 0x01u) != 0u),
        float((v & 0x02u) != 0u),
        float((v & 0x04u) != 0u),
        float((v & 0x08u) != 0u));
}

// ---- 官方 ComputeSimpleShapeBlendValues ----
vec4 ComputeSimpleShapeBlendValues(vec4 edges, vec4 edgesLeft, vec4 edgesRight, vec4 edgesTop, vec4 edgesBottom) {
    float fromRight = edges.r;
    float fromBelow = edges.g;
    float fromLeft  = edges.b;
    float fromAbove = edges.a;

    float blurCoeff = CMAA2_SIMPLE_SHAPE_BLURINESS;
    float numberOfEdges = dot(edges, vec4(1.0));
    float numberOfEdgesAllAround = dot(edgesLeft.bga + edgesRight.rga + edgesTop.rba + edgesBottom.rgb, vec3(1.0));

    if (numberOfEdges == 2.0)
        blurCoeff *= 0.75;

    float k = 0.9;
    fromRight += k * (edges.g * edgesTop.r     * (1.0 - edgesLeft.g)  + edges.a * edgesBottom.r   * (1.0 - edgesLeft.a));
    fromBelow += k * (edges.b * edgesRight.g   * (1.0 - edgesTop.b)   + edges.r * edgesLeft.g     * (1.0 - edgesTop.r));
    fromLeft  += k * (edges.a * edgesBottom.b  * (1.0 - edgesRight.a) + edges.g * edgesTop.b      * (1.0 - edgesRight.g));
    fromAbove += k * (edges.r * edgesLeft.a    * (1.0 - edgesBottom.r)+ edges.b * edgesRight.a    * (1.0 - edgesBottom.b));

    blurCoeff *= clamp(1.30 - numberOfEdgesAllAround / 10.0, 0.0, 1.0);

    return vec4(fromLeft, fromAbove, fromRight, fromBelow) * blurCoeff;
}

// ---- 官方 DetectZsHorizontal（水平 + 垂直旋转 .argb）----
void DetectZsHorizontal(vec4 edges, vec4 edgesM1P0, vec4 edgesP1P0, vec4 edgesP2P0,
                        out float invertedZScore, out float normalZScore) {
    invertedZScore  = edges.r * edges.g * edgesP1P0.a;
    invertedZScore *= 2.0 + ((edgesM1P0.g + edgesP2P0.a)) - (edges.a + edgesP1P0.g)
                     - 0.7 * (edgesP2P0.g + edgesM1P0.a + edges.b + edgesP1P0.r);

    normalZScore    = edges.r * edges.a * edgesP1P0.g;
    normalZScore    *= 2.0 + ((edgesM1P0.a + edgesP2P0.g)) - (edges.g + edgesP1P0.a)
                     - 0.7 * (edgesP2P0.a + edgesM1P0.g + edges.b + edgesP1P0.r);
}

// ---- 官方 FindZLineLengths（像素坐标版）----
void FindZLineLengths(out float lineLengthLeft, out float lineLengthRight,
                      ivec2 screenPos, bool horizontal, bool invertedZShape, ivec2 stepRight) {
    uint maskTraceLeft, maskTraceRight;
    if (horizontal) {
        maskTraceLeft  = 0x08u;
        maskTraceRight = 0x02u;
    } else {
        maskTraceLeft  = 0x04u;
        maskTraceRight = 0x01u;
    }
    if (invertedZShape) {
        uint tmp = maskTraceLeft;
        maskTraceLeft = maskTraceRight;
        maskTraceRight = tmp;
    }

    bool continueLeft = true;
    bool continueRight = true;
    lineLengthLeft = 1.0;
    lineLengthRight = 1.0;

    for (;;) {
        uint edgeLeft  = LoadEdgeFast(screenPos - stepRight * int(lineLengthLeft));
        uint edgeRight = LoadEdgeFast(screenPos + stepRight * (int(lineLengthRight) + 1));

        continueLeft  = continueLeft  && ((edgeLeft  & maskTraceLeft)  == maskTraceLeft);
        continueRight = continueRight && ((edgeRight & maskTraceRight) == maskTraceRight);

        lineLengthLeft  += continueLeft ? 1.0 : 0.0;
        lineLengthRight += continueRight ? 1.0 : 0.0;

        float maxLR = max(lineLengthRight, lineLengthLeft);
        if (!continueLeft && !continueRight)
            maxLR = CMAA2_MAX_LINE_LENGTH;

        if (maxLR >= min(CMAA2_MAX_LINE_LENGTH, (1.25 * min(lineLengthRight, lineLengthLeft) - 0.25)))
            break;
    }
}

void main() {
    ivec2 sz = textureSize(colorTex, 0);
    vec2 texel = 1.0 / vec2(sz);
    ivec2 pixelPos = ivec2(fragTexCoord * vec2(sz));

    // 7×7 luma 采样（对齐官方 4×4 LCA 需要）
    float L[7][7];
    for (int dy = -3; dy <= 3; dy++)
        for (int dx = -3; dx <= 3; dx++)
            L[dy + 3][dx + 3] = LumaAt(fragTexCoord + vec2(dx, dy) * texel);

    // 3×3 邻域像素的 edges（中心及 4 邻域；i,j ∈ 0..2 = 像素偏移 -1..1，L 索引 +3）
    uint e[3][3];
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) {
            // 像素 (i-1, j-1) 的 4 方向边（官方 ComputeEdge + 4×4 LCA 展开）
            float Lc = L[j + 2][i + 2];
            float er = abs(Lc - L[j + 2][i + 3]);
            float ed = abs(Lc - L[j + 3][i + 2]);
            float el = abs(Lc - L[j + 2][i + 1]);
            float eu = abs(Lc - L[j + 1][i + 2]);
            // 4×4 LCA（官方 quad 语义像素级展开）
            //   右边缘 LCA = max(E_down(i+1,j), E_down(i+1,j+1), E_down(i+2,j), E_down(i+2,j+1)) * 0.10
            float ed_11 = abs(L[j + 3][i + 3] - L[j + 4][i + 3]);
            float ed_12 = abs(L[j + 4][i + 3] - L[j + 5][i + 3]);
            float ed_21 = abs(L[j + 3][i + 4] - L[j + 4][i + 4]);
            float ed_22 = abs(L[j + 4][i + 4] - L[j + 5][i + 4]);
            float lca_er = max(max(ed_11, ed_12), max(ed_21, ed_22)) * CMAA2_LCA_AMOUNT;
            //   下边缘 LCA = max(E_right(i,j+1), E_right(i+1,j+1), E_right(i,j+2), E_right(i+1,j+2)) * 0.10
            float er_02 = abs(L[j + 4][i + 2] - L[j + 4][i + 3]);
            float er_12 = abs(L[j + 4][i + 3] - L[j + 4][i + 4]);
            float er_03 = abs(L[j + 5][i + 2] - L[j + 5][i + 3]);
            float er_13 = abs(L[j + 5][i + 3] - L[j + 5][i + 4]);
            float lca_ed = max(max(er_02, er_12), max(er_03, er_13)) * CMAA2_LCA_AMOUNT;
            //   左边缘 LCA = max(E_down(i-1,j), E_down(i-1,j+1), E_down(i,j), E_down(i,j+1)) * 0.10
            float ed_m11 = abs(L[j + 3][i + 1] - L[j + 4][i + 1]);
            float ed_m12 = abs(L[j + 4][i + 1] - L[j + 5][i + 1]);
            float ed_01 = abs(L[j + 3][i + 2] - L[j + 4][i + 2]);
            float ed_02 = abs(L[j + 4][i + 2] - L[j + 5][i + 2]);
            float lca_el = max(max(ed_m11, ed_m12), max(ed_01, ed_02)) * CMAA2_LCA_AMOUNT;
            //   上边缘 LCA = max(E_right(i,j-1), E_right(i+1,j-1), E_right(i,j), E_right(i+1,j)) * 0.10
            float er_0m1 = abs(L[j + 2][i + 2] - L[j + 2][i + 3]);
            float er_1m1 = abs(L[j + 2][i + 3] - L[j + 2][i + 4]);
            float er_01b = abs(L[j + 3][i + 2] - L[j + 3][i + 3]);
            float er_11b = abs(L[j + 3][i + 3] - L[j + 3][i + 4]);
            float lca_eu = max(max(er_0m1, er_1m1), max(er_01b, er_11b)) * CMAA2_LCA_AMOUNT;

            uint v = 0u;
            if ((er - lca_er) > CMAA2_EDGE_THRESHOLD) v |= 1u;
            if ((ed - lca_ed) > CMAA2_EDGE_THRESHOLD) v |= 2u;
            if ((el - lca_el) > CMAA2_EDGE_THRESHOLD) v |= 4u;
            if ((eu - lca_eu) > CMAA2_EDGE_THRESHOLD) v |= 8u;
            e[j][i] = v;
        }
    }

    vec4 edges       = UnpackEdgesFlt(e[1][1]);
    vec4 edgesLeft   = UnpackEdgesFlt(e[1][0]);
    vec4 edgesRight  = UnpackEdgesFlt(e[1][2]);
    vec4 edgesTop    = UnpackEdgesFlt(e[0][1]);
    vec4 edgesBottom = UnpackEdgesFlt(e[2][1]);

    vec4 weights = vec4(0.0);

    // Simple shapes：3×3 混合权重
    {
        vec4 blendVal = ComputeSimpleShapeBlendValues(edges, edgesLeft, edgesRight, edgesTop, edgesBottom);
        weights = max(weights, blendVal);
    }

    // Z 形状：拐点像素自权重（DetectZs + 线长 + 拐点处 lerpK）
    {
        float invertedZScore, normalZScore;
        float maxScore;
        bool horizontal = true;
        bool invertedZ = false;

        // horizontal
        {
            vec4 edgesM1P0 = edgesLeft;
            vec4 edgesP1P0 = edgesRight;
            vec4 edgesP2P0 = UnpackEdgesFlt(LoadEdgeFast(pixelPos + ivec2(2, 0)));
            DetectZsHorizontal(edges, edgesM1P0, edgesP1P0, edgesP2P0, invertedZScore, normalZScore);
            maxScore = max(invertedZScore, normalZScore);
            if (maxScore > 0.0)
                invertedZ = invertedZScore > normalZScore;
        }
        // vertical
        {
            vec4 edgesM1P0 = edgesBottom;
            vec4 edgesP1P0 = edgesTop;
            vec4 edgesP2P0 = UnpackEdgesFlt(LoadEdgeFast(pixelPos + ivec2(0, -2)));
            DetectZsHorizontal(edges.argb, edgesM1P0.argb, edgesP1P0.argb, edgesP2P0.argb, invertedZScore, normalZScore);
            float vertScore = max(invertedZScore, normalZScore);
            if (vertScore > maxScore) {
                maxScore = vertScore;
                horizontal = false;
                invertedZ = invertedZScore > normalZScore;
            }
        }

        if (maxScore > 0.0) {
            float shapeQualityScore = floor(clamp(4.0 - maxScore, 0.0, 3.0));

            ivec2 stepRight = horizontal ? ivec2(1, 0) : ivec2(0, -1);
            float lineLengthLeft, lineLengthRight;
            FindZLineLengths(lineLengthLeft, lineLengthRight, pixelPos, horizontal, invertedZ, stepRight);

            lineLengthLeft  -= shapeQualityScore;
            lineLengthRight -= shapeQualityScore;

            if ((lineLengthLeft + lineLengthRight) >= 5.0) {
                // 拐点像素（i=0）的 lerpK（BlendZs 的 i=0 项）
                ivec2 blendDir = horizontal ? ivec2(0, -1) : ivec2(-1, 0);
                if (invertedZ) blendDir = -blendDir;

                float leftOdd  = CMAA2_SYMMETRY_CORRECTION_OFFSET * float(int(lineLengthLeft) % 2);
                float rightOdd = CMAA2_SYMMETRY_CORRECTION_OFFSET * float(int(lineLengthRight) % 2);

                float dampenEffect = clamp(float(lineLengthLeft + lineLengthRight - shapeQualityScore) * CMAA2_DAMPENING_EFFECT, 0.0, 1.0);

                float loopFrom = -floor((lineLengthLeft + 1.0) / 2.0) + 1.0;
                float totalLength = (floor((lineLengthRight + 1.0) / 2.0) - loopFrom) + 1.0 - leftOdd - rightOdd;
                float lerpStep = 1.0 / totalLength;
                float lerpFromK = (0.5 - leftOdd - loopFrom) * lerpStep;

                // i = 0（拐点自身）：secondPart=0、srcOffset=+1
                float lerpK = lerpFromK * dampenEffect;
                float k = abs(lerpK);

                if (blendDir == ivec2(-1, 0)) weights.r = max(weights.r, k);
                else if (blendDir == ivec2(0, -1)) weights.g = max(weights.g, k);
                else if (blendDir == ivec2(1, 0)) weights.b = max(weights.b, k);
                else weights.a = max(weights.a, k);
            }
        }
    }

    outColor = weights;
}
