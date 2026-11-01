# SPDX-License-Identifier: Apache-2.0
# Narrow extension of pinned MIT Jolt sources. Adds separate typed quiescent
# owner metadata; original StateRecorder bytes and existing object layouts stay
# unchanged. No patch executable is required on Windows or Linux.
# All original/current hashes are checked before any source file is changed.
function(poima_jolt_owner_prepare_one path original_sha patched_sha kind output changed)
    file(SHA256 "${path}" observed)
    if(observed STREQUAL patched_sha)
        set(${changed} FALSE PARENT_SCOPE)
        return() # Keep mtime and incremental compilation stable.
    endif()
    if(NOT observed STREQUAL original_sha)
        message(FATAL_ERROR "Unsupported Jolt owner-state source: ${path}")
    endif()
    set(fragment_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/jolt_owner_state")
    file(READ "${path}" content)
    if(kind STREQUAL "body_header")
        file(READ "${fragment_root}/owner_state_type.inc" owner_type)
        file(READ "${fragment_root}/owner_state_public.inc" owner_public)
        string(REPLACE "/// Class that contains all bodies\n"
            "${owner_type}/// Class that contains all bodies\n" content "${content}")
        string(REPLACE "\t/// Saving state for replay\n"
            "${owner_public}\t/// Saving state for replay\n" content "${content}")
        string(REPLACE "private:\n\t/// Increment and get the sequence number of the body\n"
            "private:\n\tbool ValidatePoimaOwnerStateLocked(const PoimaBodyOwnerState &inState, Array<uint8> &active_seen, Array<uint8> &flagged_seen, Array<uint8> &queued_live_seen) const;\n\n\t/// Increment and get the sequence number of the body\n"
            content "${content}")
    elseif(kind STREQUAL "body_source")
        file(READ "${fragment_root}/owner_state_methods.inc" owner_methods)
        string(REPLACE "#include <Jolt/Core/QuickSort.h>"
            "#include <Jolt/Core/QuickSort.h>\n#include <Jolt/Core/ScopeExit.h>" content "${content}")
        string(REPLACE "JPH_NAMESPACE_END" "${owner_methods}JPH_NAMESPACE_END" content "${content}")
    elseif(kind STREQUAL "physics_header")
        file(READ "${fragment_root}/owner_state_forward.inc" owner_forward)
        string(REPLACE "\t/// Gets the current amount of bodies that are in the body manager\n"
            "${owner_forward}\t/// Gets the current amount of bodies that are in the body manager\n" content "${content}")
    else()
        message(FATAL_ERROR "Unknown Jolt owner-state patch kind")
    endif()
    string(SHA256 actual_patched "${content}")
    if(NOT actual_patched STREQUAL patched_sha)
        message(FATAL_ERROR "Jolt owner-state patch did not produce reviewed digest: ${path}")
    endif()
    set(${output} "${content}" PARENT_SCOPE)
    set(${changed} TRUE PARENT_SCOPE)
endfunction()

function(poima_patch_jolt_owner_state source_directory)
    set(fragment_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/jolt_owner_state")
    set(fragment_names owner_state_type.inc owner_state_public.inc owner_state_forward.inc owner_state_methods.inc)
    set(fragment_hashes
        "79cbd69c72fd679e96e242ed7e026dfe220cf09b27be4f867e54fd5023eb3431"
        "7d6912ab99c9c37b546201d4d564b1565249113d2ea14f5e716d9b82e95b7756"
        "3c1626f830db5755cf28e0f0e26fb4517ab5193e3ee7fd09eac687e9a4bc70de"
        "f6214de1da11dedde28e6ba9309ca1f4bfd4545f3608c4707839d68eebba81b0")
    # Validate templates even for an already patched cache. Both clean and
    # incremental builds must accept exactly the same reviewed extension.
    foreach(index RANGE 0 3)
        list(GET fragment_names ${index} fragment)
        list(GET fragment_hashes ${index} expected)
        set(path "${fragment_root}/${fragment}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
        file(SHA256 "${path}" observed)
        if(NOT observed STREQUAL expected)
            message(FATAL_ERROR "Unreviewed Jolt owner-state fragment; no sources changed: ${fragment}")
        endif()
    endforeach()
    set(paths "Jolt/Physics/Body/BodyManager.h" "Jolt/Physics/Body/BodyManager.cpp" "Jolt/Physics/PhysicsSystem.h")
    set(originals
        "151d8d31a120137c4696a81bccbeff832e40b9b53e24edd802e29c207a6c31e6"
        "d52f17bc72f96dbaaef5188758021d8feb83b47eb803e6b42340e3046c570dd0"
        "54e188496125baa8f802e36374f24cbdb125c28babe931f4c24d335b140a9671")
    set(patched
        "9b48e53c3e4262b3da83a99e68b4e3719a473e07af3cd0d3bf7521476a5dca7b"
        "9759a588fc4e7e5af7bfd3967e051941a649437d3da54b0888c0498856259bf0"
        "75bc2080736dc48467b3f2b04655ba92bc1cc72236d5dfa73e9caa80010f8df9")
    set(kinds "body_header" "body_source" "physics_header")
    # Prepare and hash-check EVERY output before writing even the first file.
    foreach(index RANGE 0 2)
        list(GET paths ${index} relative)
        list(GET originals ${index} before)
        list(GET patched ${index} after)
        file(SHA256 "${source_directory}/${relative}" observed)
        if(NOT observed STREQUAL before AND NOT observed STREQUAL after)
            message(FATAL_ERROR "Unsupported Jolt owner-state source; no files changed: ${relative}")
        endif()
    endforeach()
    foreach(index RANGE 0 2)
        list(GET paths ${index} relative)
        list(GET originals ${index} before)
        list(GET patched ${index} after)
        list(GET kinds ${index} kind)
        poima_jolt_owner_prepare_one("${source_directory}/${relative}" "${before}" "${after}" "${kind}"
            prepared_${index} changed_${index})
    endforeach()
    foreach(index RANGE 0 2)
        if(changed_${index})
            list(GET paths ${index} relative)
            file(WRITE "${source_directory}/${relative}" "${prepared_${index}}")
        endif()
    endforeach()
endfunction()
