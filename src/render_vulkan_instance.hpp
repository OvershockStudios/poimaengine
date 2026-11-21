// SPDX-License-Identifier: Apache-2.0
#pragma once

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
// Every Poima Vulkan-Hpp call supplies its dispatcher.
#define VULKAN_HPP_NO_DEFAULT_DISPATCHER
#include <vulkan/vulkan.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace poima::render_vulkan {
struct SharedInstance;

// The shared owner keeps the instance alive while renderers use its immutable
// handle, extension list, and instance-level dispatcher.
std::shared_ptr<const SharedInstance> acquire_instance(
    PFN_vkGetInstanceProcAddr get, const char* const* names, std::uint32_t count);
vk::Instance instance_handle(const SharedInstance& owner) noexcept;
const vk::detail::DispatchLoaderDynamic& instance_dispatch(const SharedInstance& owner) noexcept;
std::span<const std::string> instance_extensions(const SharedInstance& owner) noexcept;
} // namespace poima::render_vulkan
