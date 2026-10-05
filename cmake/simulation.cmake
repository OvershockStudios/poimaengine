# SPDX-License-Identifier: Apache-2.0
include(FetchContent)
# Bound build/runtime costs and retain the SSE2 baseline. Deterministic flags
# are a prerequisite, not evidence of cross-platform replay equivalence.
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
foreach(feature SSE4_1 SSE4_2 AVX AVX2 AVX512 LZCNT TZCNT F16C FMADD)
    set(USE_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(jolt
    URL https://codeload.github.com/jrouwe/JoltPhysics/tar.gz/036ea7b1d717b3e713ac9d8cbd47118fb9cd5d60
    URL_HASH SHA256=4427d6ce190e049b186bb88dbfb8692c2373a7fc042c984b5f15396194aac958
    SOURCE_SUBDIR Build DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(jolt)
if(MINGW)
    # Jolt 5.4 relies on a transitive type_traits include removed by newer
    # libc++. Supply the standard header without modifying the pinned source.
    target_compile_options(Jolt PUBLIC -include type_traits)
endif()

set(ENTT_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ENTT_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(entt
    URL https://codeload.github.com/skypjack/entt/tar.gz/b4e58bdd364ad72246c123a0c28538eab3252672
    URL_HASH SHA256=3e996cf255b09527faf995c7c36e3daf92cefa0fb37540a4b9c359af557e19cd
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(entt)
get_target_property(poima_jolt_includes Jolt INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(poima_core SYSTEM PRIVATE ${poima_jolt_includes} "${entt_SOURCE_DIR}/src")
target_sources(poima_core PRIVATE src/runtime.cpp src/runtime_components.cpp)
target_link_libraries(poima_core PRIVATE Jolt EnTT::EnTT)
if(NOT MSVC)
    set_source_files_properties(src/runtime.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
install(FILES "${jolt_SOURCE_DIR}/LICENSE" DESTINATION share/poima/licenses/Jolt)
install(FILES "${entt_SOURCE_DIR}/LICENSE" DESTINATION share/poima/licenses/EnTT)
