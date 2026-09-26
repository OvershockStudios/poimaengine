cmake_minimum_required(VERSION 3.24)
project(PoimaNativeFixture LANGUAGES CXX)
set(POIMA_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../include" CACHE PATH "Poima public headers")
set(POIMA_FIXTURE_SCHEMA 1 CACHE STRING "Fixture layout: 1 or 2")
set(POIMA_FIXTURE_SCALE 1 CACHE STRING "Fixture movement multiplier")
option(POIMA_FIXTURE_BAD_ABI "Deliberately incompatible fixture" OFF)
option(POIMA_FIXTURE_REJECT_MIGRATION "Deliberately fail after writing staging" OFF)
option(POIMA_FIXTURE_FAIL_STEP "Deliberately fail after writing an update" OFF)
add_library(poima_game_fixture SHARED fixture.cpp)
target_compile_features(poima_game_fixture PRIVATE cxx_std_20)
target_include_directories(poima_game_fixture PRIVATE "${POIMA_INCLUDE_DIR}")
target_compile_definitions(poima_game_fixture PRIVATE
    POIMA_FIXTURE_SCHEMA=${POIMA_FIXTURE_SCHEMA}
    POIMA_FIXTURE_SCALE=${POIMA_FIXTURE_SCALE}
    POIMA_FIXTURE_BAD_ABI=$<BOOL:${POIMA_FIXTURE_BAD_ABI}>
    POIMA_FIXTURE_REJECT_MIGRATION=$<BOOL:${POIMA_FIXTURE_REJECT_MIGRATION}>
    POIMA_FIXTURE_FAIL_STEP=$<BOOL:${POIMA_FIXTURE_FAIL_STEP}>)
set_target_properties(poima_game_fixture PROPERTIES CXX_EXTENSIONS OFF CXX_VISIBILITY_PRESET hidden)
if(MINGW)
    target_link_options(poima_game_fixture PRIVATE -static-libgcc -static-libstdc++)
endif()
