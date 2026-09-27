# SPDX-License-Identifier: Apache-2.0
include(FetchContent)
# Stable explicit API baseline; optional authoring dependency, never fetched by
# headless/default builds. Original engine integration is separate from upstream.
FetchContent_Declare(imgui
    URL https://codeload.github.com/ocornut/imgui/tar.gz/f5befd2d29e66809cd1110a152e375a7f1981f06
    URL_HASH SHA256=85f4ce357df05bcc331b587f01976f47fb55f19fadf477a7907289686bc3f4c8
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)
add_library(poima_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp" "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp" "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp")
target_compile_features(poima_imgui PUBLIC cxx_std_20)
target_include_directories(poima_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_link_libraries(poima_imgui PRIVATE SDL3::SDL3-static)
target_sources(poima_core PRIVATE src/editor.cpp)
target_link_libraries(poima_core PRIVATE poima_imgui)
foreach(stage vs ps)
    if(stage STREQUAL "vs")
        set(entry vertex_main)
    else()
        set(entry pixel_main)
    endif()
    set(header "${CMAKE_CURRENT_BINARY_DIR}/generated/poima/editor_ui_${stage}.hpp")
    add_custom_command(OUTPUT "${header}"
        BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated/poima/editor_ui_${stage}.spv"
        COMMAND "${POIMA_DXC}" -spirv -T "${stage}_6_0" -E "${entry}"
            -fspv-target-env=vulkan1.3 -fvk-use-dx-layout
            -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-u-shift 384 0
            -Fo "${CMAKE_CURRENT_BINARY_DIR}/generated/poima/editor_ui_${stage}.spv"
            -Fh "${header}" -Vn "poima_editor_ui_${stage}"
            "${CMAKE_SOURCE_DIR}/shaders/editor_ui.hlsl"
        DEPENDS "${CMAKE_SOURCE_DIR}/shaders/editor_ui.hlsl" VERBATIM)
    target_sources(poima_core PRIVATE "${header}")
endforeach()
install(FILES "${imgui_SOURCE_DIR}/LICENSE.txt" DESTINATION share/poima/licenses/DearImGui)
