// SPDX-License-Identifier: Apache-2.0
#include "render_vulkan_instance.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace poima::render_vulkan {
namespace {
void require(bool condition, const std::string& detail) {
    if (!condition) throw std::runtime_error(detail);
}
} // namespace

// NVRHI's pinned static backend uses Vulkan-Hpp's global dispatcher. Keep it
// loaded with INSTANCE-derived loader trampolines, never device-specific entry
// points: these dispatch correctly for every device descended from this shared
// instance. Each rendering context keeps its explicit device dispatcher.
// https://docs.vulkan.org/refpages/latest/refpages/source/vkGetInstanceProcAddr.html
// All windows/renderers remain on one owning UI thread; this is lifetime
// isolation for interleaved draws, not a cross-thread rendering API.
struct SharedInstance {
    vk::detail::DispatchLoaderDynamic dispatch;
    vk::Instance instance;
    PFN_vkGetInstanceProcAddr entry;
    std::vector<std::string> extensions;
    std::thread::id owner=std::this_thread::get_id();

    SharedInstance(PFN_vkGetInstanceProcAddr get,const char* const* names,std::uint32_t count):entry(get) {
        for(std::uint32_t i=0;i<count;++i)extensions.emplace_back(names[i]);
        dispatch.init(get);
        const auto available=vk::enumerateInstanceExtensionProperties(nullptr,dispatch);
        auto supports=[&](const char* name) {return std::any_of(available.begin(),available.end(),[&](const auto& e) {return std::string_view(e.extensionName.data())==name;});};
        if(supports(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
            extensions.emplace_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
            if(supports(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME))extensions.emplace_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
            if(supports(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME))extensions.emplace_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        }
        std::vector<const char*> enabled;for(const auto& extension:extensions)enabled.push_back(extension.c_str());
        require(dispatch.vkEnumerateInstanceVersion && vk::enumerateInstanceVersion(dispatch)>=VK_API_VERSION_1_3,
            "The renderer experiment needs a Vulkan 1.3 loader.");
        const vk::ApplicationInfo app("Poima",1,"Poima",1,VK_API_VERSION_1_3);
        vk::InstanceCreateInfo info;
        info.pApplicationInfo=&app;info.enabledExtensionCount=static_cast<std::uint32_t>(enabled.size());info.ppEnabledExtensionNames=enabled.data();
        instance=vk::createInstance(info,nullptr,dispatch);
        dispatch.init(instance);
        // NVRHI_BUILD_SHARED is forced OFF by render_smoke.cmake. Its static
        // backend does not reinitialize this dispatcher when creating a device.
        // Assignment occurs only when no previous Context/device remains alive.
        VULKAN_HPP_DEFAULT_DISPATCHER=dispatch;
    }
    ~SharedInstance() { if(instance)instance.destroy(nullptr,dispatch); }
};

std::shared_ptr<const SharedInstance> acquire_instance(PFN_vkGetInstanceProcAddr get,const char* const* names,std::uint32_t count) {
    static std::mutex mutex;
    static std::weak_ptr<SharedInstance> active;
    std::lock_guard lock(mutex);
    if(auto existing=active.lock()) {
        require(existing->owner==std::this_thread::get_id(),"Live graphics contexts require their common UI thread.");
        require(existing->entry==get && existing->extensions.size()>=count,"Live graphics contexts require the same Vulkan loader/platform.");
        for(std::uint32_t i=0;i<count;++i)
            require(existing->extensions[i]==names[i],"Live graphics contexts require matching Vulkan instance extensions.");
        return existing;
    }
    auto created=std::make_shared<SharedInstance>(get,names,count);active=created;return created;
}

vk::Instance instance_handle(const SharedInstance& owner) noexcept { return owner.instance; }

const vk::detail::DispatchLoaderDynamic& instance_dispatch(const SharedInstance& owner) noexcept {
    return owner.dispatch;
}

std::span<const std::string> instance_extensions(const SharedInstance& owner) noexcept {
    return owner.extensions;
}

} // namespace poima::render_vulkan
