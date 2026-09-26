// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"
#include "poima/scene.hpp"
#include "poima/player.hpp"
#include "poima/scene_vs.hpp"
#include "poima/scene_ps.hpp"
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
#include <cmath>
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

// A capture or player session owns one graphics lifetime. No concurrent
// renderer access is supported yet; Vulkan dispatch is process-global.
struct DrawConstants {
    float mvp[16];
    float normal[3][4];
    float albedo[4];
};
static_assert(sizeof(DrawConstants) == 128);
struct Vertex { float position[3]; float normal[3]; };
std::vector<Vertex> box_vertices() {
    std::vector<Vertex> result;
    for (int axis = 0; axis < 3; ++axis) for (float sign : {-1.0f, 1.0f}) {
        std::array<Vertex, 4> corners{};
        const float u[] = {-0.5f, 0.5f, 0.5f, -0.5f};
        const float v[] = {-0.5f, -0.5f, 0.5f, 0.5f};
        for (std::size_t k = 0; k < corners.size(); ++k) {
            corners[k].position[axis] = sign * 0.5f;
            corners[k].position[(axis + 1) % 3] = u[k];
            corners[k].position[(axis + 2) % 3] = v[k];
            corners[k].normal[axis] = sign;
        }
        for (auto k : {0u, 1u, 2u, 0u, 2u, 3u}) result.push_back(corners[k]);
    }
    return result;
}

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
    const SceneSnapshot* scene = nullptr;
    std::uint32_t samples = 1;
    nvrhi::TextureHandle depth;
    nvrhi::TextureHandle multisample_color;
    nvrhi::BufferHandle vertices;
    nvrhi::InputLayoutHandle input_layout;
    nvrhi::BindingLayoutHandle binding_layout;
    nvrhi::BindingSetHandle bindings;
    std::vector<DrawConstants> draws;
    bool hardware = false;
    std::string gpu_name;
    bool swapchain_dirty=false;

    ~Context() {
        if (device) {
            try { device.waitIdle(); } catch (...) { /* Preserve the original diagnostic. */ }
        }
        commands = nullptr;
        pipeline = nullptr;
        vertex_shader = nullptr;
        pixel_shader = nullptr;
        staging = nullptr;
        bindings = nullptr;
        binding_layout = nullptr;
        input_layout = nullptr;
        vertices = nullptr;
        framebuffers.clear();
        images.clear();
        depth = nullptr;
        multisample_color = nullptr;
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

    void initialize(const RenderOptions& options, const SceneSnapshot* source, bool player=false) {
        scene = source;
        samples = scene ? options.samples : 1;
        SDL_SetMainReady();
        const bool initialized_video = SDL_Init(SDL_INIT_VIDEO);
        require(initialized_video, std::string("SDL video initialization: ") + SDL_GetError());
        sdl_initialized = true;
        window = SDL_CreateWindow(scene ? "Poima — authored scene preview" : "Poima — Vulkan/NVRHI foundation", static_cast<int>(options.width),
            static_cast<int>(options.height), SDL_WINDOW_VULKAN | (player ? SDL_WINDOW_RESIZABLE : 0));
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

        const auto limits = physical.getProperties().limits;
        const auto requested_samples = samples == 4 ? vk::SampleCountFlagBits::e4 : vk::SampleCountFlagBits::e1;
        require((limits.framebufferColorSampleCounts & requested_samples) && (limits.framebufferDepthSampleCounts & requested_samples),
            "Requested scene MSAA sample count is unavailable on the selected GPU.");
        require(create_swapchain(options),"The initial window has an empty rendering extent.");
        const nvrhi::ShaderDesc vs_desc = nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main");
        const nvrhi::ShaderDesc ps_desc = nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main");
        vertex_shader = scene ? create_embedded_shader(checked, vs_desc, poima_scene_vs) : create_embedded_shader(checked, vs_desc, poima_smoke_vs);
        pixel_shader = scene ? create_embedded_shader(checked, ps_desc, poima_scene_ps) : create_embedded_shader(checked, ps_desc, poima_smoke_ps);
        require(vertex_shader && pixel_shader, "Compiled SPIR-V shader creation failed.");
        nvrhi::GraphicsPipelineDesc pipeline_desc;
        pipeline_desc.VS = vertex_shader;
        pipeline_desc.PS = pixel_shader;
        pipeline_desc.renderState.depthStencilState.depthTestEnable = scene != nullptr;
        pipeline_desc.renderState.depthStencilState.depthWriteEnable = scene != nullptr;
        pipeline_desc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
        if (scene) {
            const nvrhi::VertexAttributeDesc attributes[] = {
                nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(0).setElementStride(sizeof(Vertex)),
                nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(12).setElementStride(sizeof(Vertex))};
            input_layout = checked->createInputLayout(attributes, 2, vertex_shader);
            require(static_cast<bool>(input_layout), "Scene vertex layout creation failed.");
            binding_layout = checked->createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Vertex)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(DrawConstants))));
            require(static_cast<bool>(binding_layout), "Scene push constant layout creation failed.");
            bindings = checked->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(DrawConstants))), binding_layout);
            require(static_cast<bool>(bindings), "Scene binding set creation failed.");
            pipeline_desc.inputLayout = input_layout;
            pipeline_desc.bindingLayouts.push_back(binding_layout);
        }
        pipeline_desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        pipeline = checked->createGraphicsPipeline(pipeline_desc, framebuffers.front()->getFramebufferInfo());
        require(static_cast<bool>(pipeline), "NVRHI graphics pipeline creation failed.");
        commands = checked->createCommandList();
        require(static_cast<bool>(commands), "NVRHI command-list creation failed.");
        if (scene) prepare_scene();
    }

    void prepare_scene() {
        const auto mesh = box_vertices();
        nvrhi::BufferDesc desc;
        desc.byteSize = mesh.size() * sizeof(Vertex); desc.isVertexBuffer = true;
        desc.initialState = nvrhi::ResourceStates::VertexBuffer; desc.keepInitialState = true;
        desc.debugName = "Shared unit box";
        vertices = checked->createBuffer(desc);
        require(static_cast<bool>(vertices), "Scene vertex buffer creation failed.");
        commands->open(); commands->writeBuffer(vertices, mesh.data(), mesh.size() * sizeof(Vertex)); commands->close();
        checked->executeCommandList(commands);
        require(checked->waitForIdle(), "Scene geometry upload failed.");
        update_scene();
    }

    void update_scene() {
        draws.clear();
        const auto vp = multiply(perspective(scene->vertical_fov, static_cast<double>(extent.width) / extent.height,
            scene->near_plane, scene->far_plane), inverse_affine(scene->camera_world));
        auto number = [](double value) {
            require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(), "Scene matrix exceeds GPU float range.");
            return static_cast<float>(value);
        };
        for (const auto& object : scene->objects) {
            DrawConstants draw{};
            const auto mvp = multiply(vp, object.world);
            const auto inverse = inverse_affine(object.world);
            for (std::size_t k=0;k<16;++k) draw.mvp[k] = number(mvp[k]);
            for (std::size_t row=0;row<3;++row) for (std::size_t col=0;col<3;++col)
                draw.normal[row][col] = number(inverse[row*4+col]);
            for (std::size_t k=0;k<3;++k) draw.albedo[k] = object.albedo[k];
            draw.albedo[3] = (format == vk::Format::eB8G8R8A8Srgb || format == vk::Format::eR8G8B8A8Srgb) ? 1.0f : 0.0f;
            draws.push_back(draw);
        }
    }

    bool rebuild(const RenderOptions& options) {
        device.waitIdle();
        commands=nullptr;
        framebuffers.clear(); images.clear(); depth=nullptr; multisample_color=nullptr; staging=nullptr;
        checked->runGarbageCollection();
        for(auto semaphore:finished) device.destroySemaphore(semaphore);
        finished.clear(); initialized.clear();
        device.destroySemaphore(acquired); acquired=nullptr;
        device.destroySwapchainKHR(swapchain); swapchain=nullptr;
        const auto previous_format=format;
        if(!create_swapchain(options)) { swapchain_dirty=true; return false; }
        require(format==previous_format,"Surface format changed; restart this player session.");
        commands=checked->createCommandList();
        require(static_cast<bool>(commands),"Player command-list recreation failed.");
        swapchain_dirty=false;
        return true;
    }

    bool create_swapchain(const RenderOptions& options) {
        const auto caps = physical.getSurfaceCapabilitiesKHR(surface);
        // Native minimize/restore can race SDL's queued size notification.
        // Zero surface extent is a suspended window, not a device failure.
        if(caps.currentExtent.width==0 || caps.currentExtent.height==0) return false;
        const auto formats = physical.getSurfaceFormatsKHR(surface);
        require(!formats.empty(), "No Vulkan surface formats are available.");
        vk::SurfaceFormatKHR selected;
        nvrhi::Format nvrhi_format = nvrhi::Format::UNKNOWN;
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
        if (scene) {
            nvrhi::TextureDesc scene_desc = texture_desc;
            scene_desc.sampleCount = samples;
            scene_desc.dimension = samples > 1 ? nvrhi::TextureDimension::Texture2DMS : nvrhi::TextureDimension::Texture2D;
            scene_desc.keepInitialState = true;
            if (samples > 1) {
                scene_desc.initialState = nvrhi::ResourceStates::RenderTarget;
                multisample_color = checked->createTexture(scene_desc);
                require(static_cast<bool>(multisample_color), "Scene MSAA color creation failed.");
            }
            scene_desc.format = nvrhi::Format::D32;
            scene_desc.initialState = nvrhi::ResourceStates::DepthWrite;
            depth = checked->createTexture(scene_desc);
            require(static_cast<bool>(depth), "Scene depth creation failed.");
        }
        for (const auto image : device.getSwapchainImagesKHR(swapchain)) {
            auto texture = checked->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image,
                nvrhi::Object(static_cast<VkImage>(image)), texture_desc);
            require(static_cast<bool>(texture), "NVRHI swapchain image wrapping failed.");
            images.push_back(texture);
            auto framebuffer_desc = nvrhi::FramebufferDesc().addColorAttachment(multisample_color ? multisample_color.Get() : texture.Get());
            if (depth) framebuffer_desc.setDepthAttachment(depth);
            auto framebuffer = checked->createFramebuffer(framebuffer_desc);
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
        return true;
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

    bool frame(bool capture_frame) {
        if(!swapchain) { swapchain_dirty=true; return false; }
        // A finite acquire timeout bounds the experiment if presentation stalls.
        vk::ResultValue<std::uint32_t> next(vk::Result::eSuccess,0);
        try { next=device.acquireNextImageKHR(swapchain, 5'000'000'000ULL, acquired, {}); }
        catch(const vk::OutOfDateKHRError&) { swapchain_dirty=true; return false; }
        require(next.result == vk::Result::eSuccess || next.result == vk::Result::eSuboptimalKHR,
            "Swapchain acquisition failed or timed out.");
        const auto index = next.value;
        auto texture = images.at(index);
        native->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, acquired, 0);
        commands->open();
        commands->beginTrackingTextureState(texture, nvrhi::AllSubresources,
            initialized[index] ? nvrhi::ResourceStates::Present : nvrhi::ResourceStates::Common);
        commands->clearTextureFloat(multisample_color ? multisample_color.Get() : texture.Get(), nvrhi::AllSubresources, nvrhi::Color(0.025f, 0.035f, 0.055f, 1.0f));
        if (depth) commands->clearDepthStencilTexture(depth, nvrhi::AllSubresources, true, 1.0f, false, 0);
        nvrhi::GraphicsState state;
        state.pipeline = pipeline;
        state.framebuffer = framebuffers[index];
        state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(extent.width), static_cast<float>(extent.height)));
        if (scene) {
            state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(vertices).setSlot(0).setOffset(0));
            state.bindings.push_back(bindings);
        }
        commands->setGraphicsState(state);
        if (scene) {
            for (const auto& draw : draws) {
                commands->setPushConstants(&draw, sizeof(draw));
                commands->draw(nvrhi::DrawArguments().setVertexCount(36));
            }
            if (multisample_color) commands->resolveTexture(texture, nvrhi::AllSubresources, multisample_color, nvrhi::AllSubresources);
        } else commands->draw(nvrhi::DrawArguments().setVertexCount(3));
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
        vk::Result presented;
        try { presented=queue.presentKHR(present_info); }
        catch(const vk::OutOfDateKHRError&) { presented=vk::Result::eErrorOutOfDateKHR; }
        swapchain_dirty = next.result==vk::Result::eSuboptimalKHR || presented==vk::Result::eSuboptimalKHR || presented==vk::Result::eErrorOutOfDateKHR;
        require(presented == vk::Result::eSuccess || presented == vk::Result::eSuboptimalKHR || presented==vk::Result::eErrorOutOfDateKHR,
            "Vulkan presentation failed.");
        initialized[index] = true;
        // Deliberately serialized for this correctness test, not a frame-time benchmark.
        require(checked->waitForIdle(), "NVRHI device wait failed.");
        checked->runGarbageCollection();
        require(messages.errors == 0, "NVRHI reported a validation/backend error; inspect stderr.");
        return presented!=vk::Result::eErrorOutOfDateKHR;
    }
};
} // namespace

RenderReport render(const RenderOptions& options, const SceneSnapshot* scene) {
    RenderReport report;
    Context context;
    try {
        context.initialize(options, scene);
        report.width = context.extent.width;
        report.height = context.extent.height;
        report.samples = context.samples;
        for (std::uint32_t frame = 0; frame < options.frames; ++frame) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                    throw std::runtime_error("The window was closed before the requested frame count completed.");
            }
            const bool capture_frame = !options.capture.empty() && frame + 1 == options.frames;
            require(context.frame(capture_frame),"Surface changed during capture; retry the capture.");
            ++report.frames_presented;
            if (capture_frame) {
                context.capture(options.capture);
                report.capture_written = true;
            }
        }
        report.success = true;
        report.detail = scene ? "Authored scene rendered through the bounded forward preview." : "Vulkan triangle drawn and presented through NVRHI. Serialized smoke test; no game-performance qualification.";
    } catch (const std::exception& error) {
        report.detail = error.what();
    }
    report.hardware = context.hardware;
    report.gpu_name = context.gpu_name;
    report.validation_errors = context.messages.errors;
    return report;
}
RenderReport run_render_smoke(const RenderOptions& options) { return render(options, nullptr); }
RenderReport run_render_scene(const RenderOptions& options, const SceneSnapshot& scene) { return render(options, &scene); }

PlayerReport run_player(const PlayerOptions& options, Runtime& runtime) {
    PlayerReport result;
    auto& report=result.render;
    result.initial_tick=runtime.inspect().tick;
    Context context;
    SceneSnapshot snapshot;
    PlayerClock clock;
    PlayerInput input;
    try {
        snapshot=runtime.snapshot(options.camera);
        context.initialize(options.render,&snapshot,true);
        SDL_SetWindowTitle(context.window,options.replay ? "Poima player — recorded input replay" : "Poima player — WASD / mouse / Space — Esc exits, Tab releases mouse, click resumes");
        bool focused=(SDL_GetWindowFlags(context.window)&SDL_WINDOW_INPUT_FOCUS)!=0;
        bool captured=!options.replay && focused;
        if(captured) require(SDL_SetWindowRelativeMouseMode(context.window,true),SDL_GetError());
        bool quit=false;
        std::size_t segment=0;
        std::uint32_t offset=0;
        auto previous=SDL_GetTicksNS();
        while(!quit) {
            SDL_Event event;
            while(SDL_PollEvent(&event)) {
                if(event.type==SDL_EVENT_QUIT || event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED) { quit=true; result.stop_reason="window_closed"; }
                if(event.type==SDL_EVENT_KEY_DOWN && event.key.scancode==SDL_SCANCODE_ESCAPE) { quit=true; result.stop_reason="escape"; }
                if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST || event.type==SDL_EVENT_WINDOW_MINIMIZED) {
                    focused=false; captured=false; input.clear();
                    if(!options.replay) SDL_SetWindowRelativeMouseMode(context.window,false);
                }
                if(event.type==SDL_EVENT_WINDOW_FOCUS_GAINED) focused=true;
                if(event.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) context.swapchain_dirty=true;
                if(options.replay) continue;
                if(event.type==SDL_EVENT_KEY_DOWN && event.key.scancode==SDL_SCANCODE_TAB) {
                    captured=false; input.clear(); SDL_SetWindowRelativeMouseMode(context.window,false);
                }
                if(event.type==SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button==SDL_BUTTON_LEFT && focused && !captured) {
                    require(SDL_SetWindowRelativeMouseMode(context.window,true),SDL_GetError()); captured=true; input.clear();
                }
                if(!focused || !captured) continue;
                if(event.type==SDL_EVENT_MOUSE_MOTION) input.look(-event.motion.xrel*0.1,-event.motion.yrel*0.1);
                if(event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_KEY_UP) {
                    const bool down=event.type==SDL_EVENT_KEY_DOWN;
                    switch(event.key.scancode) {
                        case SDL_SCANCODE_W: input.button(PlayerAction::forward,down); break;
                        case SDL_SCANCODE_S: input.button(PlayerAction::backward,down); break;
                        case SDL_SCANCODE_A: input.button(PlayerAction::left,down); break;
                        case SDL_SCANCODE_D: input.button(PlayerAction::right,down); break;
                        case SDL_SCANCODE_SPACE: input.button(PlayerAction::jump,down); break;
                        default: break;
                    }
                }
            }
            if(quit) break;
            // Occluded FIFO swapchains can return immediately. Keep an idle
            // editor/player from spinning at thousands of frames per second.
            if(!options.replay && !(focused && captured)) SDL_Delay(16);
            int width=0,height=0;
            require(SDL_GetWindowSizeInPixels(context.window,&width,&height),SDL_GetError());
            const auto now=SDL_GetTicksNS();
            const double elapsed=static_cast<double>(now-previous)/1e9; previous=now;
            const bool drawable=width>0 && height>0 && !(SDL_GetWindowFlags(context.window)&SDL_WINDOW_MINIMIZED);
            if(!drawable) { clock.advance(0,false); SDL_Delay(10); continue; }
            require(width<=4096 && height<=4096,"Player drawable exceeds the initial 4096-pixel limit.");
            if(context.swapchain_dirty || context.extent.width!=static_cast<std::uint32_t>(width) || context.extent.height!=static_cast<std::uint32_t>(height)) {
                auto resized=options.render; resized.width=static_cast<std::uint32_t>(width); resized.height=static_cast<std::uint32_t>(height);
                if(!context.rebuild(resized)) { clock.advance(0,false); SDL_Delay(10); continue; }
                ++result.swapchain_rebuilds;
            }
            if(options.replay) {
                if(segment==options.sequence.size()) { result.stop_reason="replay_complete"; break; }
                auto control=options.sequence[segment].input;
                if(offset!=0) { control.look={0,0}; control.jump=false; }
                runtime.step(1,{control});
                if(++offset==options.sequence[segment].ticks) { offset=0; ++segment; }
            } else {
                const auto ticks=clock.advance(elapsed,focused && captured);
                for(std::uint32_t tick=0;tick<ticks;++tick) runtime.step(1,{input.consume(options.controller)});
            }
            snapshot=runtime.snapshot(options.camera); context.update_scene();
            if(context.frame(false)) ++report.frames_presented;
            if(options.max_frames && report.frames_presented>=options.max_frames) { result.stop_reason="frame_limit"; break; }
        }
        if(!options.render.capture.empty()) {
            // Final artifact observes the exact final tick without simulating an
            // extra tick. Rebuild once if the surface changed during shutdown.
            snapshot=runtime.snapshot(options.camera); context.update_scene();
            bool drawn=context.frame(true);
            if(!drawn) { require(context.rebuild(options.render),"Window is minimized; final capture unavailable."); ++result.swapchain_rebuilds; context.update_scene(); drawn=context.frame(true); }
            require(drawn,"Surface kept changing during final player capture.");
            ++report.frames_presented; context.capture(options.render.capture); report.capture_written=true;
        }
        report.success=true;
        report.detail="Continuous native viewport using the fixed-step runtime; serialized Vulkan presentation, no frame-time qualification.";
    } catch(const std::exception& error) {
        report.detail=error.what(); result.stop_reason="error";
    }
    result.final_tick=runtime.inspect().tick; result.dropped_seconds=clock.dropped_seconds();
    report.width=context.extent.width; report.height=context.extent.height; report.samples=context.samples;
    report.hardware=context.hardware; report.gpu_name=context.gpu_name; report.validation_errors=context.messages.errors;
    return result;
}
} // namespace poima
