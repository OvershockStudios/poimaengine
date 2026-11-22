// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "render_vulkan_instance.hpp"

#include <nvrhi/nvrhi.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace poima {
struct FrameExecutionDiagnostics;
struct RenderDiagnostics;
struct TimingSummary;
namespace frame_performance {
class GpuTicket;
class Recorder;
struct CpuSample;
}
namespace profiling { class DeferredContext; }
}

namespace poima::render_vulkan_frame {

using Dispatch = vk::detail::DispatchLoaderDynamic;

struct SwapchainPlan {
    vk::SwapchainCreateInfoKHR create_info;
    nvrhi::Format texture_format = nvrhi::Format::UNKNOWN;
};

std::optional<SwapchainPlan> plan_swapchain(vk::PhysicalDevice physical, vk::SurfaceKHR surface,
    const Dispatch& dispatch, vk::Extent2D requested, bool scene, bool capture_enabled);

// The renderer retires its frame slots, then calls retire before releasing
// graphics resources that wrap the swapchain images.
class Swapchain {
public:
    explicit operator bool() const noexcept { return bool(handle_); }
    void configure_present_fences(bool enabled, FrameExecutionDiagnostics& diagnostics);
    void create(vk::Device device, const Dispatch& dispatch, const SwapchainPlan& plan);
    std::vector<vk::Image> images(vk::Device device, const Dispatch& dispatch) const;
    void add_image(vk::Device device, const Dispatch& dispatch);
    void retire(vk::Device device, const Dispatch& dispatch, FrameExecutionDiagnostics& diagnostics);
    void clear_images(vk::Device device, const Dispatch& dispatch) noexcept;
    void destroy(vk::Device device, const Dispatch& dispatch) noexcept;

    vk::ResultValue<std::uint32_t> acquire(vk::Device device, const Dispatch& dispatch,
        std::uint64_t timeout, vk::Semaphore semaphore) const;
    void wait_for_image(vk::Device device, const Dispatch& dispatch, std::uint32_t index,
        frame_performance::CpuSample* performance);
    vk::Result present(vk::Queue queue, const Dispatch& dispatch, std::uint32_t index,
        frame_performance::CpuSample* performance);
    vk::Semaphore finished(std::uint32_t index) const noexcept { return images_[index].finished; }
    bool initialized(std::uint32_t index) const noexcept { return images_[index].initialized; }
    void mark_initialized(std::uint32_t index) noexcept { images_[index].initialized = true; }

private:
    struct Image {
        vk::Semaphore finished;
        vk::Fence fence;
        bool pending = false;
        bool initialized = false;
    };
    vk::SwapchainKHR handle_;
    std::vector<Image> images_;
    bool use_fences_ = false;
};

inline constexpr std::size_t slot_count = 2;

struct TimestampSample {
    bool recorded;
    bool deferred;
    bool ambient_occlusion;
    bool reconstruction;
    bool screen_reflections;
    bool height_fog;
    double cpu_interval_ms;
    const profiling::DeferredContext& profile;
    frame_performance::Recorder* performance;
    const frame_performance::GpuTicket& performance_ticket;
};

class Timestamps {
public:
    void prepare(vk::PhysicalDevice physical, vk::Device device, std::uint32_t queue_family,
        const Dispatch& dispatch, RenderDiagnostics& diagnostics);
    void begin(std::size_t slot, bool requested) noexcept;
    bool recording() const noexcept { return bool(recording_); }
    void reset_queries(vk::CommandBuffer commands, const Dispatch& dispatch) const;
    void write(vk::CommandBuffer commands, const Dispatch& dispatch, std::uint32_t query) const;
    void collect(vk::Device device, const Dispatch& dispatch, std::size_t slot,
        const TimestampSample& sample, RenderDiagnostics& diagnostics) const;
    void release(vk::Device device, const Dispatch& dispatch) noexcept;

private:
    std::array<vk::QueryPool, slot_count> pools_{};
    vk::QueryPool recording_;
    bool prepared_ = false;
};

void timing_sample(TimingSummary& value, double ms);

} // namespace poima::render_vulkan_frame
