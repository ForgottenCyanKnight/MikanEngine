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

set(MIKAN_ANDROID_PROJECT_PATH "" CACHE PATH "Project directory for Android gameplay")
if(MIKAN_ANDROID_PROJECT_PATH)
    get_filename_component(project_dir "${MIKAN_ANDROID_PROJECT_PATH}" REALPATH
        BASE_DIR "${PROJECT_ROOT}")
    set(manifest_path "${project_dir}/project.json")
    if(NOT EXISTS "${manifest_path}")
        message(FATAL_ERROR "Android project manifest not found: ${manifest_path}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest_path}")
    file(READ "${manifest_path}" manifest)
    string(JSON game ERROR_VARIABLE game_missing GET "${manifest}" game)
    if(NOT game_missing AND NOT game STREQUAL "")
        if(NOT game MATCHES "^[A-Za-z0-9_.-]+$" OR game STREQUAL "." OR game STREQUAL "..")
            message(FATAL_ERROR "Invalid Android gameplay module name: ${game}")
        endif()
        string(JSON code_root ERROR_VARIABLE code_missing GET "${manifest}" codeRoot)
        if(code_missing OR NOT code_root)
            set(code_root "games")
        endif()
        if(IS_ABSOLUTE "${code_root}" OR code_root MATCHES "^[A-Za-z]:|^\\\\")
            message(FATAL_ERROR "Android project codeRoot must be relative: ${code_root}")
        endif()
        get_filename_component(code_dir "${project_dir}/${code_root}" REALPATH)
        get_filename_component(game_dir "${code_dir}/${game}" REALPATH)
        cmake_path(IS_PREFIX project_dir "${code_dir}" NORMALIZE code_in_project)
        cmake_path(IS_PREFIX code_dir "${game_dir}" NORMALIZE game_in_code)
        if(NOT code_in_project OR NOT game_in_code OR code_dir STREQUAL project_dir
           OR game_dir STREQUAL code_dir)
            message(FATAL_ERROR "Android gameplay source must stay inside project codeRoot: ${game_dir}")
        endif()
        file(GLOB game_sources LIST_DIRECTORIES FALSE CONFIGURE_DEPENDS "${game_dir}/*.cpp")
        if(NOT game_sources)
            message(FATAL_ERROR "No Android gameplay module sources found in ${game_dir}")
        endif()
        target_sources(mikanengine PRIVATE ${game_sources})
    endif()
else()
    # Preserve the original standalone build when no Android project is selected.
    set(sample_game "${PROJECT_ROOT}/projects/third-person-navigation/games/cesiumwalk/CesiumWalk.cpp")
    if(EXISTS "${sample_game}")
        target_sources(mikanengine PRIVATE "${sample_game}")
    endif()
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

set(MIKAN_ANDROID_ASSIMP_LIBRARY "" CACHE FILEPATH "Optional Android libassimp.so override")
if(MIKAN_ANDROID_ASSIMP_LIBRARY)
    add_library(mikan_android_assimp SHARED IMPORTED)
    set_target_properties(mikan_android_assimp PROPERTIES IMPORTED_LOCATION "${MIKAN_ANDROID_ASSIMP_LIBRARY}")
    set(assimp_library mikan_android_assimp)
else()
    set(assimp_library "${PROJECT_ROOT}/android/app/libs/${ANDROID_ABI}/libassimp.so")
endif()

target_link_libraries(mikanengine PRIVATE
    SDL3::SDL3-shared
    SDL3_image::SDL3_image
    android
    log
    vulkan
    ${assimp_library}
)

# Android 15+ 16KB 页大小兼容：ELF LOAD 段按 16KB 对齐（AGP 打包检查要求）
target_link_options(mikanengine PRIVATE "-Wl,-z,max-page-size=16384")

# Same vendor-neutral denoiser boundary; Android keeps the legacy SSGI filter.
target_compile_definitions(mikanengine PRIVATE MIKAN_ENABLE_NRD=0)
