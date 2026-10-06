option(MIKAN_ENABLE_DLSS_RR "Enable NVIDIA-only native Vulkan DLSS Ray Reconstruction" ON)
option(MIKAN_ENABLE_DLSS_SR "Enable NVIDIA-only native Vulkan DLSS Super Resolution" ON)
target_sources(Game PRIVATE src/Rendering/Denoising/DlssRayReconstruction.cpp)
set(MIKAN_DLSS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/DLSS")
if((MIKAN_ENABLE_DLSS_RR OR MIKAN_ENABLE_DLSS_SR) AND WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MIKAN_NGX_LIBRARY "${MIKAN_DLSS_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    set(MIKAN_DLSS_RR_DLL "${MIKAN_DLSS_ROOT}/lib/Windows_x86_64/rel/nvngx_dlssd.dll")
    if(NOT EXISTS "${MIKAN_NGX_LIBRARY}" OR NOT EXISTS "${MIKAN_DLSS_RR_DLL}")
        message(FATAL_ERROR "DLSS RR SDK missing. Install dependencies/DLSS or configure MIKAN_ENABLE_DLSS_RR=OFF.")
    endif()
    # Keep vendor headers and this definition confined to the adapter translation unit.
    set_property(SOURCE src/Rendering/Denoising/DlssRayReconstruction.cpp APPEND PROPERTY
        INCLUDE_DIRECTORIES "${MIKAN_DLSS_ROOT}/include")
    set_property(SOURCE src/Rendering/Denoising/DlssRayReconstruction.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS "MIKAN_ENABLE_DLSS_RR=$<BOOL:${MIKAN_ENABLE_DLSS_RR}>;MIKAN_ENABLE_DLSS_SR=$<BOOL:${MIKAN_ENABLE_DLSS_SR}>")
    target_link_libraries(Game PRIVATE "${MIKAN_NGX_LIBRARY}")
    add_custom_command(TARGET Game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MIKAN_DLSS_RR_DLL}" "$<TARGET_FILE_DIR:Game>/nvngx_dlssd.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MIKAN_DLSS_ROOT}/lib/Windows_x86_64/rel/nvngx_dlss.dll" "$<TARGET_FILE_DIR:Game>/nvngx_dlss.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MIKAN_DLSS_ROOT}/LICENSE.txt" "$<TARGET_FILE_DIR:Game>/NVIDIA_DLSS_LICENSE.txt"
        COMMENT "Deploying NVIDIA DLSS Ray Reconstruction release runtime")
endif()
