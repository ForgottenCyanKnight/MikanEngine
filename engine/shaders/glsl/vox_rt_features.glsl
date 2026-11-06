// Compile-time feature boundary for the normal hardware-triangle path.
// The generic module retains every optional backend for explicit experiments.
#ifndef MIKAN_RT_BRICK_ONLY
#define MIKAN_RT_BRICK_ONLY 0
#endif
#ifndef MIKAN_RT_HAS_LOD
#define MIKAN_RT_HAS_LOD 1
#endif
#ifndef MIKAN_RT_HAS_TRIANGLE_DEBUG
#define MIKAN_RT_HAS_TRIANGLE_DEBUG 1
#endif
#ifndef MIKAN_RT_HAS_CUSTOM_TRAVERSAL
#define MIKAN_RT_HAS_CUSTOM_TRAVERSAL 1
#endif
#ifndef MIKAN_RT_HAS_HALF_LIGHTING
#define MIKAN_RT_HAS_HALF_LIGHTING 1
#endif
