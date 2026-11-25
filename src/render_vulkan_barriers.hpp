// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/profiler.hpp"
#include "poima/render_schedule.hpp"

#include "render_vulkan_instance.hpp"

#include <array>

namespace poima::render_vulkan {

struct BarrierCounts {
    std::uint64_t calls = 0, memory = 0, buffers = 0, images = 0;
};

struct BarrierTrace {
    static constexpr auto pass_count = static_cast<std::size_t>(render_schedule::PassId::count);
    bool recorded = false;
    BarrierCounts before_passes, after_passes;
    std::array<BarrierCounts, pass_count> passes{};
};

// The static NVRHI backend and all live Contexts record on their common UI
// thread. Borrow their dispatch entries only during a profiled frame.
class BarrierRecording {
public:
    BarrierRecording(BarrierTrace&, VkCommandBuffer, vk::detail::DispatchLoaderDynamic&);
    ~BarrierRecording();
    BarrierRecording(const BarrierRecording&) = delete;
    BarrierRecording& operator=(const BarrierRecording&) = delete;

    void pass(render_schedule::PassId id) noexcept { current_ = &trace_.passes[static_cast<std::size_t>(id)]; }
    void finish_passes() noexcept { current_ = &trace_.after_passes; }

private:
    static VKAPI_ATTR void VKAPI_CALL sync2(VkCommandBuffer, const VkDependencyInfo*);
    static VKAPI_ATTR void VKAPI_CALL legacy(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
        VkDependencyFlags, std::uint32_t, const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*,
        std::uint32_t, const VkImageMemoryBarrier*);

    void count(VkCommandBuffer, std::uint32_t memory, std::uint32_t buffers, std::uint32_t images) noexcept;

    BarrierTrace& trace_;
    BarrierCounts* current_;
    VkCommandBuffer command_buffer_;
    vk::detail::DispatchLoaderDynamic& dispatch_;
    PFN_vkCmdPipelineBarrier2 original_sync2_;
    PFN_vkCmdPipelineBarrier original_legacy_;
};

void profile_barriers(const BarrierTrace&, const profiling::DeferredContext&) noexcept;

}
