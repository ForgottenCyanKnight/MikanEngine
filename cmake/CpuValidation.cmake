# Real runtime sources; no replacement physics, renderer stubs or GPU libraries.
include(CTest)
option(MIKAN_SOFTWARE_VULKAN "Build the SDL/Vulkan geometry-only validation host" OFF)
find_package(Threads REQUIRED)
foreach(_dependency JoltPhysics/Jolt/Jolt.h glm/glm/glm.hpp box2d/include/box2d/box2d.h SDL3/CMakeLists.txt vulkan/include/vulkan/vulkan.h)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/dependencies/${_dependency}")
        message(FATAL_ERROR "Missing ${_dependency}. Run: git submodule update --init --depth 1 dependencies/JoltPhysics dependencies/glm dependencies/box2d dependencies/SDL3 dependencies/vulkan")
    endif()
endforeach()

add_library(MikanCpuHeaders INTERFACE)
target_include_directories(MikanCpuHeaders INTERFACE
    "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/dependencies"
    "${PROJECT_SOURCE_DIR}/dependencies/glm" "${PROJECT_SOURCE_DIR}/dependencies/glm/glm"
    "${PROJECT_SOURCE_DIR}/dependencies/vulkan/include"
    "${PROJECT_SOURCE_DIR}/dependencies/imgui")

set(JOLT_COMPILE_DEFINITIONS JPH_DEBUG_RENDERER=0 JPH_PROFILE_ENABLED=0
    JPH_DISABLE_TEMP_ALLOCATOR_NAMES=1 JPH_USE_CUSTOM_ALLOCATOR=0)
file(GLOB_RECURSE _jolt_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/dependencies/JoltPhysics/Jolt/*.cpp")
add_library(JoltPhysics STATIC ${_jolt_sources})
target_include_directories(JoltPhysics PUBLIC dependencies/JoltPhysics)
target_compile_definitions(JoltPhysics PUBLIC ${JOLT_COMPILE_DEFINITIONS})
target_link_libraries(JoltPhysics PUBLIC Threads::Threads)

file(GLOB _box2d_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/dependencies/box2d/src/*.c")
add_library(Box2D STATIC ${_box2d_sources})
set_target_properties(Box2D PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(Box2D PUBLIC dependencies/box2d/include PRIVATE
    dependencies/box2d/src dependencies/box2d/extern/simde)
if(UNIX)
    target_link_libraries(Box2D PUBLIC m)
endif()

# SDL provides the production file/base-path API. No window/audio backend is
# needed by these targets; do not require X11, Wayland, ALSA or a Vulkan loader.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_VIDEO ${MIKAN_SOFTWARE_VULKAN} CACHE BOOL "" FORCE)
if(MIKAN_SOFTWARE_VULKAN)
    set(SDL_X11 ON CACHE BOOL "" FORCE)
    set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
    set(SDL_VULKAN ON CACHE BOOL "" FORCE)
    set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
    set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
endif()
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
add_subdirectory(dependencies/SDL3 EXCLUDE_FROM_ALL)

add_library(MikanCpuCore STATIC src/Core/ProjectManager.cpp src/Core/Log.cpp src/Core/JobSystem.cpp
    src/Core/Physics2DManager.cpp src/Core/RuntimeCapabilities.cpp)
target_link_libraries(MikanCpuCore PUBLIC MikanCpuHeaders SDL3::SDL3-static Threads::Threads Box2D)

add_library(MikanCpuImGui STATIC dependencies/imgui/imgui.cpp
    dependencies/imgui/imgui_draw.cpp dependencies/imgui/imgui_tables.cpp
    dependencies/imgui/imgui_widgets.cpp)
target_link_libraries(MikanCpuImGui PUBLIC MikanCpuHeaders)

add_library(MikanMmdRuntime STATIC src/Animation/VmdMotion.cpp
    src/Animation/MMD/Base/MMDTypes.cpp src/Animation/MMD/Model/MMDIkSolver.cpp
    src/Animation/MMD/Model/MMDModel.cpp src/Animation/MMD/Model/MMDMorph.cpp
    src/Animation/MMD/Model/MMDNode.cpp src/Animation/MMD/Model/PMXFile.cpp
    src/Animation/MMD/Model/VMDAnimation.cpp src/Animation/MMD/Model/VMDFile.cpp
    src/Animation/MMD/Physics/MMDPhysics.cpp src/Animation/MmdRuntime.cpp)
target_link_libraries(MikanMmdRuntime PUBLIC MikanCpuHeaders JoltPhysics)
if(NOT WIN32)
    find_package(Iconv REQUIRED)
    target_link_libraries(MikanMmdRuntime PRIVATE Iconv::Iconv)
endif()

function(mikan_cpu_executable target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE MikanCpuHeaders ${ARGN})
endfunction()

function(mikan_cpu_test target source)
    mikan_cpu_executable(${target} ${source} ${ARGN})
    # Existing contracts use assert; keep checks enabled in Release too.
    # MMD's Check-based executable retains the Jolt library's NDEBUG setting
    # because Jolt's inline API must agree with its compiled assertion ABI.
    if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
    if(BUILD_TESTING)
        add_test(NAME ${target} COMMAND ${target})
        set_tests_properties(${target} PROPERTIES TIMEOUT 120)
    endif()
endfunction()

mikan_cpu_executable(MikanMmdTests tests/MmdRuntimeTests.cpp MikanMmdRuntime)
if(BUILD_TESTING)
    add_test(NAME mmd_interpolation COMMAND MikanMmdTests --interpolation)
    add_test(NAME mmd_physics COMMAND MikanMmdTests --physics)
    set_tests_properties(mmd_interpolation mmd_physics PROPERTIES TIMEOUT 120)
endif()
mikan_cpu_test(MikanCpuCoreTests tests/CpuCoreTests.cpp MikanCpuCore Box2D)
mikan_cpu_executable(MikanProjectAssetLinksTests tests/ProjectAssetLinksTests.cpp MikanCpuCore)
if(BUILD_TESTING)
    add_test(NAME project_asset_links COMMAND ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:MikanProjectAssetLinksTests>
        -DFIXTURE_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-data/asset-links
        -P ${PROJECT_SOURCE_DIR}/cmake/RunAssetLinksTest.cmake)
    set_tests_properties(project_asset_links PROPERTIES TIMEOUT 120)
endif()
mikan_cpu_test(MikanViewportResolutionTests tests/ViewportResolutionTests.cpp MikanCpuImGui)
mikan_cpu_test(MikanViewportDockLayoutTests tests/ViewportDockLayoutTests.cpp MikanCpuImGui)
mikan_cpu_test(MikanVoxSurfaceReductionTests tests/VoxSurfaceReductionStatsTests.cpp)
add_executable(MikanVoxModelStats tools/standalone/VoxModelStats.cpp src/Rendering/VoxLoader.cpp)
target_include_directories(MikanVoxModelStats PRIVATE include/Rendering include/Core src)
target_link_libraries(MikanVoxModelStats PRIVATE MikanCpuCore)
if(BUILD_TESTING)
    add_test(NAME vox_model_surface_contracts COMMAND MikanVoxModelStats --self-test)
    set_tests_properties(vox_model_surface_contracts PROPERTIES TIMEOUT 120)
endif()
mikan_cpu_test(MikanEmissiveLightIndexTests tests/EmissiveLightIndexTests.cpp)
mikan_cpu_test(MikanRayTracingDeformationTests tests/RayTracingDeformationTests.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    mikan_cpu_test(MikanLinuxVmdEncodingTests tests/LinuxVmdEncodingTests.cpp MikanMmdRuntime)
endif()
find_package(Python3 COMPONENTS Interpreter)
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME vox_public_models COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/VoxPublicModelsTests.py $<TARGET_FILE:MikanVoxModelStats>
        ${PROJECT_SOURCE_DIR}/tests/fixtures/vox-mit ${CMAKE_CURRENT_BINARY_DIR}/vox-models-report.json)
    set_tests_properties(vox_public_models PROPERTIES TIMEOUT 120)
    add_test(NAME voxel_dda_cpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/VoxelDDAHierarchyTests.py)
    add_test(NAME restir_math_cpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/test_restir_enhanced_math.py)
endif()
if(MIKAN_SOFTWARE_VULKAN)
    message(STATUS "Mikan CPU modules plus geometry-only SDL/Vulkan validation; no full editor/gameplay host")
    include(${PROJECT_SOURCE_DIR}/cmake/SoftwareVulkan.cmake)
else()
    message(STATUS "Mikan CPU validation build: no editor, full gameplay host, graphics backend or gameplay plugins")
endif()
