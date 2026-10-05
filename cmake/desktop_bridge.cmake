# SPDX-License-Identifier: Apache-2.0
if(NOT WIN32)
    message(FATAL_ERROR "POIMA_BUILD_DESKTOP_BRIDGE currently supports native Windows HWND hosting only.")
endif()
if(NOT POIMA_BUILD_RENDER_SMOKE)
    message(FATAL_ERROR "The desktop bridge requires POIMA_BUILD_RENDER_SMOKE=ON.")
endif()
add_library(poima_desktop SHARED src/desktop_bridge.cpp)
target_compile_features(poima_desktop PRIVATE cxx_std_20)
set_target_properties(poima_desktop PROPERTIES CXX_EXTENSIONS OFF PREFIX "" OUTPUT_NAME "poima_desktop")
target_compile_definitions(poima_desktop PRIVATE POIMA_DESKTOP_EXPORTS WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(poima_desktop PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_include_directories(poima_desktop SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third_party")
target_link_libraries(poima_desktop PRIVATE poima_core user32)
if(MINGW)
    # Match the executable's portable C++ runtime policy for this shared target.
    target_link_options(poima_desktop PRIVATE -static)
endif()
if(MSVC)
    target_compile_options(poima_desktop PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(poima_desktop PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
if(POIMA_ENABLE_AUDIO)
    add_custom_command(TARGET poima_desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${POIMA_STEAM_AUDIO_ROOT}/lib/windows-x64/phonon.dll" "$<TARGET_FILE_DIR:poima_desktop>/phonon.dll")
endif()
install(TARGETS poima_desktop RUNTIME DESTINATION bin LIBRARY DESTINATION lib)
# The default installation is a redistributable runtime tree. Development
# headers are opt-in and belong in a separate SDK prefix, not game bundles.
install(FILES "${CMAKE_SOURCE_DIR}/include/poima/desktop_bridge.h"
    DESTINATION include/poima COMPONENT Development EXCLUDE_FROM_ALL)
