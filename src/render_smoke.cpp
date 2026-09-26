// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"
#include "poima/smoke_vs.hpp"
#include "poima/smoke_ps.hpp"

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
#include <nvrhi/vulkan.h>
#include <nvrhi/validation.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace poima {
namespace {
struct Messages final : nvrhi::IMessageCallback {
    std::atomic<std::uint32_t> errors{0};
    void message(nvrhi::MessageSeverity severity, const char* text) override {
        if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal) ++errors;
        std::cerr << "NVRHI: " << text << '\n';
    }
};

void require(bool condition, const std::string& detail) {
    if (!condition) throw std::runtime_error(detail);
}

template<std::size_t Size>
nvrhi::ShaderHandle create_embedded_shader(nvrhi::IDevice* device, const nvrhi::ShaderDesc& desc,
                                           const unsigned char (&bytes)[Size]) {
    static_assert(Size % sizeof(std::uint32_t) == 0);
    // DXC emits byte arrays; Vulkan requires pCode aligned for uint32_t.
    std::vector<std::uint32_t> words(Size / sizeof(std::uint32_t));
    std::memcpy(words.data(), bytes, Size);
    return device->createShader(desc, words.data(), Size);
}

// One bounded experiment owns the complete graphics lifetime. It is not the
// future player/render-graph implementation or a concurrent session API.
struct Context {
    Messages messages;
    bool sdl_initialized = false;
    SDL_Window* window = nullptr;
    vk::Instance instance;
    vk::SurfaceKHR surface;
    vk::PhysicalDevice physical;
    vk::Device device;
    vk::Queue queue;
    std::uint32_t queue_family = 0;
    vk::SwapchainKHR swapchain;
    vk::Extent2D extent;
    vk::Format format = vk::Format::eUndefined;
    vk::Semaphore acquired;
    std::vector<vk::Semaphore> finished;
    std::vector<bool> initialized;
    nvrhi::vulkan::DeviceHandle native;
    nvrhi::DeviceHandle checked;
    std::vector<nvrhi::TextureHandle> images;
    std::vector<nvrhi::FramebufferHandle> framebuffers;
    nvrhi::ShaderHandle vertex_shader;
    nvrhi::ShaderHandle pixel_shader;
    nvrhi::GraphicsPipelineHandle pipeline;
    nvrhi::CommandListHandle commands;
    nvrhi::StagingTextureHandle staging;
    bool hardware = false;
    std::string gpu_name;

    ~Context() {
        if (device) {
            try { device.waitIdle(); } catch (...) { /* Preserve the original diagnostic. */ }
        }
        commands = nullptr;
        pipeline = nullptr;
        vertex_shader = nullptr;
        pixel_shader = nullptr;
        staging = nullptr;
        framebuffers.clear();
        images.clear();
        checked = nullptr;
        native = nullptr;
        if (device) {
            for (auto semaphore : finished) device.destroySemaphore(semaphore);
            if (acquired) device.destroySemaphore(acquired);
            if (swapchain) device.destroySwapchainKHR(swapchain);
            device.destroy();
        }
        if (surface) instance.destroySurfaceKHR(surface);
        if (instance) instance.destroy();
        if (window) SDL_DestroyWindow(window);
        if (sdl_initialized) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    void initialize(const RenderOptions& options) {
        SDL_SetMainReady();
        const bool initialized_video = SDL_Init(SDL_INIT_VIDEO);
        require(initialized_video, std::string("SDL video initialization: ") + SDL_GetError());
        sdl_initialized = true;
        window = SDL_CreateWindow("Poima — Vulkan/NVRHI foundation", static_cast<int>(options.width),
            static_cast<int>(options.height), SDL_WINDOW_VULKAN);
        require(window != nullptr, std::string("SDL window: ") + SDL_GetError());
        const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        require(get != nullptr, "SDL could not load the Vulkan entry point.");
        VULKAN_HPP_DEFAULT_DISPATCHER.init(get);
        require(vk::enumerateInstanceVersion() >= VK_API_VERSION_1_3, "The renderer experiment needs a Vulkan 1.3 loader.");
        std::uint32_t extension_count = 0;
        const auto extension_names = SDL_Vulkan_GetInstanceExtensions(&extension_count);
        require(extension_names != nullptr, std::string("SDL Vulkan extensions: ") + SDL_GetError());
        const vk::ApplicationInfo app("Poima", 1, "Poima", 1, VK_API_VERSION_1_3);
        vk::InstanceCreateInfo instance_info;
        instance_info.pApplicationInfo = &app;
        instance_info.enabledExtensionCount = extension_count;
        instance_info.ppEnabledExtensionNames = extension_names;
        instance = vk::createInstance(instance_info);
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);
        VkSurfaceKHR raw_surface = VK_NULL_HANDLE;
        const bool created_surface = SDL_Vulkan_CreateSurface(window, static_cast<VkInstance>(instance), nullptr, &raw_surface);
        require(created_surface,
            std::string("SDL Vulkan surface: ") + SDL_GetError());
        surface = raw_surface;

        const auto devices = instance.enumeratePhysicalDevices();
        int best_score = -1;
        for (std::size_t index = 0; index < devices.size(); ++index) {
            if (options.gpu >= 0 && index != static_cast<std::size_t>(options.gpu)) continue;
            const auto candidate = devices[index];
            const auto props = candidate.getProperties();
            const bool is_hardware = props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu ||
                props.deviceType == vk::PhysicalDeviceType::eIntegratedGpu;
            if ((!is_hardware && !options.allow_software) || props.apiVersion < VK_API_VERSION_1_3) continue;
            const auto extensions = candidate.enumerateDeviceExtensionProperties();
            const bool swapchain_supported = std::any_of(extensions.begin(), extensions.end(), [](const auto& item) {
                return std::string_view(item.extensionName.data()) == VK_KHR_SWAPCHAIN_EXTENSION_NAME;
            });
            if (!swapchain_supported) continue;
            vk::PhysicalDeviceVulkan13Features features13;
            vk::PhysicalDeviceVulkan12Features features12;
            features12.pNext = &features13;
            vk::PhysicalDeviceFeatures2 features;
            features.pNext = &features12;
            candidate.getFeatures2(&features);
            if (!features12.timelineSemaphore || !features13.synchronization2 || !features13.dynamicRendering) continue;
            const auto queues = candidate.getQueueFamilyProperties();
            for (std::uint32_t family = 0; family < queues.size(); ++family) {
                if (!(queues[family].queueFlags & vk::QueueFlagBits::eGraphics) || !candidate.getSurfaceSupportKHR(family, surface)) continue;
                const int score = props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu ? 30 : (is_hardware ? 20 : 10);
                if (score > best_score) {
                    best_score = score;
                    physical = candidate;
                    queue_family = family;
                    hardware = is_hardware;
                    gpu_name = props.deviceName.data();
                }
                break;
            }
        }
        require(static_cast<bool>(physical), "No selected device satisfies Vulkan 1.3, presentation, timeline semaphore, synchronization2 and dynamic rendering requirements. Software devices require --allow-software.");
        const float priority = 1.0f;
        vk::DeviceQueueCreateInfo queue_info({}, queue_family, 1, &priority);
        vk::PhysicalDeviceVulkan13Features enabled13;
        enabled13.synchronization2 = true;
        enabled13.dynamicRendering = true;
        vk::PhysicalDeviceVulkan12Features enabled12;
        enabled12.timelineSemaphore = true;
        enabled12.pNext = &enabled13;
        const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        vk::DeviceCreateInfo device_info;
        device_info.pNext = &enabled12;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = 1;
        device_info.ppEnabledExtensionNames = device_extensions;
        device = physical.createDevice(device_info);
        VULKAN_HPP_DEFAULT_DISPATCHER.init(device);
        queue = device.getQueue(queue_family, 0);

        nvrhi::vulkan::DeviceDesc desc{};
        desc.errorCB = &messages;
        desc.instance = instance;
        desc.physicalDevice = physical;
        desc.device = device;
        desc.graphicsQueue = queue;
        desc.graphicsQueueIndex = static_cast<int>(queue_family);
        // NVRHI's API predates the extra const on the pointer list; it only reads it.
        desc.instanceExtensions = const_cast<const char**>(extension_names);
        desc.numInstanceExtensions = extension_count;
        desc.deviceExtensions = device_extensions;
        desc.numDeviceExtensions = 1;
        native = nvrhi::vulkan::createDevice(desc);
        require(static_cast<bool>(native), "NVRHI device initialization failed.");
        checked = nvrhi::validation::createValidationLayer(native);

        create_swapchain(options);
        const nvrhi::ShaderDesc vs_desc = nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main");
        const nvrhi::ShaderDesc ps_desc = nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main");
        vertex_shader = create_embedded_shader(checked, vs_desc, poima_smoke_vs);
        pixel_shader = create_embedded_shader(checked, ps_desc, poima_smoke_ps);
        require(vertex_shader && pixel_shader, "Compiled SPIR-V shader creation failed.");
        nvrhi::GraphicsPipelineDesc pipeline_desc;
        pipeline_desc.VS = vertex_shader;
        pipeline_desc.PS = pixel_shader;
        pipeline_desc.renderState.depthStencilState.depthTestEnable = false;
        pipeline_desc.renderState.depthStencilState.depthWriteEnable = false;
        pipeline_desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        pipeline = checked->createGraphicsPipeline(pipeline_desc, framebuffers.front()->getFramebufferInfo());
        require(static_cast<bool>(pipeline), "NVRHI graphics pipeline creation failed.");
        commands = checked->createCommandList();
        require(static_cast<bool>(commands), "NVRHI command-list creation failed.");
    }

    void create_swapchain(const RenderOptions& options) {
        const auto caps = physical.getSurfaceCapabilitiesKHR(surface);
        const auto formats = physical.getSurfaceFormatsKHR(surface);
        require(!formats.empty(), "No Vulkan surface formats are available.");
        vk::SurfaceFormatKHR selected;
        nvrhi::Format nvrhi_format = nvrhi::Format::UNKNOWN;
        for (const auto preferred : {vk::Format::eB8G8R8A8Unorm, vk::Format::eR8G8B8A8Unorm,
                                    vk::Format::eB8G8R8A8Srgb, vk::Format::eR8G8B8A8Srgb}) {
            const auto found = std::find_if(formats.begin(), formats.end(), [preferred](const auto& item) {
                return item.format == preferred && item.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
            });
            if (found == formats.end()) continue;
            selected = *found;
            if (preferred == vk::Format::eB8G8R8A8Unorm) nvrhi_format = nvrhi::Format::BGRA8_UNORM;
            if (preferred == vk::Format::eR8G8B8A8Unorm) nvrhi_format = nvrhi::Format::RGBA8_UNORM;
            if (preferred == vk::Format::eB8G8R8A8Srgb) nvrhi_format = nvrhi::Format::SBGRA8_UNORM;
            if (preferred == vk::Format::eR8G8B8A8Srgb) nvrhi_format = nvrhi::Format::SRGBA8_UNORM;
            break;
        }
        require(nvrhi_format != nvrhi::Format::UNKNOWN, "No supported 8-bit RGBA/BGRA surface format.");
        format = selected.format;
        const auto required_usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
            (options.capture.empty() ? vk::ImageUsageFlags{} : vk::ImageUsageFlagBits::eTransferSrc);
        require((caps.supportedUsageFlags & required_usage) == required_usage, "Surface lacks required render/capture image usage.");
        extent = caps.currentExtent;
        if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
            extent.width = std::clamp(options.width, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(options.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        require(extent.width > 0 && extent.height > 0, "The window has an empty rendering extent.");
        std::uint32_t count = caps.minImageCount + 1;
        if (caps.maxImageCount > 0) count = std::min(count, caps.maxImageCount);
        vk::CompositeAlphaFlagBitsKHR alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
        for (const auto candidate : {vk::CompositeAlphaFlagBitsKHR::eOpaque, vk::CompositeAlphaFlagBitsKHR::ePreMultiplied,
                                    vk::CompositeAlphaFlagBitsKHR::ePostMultiplied, vk::CompositeAlphaFlagBitsKHR::eInherit}) {
            if (caps.supportedCompositeAlpha & candidate) { alpha = candidate; break; }
        }
        vk::SwapchainCreateInfoKHR swapchain_info;
        swapchain_info.surface = surface;
        swapchain_info.minImageCount = count;
        swapchain_info.imageFormat = format;
        swapchain_info.imageColorSpace = selected.colorSpace;
        swapchain_info.imageExtent = extent;
        swapchain_info.imageArrayLayers = 1;
        swapchain_info.imageUsage = required_usage;
        swapchain_info.imageSharingMode = vk::SharingMode::eExclusive;
        swapchain_info.preTransform = caps.currentTransform;
        swapchain_info.compositeAlpha = alpha;
        swapchain_info.presentMode = vk::PresentModeKHR::eFifo;
        swapchain_info.clipped = true;
        swapchain = device.createSwapchainKHR(swapchain_info);
        nvrhi::TextureDesc texture_desc;
        texture_desc.width = extent.width;
        texture_desc.height = extent.height;
        texture_desc.format = nvrhi_format;
        texture_desc.isRenderTarget = true;
        texture_desc.isShaderResource = false;
        for (const auto image : device.getSwapchainImagesKHR(swapchain)) {
            auto texture = checked->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image,
                nvrhi::Object(static_cast<VkImage>(image)), texture_desc);
            require(static_cast<bool>(texture), "NVRHI swapchain image wrapping failed.");
            images.push_back(texture);
            auto framebuffer = checked->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
            require(static_cast<bool>(framebuffer), "NVRHI framebuffer creation failed.");
            framebuffers.push_back(framebuffer);
            finished.push_back(device.createSemaphore({}));
        }
        initialized.resize(images.size(), false);
        acquired = device.createSemaphore({});
        if (!options.capture.empty()) {
            texture_desc.isRenderTarget = false;
            staging = checked->createStagingTexture(texture_desc, nvrhi::CpuAccessMode::Read);
            require(static_cast<bool>(staging), "NVRHI capture staging texture creation failed.");
        }
    }

    void capture(const std::string& path) {
        std::size_t row_pitch = 0;
        void* pixels = checked->mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, &row_pitch);
        require(pixels != nullptr, "GPU capture readback mapping failed.");
        const bool bgra = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
        SDL_Surface* image = SDL_CreateSurfaceFrom(static_cast<int>(extent.width), static_cast<int>(extent.height),
            bgra ? SDL_PIXELFORMAT_BGRA32 : SDL_PIXELFORMAT_RGBA32, pixels, static_cast<int>(row_pitch));
        const bool saved = image && SDL_SaveBMP(image, path.c_str());
        const std::string detail = saved ? "" : std::string("BMP capture: ") + SDL_GetError();
        if (image) SDL_DestroySurface(image);
        checked->unmapStagingTexture(staging);
        require(saved, detail);
    }

    void frame(bool capture_frame) {
        // A finite acquire timeout bounds the experiment if presentation stalls.
        const auto next = device.acquireNextImageKHR(swapchain, 5'000'000'000ULL, acquired, {});
        require(next.result == vk::Result::eSuccess || next.result == vk::Result::eSuboptimalKHR,
            "Swapchain acquisition failed or timed out.");
        const auto index = next.value;
        auto texture = images.at(index);
        native->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, acquired, 0);
        commands->open();
        commands->beginTrackingTextureState(texture, nvrhi::AllSubresources,
            initialized[index] ? nvrhi::ResourceStates::Present : nvrhi::ResourceStates::Common);
        commands->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(0.025f, 0.035f, 0.055f, 1.0f));
        nvrhi::GraphicsState state;
        state.pipeline = pipeline;
        state.framebuffer = framebuffers[index];
        state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(extent.width), static_cast<float>(extent.height)));
        commands->setGraphicsState(state);
        commands->draw(nvrhi::DrawArguments().setVertexCount(3));
        if (capture_frame) commands->copyTexture(staging, {}, texture, {});
        commands->setTextureState(texture, nvrhi::AllSubresources, nvrhi::ResourceStates::Present);
        commands->commitBarriers();
        commands->close();
        native->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, finished[index], 0);
        checked->executeCommandList(commands);
        vk::PresentInfoKHR present_info;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &finished[index];
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain;
        present_info.pImageIndices = &index;
        const auto presented = queue.presentKHR(present_info);
        require(presented == vk::Result::eSuccess || presented == vk::Result::eSuboptimalKHR,
            "Vulkan presentation failed.");
        initialized[index] = true;
        // Deliberately serialized for this correctness test, not a frame-time benchmark.
        require(checked->waitForIdle(), "NVRHI device wait failed.");
        checked->runGarbageCollection();
        require(messages.errors == 0, "NVRHI reported a validation/backend error; inspect stderr.");
    }
};
} // namespace

RenderReport run_render_smoke(const RenderOptions& options) {
    RenderReport report;
    Context context;
    try {
        context.initialize(options);
        report.width = context.extent.width;
        report.height = context.extent.height;
        for (std::uint32_t frame = 0; frame < options.frames; ++frame) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                    throw std::runtime_error("The window was closed before the requested frame count completed.");
            }
            const bool capture_frame = !options.capture.empty() && frame + 1 == options.frames;
            context.frame(capture_frame);
            ++report.frames_presented;
            if (capture_frame) {
                context.capture(options.capture);
                report.capture_written = true;
            }
        }
        report.success = true;
        report.detail = "Vulkan triangle drawn and presented through NVRHI. Serialized smoke test; no game-performance qualification.";
    } catch (const std::exception& error) {
        report.detail = error.what();
    }
    report.hardware = context.hardware;
    report.gpu_name = context.gpu_name;
    report.validation_errors = context.messages.errors;
    return report;
}
} // namespace poima
