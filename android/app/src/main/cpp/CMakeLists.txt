cmake_minimum_required(VERSION 3.22.1)
project("mikanengine")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Android 平台强制使用 Release 构建优化
# 如果未指定构建类型，默认为 Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 设置 Release 优化标志（无论构建类型如何）
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

include("${PROJECT_ROOT}/cmake/MikanMobileRuntimeSources.cmake")

# Android exposes aligned_alloc starting at API 28. Keep minSdk 26 support.
set_source_files_properties("${PROJECT_ROOT}/dependencies/box2d/src/allocate.c"
    PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/Box2DAndroidAllocation.h")

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory("${PROJECT_ROOT}/dependencies/SDL3" SDL3 EXCLUDE_FROM_ALL)
find_package(SDL3_image REQUIRED CONFIG)

# ======================= mikanengine - runtime (renderer, physics, ECS, scene) =======================
# Android consumer target: shared runtime sources and optional Android gameplay.
add_library(mikanengine SHARED ${MIKAN_MOBILE_RUNTIME_SOURCES})

# Project contents are local-only; Android can build the engine without sample gameplay.
set(MIKAN_ANDROID_SAMPLE_GAME "${PROJECT_ROOT}/projects/third-person-navigation/games/cesiumwalk/CesiumWalk.cpp")
if(EXISTS "${MIKAN_ANDROID_SAMPLE_GAME}")
    target_sources(mikanengine PRIVATE "${MIKAN_ANDROID_SAMPLE_GAME}")
endif()
# JoltPhysics definitions
target_compile_definitions(mikanengine PRIVATE
    JPH_DEBUG_RENDERER=0
    JPH_PROFILE_ENABLED=0
    JPH_DISABLE_TEMP_ALLOCATOR_NAMES=1
    JPH_USE_CUSTOM_ALLOCATOR=0
)

# Android 平台定义 - 禁用桌面端特性
target_compile_definitions(mikanengine PRIVATE
    ANDROID_BUILD=1
    DISABLE_RAY_TRACING=1
)

target_include_directories(mikanengine PRIVATE
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/include/Core
    ${PROJECT_ROOT}/include/Rendering
    ${PROJECT_ROOT}/include/ECS
    ${PROJECT_ROOT}/include/Editor
    ${PROJECT_ROOT}/src
    # libassimp.so is Assimp 6.0.4; keep its generated public headers ahead
    # of the legacy flat header copies under dependencies/assimp.
    ${PROJECT_ROOT}/dependencies/assimp/include
    ${PROJECT_ROOT}/dependencies/box2d/include
    ${PROJECT_ROOT}/dependencies/box2d/src
    ${PROJECT_ROOT}/dependencies/box2d/msvc_compat
    ${PROJECT_ROOT}/dependencies/box2d/extern/simde
    ${PROJECT_ROOT}/dependencies
    ${PROJECT_ROOT}/dependencies/imgui
    ${PROJECT_ROOT}/dependencies/FastNoiseLite/Cpp
    ${PROJECT_ROOT}/dependencies/meshoptimizer/src
    ${PROJECT_ROOT}/dependencies/ImGuizmo
    ${PROJECT_ROOT}/dependencies/SDL3/include
    ${PROJECT_ROOT}/dependencies/SDL3_image/include
    ${PROJECT_ROOT}/dependencies/vulkan/include
    ${PROJECT_ROOT}/dependencies/glm
    ${PROJECT_ROOT}/dependencies/JoltPhysics
    ${PROJECT_ROOT}/tools/ktx/include
)

target_link_libraries(mikanengine PRIVATE
    SDL3::SDL3-shared
    SDL3_image::SDL3_image
    android
    log
    vulkan
    ${PROJECT_ROOT}/android/app/libs/${ANDROID_ABI}/libassimp.so
)

# Android 15+ 16KB 页大小兼容：ELF LOAD 段按 16KB 对齐（AGP 打包检查要求）
target_link_options(mikanengine PRIVATE "-Wl,-z,max-page-size=16384")

# Same vendor-neutral denoiser boundary; Android keeps the legacy SSGI filter.
target_compile_definitions(mikanengine PRIVATE MIKAN_ENABLE_NRD=0)
