# SPDX-License-Identifier: Apache-2.0

# Build the pinned source with one synchronization2 call per state commit.
# Keep other users of the fetched or externally supplied source tree intact.
function(poima_batch_nvrhi_vulkan_barriers)
    set(relative "src/vulkan/vulkan-state-tracking.cpp")
    set(original "${nvrhi_SOURCE_DIR}/${relative}")
    file(SHA256 "${original}" digest)
    if(NOT digest STREQUAL "943cf750732b276566c34128e0f74ae9a4fe3e41dd7ab5aafdb5615af2459975")
        message(FATAL_ERROR "Review the Vulkan barrier adaptation against the new NVRHI source.")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    file(READ "${original}" source)

    string(REPLACE [=[
        if (!imageBarriers.empty())
        {
            vk::DependencyInfo dep_info;
            dep_info.setImageMemoryBarriers(imageBarriers);

            m_CurrentCmdBuf->cmdBuf.pipelineBarrier2(dep_info);
        }

        imageBarriers.clear();

]=] "" source "${source}")
    string(REPLACE [=[
        if (!bufferBarriers.empty())
        {
            vk::DependencyInfo dep_info;
            dep_info.setBufferMemoryBarriers(bufferBarriers);

            m_CurrentCmdBuf->cmdBuf.pipelineBarrier2(dep_info);
        }
        bufferBarriers.clear();
]=] [=[
        if (!imageBarriers.empty() || !bufferBarriers.empty())
        {
            vk::DependencyInfo dep_info;
            dep_info.setImageMemoryBarriers(imageBarriers);
            dep_info.setBufferMemoryBarriers(bufferBarriers);

            m_CurrentCmdBuf->cmdBuf.pipelineBarrier2(dep_info);
        }
]=] source "${source}")

    get_target_property(sources nvrhi_vk SOURCES)
    if(NOT relative IN_LIST sources)
        message(FATAL_ERROR "The pinned NVRHI target is missing its state tracking source.")
    endif()
    list(REMOVE_ITEM sources "${relative}")
    set(adapted "${CMAKE_CURRENT_BINARY_DIR}/generated/nvrhi/vulkan-state-tracking.cpp")
    file(CONFIGURE OUTPUT "${adapted}" CONTENT "${source}" @ONLY NEWLINE_STYLE UNIX)
    set_source_files_properties("${adapted}" TARGET_DIRECTORY nvrhi_vk PROPERTIES
        INCLUDE_DIRECTORIES "${nvrhi_SOURCE_DIR}/src/vulkan")
    set_property(TARGET nvrhi_vk PROPERTY SOURCES "${sources};${adapted}")
endfunction()
