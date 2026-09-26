// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"

#include <utility>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace poima {
namespace {
class Loader {
public:
#ifdef _WIN32
    Loader() : handle_(LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {}
    ~Loader() { if (handle_) FreeLibrary(handle_); }
    PFN_vkGetInstanceProcAddr get_entry() const {
        return handle_ ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(handle_, "vkGetInstanceProcAddr")) : nullptr;
    }
private:
    HMODULE handle_;
#else
    Loader() : handle_(dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL)) {}
    ~Loader() { if (handle_) dlclose(handle_); }
    PFN_vkGetInstanceProcAddr get_entry() const {
        return handle_ ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(handle_, "vkGetInstanceProcAddr")) : nullptr;
    }
private:
    void* handle_;
#endif
public:
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;
};

struct Instance {
    VkInstance value = VK_NULL_HANDLE;
    PFN_vkDestroyInstance destroy = nullptr;
    ~Instance() { if (value && destroy) destroy(value, nullptr); }
};

std::string api_version(std::uint32_t value) {
    return std::to_string(VK_API_VERSION_MAJOR(value)) + "." +
        std::to_string(VK_API_VERSION_MINOR(value)) + "." +
        std::to_string(VK_API_VERSION_PATCH(value));
}

std::string device_type(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete_gpu";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated_gpu";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual_gpu";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}
} // namespace

GraphicsProbe inspect_vulkan() {
    GraphicsProbe probe{"unavailable", "", "", {}};
    Loader loader;
    const auto get = loader.get_entry();
    if (!get) {
        probe.detail = "The system Vulkan loader or its entry point is unavailable.";
        return probe;
    }
    std::uint32_t loader_version = VK_API_VERSION_1_0;
    const auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        get(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (enumerate_version && enumerate_version(&loader_version) != VK_SUCCESS) {
        probe.status = "error";
        probe.detail = "Vulkan loader API-version query failed.";
        return probe;
    }
    probe.loader_api_version = api_version(loader_version);
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) {
        probe.status = "error";
        probe.detail = "The loader does not expose vkCreateInstance.";
        return probe;
    }
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Poima capability inspection";
    application.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &application;
    Instance instance;
    const auto created = create(&create_info, nullptr, &instance.value);
    if (created != VK_SUCCESS) {
        if (created != VK_ERROR_INCOMPATIBLE_DRIVER && created != VK_ERROR_INITIALIZATION_FAILED) {
            probe.status = "error";
        }
        probe.detail = "vkCreateInstance failed with VkResult " + std::to_string(created) + ".";
        return probe;
    }
    instance.destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance.value, "vkDestroyInstance"));
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        get(instance.value, "vkEnumeratePhysicalDevices"));
    const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        get(instance.value, "vkGetPhysicalDeviceProperties"));
    const auto queue_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        get(instance.value, "vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!instance.destroy || !enumerate || !properties || !queue_properties) {
        probe.status = "error";
        probe.detail = "The Vulkan instance is missing required inspection entry points.";
        return probe;
    }
    std::vector<VkPhysicalDevice> devices;
    bool complete = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::uint32_t count = 0;
        auto result = enumerate(instance.value, &count, nullptr);
        if (result != VK_SUCCESS || count > 4096) break;
        devices.resize(count);
        if (count == 0) { complete = true; break; }
        result = enumerate(instance.value, &count, devices.data());
        if (result == VK_SUCCESS) {
            devices.resize(count);
            complete = true;
            break;
        }
        if (result != VK_INCOMPLETE) break;
    }
    if (!complete) {
        probe.status = "error";
        probe.detail = "Physical-device enumeration failed or remained incomplete after four attempts.";
        return probe;
    }
    for (const auto device : devices) {
        VkPhysicalDeviceProperties props{};
        properties(device, &props);
        GraphicsDevice record;
        record.name = props.deviceName;
        record.type = device_type(props.deviceType);
        record.api_version = api_version(props.apiVersion);
        record.vendor_id = props.vendorID;
        record.device_id = props.deviceID;
        record.hardware = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
            props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        record.api_at_least_1_3 = VK_API_VERSION_VARIANT(props.apiVersion) == 0 &&
            props.apiVersion >= VK_API_VERSION_1_3;
        std::uint32_t count = 0;
        queue_properties(device, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        if (count > 0) {
            queue_properties(device, &count, queues.data());
            queues.resize(count);
            for (const auto& queue : queues) {
                record.graphics_queue |= queue.queueCount > 0 && (queue.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            }
        }
        probe.devices.push_back(std::move(record));
    }
    probe.status = probe.devices.empty() ? "unavailable" : "ok";
    probe.detail = "Enumeration only; device creation, rendering, presentation and performance are untested.";
    return probe;
}
} // namespace poima
