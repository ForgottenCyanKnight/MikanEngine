if(WIN32)
    set(Vulkan_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/vulkan/include" CACHE PATH "" FORCE)
    set(Vulkan_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/lib/x64/vulkan-1.lib" CACHE FILEPATH "" FORCE)
    set(FFX_FSR2_API_DX12 OFF CACHE BOOL "" FORCE)
    set(FFX_FSR2_API_VK ON CACHE BOOL "" FORCE)
    set(FSR2_PLATFORM_NAME x64)
    # Upstream uses this to identify a 64-bit Ninja target.
    set(_fsr_saved_linker_flags "${CMAKE_EXE_LINKER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "/machine:x64")
    add_subdirectory(dependencies/FidelityFX-FSR2/src/ffx-fsr2-api EXCLUDE_FROM_ALL)
    set(CMAKE_EXE_LINKER_FLAGS "${_fsr_saved_linker_flags}")
    set_target_properties(ffx_fsr2_api_x64 ffx_fsr2_api_vk_x64 PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/fsr2/lib"
        ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/fsr2/lib")
    target_link_libraries(Game PRIVATE ffx_fsr2_api_x64 ffx_fsr2_api_vk_x64)
    target_include_directories(Game PRIVATE dependencies/FidelityFX-FSR2/src/ffx-fsr2-api)
    target_sources(Game PRIVATE src/Rendering/Denoising/Fsr2.cpp)
    target_compile_definitions(Game PRIVATE MIKAN_ENABLE_FSR2=1)
endif()
