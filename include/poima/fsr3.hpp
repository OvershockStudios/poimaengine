// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <memory>

namespace poima::fsr3 {
struct Extent { std::uint32_t width{}, height{}; };
struct Image { VkImage image{}; VkFormat format{}; Extent extent{}; };
struct Dispatch {
    VkCommandBuffer commands{};
    Image color, depth, motion, reactive, output;
    // Render-size shared outputs required by FSR 3.1.4, owned by the renderer.
    Image dilated_depth, dilated_motion, reconstructed_depth;
    std::array<float, 2> jitter_pixels{};
    float near_plane{0.1f}, far_plane{1000.f}, vertical_fov_radians{1.f};
    float delta_milliseconds{16.666667f};
    bool reset{};
};
// Diagnostic-only borrowed images from the exact pinned SDK. Order is masks,
// previous history, current history, luma instability. No copies or allocation.
// Masks RGBA8: detail-protection takedown, disocclusion, shading change,
// accumulation. History RGBA16F: internal scene RGB and lock in alpha.
// Luma instability is R16F. History is output-size; masks/luma are render-size.
struct DiagnosticImage {
    Image image{};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkPipelineStageFlags stages{};
    VkAccessFlags access{};
    std::uint32_t sdk_resource_index{};
    bool available{};
};
struct DiagnosticResources {
    std::array<DiagnosticImage,4> images{};
    std::uint64_t recorded_dispatches{}; // Not accepted GPU submissions.
    std::uint32_t sdk_effect_version{}, effect_context_id{}, next_resource_frame_index{};
    float accumulation_frame_index{};
    bool reset{}, previous_history_used{};
};
// Caller owns GPU ordering/lifetimes: one dispatch per accepted view submission,
// at most two outstanding. Drain before destruction/replacement. An abandoned
// recorded dispatch requires context recreation; SDK CPU history already advanced.
// Inputs enter/leave COMPUTE_READ; outputs enter/leave UAV. Reconcile external
// command recording with the host's resource-state and binding caches.
class Context {
public:
    Context(VkDevice device, VkPhysicalDevice physical_device,
            PFN_vkGetDeviceProcAddr device_proc, Extent render, Extent output);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    void dispatch(const Dispatch&);
    // Call after successful dispatch on the owner thread. Record diagnostic reads
    // before the next dispatch, on the same graphics queue. Restore EXACT layout
    // and stage/access scopes after transfer reads. Never retain past context
    // replacement/destruction. First-dispatch previous history is unavailable.
    DiagnosticResources diagnostic_resources() const;
    std::uint64_t gpu_bytes() const noexcept;
    std::uint64_t scratch_bytes() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// SDK sequence; index is accepted view submissions, not simulation ticks.
std::array<float,2> jitter(std::uint64_t index, Extent render, Extent output);
} // namespace poima::fsr3
