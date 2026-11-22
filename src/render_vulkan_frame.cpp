// SPDX-License-Identifier: Apache-2.0
#include "render_vulkan_frame.hpp"

#include "frame_wall_scope.hpp"
#include "poima/core.hpp"
#include "poima/frame_performance.hpp"
#include "poima/profiler.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace poima::render_vulkan_frame {
namespace fp = frame_performance;
namespace {

constexpr std::uint64_t retirement_timeout_ns = 5'000'000'000ULL;
constexpr std::uint32_t timestamp_count = 18;

void require(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}

} // namespace

std::optional<SwapchainPlan> plan_swapchain(vk::PhysicalDevice physical, vk::SurfaceKHR surface,
    const Dispatch& dispatch, vk::Extent2D requested, bool scene, bool capture_enabled) {
    const auto caps = physical.getSurfaceCapabilitiesKHR(surface,dispatch);
    // Native minimize/restore can race SDL's queued size notification.
    // Zero surface extent is a suspended window, not a device failure.
    if(caps.currentExtent.width==0 || caps.currentExtent.height==0) return std::nullopt;
    const auto formats = physical.getSurfaceFormatsKHR(surface,dispatch);
    require(!formats.empty(), "No Vulkan surface formats are available.");
    vk::SurfaceFormatKHR selected;
    SwapchainPlan plan;
    // Prefer sRGB attachments for scene previews: lighting and MSAA resolve
    // operate in linear light. Preserve the triangle fixture's old format.
    const std::array preferred_formats = scene
        ? std::array{vk::Format::eB8G8R8A8Srgb, vk::Format::eR8G8B8A8Srgb, vk::Format::eB8G8R8A8Unorm, vk::Format::eR8G8B8A8Unorm}
        : std::array{vk::Format::eB8G8R8A8Unorm, vk::Format::eR8G8B8A8Unorm, vk::Format::eB8G8R8A8Srgb, vk::Format::eR8G8B8A8Srgb};
    for (const auto preferred : preferred_formats) {
        const auto found = std::find_if(formats.begin(), formats.end(), [preferred](const auto& item) {
            return item.format == preferred && item.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
        });
        if (found == formats.end()) continue;
        selected = *found;
        if (preferred == vk::Format::eB8G8R8A8Unorm) plan.texture_format = nvrhi::Format::BGRA8_UNORM;
        if (preferred == vk::Format::eR8G8B8A8Unorm) plan.texture_format = nvrhi::Format::RGBA8_UNORM;
        if (preferred == vk::Format::eB8G8R8A8Srgb) plan.texture_format = nvrhi::Format::SBGRA8_UNORM;
        if (preferred == vk::Format::eR8G8B8A8Srgb) plan.texture_format = nvrhi::Format::SRGBA8_UNORM;
        break;
    }
    require(plan.texture_format != nvrhi::Format::UNKNOWN, "No supported 8-bit RGBA/BGRA surface format.");
    const auto required_usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
        (capture_enabled ? vk::ImageUsageFlagBits::eTransferSrc : vk::ImageUsageFlags{});
    require((caps.supportedUsageFlags & required_usage) == required_usage, "Surface lacks required render/capture image usage.");
    auto extent = caps.currentExtent;
    if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
        extent.width = std::clamp(requested.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(requested.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    require(extent.width > 0 && extent.height > 0, "The window has an empty rendering extent.");
    std::uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) count = std::min(count, caps.maxImageCount);
    vk::CompositeAlphaFlagBitsKHR alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    for (const auto candidate : {vk::CompositeAlphaFlagBitsKHR::eOpaque, vk::CompositeAlphaFlagBitsKHR::ePreMultiplied,
                                vk::CompositeAlphaFlagBitsKHR::ePostMultiplied, vk::CompositeAlphaFlagBitsKHR::eInherit}) {
        if (caps.supportedCompositeAlpha & candidate) { alpha = candidate; break; }
    }
    auto& info = plan.create_info;
    info.surface = surface;
    info.minImageCount = count;
    info.imageFormat = selected.format;
    info.imageColorSpace = selected.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = required_usage;
    info.imageSharingMode = vk::SharingMode::eExclusive;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = vk::PresentModeKHR::eFifo;
    info.clipped = true;
    return plan;
}

void Swapchain::configure_present_fences(bool enabled, FrameExecutionDiagnostics& diagnostics) {
    use_fences_ = enabled;
    diagnostics.presentation_fences = enabled;
    if(enabled) diagnostics.presentation_retirement = "maintenance1 present fences";
}

void Swapchain::create(vk::Device device, const Dispatch& dispatch, const SwapchainPlan& plan) {
    handle_ = device.createSwapchainKHR(plan.create_info,nullptr,dispatch);
}

std::vector<vk::Image> Swapchain::images(vk::Device device, const Dispatch& dispatch) const {
    return device.getSwapchainImagesKHR(handle_,dispatch);
}

void Swapchain::add_image(vk::Device device, const Dispatch& dispatch) {
    auto& image = images_.emplace_back();
    image.finished = device.createSemaphore({},nullptr,dispatch);
    if(use_fences_) image.fence = device.createFence({},nullptr,dispatch);
}

void Swapchain::retire(vk::Device device, const Dispatch& dispatch, FrameExecutionDiagnostics& diagnostics) {
    for(auto& image : images_) if(image.pending) {
        require(device.waitForFences(1,&image.fence,true,retirement_timeout_ns,dispatch)==vk::Result::eSuccess,
            "Presentation retirement timed out; recreate the renderer session.");
        image.pending = false;
    }
    if(!use_fences_) {
        // This conventional idle cleanup is not formal presentation completion.
        ++diagnostics.device_idle_waits;
        device.waitIdle(dispatch);
    }
}

void Swapchain::clear_images(vk::Device device, const Dispatch& dispatch) noexcept {
    for(auto& image : images_) {
        if(image.fence) device.destroyFence(image.fence,nullptr,dispatch);
        if(image.finished) device.destroySemaphore(image.finished,nullptr,dispatch);
    }
    images_.clear();
}

void Swapchain::destroy(vk::Device device, const Dispatch& dispatch) noexcept {
    if(handle_) device.destroySwapchainKHR(handle_,nullptr,dispatch);
    handle_ = nullptr;
}

vk::ResultValue<std::uint32_t> Swapchain::acquire(vk::Device device, const Dispatch& dispatch,
    std::uint64_t timeout, vk::Semaphore semaphore) const {
    return device.acquireNextImageKHR(handle_,timeout,semaphore,{},dispatch);
}

void Swapchain::wait_for_image(vk::Device device, const Dispatch& dispatch, std::uint32_t index,
    fp::CpuSample* performance) {
    auto& image = images_[index];
    if(!image.pending) return;
    PerformanceWallScope wall(performance ? &performance->work.gpu_wait_ns : nullptr);
    require(device.waitForFences(1,&image.fence,true,retirement_timeout_ns,dispatch)==vk::Result::eSuccess,
        "Previous image presentation did not retire.");
    require(device.resetFences(1,&image.fence,dispatch)==vk::Result::eSuccess,"Presentation fence reset failed.");
    image.pending = false;
}

vk::Result Swapchain::present(vk::Queue queue, const Dispatch& dispatch, std::uint32_t index,
    fp::CpuSample* performance) {
    auto& image = images_[index];
    vk::SwapchainPresentFenceInfoKHR fence_info;
    vk::PresentInfoKHR present_info;
    if(use_fences_) {
        fence_info.swapchainCount = 1;
        fence_info.pFences = &image.fence;
        present_info.pNext = &fence_info;
    }
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &image.finished;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &handle_;
    present_info.pImageIndices = &index;
    vk::Result presented;
    {
        profiling::Scope present_scope("render.present");
        PerformanceWallScope wall(performance ? &performance->work.present_ns : nullptr);
        presented = static_cast<vk::Result>(dispatch.vkQueuePresentKHR(static_cast<VkQueue>(queue),
            reinterpret_cast<const VkPresentInfoKHR*>(&present_info)));
        if(performance) {
            performance->present_return_ns = fp::Recorder::now_ns();
            performance->successful_present = presented==vk::Result::eSuccess || presented==vk::Result::eSuboptimalKHR;
        }
    }
    // OUT_OF_DATE still enqueues the present semaphore wait and fence.
    if(use_fences_ && (presented==vk::Result::eSuccess || presented==vk::Result::eSuboptimalKHR ||
        presented==vk::Result::eErrorOutOfDateKHR || presented==vk::Result::eErrorSurfaceLostKHR ||
        presented==vk::Result::eErrorFullScreenExclusiveModeLostEXT)) image.pending = true;
    return presented;
}

void timing_sample(TimingSummary& value,double ms) {
    if(value.samples==0)value.min_ms=value.max_ms=ms;
    else { value.min_ms=std::min(value.min_ms,ms);value.max_ms=std::max(value.max_ms,ms); }
    ++value.samples;value.total_ms+=ms;value.last_ms=ms;
}

void Timestamps::prepare(vk::PhysicalDevice physical, vk::Device device, std::uint32_t queue_family,
    const Dispatch& dispatch, RenderDiagnostics& diagnostics) {
    if(prepared_) return;
    prepared_ = true;
    const auto limits = physical.getProperties(dispatch).limits;
    diagnostics.timestamp_period_ns = limits.timestampPeriod;
    diagnostics.timestamp_valid_bits = physical.getQueueFamilyProperties(dispatch).at(queue_family).timestampValidBits;
    if(!(limits.timestampPeriod>0) || diagnostics.timestamp_valid_bits==0) {
        diagnostics.gpu_timing_detail = "Selected graphics queue does not support timestamps.";
        return;
    }
    for(auto& pool : pools_)
        pool = device.createQueryPool(vk::QueryPoolCreateInfo({},vk::QueryType::eTimestamp,timestamp_count),nullptr,dispatch);
    diagnostics.gpu_timestamps = true;
    diagnostics.gpu_timing_detail = "64-bit graphics-queue timestamps; approximate pass intervals, not presentation latency or game frame time.";
}

void Timestamps::begin(std::size_t slot, bool requested) noexcept {
    recording_ = requested ? pools_[slot] : vk::QueryPool{};
}

void Timestamps::reset_queries(vk::CommandBuffer commands, const Dispatch& dispatch) const {
    commands.resetQueryPool(recording_,0,timestamp_count,dispatch);
}

void Timestamps::write(vk::CommandBuffer commands, const Dispatch& dispatch, std::uint32_t query) const {
    commands.writeTimestamp(query==0 ? vk::PipelineStageFlagBits::eTopOfPipe : vk::PipelineStageFlagBits::eBottomOfPipe,
        recording_,query,dispatch);
}

void Timestamps::collect(vk::Device device, const Dispatch& dispatch, std::size_t slot,
    const TimestampSample& frame, RenderDiagnostics& diagnostics) const {
    if(!frame.recorded) {
        if(frame.performance)frame.performance->gpu_drop(frame.performance_ticket,fp::GpuDropReason::query_unavailable);
        return;
    }
    std::array<std::uint64_t,timestamp_count> values{};
    const auto status=device.getQueryPoolResults(pools_[slot],0,static_cast<std::uint32_t>(values.size()),sizeof(values),values.data(),sizeof(values[0]),vk::QueryResultFlagBits::e64,dispatch);
    const auto bits=diagnostics.timestamp_valid_bits;
    const double wrap_ms=std::ldexp(diagnostics.timestamp_period_ns*1e-6,static_cast<int>(bits));
    if(status!=vk::Result::eSuccess || frame.cpu_interval_ms>=wrap_ms) {
        ++diagnostics.gpu_samples_dropped;profiling::deferred_counter(frame.profile,"gpu.samples_dropped",diagnostics.gpu_samples_dropped);
        if(frame.performance)frame.performance->gpu_drop(frame.performance_ticket,fp::GpuDropReason::query_failed);
        return;
    }
    const auto mask=bits==64 ? ~std::uint64_t(0) : (std::uint64_t(1)<<bits)-1;
    auto ms=[&](std::size_t a,std::size_t b) { return static_cast<double>((values[b]-values[a])&mask)*diagnostics.timestamp_period_ns*1e-6; };
    if(diagnostics.profile_requested) {
        timing_sample(diagnostics.skinning_gpu,ms(0,1));timing_sample(diagnostics.light_assignment_gpu,ms(1,2));
        timing_sample(diagnostics.shadow_gpu,ms(2,3));timing_sample(diagnostics.opaque_gpu,ms(3,4));
        if(frame.deferred)timing_sample(diagnostics.deferred_lighting_gpu,ms(8,9));
        if(frame.ambient_occlusion) {timing_sample(diagnostics.ambient_occlusion_gpu,ms(10,11));timing_sample(diagnostics.ambient_occlusion_filter_gpu,ms(12,13));}
        if(frame.reconstruction)timing_sample(diagnostics.reconstruction.gpu,ms(6,7));
        if(frame.screen_reflections)timing_sample(diagnostics.screen_reflections_gpu,ms(14,15));
        if(frame.height_fog)timing_sample(diagnostics.height_fog_gpu,ms(16,17));
        timing_sample(diagnostics.post_gpu,ms(4,5));timing_sample(diagnostics.total_gpu,ms(0,5));
    }
    // GPU values are queue durations observed after completion, not CPU
    // timeline timestamps. Never fabricate cross-clock synchronization.
    auto sample=[&](std::string_view name,std::size_t a,std::size_t b) {
        const long double ns=static_cast<long double>((values[b]-values[a])&mask)*diagnostics.timestamp_period_ns;
        if(std::isfinite(ns) && ns>=0 && ns<static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
            profiling::deferred_counter(frame.profile,name,static_cast<std::uint64_t>(ns),profiling::Kind::gpu);
    };
    sample("gpu.skinning.ns",0,1);sample("gpu.light_assignment.ns",1,2);sample("gpu.shadows.ns",2,3);sample("gpu.opaque.ns",3,4);
    if(frame.deferred)sample("gpu.deferred_lighting.ns",8,9);
    if(frame.ambient_occlusion) {sample("gpu.ambient_occlusion.ns",10,11);sample("gpu.ambient_occlusion_filter.ns",12,13);}
    if(frame.reconstruction)sample("gpu.reconstruction.ns",6,7);
    if(frame.screen_reflections)sample("gpu.screen_reflections.ns",14,15);
    if(frame.height_fog)sample("gpu.height_fog.ns",16,17);
    sample("gpu.post.ns",4,5);sample("gpu.total.ns",0,5);
    if(frame.performance) {
        fp::GpuTiming timing;bool valid=true;
        auto duration=[&](std::size_t a,std::size_t b) {
            const long double ns=static_cast<long double>((values[b]-values[a])&mask)*diagnostics.timestamp_period_ns;
            if(!std::isfinite(ns) || ns<0 || ns>=static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {valid=false;return std::uint64_t{0};}
            return static_cast<std::uint64_t>(ns);
        };
        timing.total_ns=duration(0,5);timing.skinning_ns=duration(0,1);timing.light_assignment_ns=duration(1,2);
        timing.shadows_ns=duration(2,3);timing.opaque_ns=duration(3,4);timing.post_ns=duration(4,5);
        timing.pass_mask=fp::GpuPass::total|fp::GpuPass::skinning|fp::GpuPass::light_assignment|fp::GpuPass::shadows|fp::GpuPass::opaque|fp::GpuPass::post;
        if(frame.deferred) {timing.deferred_lighting_ns=duration(8,9);timing.pass_mask|=fp::GpuPass::deferred_lighting;}
        if(frame.ambient_occlusion) {timing.ambient_occlusion_ns=duration(10,11);timing.ambient_occlusion_filter_ns=duration(12,13);timing.pass_mask|=fp::GpuPass::ambient_occlusion|fp::GpuPass::ambient_occlusion_filter;}
        if(frame.reconstruction) {timing.reconstruction_ns=duration(6,7);timing.pass_mask|=fp::GpuPass::reconstruction;}
        if(valid)frame.performance->gpu_complete(frame.performance_ticket,timing);
        else frame.performance->gpu_drop(frame.performance_ticket,fp::GpuDropReason::invalid_timing);
    }
}

void Timestamps::release(vk::Device device, const Dispatch& dispatch) noexcept {
    for(auto& pool : pools_) if(pool) {
        device.destroyQueryPool(pool,nullptr,dispatch);
        pool = nullptr;
    }
    recording_ = nullptr;
    prepared_ = false;
}

} // namespace poima::render_vulkan_frame
