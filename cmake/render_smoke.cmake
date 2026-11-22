# SPDX-License-Identifier: Apache-2.0
include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/spirv_validation.cmake")
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
set(SDL_AUDIO ${POIMA_ENABLE_AUDIO} CACHE BOOL "" FORCE)
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
foreach(shader smoke scene scene_products scene_deferred scene_deferred_color deferred_lighting deferred_lighting_ao deferred_lighting_color deferred_lighting_color_ao
        deferred_lighting_reflection deferred_lighting_ao_reflection
        deferred_lighting_color_reflection deferred_lighting_color_ao_reflection screen_reflections height_fog height_fog_aerial aerial_perspective auto_exposure
        ambient_occlusion ambient_occlusion_filter scene_output scene_output_ao scene_output_products scene_output_bloom scene_output_products_bloom scene_output_grade scene_output_grade_bloom
        scene_output_exposure scene_output_bloom_exposure scene_output_grade_exposure scene_output_grade_bloom_exposure
        spatial_aa bloom viewmodel_composite viewmodel_resolve sky sky_atmosphere atmosphere_density atmosphere_sky_view shadow skinning cluster_lights temporal_inputs temporal_composed_inputs temporal_sprite_inputs temporal_composed_sprite_inputs editor_overlay game_ui particle_sprite particle_sprite_temporal)
if(shader MATCHES "^(skinning|cluster_lights|temporal(_composed)?(_sprite)?_inputs|deferred_lighting.*|ambient_occlusion.*|screen_reflections|height_fog(_aerial)?|aerial_perspective|auto_exposure|atmosphere_density|atmosphere_sky_view)$")
    set(shader_stages cs)
elseif(shader MATCHES "^(scene_output_grade|spatial_aa)" OR shader MATCHES "^scene_output.*_exposure$" OR shader STREQUAL "sky_atmosphere")
    set(shader_stages ps)
else()
    set(shader_stages vs ps)
endif()
foreach(stage IN LISTS shader_stages)
    if(stage STREQUAL "vs")
        set(entry vertex_main)
    elseif(stage STREQUAL "cs")
        set(entry compute_main)
    else()
        set(entry pixel_main)
    endif()
    if(shader STREQUAL "shadow")
        if(stage STREQUAL "vs")
            set(entry shadow_vertex_main)
        else()
            set(entry shadow_pixel_main)
        endif()
    endif()
    set(shader_source "${CMAKE_SOURCE_DIR}/shaders/${shader}.hlsl")
    set(shader_defines)
    if(shader MATCHES "^deferred_lighting")
        set(shader_source "${CMAKE_SOURCE_DIR}/shaders/deferred_lighting.hlsl")
        if(NOT shader MATCHES "_color")
            list(APPEND shader_defines -DPOIMA_SCENE_PRODUCTS=1)
        endif()
        if(shader MATCHES "_ao(_reflection)?$")
            list(APPEND shader_defines -DPOIMA_AMBIENT_OCCLUSION=1)
        endif()
        if(shader MATCHES "_reflection$")
            list(APPEND shader_defines -DPOIMA_REFLECTION_ROUGHNESS=1)
        endif()
    elseif(shader MATCHES "^scene_output")
        set(shader_source "${CMAKE_SOURCE_DIR}/shaders/scene_output.hlsl")
        if(shader MATCHES "_products" OR shader STREQUAL "scene_output_ao")
            list(APPEND shader_defines -DPOIMA_SCENE_PRODUCTS=1)
        endif()
        if(shader STREQUAL "scene_output_ao")
            list(APPEND shader_defines -DPOIMA_AMBIENT_OCCLUSION=1)
        endif()
        if(shader MATCHES "_bloom(_exposure)?$")
            list(APPEND shader_defines -DPOIMA_BLOOM=1)
        endif()
        if(shader MATCHES "_grade")
            list(APPEND shader_defines -DPOIMA_OUTPUT_GRADE=1)
        endif()
        if(shader MATCHES "_exposure$")
            list(APPEND shader_defines -DPOIMA_AUTO_EXPOSURE=1)
        endif()
    elseif(shader MATCHES "^scene_deferred")
        set(shader_source "${CMAKE_SOURCE_DIR}/shaders/scene.hlsl")
        list(APPEND shader_defines -DPOIMA_SCENE_DEFERRED=1)
        if(shader STREQUAL "scene_deferred")
            list(APPEND shader_defines -DPOIMA_SCENE_PRODUCTS=1)
        endif()
    elseif(shader STREQUAL "scene_products")
        set(shader_source "${CMAKE_SOURCE_DIR}/shaders/scene.hlsl")
        list(APPEND shader_defines -DPOIMA_SCENE_PRODUCTS=1)
    elseif(shader STREQUAL "sky_atmosphere")
        set(shader_source "${CMAKE_SOURCE_DIR}/shaders/sky.hlsl")
        list(APPEND shader_defines -DPOIMA_ATMOSPHERE_SKY=1)
    elseif(shader STREQUAL "height_fog_aerial")
        set(shader_source "${CMAKE_SOURCE_DIR}/shaders/height_fog.hlsl")
        list(APPEND shader_defines -DPOIMA_AERIAL_PERSPECTIVE=1)
    endif()
    set(shader_dependencies "${shader_source}")
    if(shader STREQUAL "shadow")
        list(APPEND shader_dependencies "${CMAKE_SOURCE_DIR}/shaders/scene.hlsl")
    endif()
    if(shader MATCHES "^(scene|scene_products|scene_deferred.*|shadow)$")
        list(APPEND shader_dependencies "${CMAKE_SOURCE_DIR}/shaders/water_ripple_normal.hlsli"
            "${CMAKE_SOURCE_DIR}/shaders/foliage_motion.hlsli")
    endif()
    if(shader MATCHES "^(ambient_occlusion.*|deferred_lighting.*|scene_deferred.*|scene|scene_products|shadow|cluster_lights|temporal(_composed)?(_sprite)?_inputs|sky(_atmosphere)?|screen_reflections|height_fog(_aerial)?|aerial_perspective)$")
        list(APPEND shader_dependencies "${CMAKE_SOURCE_DIR}/shaders/scene_frame.hlsli")
    endif()
    if(shader MATCHES "^(sky_atmosphere|atmosphere_density|atmosphere_sky_view|aerial_perspective)$")
        list(APPEND shader_dependencies "${CMAKE_SOURCE_DIR}/shaders/atmosphere_lookup.hlsli")
    endif()
    if(shader MATCHES "^(aerial_perspective|height_fog_aerial)$")
        list(APPEND shader_dependencies "${CMAKE_SOURCE_DIR}/shaders/aerial_perspective_lookup.hlsli")
    endif()
    if(shader MATCHES "^(deferred_lighting.*|scene_deferred.*|scene|scene_products|shadow|temporal(_composed)?(_sprite)?_inputs)$")
        list(APPEND shader_dependencies "${CMAKE_SOURCE_DIR}/shaders/scene_correspondence.hlsli")
    endif()
    if(shader MATCHES "^temporal_.+_inputs$")
        list(APPEND shader_dependencies "${CMAKE_SOURCE_DIR}/shaders/temporal_inputs.hlsl")
    endif()
    if(shader MATCHES "^(ambient_occlusion.*|deferred_lighting.*|scene_deferred.*|scene|scene_products|shadow)$")
        list(APPEND shader_dependencies "${CMAKE_SOURCE_DIR}/shaders/scene_lighting.hlsli" "${CMAKE_SOURCE_DIR}/shaders/scene_surface.hlsli" "${CMAKE_SOURCE_DIR}/shaders/environment_ibl.hlsli")
    endif()
    if(shader STREQUAL "particle_sprite_temporal")
        list(APPEND shader_dependencies "${CMAKE_SOURCE_DIR}/shaders/particle_sprite.hlsl")
    endif()
    if(shader STREQUAL "screen_reflections")
        list(APPEND shader_dependencies "${CMAKE_SOURCE_DIR}/shaders/environment_ibl.hlsli")
    endif()
    set(header "${CMAKE_CURRENT_BINARY_DIR}/generated/poima/${shader}_${stage}.hpp")
    add_custom_command(OUTPUT "${header}"
        BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated/poima/${shader}_${stage}.spv"
        COMMAND "${POIMA_DXC}" ${shader_defines} -spirv -T "${stage}_6_0" -E "${entry}"
            -fspv-target-env=vulkan1.3 -fvk-use-dx-layout
            -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-u-shift 384 0
            -Fo "${CMAKE_CURRENT_BINARY_DIR}/generated/poima/${shader}_${stage}.spv"
            -Fh "${header}" -Vn "poima_${shader}_${stage}"
            "${shader_source}"
        DEPENDS ${shader_dependencies}
        VERBATIM)
    target_sources(poima_core PRIVATE "${header}")
endforeach()
endforeach()
set(poima_startup_wordmark "${CMAKE_CURRENT_BINARY_DIR}/generated/poima/startup_wordmark.hpp")
set(poima_startup_png "${CMAKE_CURRENT_SOURCE_DIR}/desktop/Poima.Launcher/Poima.png")
add_custom_command(OUTPUT "${poima_startup_wordmark}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${poima_startup_png}" "-DOUTPUT=${poima_startup_wordmark}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_startup_wordmark.cmake"
    DEPENDS "${poima_startup_png}" cmake/embed_startup_wordmark.cmake VERBATIM)
target_sources(poima_core PRIVATE
    src/render_smoke.cpp src/render_readback.cpp src/render_screen_reflections.cpp src/render_height_fog.cpp src/render_viewmodel.cpp src/render_hdr_effects.cpp
    src/render_vulkan_instance.cpp src/render_vulkan_frame.cpp src/render_deferred_lighting.cpp
    src/player_window.cpp src/gamepad_sdl.cpp
    src/render_ui.cpp src/render_environment_ibl.cpp src/render_ambient_occlusion.cpp
    src/render_shadows.cpp src/render_scene_assets.cpp src/render_light_clusters.cpp src/render_sky.cpp
    src/render_auto_exposure.cpp
    src/render_skinning.cpp src/render_spatial_aa.cpp src/render_scene_output.cpp src/render_temporal_inputs.cpp src/render_sprite_targets.cpp
    "${poima_startup_wordmark}")
target_link_libraries(poima_core PRIVATE nvrhi nvrhi_vk SDL3::SDL3-static)
target_compile_definitions(poima_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

install(FILES "${nvrhi_SOURCE_DIR}/LICENSE.txt" DESTINATION share/poima/licenses/NVRHI)
install(FILES docs/branding/Playball-OFL.txt DESTINATION share/poima/licenses/Playball)
include("${CMAKE_CURRENT_LIST_DIR}/sdl_notices.cmake")
poima_install_sdl_notices("${sdl3_SOURCE_DIR}")

if(POIMA_BUILD_TESTS)
    add_executable(poima-gamepad-sdl-test tests/gamepad_sdl.cpp src/gamepad_sdl.cpp src/input_profile.cpp src/player.cpp)
else()
    add_executable(poima-gamepad-sdl-test EXCLUDE_FROM_ALL tests/gamepad_sdl.cpp src/gamepad_sdl.cpp src/input_profile.cpp src/player.cpp)
endif()
target_compile_features(poima-gamepad-sdl-test PRIVATE cxx_std_20)
target_include_directories(poima-gamepad-sdl-test PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(poima-gamepad-sdl-test PRIVATE SDL3::SDL3-static)
target_include_directories(poima-gamepad-sdl-test SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third_party")
if(POIMA_BUILD_TESTS)
    add_test(NAME gamepad_sdl COMMAND poima-gamepad-sdl-test)
endif()
