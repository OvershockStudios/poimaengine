# SPDX-License-Identifier: Apache-2.0
include(FetchContent)
set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_NVAPI OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_RTXMU OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_AFTERMATH OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
set(NVRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nvrhi
    URL https://codeload.github.com/NVIDIA-RTX/NVRHI/tar.gz/d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076
    URL_HASH SHA256=2f62a5115ff8a11384b0d10c336b97aad73a78175addc11ca94c385eff94a452
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nvrhi)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
set(SDL_DIRECTX OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN ON CACHE BOOL "" FORCE)
FetchContent_Declare(sdl3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
    URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sdl3)

find_program(POIMA_DXC NAMES dxc
    HINTS "${CMAKE_SOURCE_DIR}/.cache/toolchains/dxc-v1.8.2505.1/bin"
    DOC "Host DXC executable with SPIR-V support" REQUIRED NO_CMAKE_FIND_ROOT_PATH)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/poima")
foreach(shader smoke scene shadow)
foreach(stage vs ps)
    if(shader STREQUAL "shadow" AND stage STREQUAL "ps")
        continue()
    endif()
    if(stage STREQUAL "vs")
        set(entry vertex_main)
    else()
        set(entry pixel_main)
    endif()
    if(shader STREQUAL "shadow")
        set(entry shadow_vertex_main)
    endif()
    set(shader_dependencies "${CMAKE_SOURCE_DIR}/shaders/${shader}.hlsl")
    if(shader STREQUAL "shadow")
        list(APPEND shader_dependencies "${CMAKE_SOURCE_DIR}/shaders/scene.hlsl")
    endif()
    set(header "${CMAKE_CURRENT_BINARY_DIR}/generated/poima/${shader}_${stage}.hpp")
    add_custom_command(OUTPUT "${header}"
        BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated/poima/${shader}_${stage}.spv"
        COMMAND "${POIMA_DXC}" -spirv -T "${stage}_6_0" -E "${entry}"
            -fspv-target-env=vulkan1.3 -fvk-use-dx-layout
            -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-u-shift 384 0
            -Fo "${CMAKE_CURRENT_BINARY_DIR}/generated/poima/${shader}_${stage}.spv"
            -Fh "${header}" -Vn "poima_${shader}_${stage}"
            "${CMAKE_SOURCE_DIR}/shaders/${shader}.hlsl"
        DEPENDS ${shader_dependencies}
        VERBATIM)
    target_sources(poima_core PRIVATE "${header}")
endforeach()
endforeach()
target_sources(poima_core PRIVATE src/render_smoke.cpp)
target_link_libraries(poima_core PRIVATE nvrhi nvrhi_vk SDL3::SDL3-static)
target_compile_definitions(poima_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

install(FILES "${nvrhi_SOURCE_DIR}/LICENSE.txt" DESTINATION share/poima/licenses/NVRHI)
install(FILES "${sdl3_SOURCE_DIR}/LICENSE.txt" DESTINATION share/poima/licenses/SDL3)
