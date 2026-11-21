# SPDX-License-Identifier: Apache-2.0
include(FetchContent)

# Populate both sources separately. EXCLUDE_FROM_ALL keeps optional upstream
# tools and optimizer targets out of Poima's default renderer build.
FetchContent_Declare(poima_spirv_headers
    URL https://codeload.github.com/KhronosGroup/SPIRV-Headers/tar.gz/496543121ce6419f23d6fa5d7194ba66c36212d2
    URL_HASH SHA256=a9bb9c48713245eacf97cc539b6f1d45405a92d8813f8b82e635f0a085ee9898
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR poima-source-only)
FetchContent_Declare(poima_spirv_tools
    URL https://codeload.github.com/KhronosGroup/SPIRV-Tools/tar.gz/refs/tags/v2026.4
    URL_HASH SHA256=92df4c377603f086c3d2a1eb2bd9b8f883a73ca890fb939d78a0cecdcc01686c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR poima-source-only)
FetchContent_MakeAvailable(poima_spirv_headers poima_spirv_tools)

set(SPIRV-Headers_SOURCE_DIR "${poima_spirv_headers_SOURCE_DIR}")
set(SKIP_SPIRV_TOOLS_INSTALL ON)
set(SPIRV_SKIP_EXECUTABLES ON)
set(SPIRV_SKIP_TESTS ON)
set(SPIRV_TOOLS_BUILD_STATIC ON)
set(SPIRV_TOOLS_USE_MIMALLOC OFF)
set(SPIRV_WERROR OFF)
add_subdirectory("${poima_spirv_tools_SOURCE_DIR}" "${poima_spirv_tools_BINARY_DIR}" EXCLUDE_FROM_ALL)

target_sources(poima_core PRIVATE src/post_effect_vulkan.cpp)
target_link_libraries(poima_core PRIVATE SPIRV-Tools-static)
install(FILES "${poima_spirv_tools_SOURCE_DIR}/LICENSE" DESTINATION share/poima/licenses/SPIRV-Tools)
install(FILES "${poima_spirv_headers_SOURCE_DIR}/LICENSE" DESTINATION share/poima/licenses/SPIRV-Headers)
install(DIRECTORY "${poima_spirv_headers_SOURCE_DIR}/LICENSES" DESTINATION share/poima/licenses/SPIRV-Headers)
