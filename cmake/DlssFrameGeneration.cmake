option(MIKAN_ENABLE_DLSS_FG "Build optional NVIDIA Vulkan Streamline Frame Generation" ON)
target_sources(Game PRIVATE src/Core/DlssFrameGeneration.cpp)
if(MIKAN_ENABLE_DLSS_FG AND WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MIKAN_STREAMLINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/Streamline")
    if(NOT EXISTS "${MIKAN_STREAMLINE_ROOT}/bin/sl.interposer.dll")
        message(FATAL_ERROR "Install dependencies/Streamline or set MIKAN_ENABLE_DLSS_FG=OFF")
    endif()
    target_compile_definitions(Game PRIVATE MIKAN_ENABLE_DLSS_FG=1)
    # The cached ImGui backend otherwise bypasses Streamline swapchain hooks.
    # Build a separate variant without changing the shared dependency package.
    add_library(MikanFGImGui STATIC ${IMGUI_SOURCES})
    target_include_directories(MikanFGImGui PRIVATE dependencies dependencies/imgui dependencies/SDL3 dependencies/vulkan)
    target_compile_definitions(MikanFGImGui PRIVATE "IMGUI_API=__declspec(dllexport)" "IMGUI_IMPL_API=__declspec(dllexport)")
    target_compile_options(MikanFGImGui PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/include/Core/DlssVulkanHooks.h")
    set(MIKAN_RUNTIME_IMGUI MikanFGImGui)

    target_compile_options(Game PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/include/Core/DlssVulkanHooks.h")
    set_property(SOURCE src/Core/DlssFrameGeneration.cpp APPEND PROPERTY INCLUDE_DIRECTORIES "${MIKAN_STREAMLINE_ROOT}/include")
    target_link_libraries(Game PRIVATE wintrust crypt32)
    foreach(runtime sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll NvLowLatencyVk.dll nvngx_dlssg.dll reflex.license.txt nvngx_dlss.license.txt)
        add_custom_command(TARGET Game POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MIKAN_STREAMLINE_ROOT}/bin/${runtime}" "$<TARGET_FILE_DIR:Game>/${runtime}")
    endforeach()
    add_custom_command(TARGET Game POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MIKAN_STREAMLINE_ROOT}/license.txt" "$<TARGET_FILE_DIR:Game>/NVIDIA_STREAMLINE_LICENSE.txt")
endif()
