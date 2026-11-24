#pragma once

#include <stdlib.h>

#if defined(__ANDROID__) && __ANDROID_API__ < 28
static inline void* box2dAndroidAlignedAlloc(size_t alignment, size_t size)
{
    void* memory = NULL;
    return posix_memalign(&memory, alignment, size) == 0 ? memory : NULL;
}

#define aligned_alloc box2dAndroidAlignedAlloc
#endif
