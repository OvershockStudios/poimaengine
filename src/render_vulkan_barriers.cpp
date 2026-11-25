// SPDX-License-Identifier: Apache-2.0
#include "render_vulkan_barriers.hpp"

#include <cstring>
#include <stdexcept>
#include <string_view>

namespace poima::render_vulkan {
namespace {
thread_local BarrierRecording* recording = nullptr;
}

BarrierRecording::BarrierRecording(BarrierTrace& trace, VkCommandBuffer buffer,
    vk::detail::DispatchLoaderDynamic& dispatch)
    : trace_(trace), current_(&trace.before_passes), command_buffer_(buffer), dispatch_(dispatch),
      original_sync2_(VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2),
      original_legacy_(dispatch.vkCmdPipelineBarrier) {
    if (recording || !buffer || !original_sync2_ || !original_legacy_)
        throw std::logic_error("Cannot observe barriers for this Vulkan recording.");
    trace_ = {};
    trace_.recorded = true;
    recording = this;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2 = sync2;
    dispatch_.vkCmdPipelineBarrier = legacy;
}

BarrierRecording::~BarrierRecording() {
    dispatch_.vkCmdPipelineBarrier = original_legacy_;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2 = original_sync2_;
    recording = nullptr;
}

VKAPI_ATTR void VKAPI_CALL BarrierRecording::sync2(VkCommandBuffer buffer, const VkDependencyInfo* dependency) {
    recording->count(buffer, dependency->memoryBarrierCount, dependency->bufferMemoryBarrierCount,
        dependency->imageMemoryBarrierCount);
    recording->original_sync2_(buffer, dependency);
}

VKAPI_ATTR void VKAPI_CALL BarrierRecording::legacy(VkCommandBuffer buffer,
    VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags,
    std::uint32_t memory_count, const VkMemoryBarrier* memory,
    std::uint32_t buffer_count, const VkBufferMemoryBarrier* buffers,
    std::uint32_t image_count, const VkImageMemoryBarrier* images) {
    recording->count(buffer, memory_count, buffer_count, image_count);
    recording->original_legacy_(buffer, src, dst, flags, memory_count, memory, buffer_count, buffers, image_count, images);
}

void BarrierRecording::count(VkCommandBuffer buffer, std::uint32_t memory, std::uint32_t buffers,
    std::uint32_t images) noexcept {
    if (buffer != command_buffer_) return;
    ++current_->calls;
    current_->memory += memory;
    current_->buffers += buffers;
    current_->images += images;
}

void profile_barriers(const BarrierTrace& trace, const profiling::DeferredContext& ticket) noexcept {
    if (!trace.recorded) return;
    BarrierCounts total;
    auto emit = [&](std::string_view phase, const BarrierCounts& counts) {
        if (!counts.calls) return;
        constexpr std::string_view prefix = "renderer.barriers.";
        std::array<char, 64> event_name{};
        const auto length = prefix.size() + phase.size();
        if (length >= event_name.size()) return;
        std::memcpy(event_name.data(), prefix.data(), prefix.size());
        std::memcpy(event_name.data() + prefix.size(), phase.data(), phase.size());
        profiling::deferred_counter(ticket, std::string_view(event_name.data(), length), counts.calls);
        total.calls += counts.calls;
        total.memory += counts.memory;
        total.buffers += counts.buffers;
        total.images += counts.images;
    };
    emit("before_passes", trace.before_passes);
    constexpr std::string_view pass_prefix = "render.pass.";
    for (std::size_t i = 0; i < trace.passes.size(); ++i) {
        const auto& counts = trace.passes[i];
        if (!counts.calls) continue;
        const std::string_view name = render_schedule::pass_name(static_cast<render_schedule::PassId>(i));
        emit(name.substr(pass_prefix.size()), counts);
    }
    emit("after_passes", trace.after_passes);
    profiling::deferred_counter(ticket, "renderer.barriers.calls", total.calls);
    profiling::deferred_counter(ticket, "renderer.barriers.memory_entries", total.memory);
    profiling::deferred_counter(ticket, "renderer.barriers.buffer_entries", total.buffers);
    profiling::deferred_counter(ticket, "renderer.barriers.image_entries", total.images);
}

}
