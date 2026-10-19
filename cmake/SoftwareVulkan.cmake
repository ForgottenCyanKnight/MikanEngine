if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The software Vulkan geometry validation host currently targets Linux")
endif()
find_package(Vulkan REQUIRED)
find_program(MIKAN_GLSLANG glslangValidator REQUIRED)
set(_raster_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/raster-shaders")
file(MAKE_DIRECTORY "${_raster_shader_dir}")
set(_raster_spirv)
foreach(_shader voxel_quad.vert voxel_mesh.frag)
    set(_source "${PROJECT_SOURCE_DIR}/engine/shaders/glsl/${_shader}")
    set(_output "${_raster_shader_dir}/${_shader}.spv")
    add_custom_command(OUTPUT "${_output}"
        COMMAND "${MIKAN_GLSLANG}" -V --target-env vulkan1.1 "${_source}" -o "${_output}"
        DEPENDS "${_source}"
            "${PROJECT_SOURCE_DIR}/engine/shaders/glsl/vox_quad_decode.glsl"
            "${PROJECT_SOURCE_DIR}/engine/shaders/glsl/voxel_surface.glsl"
        VERBATIM)
    list(APPEND _raster_spirv "${_output}")
endforeach()
add_custom_target(MikanRasterShaders DEPENDS ${_raster_spirv})
add_executable(MikanVoxRasterPreview tools/standalone/VoxRasterPreview.cpp src/Rendering/VoxLoader.cpp)
target_include_directories(MikanVoxRasterPreview PRIVATE include/Core include/Rendering src)
target_compile_definitions(MikanVoxRasterPreview PRIVATE MIKAN_RASTER_SHADER_DIR="${_raster_shader_dir}")
target_link_libraries(MikanVoxRasterPreview PRIVATE MikanCpuCore Vulkan::Vulkan)
add_dependencies(MikanVoxRasterPreview MikanRasterShaders)

# Native RT validation: original compute expansion + Ray Query shader, and the
# production buffer, feature negotiation and acceleration-structure code.
set(_rt_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/ray-query-shaders")
file(MAKE_DIRECTORY "${_rt_shader_dir}")
set(_rt_spirv)
foreach(_shader vox_rt_expand.comp vox_rt_validate.comp)
    set(_output "${_rt_shader_dir}/${_shader}.spv")
    add_custom_command(OUTPUT "${_output}"
        COMMAND "${MIKAN_GLSLANG}" -V --target-env vulkan1.2
            "${PROJECT_SOURCE_DIR}/engine/shaders/glsl/${_shader}" -o "${_output}"
        DEPENDS "${PROJECT_SOURCE_DIR}/engine/shaders/glsl/${_shader}"
            "${PROJECT_SOURCE_DIR}/engine/shaders/glsl/vox_quad_decode.glsl"
            "${PROJECT_SOURCE_DIR}/engine/shaders/glsl/vox_rt_palette.glsl"
        VERBATIM)
    list(APPEND _rt_spirv "${_output}")
endforeach()
add_custom_target(MikanRayQueryShaders DEPENDS ${_rt_spirv})
add_executable(MikanVoxRayQueryPreview tools/standalone/VoxRayQueryPreview.cpp
    src/Rendering/VoxLoader.cpp src/Rendering/RendererBase.cpp
    src/Rendering/RayTracing/AccelerationStructure.cpp
    src/Rendering/RayTracing/RayTracingValidationDevice.cpp
    src/Core/VulkanRayTracingDevice.cpp src/Core/VulkanGpuProfiler.cpp)
target_include_directories(MikanVoxRayQueryPreview PRIVATE include/Core include/Rendering include/ECS src)
target_compile_definitions(MikanVoxRayQueryPreview PRIVATE MIKAN_RT_SHADER_DIR="${_rt_shader_dir}")
# RendererBase also contains unrelated graphics-pipeline utilities. Keep the
# actual buffer implementation and discard unused sections, rather than stubbing
# editor/scene globals to make a full runtime appear linked.
target_compile_options(MikanVoxRayQueryPreview PRIVATE -ffunction-sections -fdata-sections)
target_link_options(MikanVoxRayQueryPreview PRIVATE -Wl,--gc-sections)
target_link_libraries(MikanVoxRayQueryPreview PRIVATE MikanCpuCore Vulkan::Vulkan)
add_dependencies(MikanVoxRayQueryPreview MikanRayQueryShaders)
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME vox_software_ray_query COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/VoxSoftwareRayQueryTests.py $<TARGET_FILE:MikanVoxRayQueryPreview>
        --output ${CMAKE_CURRENT_BINARY_DIR}/software-ray-query)
    set_tests_properties(vox_software_ray_query PROPERTIES TIMEOUT 240 LABELS software-vulkan)
    add_test(NAME vox_software_raster COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/VoxSoftwareRasterTests.py $<TARGET_FILE:MikanVoxRasterPreview>
        --output ${CMAKE_CURRENT_BINARY_DIR}/software-vulkan)
    set_tests_properties(vox_software_raster PROPERTIES TIMEOUT 240 LABELS software-vulkan)
endif()

# Optional high-quality still host: production NEE GI and full Bruneton LUTs.
set(_full_dir "${CMAKE_CURRENT_BINARY_DIR}/full-preview/engine/shaders/spv")
file(MAKE_DIRECTORY "${_full_dir}")
file(GLOB _full_includes CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/engine/shaders/glsl/*.glsl")
set(_full_spirv)
foreach(_shader vox_rt_realtime.comp vox_rt_composite.comp atmo_transmittance.comp atmo_scattering.comp atmo_density.comp atmo_multiscatter.comp atmo_irradiance.comp atmo_sky.comp atmo_sky_cube.comp atmo_pano_to_cube.comp atmo_sh_proj.comp atmo_cube_prefilter.comp brdf_lut.comp denoiser_prepare.comp denoiser_unpack.comp)
    set(_output "${_full_dir}/${_shader}.spv")
    add_custom_command(OUTPUT "${_output}" COMMAND "${MIKAN_GLSLANG}" -V --target-env vulkan1.2 "${PROJECT_SOURCE_DIR}/engine/shaders/glsl/${_shader}" -o "${_output}"
        DEPENDS "${PROJECT_SOURCE_DIR}/engine/shaders/glsl/${_shader}" ${_full_includes} VERBATIM)
    list(APPEND _full_spirv "${_output}")
endforeach()
add_custom_command(OUTPUT "${_full_dir}/preview_accumulate.comp.spv" "${_full_dir}/preview_single.comp.spv" "${_full_dir}/preview_copy.comp.spv"
    COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/standalone/BuildCompleteExportShader.py" "${PROJECT_SOURCE_DIR}/engine/shaders/glsl/hardware_rt_present.frag" "${_full_dir}/preview_accumulate.comp"
    COMMAND "${MIKAN_GLSLANG}" -V --target-env vulkan1.2 "${_full_dir}/preview_accumulate.comp" -o "${_full_dir}/preview_accumulate.comp.spv"
    COMMAND "${MIKAN_GLSLANG}" -V --target-env vulkan1.2 "${_full_dir}/preview_single.comp" -o "${_full_dir}/preview_single.comp.spv"
    COMMAND "${MIKAN_GLSLANG}" -V --target-env vulkan1.2 "${_full_dir}/preview_copy.comp" -o "${_full_dir}/preview_copy.comp.spv"
    DEPENDS tools/standalone/BuildCompleteExportShader.py engine/shaders/glsl/tonemap.frag engine/shaders/glsl/vox_rt_color.glsl engine/shaders/glsl/hardware_rt_present.frag VERBATIM)
add_custom_target(MikanCompleteShaders DEPENDS ${_full_spirv} "${_full_dir}/preview_accumulate.comp.spv" "${_full_dir}/preview_single.comp.spv" "${_full_dir}/preview_copy.comp.spv")
add_executable(MikanVoxCompletePreview tools/standalone/VoxCompletePreview.cpp
    src/Rendering/VoxLoader.cpp src/Rendering/RendererBase.cpp src/Rendering/AtmosphereLUT.cpp
    src/Rendering/RayTracing/AccelerationStructure.cpp src/Rendering/RayTracing/RayTracingValidationDevice.cpp
    src/Core/VulkanRayTracingDevice.cpp src/Core/VulkanGpuProfiler.cpp)
target_include_directories(MikanVoxCompletePreview PRIVATE include/Core include/Rendering include/ECS src)
target_compile_definitions(MikanVoxCompletePreview PRIVATE MIKAN_RT_SHADER_DIR="${_rt_shader_dir}" MIKAN_FULL_SHADER_DIR="${_full_dir}")
target_compile_options(MikanVoxCompletePreview PRIVATE -ffunction-sections -fdata-sections)
target_link_options(MikanVoxCompletePreview PRIVATE -Wl,--gc-sections)
target_link_libraries(MikanVoxCompletePreview PRIVATE MikanCpuCore Vulkan::Vulkan)
add_dependencies(MikanVoxCompletePreview MikanCompleteShaders MikanRayQueryShaders)
option(MIKAN_SOFTWARE_NRD "Enable production NRD in the software Vulkan still host" OFF)
if(MIKAN_SOFTWARE_NRD)
    include(${CMAKE_CURRENT_LIST_DIR}/SoftwareNrd.cmake)
endif()
