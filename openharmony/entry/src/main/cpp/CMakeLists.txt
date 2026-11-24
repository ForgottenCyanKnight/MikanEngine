cmake_minimum_required(VERSION 3.22)
project(MikanEngineHarmony LANGUAGES C CXX)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include("${PROJECT_ROOT}/cmake/MikanMobileRuntimeSources.cmake")

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN ON CACHE BOOL "" FORCE)
add_subdirectory("${PROJECT_ROOT}/dependencies/SDL3" SDL3 EXCLUDE_FROM_ALL)

set(BUILD_SHARED_LIBS OFF)
set(SDLIMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
set(SDLIMAGE_VENDORED OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)
add_subdirectory("${PROJECT_ROOT}/dependencies/SDL3_image" SDL3_image EXCLUDE_FROM_ALL)

set(MIKAN_HARMONY_ASSIMP_SOURCE_DIR "${PROJECT_ROOT}/out/vendor/assimp-6.0.4" CACHE PATH "Assimp 6.0.4 sources")
if(NOT EXISTS "${MIKAN_HARMONY_ASSIMP_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Assimp sources missing. Run openharmony/build_voxel_prototype.ps1.")
endif()
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_MMD_IMPORTER ON CACHE BOOL "" FORCE)
add_subdirectory("${MIKAN_HARMONY_ASSIMP_SOURCE_DIR}" assimp EXCLUDE_FROM_ALL)

# The SDL HarmonyOS host loads libmain.so and resolves the application callbacks.
add_library(main SHARED ${MIKAN_MOBILE_RUNTIME_SOURCES})
target_compile_definitions(main PRIVATE
    MIKAN_OPENHARMONY=1 DISABLE_RAY_TRACING=1 MIKAN_ENABLE_NRD=0
    JPH_DEBUG_RENDERER=0 JPH_PROFILE_ENABLED=0
    JPH_DISABLE_TEMP_ALLOCATOR_NAMES=1 JPH_USE_CUSTOM_ALLOCATOR=0
)
target_include_directories(main PRIVATE
    "${PROJECT_ROOT}/include"
    "${PROJECT_ROOT}/include/Core"
    "${PROJECT_ROOT}/include/Rendering"
    "${PROJECT_ROOT}/include/ECS"
    "${PROJECT_ROOT}/include/Editor"
    "${PROJECT_ROOT}/src"
    "${PROJECT_ROOT}/dependencies/box2d/include"
    "${PROJECT_ROOT}/dependencies/box2d/src"
    "${PROJECT_ROOT}/dependencies/box2d/extern/simde"
    "${PROJECT_ROOT}/dependencies"
    "${PROJECT_ROOT}/dependencies/imgui"
    "${PROJECT_ROOT}/dependencies/FastNoiseLite/Cpp"
    "${PROJECT_ROOT}/dependencies/meshoptimizer/src"
    "${PROJECT_ROOT}/dependencies/ImGuizmo"
    "${PROJECT_ROOT}/dependencies/vulkan/include"
    "${PROJECT_ROOT}/dependencies/glm"
    "${PROJECT_ROOT}/dependencies/JoltPhysics"
    "${PROJECT_ROOT}/tools/ktx/include"
)
target_link_libraries(main PRIVATE SDL3::SDL3-shared SDL3_image::SDL3_image-static assimp::assimp vulkan hilog_ndk.z)
