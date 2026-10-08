# SPDX-License-Identifier: Apache-2.0
include(FetchContent)
include(GNUInstallDirs)
# Recast Navigation v1.6.0, peeled release commit. Only the static bake and
# query libraries are included; no demo, crowd, tile-cache or bundled tests.
FetchContent_Declare(poima_recast
    URL https://codeload.github.com/recastnavigation/recastnavigation/tar.gz/6dc1667f580357e8a2154c28b7867bea7e8ad3a7
    URL_HASH SHA256=f565cc91b85df95a656cfc672e41c02e8aa44ba2363905aa8277ce20ea87491d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(poima_recast)
if(NOT poima_recast_POPULATED)
    FetchContent_Populate(poima_recast)
endif()
# Upstream uses untyped add_library. Scope its root variables and static policy
# so ambient BUILD_SHARED_LIBS cannot introduce unbundled runtime dependencies.
function(poima_add_static_recast)
    set(BUILD_SHARED_LIBS OFF)
    set(RECASTNAVIGATION_DT_POLYREF64 OFF)
    set(RECASTNAVIGATION_DT_VIRTUAL_QUERYFILTER OFF)
    set(SOVERSION 1)
    set(LIB_VERSION 1.6.0)
    add_subdirectory("${poima_recast_SOURCE_DIR}/Recast" "${poima_recast_BINARY_DIR}/Recast" EXCLUDE_FROM_ALL)
    add_subdirectory("${poima_recast_SOURCE_DIR}/Detour" "${poima_recast_BINARY_DIR}/Detour" EXCLUDE_FROM_ALL)
endfunction()
poima_add_static_recast()
set_target_properties(Recast Detour PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Keep upstream include paths and definitions out of unrelated core compile
# commands when this optional backend is enabled.
add_library(poima_navigation_backend STATIC src/navigation.cpp)
target_compile_features(poima_navigation_backend PRIVATE cxx_std_20)
target_include_directories(poima_navigation_backend PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(poima_navigation_backend SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    "${poima_recast_SOURCE_DIR}/Recast/Include" "${poima_recast_SOURCE_DIR}/Detour/Include")
set_target_properties(poima_navigation_backend PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(poima_navigation_backend PRIVATE /W4 /permissive-)
else()
    target_compile_options(poima_navigation_backend PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
target_link_libraries(poima_navigation_backend PRIVATE Recast Detour)
target_link_libraries(poima_core PRIVATE poima_navigation_backend)
install(FILES "${poima_recast_SOURCE_DIR}/License.txt" DESTINATION share/poima/licenses/RecastNavigation)
