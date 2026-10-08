# SPDX-License-Identifier: Apache-2.0
include(FetchContent)
# Stable explicit API baseline; optional authoring dependency, never fetched by
# headless/default builds. Original engine integration is separate from upstream.
FetchContent_Declare(imgui_docking
    URL https://codeload.github.com/ocornut/imgui/tar.gz/52fe0a05a7b1aa180a202bb24f0f2a049a9c1b7d
    URL_HASH SHA256=84196b24c66cd3be22cb0b313ef70941481238ec33e595401b07976baeb7c3db
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui_docking)
set(imgui_SOURCE_DIR "${imgui_docking_SOURCE_DIR}")
add_library(poima_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp" "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp" "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp")
target_compile_features(poima_imgui PUBLIC cxx_std_20)
target_include_directories(poima_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_link_libraries(poima_imgui PRIVATE SDL3::SDL3-static)
target_sources(poima_core PRIVATE src/editor.cpp src/editor_style.cpp)
set(font_header "${CMAKE_CURRENT_BINARY_DIR}/generated/poima/editor_fonts.hpp")
set(font_contents "// Generated from unmodified SIL OFL Source Sans 3 fonts.\n#pragma once\n")
foreach(weight Regular Semibold)
    set(font "${CMAKE_SOURCE_DIR}/third_party/source_sans/SourceSans3-${weight}.ttf")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${font}")
    file(READ "${font}" font_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," font_array "${font_hex}")
    string(APPEND font_contents "inline const unsigned char source_sans_${weight}[]={${font_array}};\n")
endforeach()
# Preserve the header's timestamp when font bytes are unchanged. Touching it on
# every configure needlessly recompiles editor_style.cpp during version builds.
file(CONFIGURE OUTPUT "${font_header}" CONTENT "${font_contents}" @ONLY)
unset(font_contents)
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
install(FILES "${CMAKE_SOURCE_DIR}/third_party/source_sans/LICENSE.md" DESTINATION share/poima/licenses/SourceSans3)
