# SPDX-License-Identifier: Apache-2.0
# Redistribution notices for SDL 3.4.16 and its bundled native dependencies.
# Keep these source files unchanged; SDL's root zlib license does not replace
# the BSD terms of its yuv2rgb and HIDAPI implementations.
function(poima_install_sdl_notices sdl_source_dir)
    set(notice_sources
        "LICENSE.txt"
        "src/video/yuv2rgb/LICENSE"
        "src/hidapi/LICENSE-bsd.txt"
        "src/hidapi/LICENSE.txt")
    set(notice_hashes
        "1c040b8271b37e5076359f8fd54240e371114112924d2df81ef87c7d6a1dfdfd"
        "f7d74004cc75778cae46c5f0139f244d67c07ef64cf51937e8272352197dcb18"
        "30eb1bef29b46f8ba7ab8b416035dbd93cb034a45481dd97815b944284582cd2"
        "7d3b087c34f35d4d538e3bcddd1ff8f66e92f9ef336881999482800ddf840913")
    foreach(index RANGE 0 3)
        list(GET notice_sources ${index} relative_path)
        list(GET notice_hashes ${index} expected_hash)
        set(source "${sdl_source_dir}/${relative_path}")
        if(NOT EXISTS "${source}" OR IS_DIRECTORY "${source}")
            message(FATAL_ERROR "SDL redistribution notice is missing: ${relative_path}")
        endif()
        file(SHA256 "${source}" actual_hash)
        if(NOT actual_hash STREQUAL expected_hash)
            message(FATAL_ERROR "SDL 3.4.16 redistribution notice changed: ${relative_path}")
        endif()
    endforeach()

    install(FILES "${sdl_source_dir}/LICENSE.txt"
        DESTINATION share/poima/licenses/SDL3)
    install(FILES "${sdl_source_dir}/src/video/yuv2rgb/LICENSE"
        DESTINATION share/poima/licenses/SDL3/yuv2rgb)
    install(FILES "${sdl_source_dir}/src/hidapi/LICENSE-bsd.txt"
        "${sdl_source_dir}/src/hidapi/LICENSE.txt"
        DESTINATION share/poima/licenses/SDL3/HIDAPI)

    set(selection "${CMAKE_CURRENT_BINARY_DIR}/generated/poima/sdl-notices/HIDAPI-LICENSE-SELECTION.txt")
    file(WRITE "${selection}" [=[Poima selects the BSD-style license for the HIDAPI implementation bundled
with SDL 3.4.16. The complete selected terms are in LICENSE-bsd.txt.
The unchanged upstream LICENSE.txt describes the available alternatives;
its inclusion does not select the GPL alternative.
]=])
    install(FILES "${selection}"
        DESTINATION share/poima/licenses/SDL3/HIDAPI)
endfunction()
