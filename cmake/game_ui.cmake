# SPDX-License-Identifier: Apache-2.0
# Included only when POIMA_ENABLE_GAME_UI is enabled. The dependency target is
# independent of a window, renderer, scripting runtime, or Poima's adapter.
include(FetchContent)

function(poima_add_game_ui_dependencies)
    # Scope generic upstream options so they cannot change other engine builds.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(SKIP_INSTALL_ALL TRUE)
    foreach(feature ZLIB BZIP2 PNG HARFBUZZ BROTLI)
        set(FT_DISABLE_${feature} TRUE)
    endforeach()
    # Disabling system zlib retains FreeType's bundled, licensed inflater.
    FetchContent_Declare(freetype
        URL https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz
        URL_HASH SHA256=36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        OVERRIDE_FIND_PACKAGE)
    FetchContent_MakeAvailable(freetype)
    if(NOT TARGET Freetype::Freetype)
        add_library(Freetype::Freetype ALIAS freetype)
    endif()
    set_property(DIRECTORY "${freetype_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)

    foreach(feature SAMPLES TESTS SHELL LUA_BINDINGS LOTTIE_PLUGIN SVG_PLUGIN
            HARFBUZZ_SAMPLE TRACY_PROFILING TRACY_MEMORY_PROFILING
            IME_SAMPLE_USE_NOTO_FONTS INSTALL_LICENSES_AND_BUILD_INFO
            INSTALL_RUNTIME_DEPENDENCIES INSTALL_DEPENDENCIES_DIR PRECOMPILED_HEADERS)
        set(RMLUI_${feature} OFF)
    endforeach()
    set(RMLUI_FONT_ENGINE freetype)
    set(RMLUI_THIRDPARTY_CONTAINERS ON)
    FetchContent_Declare(rmlui
        URL https://codeload.github.com/mikke89/RmlUi/tar.gz/ba95ffe8bfb6370efb2cdcca927eaad4710c5413
        URL_HASH SHA256=1541ef5577115e9368f8ed389b29f0925ef6572f326a33d378ea16c3cfa2cde8
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(rmlui)
    # Upstream 6.3 always declares a debugger target and installs development
    # files. Exclude its directory from default build/install; consumers link
    # Core only. No upstream backend, debugger, headers, or archives are shipped.
    set_property(DIRECTORY "${rmlui_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
    set_property(TARGET rmlui_debugger PROPERTY EXCLUDE_FROM_ALL TRUE)
    add_library(poima_game_ui INTERFACE)
    target_link_libraries(poima_game_ui INTERFACE RmlUi::Core)
    target_include_directories(poima_game_ui SYSTEM INTERFACE "${rmlui_SOURCE_DIR}/Include")

    # Keep redistribution notices with the engine even though upstream installs
    # are excluded. No sample assets or font downloads are needed at build time.
    install(DIRECTORY "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/game_ui/"
        DESTINATION share/poima/licenses/game_ui
        FILES_MATCHING PATTERN "*.txt" PATTERN "*.TXT" PATTERN "*.json")
    install(FILES "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/inter/Inter-Regular.ttf"
        DESTINATION share/poima/fonts)
    install(FILES "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/inter/LICENSE.txt"
        DESTINATION share/poima/licenses/Inter)
endfunction()

poima_add_game_ui_dependencies()
