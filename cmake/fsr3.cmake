# SPDX-License-Identifier: Apache-2.0
if(NOT WIN32 OR NOT POIMA_BUILD_RENDER_SMOKE)
    message(FATAL_ERROR "The optional pinned FSR3 Vulkan adapter currently requires the Windows renderer.")
endif()
set(POIMA_FSR3_SOURCE_DIR "" CACHE PATH "Pinned FidelityFX-SDK v1.1.4 source checkout")
set(POIMA_FSR3_SHADER_DIR "" CACHE PATH "Verified shader cache from scripts/prepare_fsr3.py")
set(POIMA_FSR3_VULKAN_LIBRARY "" CACHE FILEPATH "Vulkan loader import library for the target toolchain")
if(NOT EXISTS "${POIMA_FSR3_SOURCE_DIR}/sdk/LICENSE.txt" OR NOT EXISTS "${POIMA_FSR3_SHADER_DIR}/manifest.json" OR NOT EXISTS "${POIMA_FSR3_VULKAN_LIBRARY}")
    message(FATAL_ERROR "FSR is opt-in: provide pinned source, prepared shader cache and target Vulkan import library; see scripts/prepare_fsr3.py. No SDK download is performed by CMake.")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare_fsr3.py"
    --source "${POIMA_FSR3_SOURCE_DIR}" --output "${POIMA_FSR3_SHADER_DIR}" --verify-only
    RESULT_VARIABLE fsr_verify_result OUTPUT_VARIABLE fsr_verify_output ERROR_VARIABLE fsr_verify_error)
if(NOT fsr_verify_result EQUAL 0)
    message(FATAL_ERROR "FSR dependency verification failed: ${fsr_verify_output} ${fsr_verify_error}")
endif()
set(fsr_sdk "${POIMA_FSR3_SOURCE_DIR}/sdk")
set(fsr_backend_source "${fsr_sdk}/src/backends/vk/ffx_vk.cpp")
set(fsr_backend_patched "${CMAKE_CURRENT_BINARY_DIR}/generated/fsr3/ffx_vk.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${fsr_backend_source}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/patch_fsr3_backend.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/patch_fsr3_backend.py"
    --source "${fsr_backend_source}" --output "${fsr_backend_patched}"
    RESULT_VARIABLE fsr_patch_result OUTPUT_VARIABLE fsr_patch_output ERROR_VARIABLE fsr_patch_error)
if(NOT fsr_patch_result EQUAL 0)
    message(FATAL_ERROR "FSR backend alignment patch failed: ${fsr_patch_output} ${fsr_patch_error}")
endif()

add_library(poima_fsr3_vendor STATIC
    ${fsr_sdk}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
    ${fsr_backend_patched}
    ${fsr_sdk}/src/backends/shared/ffx_shader_blobs.cpp
    ${fsr_sdk}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
    ${fsr_sdk}/src/shared/ffx_assert.cpp ${fsr_sdk}/src/shared/ffx_message.cpp
    ${fsr_sdk}/src/shared/ffx_breadcrumbs_list.cpp ${fsr_sdk}/src/shared/ffx_object_management.cpp
    src/fsr3_no_frame_generation.cpp)
target_compile_features(poima_fsr3_vendor PRIVATE cxx_std_17)
target_compile_definitions(poima_fsr3_vendor PRIVATE FFX_FSR3UPSCALER=1 FFX_API_VK=1 UNICODE _UNICODE)
target_include_directories(poima_fsr3_vendor SYSTEM PUBLIC "${fsr_sdk}/include" PRIVATE
    "${fsr_sdk}/src/components" "${fsr_sdk}/src/shared" "${fsr_sdk}/src/backends/shared" "${POIMA_FSR3_SHADER_DIR}")
if(MSVC)
    target_compile_options(poima_fsr3_vendor PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/cmake/fsr3_compat.hpp")
else()
    target_compile_options(poima_fsr3_vendor PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fsr3_compat.hpp")
endif()
target_link_libraries(poima_fsr3_vendor PUBLIC Vulkan::Headers "${POIMA_FSR3_VULKAN_LIBRARY}")
target_sources(poima_core PRIVATE src/fsr3.cpp)
target_link_libraries(poima_core PRIVATE poima_fsr3_vendor)
install(FILES "${fsr_sdk}/LICENSE.txt" DESTINATION share/poima/licenses/FidelityFX-SDK)
