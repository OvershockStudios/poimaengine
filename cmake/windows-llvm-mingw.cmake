# SPDX-License-Identifier: Apache-2.0
# Linux/WSL host -> native x64 Windows executable. Tools stay in the workspace.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
get_filename_component(POIMA_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(POIMA_LLVM_MINGW "${POIMA_SOURCE_ROOT}/.cache/toolchains/llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64"
    CACHE PATH "Pinned portable LLVM-MinGW toolchain")
set(CMAKE_C_COMPILER "${POIMA_LLVM_MINGW}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${POIMA_LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER "${POIMA_LLVM_MINGW}/bin/x86_64-w64-mingw32-windres")
set(CMAKE_FIND_ROOT_PATH "${POIMA_LLVM_MINGW}/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Keep the initial CLI standalone; no toolchain runtime DLLs need copying.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
