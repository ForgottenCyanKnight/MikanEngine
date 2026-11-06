#ifdef MIKAN_TRIANGLE_DEBUG
#undef main
#ifndef TRIANGLE_DEBUG_OUTPUT
#define TRIANGLE_DEBUG_OUTPUT outColor
#endif
#ifndef TRIANGLE_DEBUG_PRIMITIVE
#define TRIANGLE_DEBUG_PRIMITIVE uint(gl_PrimitiveID)
#endif
void main() {
    // Preserve alpha masks, authored clipping, and depth before overriding color.
    triangleDebugMaterialMain();
    if (gl_FragCoord.z < 1.0)
        TRIANGLE_DEBUG_OUTPUT = vec4(triangleDebugColor(TRIANGLE_DEBUG_PRIMITIVE), 1.0);
}
#endif
