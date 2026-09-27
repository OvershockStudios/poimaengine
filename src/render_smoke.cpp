// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"
#include "poima/scene.hpp"
#include "poima/player.hpp"
#include "poima/input_profile.hpp"
#include "poima/assets.hpp"
#include "poima/animation.hpp"
#include "poima/editor_viewport.hpp"
#if POIMA_EDITOR
#include <imgui.h>
#include "poima/editor_ui_vs.hpp"
#include "poima/editor_ui_ps.hpp"
#endif
#include "poima/skinning_cs.hpp"
#include <set>
#include <map>
#include "poima/scene_vs.hpp"
#include "poima/scene_ps.hpp"
#include "poima/shadow_vs.hpp"
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
#include "player_audio.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#if POIMA_EDITOR
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif
#endif

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

#if POIMA_EDITOR
void save_editor_bmp_exclusive(SDL_Surface* surface,const std::string& path) {
    require(!path.empty() && path.find('\0')==std::string::npos,"Invalid editor capture path.");
    std::unique_ptr<SDL_IOStream,decltype(&SDL_CloseIO)> memory(SDL_IOFromDynamicMem(),SDL_CloseIO);
    require(bool(memory),std::string("BMP memory stream: ")+SDL_GetError());
    require(SDL_SaveBMP_IO(surface,memory.get(),false),std::string("BMP serialization: ")+SDL_GetError());
    const auto size=SDL_GetIOSize(memory.get());
    require(size>0 && size<=512*1024*1024,"Editor BMP exceeds the 512 MiB capture budget.");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    require(SDL_SeekIO(memory.get(),0,SDL_IO_SEEK_SET)==0 && SDL_ReadIO(memory.get(),bytes.data(),bytes.size())==bytes.size(),"Cannot read serialized editor BMP.");
    const auto output=std::filesystem::path(std::u8string(path.begin(),path.end()));
    // The filesystem performs the absence check and creation in one operation.
    // A competing creator cannot have its file truncated between preflight and
    // publication. Write failure can leave our own partial new file; report it.
#ifdef _WIN32
    HANDLE file=CreateFileW(output.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(file!=INVALID_HANDLE_VALUE,"Editor capture destination exists or cannot be exclusively created.");
    try {
        std::size_t offset=0;
        while(offset<bytes.size()) {
            const auto length=static_cast<DWORD>(std::min(bytes.size()-offset,std::size_t(1024*1024)));DWORD written=0;
            require(WriteFile(file,bytes.data()+offset,length,&written,nullptr) && written>0,"Editor BMP write failed.");offset+=written;
        }
        require(FlushFileBuffers(file),"Editor BMP flush failed.");
        const bool closed=CloseHandle(file)!=0;file=INVALID_HANDLE_VALUE;require(closed,"Editor BMP close failed.");
    }catch(...) { if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);throw; }
#else
    int file=::open(output.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    require(file>=0,"Editor capture destination exists or cannot be exclusively created.");
    try {
        std::size_t offset=0;
        while(offset<bytes.size()) {
            const auto written=::write(file,bytes.data()+offset,bytes.size()-offset);
            if(written<0 && errno==EINTR)continue;
            require(written>0,"Editor BMP write failed.");offset+=static_cast<std::size_t>(written);
        }
        require(::fsync(file)==0,"Editor BMP flush failed.");
        const bool closed=::close(file)==0;file=-1;require(closed,"Editor BMP close failed.");
    }catch(...) { if(file>=0)::close(file);throw; }
#endif
}
#endif

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
    float model[3][4];
    float normal[3][4];
    float base_metallic[4];
    float emissive_roughness[4];
};
static_assert(sizeof(DrawConstants)==128);
struct GpuLight { float position_kind[4],direction_range[4],color_intensity[4],cone[4],shadow[4]; };
struct GpuShadow { float view_projection[16],splits[4]; };
struct FrameConstants { float view_projection[16];float camera[4];float ambient_exposure[4];std::uint32_t light_count[4];float camera_forward[4];GpuLight lights[max_scene_lights];GpuShadow shadows[max_shadow_views]; };
static_assert(sizeof(GpuLight)==80 && sizeof(GpuShadow)==80 && sizeof(FrameConstants)==6528);
struct Geometry { nvrhi::BufferHandle vertices,indices;std::uint32_t count=0; };
struct GpuInfluence { std::uint32_t joints[4];float weights[4]; };
struct GpuJoint { float rows[3][4]; };
static_assert(sizeof(GpuInfluence)==32 && sizeof(GpuJoint)==48);
struct SkinSource { std::shared_ptr<const MeshAsset> mesh;nvrhi::BufferHandle influences;SkinBounds bounds; };
struct SkinInstance {
    std::shared_ptr<const MeshAsset> mesh;
    nvrhi::BufferHandle vertices,palette;
    nvrhi::BindingSetHandle bindings;
    std::vector<GpuJoint> joints;
    std::size_t bytes=0;
};
struct DrawItem { std::string entity_id;SkinInstance* skin=nullptr; DrawConstants constants{};Geometry geometry;nvrhi::BindingSetHandle bindings;bool cull=false,camera_visible=true;std::uint32_t shadow_mask=0; };
using SteadyClock=std::chrono::steady_clock;
double elapsed_ms(SteadyClock::time_point start) { return std::chrono::duration<double,std::milli>(SteadyClock::now()-start).count(); }
void timing_sample(TimingSummary& value,double ms) {
    if(value.samples==0)value.min_ms=value.max_ms=ms;
    else { value.min_ms=std::min(value.min_ms,ms);value.max_ms=std::max(value.max_ms,ms); }
    ++value.samples;value.total_ms+=ms;value.last_ms=ms;
}
using Vertex=MeshVertex;
std::vector<Vertex> box_vertices() {
    std::vector<Vertex> result;
    for (std::size_t axis = 0; axis < 3; ++axis) for (float sign : {-1.0f, 1.0f}) {
        std::array<Vertex, 4> corners{};
        const float u[] = {-0.5f, 0.5f, 0.5f, -0.5f};
        const float v[] = {-0.5f, -0.5f, 0.5f, 0.5f};
        for (std::size_t k = 0; k < corners.size(); ++k) {
            corners[k].position[axis] = sign * 0.5f;
            corners[k].position[(axis + 1) % 3] = u[k];
            corners[k].position[(axis + 2) % 3] = v[k];
            corners[k].normal[axis] = sign;
            corners[k].uv={u[k]+0.5f, v[k]+0.5f};corners[k].tangent={0,0,0,sign};corners[k].tangent[(axis+1)%3]=1;
        }
        const std::array<unsigned,6> indices=sign>0 ? std::array<unsigned,6>{0,1,2,0,2,3} : std::array<unsigned,6>{0,2,1,0,3,2};
        for(auto k:indices)result.push_back(corners[k]);
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
    nvrhi::GraphicsPipelineHandle pipeline,culled_pipeline;
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
    std::vector<DrawItem> draws;
    FrameConstants frame_constants{};
    nvrhi::BufferHandle frame_buffer;
    nvrhi::TextureHandle shadow_texture;
    std::vector<nvrhi::FramebufferHandle> shadow_framebuffers;
    nvrhi::ShaderHandle shadow_shader;
    nvrhi::BindingLayoutHandle shadow_layout;
    nvrhi::BindingSetHandle shadow_bindings;
    nvrhi::GraphicsPipelineHandle shadow_pipeline;
    std::vector<ShadowView> shadow_plan;
    std::map<const MeshAsset*,Geometry> geometry_cache;
    std::map<const MeshAsset*,std::shared_ptr<const MeshAsset>> mesh_owners;
    std::map<const MaterialTextures*,std::shared_ptr<const MaterialTextures>> material_owners;
    std::map<const TextureImage*,std::shared_ptr<const TextureImage>> image_owners;
    std::map<const MeshAsset*,Bounds> bounds_cache;
    std::map<const MeshAsset*,SkinSource> skin_sources;
    std::map<std::string,SkinInstance> skin_instances;
    std::size_t skin_bytes=0;
    nvrhi::ShaderHandle skin_shader;
    nvrhi::BindingLayoutHandle skin_layout;
    nvrhi::ComputePipelineHandle skin_pipeline;
    nvrhi::BufferHandle skin_errors,skin_readback;
    RenderDiagnostics diagnostics;
    DrawCounts pending_draws;
    vk::QueryPool timestamp_pool;
    std::map<std::pair<const MeshAsset*,const MaterialTextures*>,nvrhi::BindingSetHandle> material_cache;
    std::map<const TextureImage*,nvrhi::TextureHandle> texture_cache;
    std::map<std::array<int,4>,nvrhi::SamplerHandle> sampler_cache;
    std::shared_ptr<const TextureImage> white_image;
    std::size_t texture_bytes=0;
    bool hardware = false;
    std::string gpu_name;
    bool swapchain_dirty=false;
    bool shadow_ready=false,skin_pipeline_ready=false,renderer_fault=false;
    bool editor=false,scene_visible=true;
    std::optional<nvrhi::Viewport> scene_viewport;
#if POIMA_EDITOR
    nvrhi::ShaderHandle ui_vs,ui_ps;
    nvrhi::InputLayoutHandle ui_input;
    nvrhi::BindingLayoutHandle ui_layout;
    nvrhi::BindingSetHandle ui_bindings;
    nvrhi::GraphicsPipelineHandle ui_pipeline;
    nvrhi::TextureHandle ui_font;
    nvrhi::SamplerHandle ui_sampler;
    nvrhi::BufferHandle ui_vertices,ui_indices;
    std::vector<nvrhi::FramebufferHandle> ui_framebuffers;
    std::vector<ImDrawVert> ui_vertex_data;
    std::vector<ImDrawIdx> ui_index_data;
    const ImDrawData* ui_data=nullptr;
    ImGuiContext* ui_context=nullptr;
    std::size_t ui_vertex_capacity=0,ui_index_capacity=0;
#endif

    ~Context() {
        if (device) {
            try { device.waitIdle(); } catch (...) { /* Preserve the original diagnostic. */ }
        }
        commands = nullptr;
#if POIMA_EDITOR
        if(ui_context && ImGui::GetCurrentContext()==ui_context) {
            auto& io=ImGui::GetIO();
            if(io.BackendRendererName && std::strcmp(io.BackendRendererName,"poima_nvrhi")==0) {
                if(io.Fonts->TexID==static_cast<ImTextureID>(1))io.Fonts->SetTexID(0);
                io.BackendFlags&=~ImGuiBackendFlags_RendererHasVtxOffset;io.BackendRendererName=nullptr;
            }
        }
        ui_framebuffers.clear();ui_pipeline=nullptr;ui_bindings=nullptr;ui_layout=nullptr;
        ui_vertices=nullptr;ui_indices=nullptr;ui_font=nullptr;ui_sampler=nullptr;ui_input=nullptr;ui_vs=nullptr;ui_ps=nullptr;
#endif
        skin_instances.clear();skin_sources.clear();skin_pipeline=nullptr;skin_layout=nullptr;skin_shader=nullptr;skin_errors=nullptr;skin_readback=nullptr;
        shadow_pipeline=nullptr;shadow_bindings=nullptr;shadow_layout=nullptr;shadow_shader=nullptr;shadow_framebuffers.clear();shadow_texture=nullptr;
        pipeline = nullptr; culled_pipeline=nullptr;
        vertex_shader = nullptr;
        pixel_shader = nullptr;
        staging = nullptr;
        bindings = nullptr;
        binding_layout = nullptr;
        input_layout = nullptr;
        vertices = nullptr; frame_buffer=nullptr; draws.clear(); geometry_cache.clear(); material_cache.clear();texture_cache.clear();sampler_cache.clear();
        framebuffers.clear();
        images.clear();
        depth = nullptr;
        multisample_color = nullptr;
        checked = nullptr;
        native = nullptr;
        if (device) {
            for (auto semaphore : finished) device.destroySemaphore(semaphore);
            if (acquired) device.destroySemaphore(acquired);
            if (timestamp_pool) device.destroyQueryPool(timestamp_pool);
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
        diagnostics.culling=options.culling;diagnostics.profile_requested=options.profile;
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
                nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(12).setElementStride(sizeof(Vertex)),
                nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(24).setElementStride(sizeof(Vertex)),
                nvrhi::VertexAttributeDesc().setName("TANGENT").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(32).setElementStride(sizeof(Vertex))};
            input_layout = checked->createInputLayout(attributes, 4, vertex_shader);
            require(static_cast<bool>(input_layout), "Scene vertex layout creation failed.");
            auto layout=nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(DrawConstants))).addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1));
            for(std::uint32_t slot=0;slot<5;++slot)layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot)).addItem(nvrhi::BindingLayoutItem::Sampler(slot));
            layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(5));
            binding_layout = checked->createBindingLayout(layout);
            require(static_cast<bool>(binding_layout), "Scene push constant layout creation failed.");
            nvrhi::BufferDesc frame_desc;frame_desc.byteSize=sizeof(FrameConstants);frame_desc.isConstantBuffer=true;
            frame_desc.initialState=nvrhi::ResourceStates::ConstantBuffer;frame_desc.keepInitialState=true;frame_desc.debugName="Scene frame uniforms";
            frame_buffer=checked->createBuffer(frame_desc);require(static_cast<bool>(frame_buffer),"Frame uniform buffer creation failed.");
            pipeline_desc.inputLayout = input_layout;
            pipeline_desc.bindingLayouts.push_back(binding_layout);
        }
        pipeline_desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        pipeline_desc.renderState.rasterState.scissorEnable=editor;
        pipeline_desc.renderState.rasterState.frontCounterClockwise=true;
        pipeline = checked->createGraphicsPipeline(pipeline_desc, framebuffers.front()->getFramebufferInfo());
        require(static_cast<bool>(pipeline), "NVRHI graphics pipeline creation failed.");
        if(scene) {
            pipeline_desc.renderState.rasterState.cullMode=nvrhi::RasterCullMode::Back;
            culled_pipeline=checked->createGraphicsPipeline(pipeline_desc,framebuffers.front()->getFramebufferInfo());
            require(static_cast<bool>(culled_pipeline),"Culled mesh pipeline creation failed.");
        }
        commands = checked->createCommandList();
        require(static_cast<bool>(commands), "NVRHI command-list creation failed.");
        if(options.profile)prepare_timestamps();
        if (scene) { prepare_shadows();prepare_scene(); }
    }

    void prepare_timestamps() {
        const auto limits=physical.getProperties().limits;
        diagnostics.timestamp_period_ns=limits.timestampPeriod;
        diagnostics.timestamp_valid_bits=physical.getQueueFamilyProperties().at(queue_family).timestampValidBits;
        if(!(limits.timestampPeriod>0) || diagnostics.timestamp_valid_bits==0) {
            diagnostics.gpu_timing_detail="Selected graphics queue does not support timestamps.";return;
        }
        timestamp_pool=device.createQueryPool(vk::QueryPoolCreateInfo({},vk::QueryType::eTimestamp,5));
        diagnostics.gpu_timestamps=true;
        diagnostics.gpu_timing_detail="64-bit graphics-queue timestamps; approximate pass intervals, not presentation latency or game frame time.";
    }
    vk::CommandBuffer native_commands() {
        const auto object=commands->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
        require(object.pointer!=nullptr,"Native Vulkan command buffer unavailable.");
        return vk::CommandBuffer(static_cast<VkCommandBuffer>(object.pointer));
    }
    void timestamp(std::uint32_t index) {
        if(timestamp_pool)native_commands().writeTimestamp(index==0 ? vk::PipelineStageFlagBits::eTopOfPipe : vk::PipelineStageFlagBits::eBottomOfPipe,timestamp_pool,index);
    }
    void collect_timestamps(double cpu_interval_ms) {
        if(!timestamp_pool)return;
        std::array<std::uint64_t,5> values{};
        const auto status=device.getQueryPoolResults(timestamp_pool,0,5,sizeof(values),values.data(),sizeof(values[0]),vk::QueryResultFlagBits::e64);
        const auto bits=diagnostics.timestamp_valid_bits;
        const double wrap_ms=std::ldexp(diagnostics.timestamp_period_ns*1e-6,static_cast<int>(bits));
        if(status!=vk::Result::eSuccess || cpu_interval_ms>=wrap_ms) { ++diagnostics.gpu_samples_dropped;return; }
        const auto mask=bits==64 ? ~std::uint64_t(0) : (std::uint64_t(1)<<bits)-1;
        auto ms=[&](std::size_t a,std::size_t b) { return static_cast<double>((values[b]-values[a])&mask)*diagnostics.timestamp_period_ns*1e-6; };
        timing_sample(diagnostics.skinning_gpu,ms(0,1));timing_sample(diagnostics.shadow_gpu,ms(1,2));timing_sample(diagnostics.opaque_gpu,ms(2,3));
        timing_sample(diagnostics.post_gpu,ms(3,4));timing_sample(diagnostics.total_gpu,ms(0,4));
    }

    void prepare_shadows() {
        auto lighting=scene->lighting;finalize_lighting(lighting);std::size_t count=0;
        for(const auto& source:lighting.lights)count+=shadow_view_count(source.light);
        validate_shadow_budget(count,lighting.environment.shadow_resolution);
        const auto resolution=count ? lighting.environment.shadow_resolution : 1u;
        const auto layers=static_cast<std::uint32_t>(std::max(count,std::size_t(1)));
        if(shadow_ready && shadow_texture && shadow_texture->getDesc().width==resolution && shadow_texture->getDesc().arraySize==layers)return;
        // Frames are serialized. Retire bindings that reference the old array
        // before replacing it after an authored light or quality edit.
        shadow_ready=false;material_cache.clear();bindings=nullptr;
        shadow_framebuffers.clear();shadow_pipeline=nullptr;shadow_bindings=nullptr;shadow_texture=nullptr;
        const auto limits=physical.getProperties().limits;
        require(resolution<=limits.maxImageDimension2D && layers<=limits.maxImageArrayLayers,"Shadow texture dimensions are unavailable on this GPU.");
        const auto flags=physical.getFormatProperties(vk::Format::eD32Sfloat).optimalTilingFeatures;
        require(bool(flags & vk::FormatFeatureFlagBits::eDepthStencilAttachment) && bool(flags & vk::FormatFeatureFlagBits::eSampledImage),"GPU cannot sample D32 shadow maps.");
        nvrhi::TextureDesc td;td.width=resolution;td.height=resolution;td.arraySize=layers;td.dimension=nvrhi::TextureDimension::Texture2DArray;
        td.format=nvrhi::Format::D32;td.isRenderTarget=true;td.isShaderResource=true;td.initialState=nvrhi::ResourceStates::ShaderResource;td.keepInitialState=true;td.debugName="Shadow depth array";
        shadow_texture=checked->createTexture(td);require(bool(shadow_texture),"Shadow texture allocation failed.");
        for(std::uint32_t i=0;i<layers;++i) {
            auto fb=checked->createFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(shadow_texture,nvrhi::TextureSubresourceSet(0,1,i,1)));
            require(bool(fb),"Shadow framebuffer creation failed.");shadow_framebuffers.push_back(fb);
        }
        shadow_shader=create_embedded_shader(checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("shadow_vertex_main"),poima_shadow_vs);
        shadow_layout=checked->createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Vertex)
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0,sizeof(DrawConstants))).addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1)));
        require(shadow_shader && shadow_layout,"Shadow shader/layout creation failed.");
        shadow_bindings=checked->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0,sizeof(DrawConstants)))
            .addItem(nvrhi::BindingSetItem::ConstantBuffer(1,frame_buffer)),shadow_layout);
        nvrhi::GraphicsPipelineDesc pd;pd.VS=shadow_shader;pd.inputLayout=input_layout;pd.bindingLayouts.push_back(shadow_layout);
        pd.renderState.depthStencilState.depthTestEnable=true;pd.renderState.depthStencilState.depthWriteEnable=true;pd.renderState.depthStencilState.depthFunc=nvrhi::ComparisonFunc::LessOrEqual;
        pd.renderState.rasterState.cullMode=nvrhi::RasterCullMode::None;pd.renderState.rasterState.frontCounterClockwise=true;
        shadow_pipeline=checked->createGraphicsPipeline(pd,shadow_framebuffers.front()->getFramebufferInfo());
        require(shadow_bindings && shadow_pipeline,"Shadow pipeline creation failed.");
        shadow_ready=true;
    }
    void render_shadows() {
        commands->clearDepthStencilTexture(shadow_texture,nvrhi::AllSubresources,true,1,false,0);
        for(std::size_t layer=0;layer<shadow_plan.size();++layer) {
            nvrhi::GraphicsState state;state.pipeline=shadow_pipeline;state.framebuffer=shadow_framebuffers.at(layer);state.bindings.push_back(shadow_bindings);
            const auto size=static_cast<float>(shadow_texture->getDesc().width);state.viewport.addViewportAndScissorRect(nvrhi::Viewport(size,size));
            state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setSlot(0));
            for(const auto& draw:draws) {
                if(!(draw.shadow_mask & (1u<<layer)))continue;
                state.vertexBuffers[0].buffer=draw.geometry.vertices;
                state.indexBuffer=draw.geometry.indices ? nvrhi::IndexBufferBinding(draw.geometry.indices,nvrhi::Format::R32_UINT,0) : nvrhi::IndexBufferBinding();
                commands->setGraphicsState(state);auto constants=draw.constants;constants.normal[0][3]=static_cast<float>(layer);
                commands->setPushConstants(&constants,sizeof(constants));
                if(draw.geometry.indices)commands->drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.geometry.count));
                else commands->draw(nvrhi::DrawArguments().setVertexCount(draw.geometry.count));
            }
        }
        commands->setTextureState(shadow_texture,nvrhi::AllSubresources,nvrhi::ResourceStates::ShaderResource);commands->commitBarriers();
    }

    void prepare_scene() {
        auto white=std::make_shared<TextureImage>();white->mips.push_back({1,1,{255,255,255,255}});white_image=white;
        bindings=mesh_bindings(nullptr,nullptr);
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

    Geometry mesh_geometry(const std::shared_ptr<const MeshAsset>& mesh) {
        if(!mesh)return {vertices,nullptr,36};
        if(const auto found=geometry_cache.find(mesh.get());found!=geometry_cache.end())return found->second;
        Geometry result;result.count=static_cast<std::uint32_t>(mesh->indices.size());
        nvrhi::BufferDesc vertex_desc;vertex_desc.byteSize=mesh->vertices.size()*sizeof(Vertex);vertex_desc.isVertexBuffer=true;vertex_desc.structStride=mesh->influences.empty() ? 0u : std::uint32_t(sizeof(Vertex));
        vertex_desc.initialState=nvrhi::ResourceStates::VertexBuffer;vertex_desc.keepInitialState=true;
        result.vertices=checked->createBuffer(vertex_desc);
        auto index_desc=vertex_desc;index_desc.structStride=0;index_desc.byteSize=mesh->indices.size()*sizeof(std::uint32_t);index_desc.isVertexBuffer=false;index_desc.isIndexBuffer=true;index_desc.initialState=nvrhi::ResourceStates::IndexBuffer;
        result.indices=checked->createBuffer(index_desc);require(result.vertices && result.indices,"Imported geometry buffer creation failed.");
        commands->open();commands->writeBuffer(result.vertices,mesh->vertices.data(),vertex_desc.byteSize);
        commands->writeBuffer(result.indices,mesh->indices.data(),index_desc.byteSize);commands->close();checked->executeCommandList(commands);
        require(checked->waitForIdle(),"Imported geometry upload failed.");
        geometry_cache.emplace(mesh.get(),result);return result;
    }
    void prepare_skin_pipeline() {
        if(skin_pipeline_ready)return;
        require(bool(physical.getQueueFamilyProperties().at(queue_family).queueFlags & vk::QueueFlagBits::eCompute),"Selected graphics queue cannot run skinning compute.");
        skin_shader=create_embedded_shader(checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Compute).setEntryName("compute_main"),poima_skinning_cs);
        skin_layout=checked->createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0,16))
            .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0)).addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
            .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2)).addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0))
            .addItem(nvrhi::BindingLayoutItem::RawBuffer_UAV(1)));
        require(skin_shader && skin_layout,"Skinning shader/layout creation failed.");
        skin_pipeline=checked->createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(skin_shader).addBindingLayout(skin_layout));
        nvrhi::BufferDesc error;error.byteSize=16;error.canHaveUAVs=true;error.canHaveRawViews=true;
        error.initialState=nvrhi::ResourceStates::UnorderedAccess;error.keepInitialState=true;error.debugName="Skinning error flags";
        skin_errors=checked->createBuffer(error);
        nvrhi::BufferDesc readback;readback.byteSize=16;readback.cpuAccess=nvrhi::CpuAccessMode::Read;
        readback.initialState=nvrhi::ResourceStates::CopyDest;readback.keepInitialState=true;readback.debugName="Skinning error readback";
        skin_readback=checked->createBuffer(readback);
        require(skin_pipeline && skin_errors && skin_readback,"Skinning compute resources unavailable.");
        skin_pipeline_ready=true;
    }
    SkinSource& skin_source(const std::shared_ptr<const MeshAsset>& mesh) {
        if(const auto found=skin_sources.find(mesh.get());found!=skin_sources.end())return found->second;
        SkinSource result;result.mesh=mesh;result.bounds=skin_bounds(*mesh);
        std::vector<GpuInfluence> influences(mesh->influences.size());
        for(std::size_t i=0;i<influences.size();++i)for(std::size_t k=0;k<4;++k) {
            influences[i].joints[k]=mesh->influences[i].joints[k];influences[i].weights[k]=mesh->influences[i].weights[k];
        }
        nvrhi::BufferDesc desc;desc.byteSize=influences.size()*sizeof(GpuInfluence);desc.structStride=sizeof(GpuInfluence);
        desc.initialState=nvrhi::ResourceStates::ShaderResource;desc.keepInitialState=true;desc.debugName="Immutable skin influences";
        result.influences=checked->createBuffer(desc);require(bool(result.influences),"Skin influence allocation failed.");
        commands->open();commands->writeBuffer(result.influences,influences.data(),desc.byteSize);commands->close();checked->executeCommandList(commands);
        require(checked->waitForIdle(),"Skin influence upload failed.");
        return skin_sources.emplace(mesh.get(),std::move(result)).first->second;
    }
    SkinInstance& skin_instance(const SceneObject& object,const Geometry& geometry,SkinSource& source) {
        prepare_skin_pipeline();const auto& palette=object.skin->palette;
        require(!palette.empty() && palette.size()<=max_skin_joints && source.bounds.joints.size()<=palette.size(),"Skin palette does not cover its vertex joints.");
        auto& deformation=skin_instances[object.entity_id];
        if(deformation.mesh!=object.mesh || deformation.joints.size()!=palette.size()) {
            skin_bytes-=deformation.bytes;deformation={};
            const auto bytes=object.mesh->vertices.size()*sizeof(Vertex)+palette.size()*sizeof(GpuJoint);
            require(bytes<=128*1024*1024-skin_bytes,"GPU skinned deformation buffers exceed the initial 128 MiB budget.");
            nvrhi::BufferDesc desc;desc.byteSize=object.mesh->vertices.size()*sizeof(Vertex);desc.structStride=sizeof(Vertex);
            desc.isVertexBuffer=true;desc.canHaveUAVs=true;desc.initialState=nvrhi::ResourceStates::VertexBuffer;desc.keepInitialState=true;desc.debugName="Computed skin vertices";
            deformation.vertices=checked->createBuffer(desc);
            desc.byteSize=palette.size()*sizeof(GpuJoint);desc.structStride=sizeof(GpuJoint);desc.isVertexBuffer=false;desc.canHaveUAVs=false;
            desc.initialState=nvrhi::ResourceStates::ShaderResource;desc.debugName="Instance skin palette";deformation.palette=checked->createBuffer(desc);
            require(deformation.vertices && deformation.palette,"Skin deformation buffer allocation failed.");
            deformation.bindings=checked->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0,16))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0,geometry.vertices))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1,source.influences))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2,deformation.palette))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0,deformation.vertices))
                .addItem(nvrhi::BindingSetItem::RawBuffer_UAV(1,skin_errors)),skin_layout);
            require(bool(deformation.bindings),"Skin compute binding creation failed.");
            deformation.mesh=object.mesh;deformation.joints.resize(palette.size());deformation.bytes=bytes;skin_bytes+=bytes;
        }
        for(std::size_t i=0;i<palette.size();++i) {
            const auto& matrix=palette[i];require(matrix[3]==0 && matrix[7]==0 && matrix[11]==0 && matrix[15]==1,"GPU palette must be affine.");
            for(std::size_t row=0;row<3;++row)for(std::size_t col=0;col<4;++col) {
                const double value=matrix[col*4+row];require(std::isfinite(value) && std::abs(value)<=std::numeric_limits<float>::max(),"GPU palette exceeds float range.");
                deformation.joints[i].rows[row][col]=static_cast<float>(value);
            }
        }
        return deformation;
    }
    void dispatch_skinning() {
        if(!pending_draws.skinned_instances)return;
        commands->clearBufferUInt(skin_errors,0);
        for(std::size_t i=0;i<draws.size();++i) {
            const auto& draw=draws[i];if(!draw.skin || (!draw.camera_visible && !draw.shadow_mask))continue;
            const auto& deformation=*draw.skin;commands->writeBuffer(deformation.palette,deformation.joints.data(),deformation.joints.size()*sizeof(GpuJoint));
            nvrhi::ComputeState state;state.pipeline=skin_pipeline;state.bindings.push_back(deformation.bindings);commands->setComputeState(state);
            const std::uint32_t parameters[4]={static_cast<std::uint32_t>(deformation.mesh->vertices.size()),static_cast<std::uint32_t>(deformation.joints.size()),static_cast<std::uint32_t>(i),0};
            commands->setPushConstants(parameters,sizeof(parameters));commands->dispatch((parameters[0]+63)/64);
        }
        commands->copyBuffer(skin_readback,0,skin_errors,0,16);
    }
    void validate_skin_dispatch() {
        if(!pending_draws.skinned_instances)return;
        const void* data=checked->mapBuffer(skin_readback,nvrhi::CpuAccessMode::Read);require(data!=nullptr,"Skinning status readback failed.");
        std::uint32_t errors[4]{};std::memcpy(errors,data,sizeof(errors));checked->unmapBuffer(skin_readback);
        if(errors[0]) {
            const auto id=errors[1]<draws.size() ? draws[errors[1]].entity_id : std::string("unknown");
            throw std::runtime_error("GPU skinning rejected instance "+id+" vertex "+std::to_string(errors[2])+": singular, nonfinite or out-of-range result; capture was not published.");
        }
    }
    nvrhi::TextureHandle upload_texture(const std::shared_ptr<const TextureImage>& image) {
        if(const auto found=texture_cache.find(image.get());found!=texture_cache.end())return found->second;
        std::size_t bytes=0;for(const auto& mip:image->mips)bytes+=mip.rgba.size();
        require(bytes<=256*1024*1024-texture_bytes,"GPU texture data exceeds the initial 256 MiB budget.");
        nvrhi::TextureDesc desc;desc.width=image->mips.front().width;desc.height=image->mips.front().height;desc.mipLevels=static_cast<std::uint32_t>(image->mips.size());
        desc.format=image->srgb ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM;
        desc.initialState=nvrhi::ResourceStates::ShaderResource;desc.keepInitialState=true;desc.debugName="Cooked material texture";
        auto texture=checked->createTexture(desc);require(bool(texture),"Texture allocation failed.");
        commands->open();
        for(std::uint32_t level=0;level<image->mips.size();++level) { const auto& mip=image->mips[level];commands->writeTexture(texture,0,level,mip.rgba.data(),std::size_t(mip.width)*4); }
        commands->close();checked->executeCommandList(commands);require(checked->waitForIdle(),"Texture upload failed.");
        texture_bytes+=bytes;texture_cache.emplace(image.get(),texture);return texture;
    }
    nvrhi::BindingSetHandle mesh_bindings(const MeshAsset* mesh,const MaterialTextures* override) {
        const auto key_material=std::make_pair(mesh,override);
        if(const auto found=material_cache.find(key_material);found!=material_cache.end())return found->second;
        auto desc=nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0,sizeof(DrawConstants))).addItem(nvrhi::BindingSetItem::ConstantBuffer(1,frame_buffer));
        for(std::uint32_t slot=0;slot<5;++slot) {
            const TextureMap map=override ? override->maps[slot] : mesh ? mesh->textures[slot] : TextureMap{};
            const auto texture=upload_texture(map.image ? map.image : white_image);
            const std::array<int,4> key{map.wrap_s,map.wrap_t,map.min_filter,map.mag_filter};
            auto& sampler=sampler_cache[key];
            if(!sampler) {
                auto address=[](int v) { return v==33071 ? nvrhi::SamplerAddressMode::Clamp : v==33648 ? nvrhi::SamplerAddressMode::Mirror : nvrhi::SamplerAddressMode::Wrap; };
                nvrhi::SamplerDesc sd;sd.addressU=address(map.wrap_s);sd.addressV=address(map.wrap_t);
                sd.minFilter=map.min_filter==9729 || map.min_filter==9985 || map.min_filter==9987;sd.magFilter=map.mag_filter==9729;
                sd.mipFilter=map.min_filter==9986 || map.min_filter==9987;sampler=checked->createSampler(sd);require(bool(sampler),"Texture sampler creation failed.");
            }
            auto levels=nvrhi::AllSubresources;if(map.min_filter==9728 || map.min_filter==9729)levels.setMipLevels(0,1);
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(slot,texture,nvrhi::Format::UNKNOWN,levels)).addItem(nvrhi::BindingSetItem::Sampler(slot,sampler));
        }
        desc.addItem(nvrhi::BindingSetItem::Texture_SRV(5,shadow_texture));
        auto result=checked->createBindingSet(desc,binding_layout);require(bool(result),"Material texture bindings failed.");material_cache.emplace(key_material,result);return result;
    }
    void retain_scene_resources() {
        std::set<const MeshAsset*> meshes;
        std::set<const MaterialTextures*> materials;
        std::set<const TextureImage*> images_used;
        std::set<std::string> skin_ids;
        std::set<std::pair<const MeshAsset*,const MaterialTextures*>> combinations{{nullptr,nullptr}};
        std::set<std::array<int,4>> samplers;
        auto retain_image=[&](const std::shared_ptr<const TextureImage>& image) {
            if(image) { images_used.insert(image.get());image_owners[image.get()]=image; }
        };
        retain_image(white_image);
        const TextureMap fallback;
        samplers.insert({fallback.wrap_s,fallback.wrap_t,fallback.min_filter,fallback.mag_filter});
        for(const auto& object:scene->objects) {
            if(object.skin)skin_ids.insert(object.entity_id);
            if(object.mesh) { meshes.insert(object.mesh.get());mesh_owners[object.mesh.get()]=object.mesh; }
            if(object.textures) { materials.insert(object.textures.get());material_owners[object.textures.get()]=object.textures; }
            combinations.emplace(object.mesh.get(),object.textures.get());
            for(std::size_t slot=0;slot<5;++slot) {
                const auto map=object.textures ? object.textures->maps[slot] : object.mesh ? object.mesh->textures[slot] : TextureMap{};
                retain_image(map.image);samplers.insert({map.wrap_s,map.wrap_t,map.min_filter,map.mag_filter});
            }
        }
        // Owner maps retain old pointer identities until the associated GPU
        // cache entries are gone, preventing allocator-address reuse hits.
        std::erase_if(material_cache,[&](const auto& item){return !combinations.contains(item.first);});
        std::erase_if(geometry_cache,[&](const auto& item){return !meshes.contains(item.first);});
        std::erase_if(bounds_cache,[&](const auto& item){return item.first && !meshes.contains(item.first);});
        std::erase_if(skin_sources,[&](const auto& item){return !meshes.contains(item.first);});
        std::erase_if(skin_instances,[&](const auto& item){if(skin_ids.contains(item.first))return false;skin_bytes-=item.second.bytes;return true;});
        std::erase_if(texture_cache,[&](const auto& item){return !images_used.contains(item.first);});
        std::erase_if(sampler_cache,[&](const auto& item){return !samplers.contains(item.first);});
        std::erase_if(mesh_owners,[&](const auto& item){return !meshes.contains(item.first);});
        std::erase_if(material_owners,[&](const auto& item){return !materials.contains(item.first);});
        std::erase_if(image_owners,[&](const auto& item){return !images_used.contains(item.first);});
        texture_bytes=0;
        for(const auto& [image,unused]:texture_cache) { (void)unused;for(const auto& mip:image_owners.at(image)->mips)texture_bytes+=mip.rgba.size(); }
    }
    void update_scene() {
        const auto started=SteadyClock::now();draws.clear();pending_draws={};
        retain_scene_resources();prepare_shadows();bindings=mesh_bindings(nullptr,nullptr);
        auto lighting=scene->lighting;finalize_lighting(lighting);
        require(lighting.lights.size()<=max_scene_lights,"Too many lights for the forward renderer.");
        const double aspect=scene_viewport ? static_cast<double>(scene_viewport->maxX-scene_viewport->minX)/(scene_viewport->maxY-scene_viewport->minY) : static_cast<double>(extent.width)/extent.height;
        const auto vp=multiply(perspective(scene->vertical_fov,aspect,scene->near_plane,scene->far_plane),inverse_affine(scene->camera_world));
        auto number=[](double value) { require(std::isfinite(value) && std::abs(value)<=std::numeric_limits<float>::max(),"Scene matrix exceeds GPU float range.");return static_cast<float>(value); };
        for(std::size_t k=0;k<16;++k)frame_constants.view_projection[k]=number(vp[k]);
        for(std::size_t k=0;k<3;++k)frame_constants.camera[k]=number(scene->camera_world[12+k]);
        frame_constants.camera[3]=(format==vk::Format::eB8G8R8A8Srgb || format==vk::Format::eR8G8B8A8Srgb) ? 1.0f : 0.0f;
        for(std::size_t k=0;k<3;++k)frame_constants.ambient_exposure[k]=lighting.environment.ambient[k];
        frame_constants.ambient_exposure[3]=lighting.environment.exposure;
        frame_constants.light_count[0]=static_cast<std::uint32_t>(lighting.lights.size());
        frame_constants.light_count[1]=lighting.environment.shadow_resolution;
        for(std::size_t k=0;k<3;++k)frame_constants.camera_forward[k]=number(-scene->camera_world[8+k]);
        shadow_plan=shadow_views(*scene,aspect);
        frame_constants.light_count[2]=static_cast<std::uint32_t>(shadow_plan.size());
        for(std::size_t i=0;i<lighting.lights.size();++i) {
            const auto& source=lighting.lights[i];validate_light(source.light);auto& light=frame_constants.lights[i];light={};
            for(std::size_t k=0;k<3;++k) { light.position_kind[k]=number(source.position[k]);light.direction_range[k]=number(source.direction[k]);light.color_intensity[k]=source.light.color[k]; }
            light.position_kind[3]=static_cast<float>(source.light.kind);light.direction_range[3]=source.light.range;light.color_intensity[3]=source.light.intensity;
            light.cone[0]=std::cos(source.light.inner_angle*0.017453292519943295f);light.cone[1]=std::cos(source.light.outer_angle*0.017453292519943295f);
            light.cone[2]=source.light.shadow.distance;light.cone[3]=source.light.shadow.near_plane;
            light.shadow[2]=source.light.shadow.bias;light.shadow[3]=source.light.shadow.normal_bias;
        }
        for(std::size_t i=0;i<shadow_plan.size();++i) {
            const auto& source=shadow_plan[i];auto& view=frame_constants.shadows[i];auto& light=frame_constants.lights[source.light_index];
            if(light.shadow[1]==0)light.shadow[0]=static_cast<float>(i);light.shadow[1]+=1;
            for(std::size_t k=0;k<16;++k)view.view_projection[k]=number(source.view_projection[k]);
            view.splits[0]=number(source.split_near);view.splits[1]=number(source.split_far);
        }
        // Use the actual rounded GPU matrices for clipping decisions.
        auto frustum=[](const float* matrix) { Matrix4 value;std::copy_n(matrix,16,value.begin());return make_frustum(value); };
        const auto camera_frustum=frustum(frame_constants.view_projection);
        std::vector<Frustum> shadow_frusta;for(std::size_t i=0;i<shadow_plan.size();++i)shadow_frusta.push_back(frustum(frame_constants.shadows[i].view_projection));
        pending_draws.objects=scene->objects.size();pending_draws.shadow_views=shadow_plan.size();pending_draws.shadow_candidates=scene->objects.size()*shadow_plan.size();
        std::set<std::string> active_skins;
        for(const auto& object:scene->objects) {
            DrawItem item;item.entity_id=object.entity_id;auto& draw=item.constants;const auto inverse=inverse_affine(object.world);
            for(std::size_t row=0;row<3;++row) {
                for(std::size_t col=0;col<4;++col)draw.model[row][col]=number(object.world[col*4+row]);
                for(std::size_t col=0;col<3;++col)draw.normal[row][col]=number(inverse[row*4+col]);
            }
            if(object.material || !lighting.preview) {
                PbrMaterial m;if(object.material)m=*object.material;else { m.base_color=object.albedo;m.metallic=0; }
                for(std::size_t k=0;k<3;++k) { draw.base_metallic[k]=m.base_color[k];draw.emissive_roughness[k]=m.emissive[k]; }
                draw.base_metallic[3]=m.metallic;draw.emissive_roughness[3]=m.roughness;item.cull=!m.double_sided;
            } else { for(std::size_t k=0;k<3;++k)draw.base_metallic[k]=object.albedo[k];draw.base_metallic[3]=-1; }
            draw.normal[0][3]=object.textures ? object.textures->occlusion_strength : object.mesh ? object.mesh->occlusion_strength : 1.0f;
            draw.normal[1][3]=object.textures ? object.textures->normal_scale : object.mesh ? object.mesh->normal_scale : 1.0f;
            draw.normal[2][3]=(object.textures ? bool(object.textures->maps[4].image) : object.mesh && object.mesh->textures[4].image) ? 1.0f : 0.0f;
            item.bindings=mesh_bindings(object.mesh.get(),object.textures.get());
            item.geometry=mesh_geometry(object.mesh);
            Bounds local_bounds;
            if(object.skin) {
                require(object.mesh && !object.mesh->influences.empty() && active_skins.insert(object.entity_id).second,"Skin snapshot requires a weighted mesh and unique instance ID.");
                auto& source=skin_source(object.mesh);local_bounds=posed_bounds(source.bounds,object.skin->palette);
                item.skin=&skin_instance(object,item.geometry,source);item.geometry.vertices=item.skin->vertices;
            } else {
                require(!object.mesh || object.mesh->influences.empty(),"Weighted geometry requires a skin pose.");
                auto [entry,inserted]=bounds_cache.try_emplace(object.mesh.get());if(inserted)entry->second=mesh_bounds(object.mesh.get());
                local_bounds=entry->second;
            }
            const auto bounds=transform_bounds(local_bounds,object.world);
            item.camera_visible=!diagnostics.culling || intersects(bounds,camera_frustum);
            if(item.camera_visible) { ++pending_draws.camera_draws;pending_draws.camera_triangles+=item.geometry.count/3; }else ++pending_draws.camera_culled;
            for(std::size_t layer=0;layer<shadow_frusta.size();++layer) {
                if(!diagnostics.culling || intersects(bounds,shadow_frusta[layer])) { item.shadow_mask|=1u<<layer;++pending_draws.shadow_draws;pending_draws.shadow_triangles+=item.geometry.count/3; }
                else ++pending_draws.shadow_culled;
            }
            if(item.skin && (item.camera_visible || item.shadow_mask)) { ++pending_draws.skinned_instances;pending_draws.skinned_vertices+=object.mesh->vertices.size(); }
            draws.push_back(std::move(item));
        }
        for(auto it=skin_instances.begin();it!=skin_instances.end();) {
            if(!active_skins.contains(it->first)) { skin_bytes-=it->second.bytes;it=skin_instances.erase(it); }else ++it;
        }
        if(diagnostics.profile_requested)timing_sample(diagnostics.prepare_cpu,elapsed_ms(started));
    }

    bool rebuild(const RenderOptions& options) {
        swapchain_dirty=true;
        device.waitIdle();
        commands=nullptr;
#if POIMA_EDITOR
        ui_framebuffers.clear();
#endif
        framebuffers.clear(); images.clear(); depth=nullptr; multisample_color=nullptr; staging=nullptr;
        checked->runGarbageCollection();
        for(auto semaphore:finished) device.destroySemaphore(semaphore);
        finished.clear(); initialized.clear();
        device.destroySemaphore(acquired); acquired=nullptr;
        device.destroySwapchainKHR(swapchain); swapchain=nullptr;
        const auto previous_format=format;
        if(!create_swapchain(options)) { swapchain_dirty=true; return false; }
        if(format!=previous_format) { renderer_fault=true;throw std::runtime_error("Surface format changed; recreate the renderer session."); }
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
#if POIMA_EDITOR
            if(editor) {
                auto ui_framebuffer=checked->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
                require(bool(ui_framebuffer),"Editor UI framebuffer creation failed.");ui_framebuffers.push_back(ui_framebuffer);
            }
#endif
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
#if POIMA_EDITOR
        if(editor) {
            try { require(image!=nullptr,std::string("BMP capture surface: ")+SDL_GetError());save_editor_bmp_exclusive(image,path); }
            catch(...) { if(image)SDL_DestroySurface(image);checked->unmapStagingTexture(staging);throw; }
            SDL_DestroySurface(image);checked->unmapStagingTexture(staging);return;
        }
#endif
        const bool saved = image && SDL_SaveBMP(image, path.c_str());
        const std::string detail = saved ? "" : std::string("BMP capture: ") + SDL_GetError();
        if (image) SDL_DestroySurface(image);
        checked->unmapStagingTexture(staging);
        require(saved, detail);
    }

#if POIMA_EDITOR
    void prepare_ui() {
        require(ImGui::GetCurrentContext()!=nullptr,"Create an ImGui context before the editor viewport.");
        ui_context=ImGui::GetCurrentContext();
        unsigned char* pixels=nullptr;int width=0,height=0;
        auto& io=ImGui::GetIO();io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
        require(pixels && width>0 && height>0,"Editor font atlas is empty.");
        nvrhi::TextureDesc td;td.width=static_cast<std::uint32_t>(width);td.height=static_cast<std::uint32_t>(height);
        td.format=nvrhi::Format::RGBA8_UNORM;td.initialState=nvrhi::ResourceStates::ShaderResource;td.keepInitialState=true;td.debugName="Editor font atlas";
        ui_font=checked->createTexture(td);require(bool(ui_font),"Editor font texture creation failed.");
        commands->open();commands->writeTexture(ui_font,0,0,pixels,static_cast<std::size_t>(width)*4);commands->close();checked->executeCommandList(commands);
        require(checked->waitForIdle(),"Editor font upload failed.");
        nvrhi::SamplerDesc sd;sd.addressU=sd.addressV=nvrhi::SamplerAddressMode::Clamp;sd.minFilter=sd.magFilter=true;
        ui_sampler=checked->createSampler(sd);
        ui_vs=create_embedded_shader(checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main"),poima_editor_ui_vs);
        ui_ps=create_embedded_shader(checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main"),poima_editor_ui_ps);
        const nvrhi::VertexAttributeDesc attrs[]={
            nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(ImDrawVert,pos)).setElementStride(sizeof(ImDrawVert)),
            nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(ImDrawVert,uv)).setElementStride(sizeof(ImDrawVert)),
            nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGBA8_UNORM).setOffset(offsetof(ImDrawVert,col)).setElementStride(sizeof(ImDrawVert))};
        ui_input=checked->createInputLayout(attrs,3,ui_vs);
        ui_layout=checked->createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All)
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0,32)).addItem(nvrhi::BindingLayoutItem::Texture_SRV(0)).addItem(nvrhi::BindingLayoutItem::Sampler(0)));
        require(ui_vs && ui_ps && ui_input && ui_layout && ui_sampler,"Editor UI shader/layout creation failed.");
        ui_bindings=checked->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0,32))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(0,ui_font)).addItem(nvrhi::BindingSetItem::Sampler(0,ui_sampler)),ui_layout);
        nvrhi::GraphicsPipelineDesc pd;pd.VS=ui_vs;pd.PS=ui_ps;pd.inputLayout=ui_input;pd.bindingLayouts.push_back(ui_layout);
        pd.renderState.depthStencilState.depthTestEnable=false;pd.renderState.depthStencilState.depthWriteEnable=false;
        pd.renderState.rasterState.cullMode=nvrhi::RasterCullMode::None;
        pd.renderState.rasterState.scissorEnable=true;
        auto& blend=pd.renderState.blendState.targets[0];blend.blendEnable=true;blend.srcBlend=nvrhi::BlendFactor::SrcAlpha;blend.destBlend=nvrhi::BlendFactor::InvSrcAlpha;
        blend.srcBlendAlpha=nvrhi::BlendFactor::One;blend.destBlendAlpha=nvrhi::BlendFactor::InvSrcAlpha;
        ui_pipeline=checked->createGraphicsPipeline(pd,ui_framebuffers.front()->getFramebufferInfo());
        require(ui_bindings && ui_pipeline,"Editor UI pipeline creation failed.");
        io.Fonts->SetTexID(static_cast<ImTextureID>(1));io.BackendFlags|=ImGuiBackendFlags_RendererHasVtxOffset;
        io.BackendRendererName="poima_nvrhi";
    }
    void prepare_ui_frame(const ImDrawData* data) {
        ui_data=data;ui_vertex_data.clear();ui_index_data.clear();
        if(!data || data->TotalVtxCount==0)return;
        require(data->Valid && data->TotalVtxCount>=0 && data->TotalIdxCount>=0 && data->TotalVtxCount<=4000000 && data->TotalIdxCount<=12000000,"Invalid or oversized editor UI draw data.");
        require(std::isfinite(data->DisplaySize.x) && std::isfinite(data->DisplaySize.y) && data->DisplaySize.x>0 && data->DisplaySize.y>0,"Invalid editor UI display size.");
        ui_vertex_data.reserve(static_cast<std::size_t>(data->TotalVtxCount));ui_index_data.reserve(static_cast<std::size_t>(data->TotalIdxCount));
        for(int list=0;list<data->CmdListsCount;++list) {
            const auto* source=data->CmdLists[list];
            ui_vertex_data.insert(ui_vertex_data.end(),source->VtxBuffer.begin(),source->VtxBuffer.end());
            ui_index_data.insert(ui_index_data.end(),source->IdxBuffer.begin(),source->IdxBuffer.end());
            for(const auto& command:source->CmdBuffer) {
                require(!command.UserCallback || command.UserCallback==ImDrawCallback_ResetRenderState,"Custom editor UI draw callbacks are unavailable.");
                if(command.UserCallback)continue;
                require(command.GetTexID()==static_cast<ImTextureID>(1),"Editor UI currently accepts only the font atlas texture.");
                require(std::isfinite(command.ClipRect.x) && std::isfinite(command.ClipRect.y) && std::isfinite(command.ClipRect.z) && std::isfinite(command.ClipRect.w),"Editor UI clip rectangle must be finite.");
                require(std::uint64_t(command.IdxOffset)+command.ElemCount<=static_cast<std::uint64_t>(source->IdxBuffer.Size) && command.VtxOffset<=static_cast<unsigned>(source->VtxBuffer.Size),"Editor UI command exceeds its draw list.");
            }
        }
        require(ui_vertex_data.size()==static_cast<std::size_t>(data->TotalVtxCount) && ui_index_data.size()==static_cast<std::size_t>(data->TotalIdxCount),"Editor UI totals do not match draw lists.");
        auto allocate=[&](nvrhi::BufferHandle& buffer,std::size_t& capacity,std::size_t bytes,bool vertex) {
            if(buffer && capacity>=bytes)return;
            capacity=bytes+16384;nvrhi::BufferDesc desc;desc.byteSize=capacity;desc.isVertexBuffer=vertex;desc.isIndexBuffer=!vertex;
            desc.initialState=vertex ? nvrhi::ResourceStates::VertexBuffer : nvrhi::ResourceStates::IndexBuffer;desc.keepInitialState=true;
            buffer=checked->createBuffer(desc);require(bool(buffer),"Editor UI buffer allocation failed.");
        };
        allocate(ui_vertices,ui_vertex_capacity,ui_vertex_data.size()*sizeof(ImDrawVert),true);
        allocate(ui_indices,ui_index_capacity,ui_index_data.size()*sizeof(ImDrawIdx),false);
    }
    void render_ui(std::uint32_t image_index) {
        if(!ui_data || ui_vertex_data.empty())return;
        commands->writeBuffer(ui_vertices,ui_vertex_data.data(),ui_vertex_data.size()*sizeof(ImDrawVert));
        if(!ui_index_data.empty())commands->writeBuffer(ui_indices,ui_index_data.data(),ui_index_data.size()*sizeof(ImDrawIdx));
        const float sx=2.0f/ui_data->DisplaySize.x,sy=-2.0f/ui_data->DisplaySize.y;
        const float constants[8]={sx,sy,-1.0f-ui_data->DisplayPos.x*sx,1.0f-ui_data->DisplayPos.y*sy,
            (format==vk::Format::eB8G8R8A8Srgb || format==vk::Format::eR8G8B8A8Srgb) ? 1.0f : 0.0f,0,0,0};
        std::uint32_t vertex_offset=0,index_offset=0;
        for(int list=0;list<ui_data->CmdListsCount;++list) {
            const auto* source=ui_data->CmdLists[list];
            for(const auto& draw:source->CmdBuffer) {
                if(draw.UserCallback || draw.ElemCount==0)continue;
                auto x=[&](float value){return std::clamp((value-ui_data->DisplayPos.x)*ui_data->FramebufferScale.x,0.0f,static_cast<float>(extent.width));};
                auto y=[&](float value){return std::clamp((value-ui_data->DisplayPos.y)*ui_data->FramebufferScale.y,0.0f,static_cast<float>(extent.height));};
                const int left=static_cast<int>(std::floor(x(draw.ClipRect.x))),right=static_cast<int>(std::ceil(x(draw.ClipRect.z)));
                const int top=static_cast<int>(std::floor(y(draw.ClipRect.y))),bottom=static_cast<int>(std::ceil(y(draw.ClipRect.w)));
                if(right<=left || bottom<=top)continue;
                nvrhi::GraphicsState state;state.pipeline=ui_pipeline;state.framebuffer=ui_framebuffers.at(image_index);state.bindings.push_back(ui_bindings);
                state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(ui_vertices));
                state.indexBuffer=nvrhi::IndexBufferBinding(ui_indices,sizeof(ImDrawIdx)==2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT,0);
                state.viewport.addViewport(nvrhi::Viewport(static_cast<float>(extent.width),static_cast<float>(extent.height)));
                state.viewport.addScissorRect(nvrhi::Rect(left,right,top,bottom));commands->setGraphicsState(state);commands->setPushConstants(constants,sizeof(constants));
                commands->drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.ElemCount).setStartIndexLocation(index_offset+draw.IdxOffset).setStartVertexLocation(vertex_offset+draw.VtxOffset));
            }
            vertex_offset+=static_cast<std::uint32_t>(source->VtxBuffer.Size);index_offset+=static_cast<std::uint32_t>(source->IdxBuffer.Size);
        }
    }
#endif
    bool frame(bool capture_frame) {
        require(!renderer_fault,"Renderer synchronization failed; recreate the renderer session.");
        const auto frame_started=SteadyClock::now();
        if(!swapchain) { swapchain_dirty=true; return false; }
        // A finite acquire timeout bounds the experiment if presentation stalls.
        vk::ResultValue<std::uint32_t> next(vk::Result::eSuccess,0);
        try { next=device.acquireNextImageKHR(swapchain, editor ? 16'000'000ULL : 5'000'000'000ULL, acquired, {}); }
        catch(const vk::OutOfDateKHRError&) { swapchain_dirty=true; return false; }
        if(editor && (next.result==vk::Result::eTimeout || next.result==vk::Result::eNotReady))return false;
        require(next.result == vk::Result::eSuccess || next.result == vk::Result::eSuboptimalKHR,
            "Swapchain acquisition failed or timed out.");
        // Once acquired, an exception may leave a signaled binary semaphore
        // or an open/submitted command list. Recreating the session is the
        // supported recovery; retrying the same resources is not safe.
        struct FrameFailure {
            bool& failed;bool complete=false;
            ~FrameFailure() { if(!complete)failed=true; }
        } failure{renderer_fault};
        const auto index = next.value;
        auto texture = images.at(index);
        native->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, acquired, 0);
        const auto record_started=SteadyClock::now();commands->open();
        if(timestamp_pool)native_commands().resetQueryPool(timestamp_pool,0,5);
        timestamp(0);
        if(scene) { commands->writeBuffer(frame_buffer,&frame_constants,sizeof(frame_constants));dispatch_skinning(); }
        timestamp(1);
        if(scene)render_shadows();
        timestamp(2);
        commands->beginTrackingTextureState(texture, nvrhi::AllSubresources,
            initialized[index] ? nvrhi::ResourceStates::Present : nvrhi::ResourceStates::Common);
        commands->clearTextureFloat(multisample_color ? multisample_color.Get() : texture.Get(), nvrhi::AllSubresources, nvrhi::Color(0.025f, 0.035f, 0.055f, 1.0f));
        if (depth) commands->clearDepthStencilTexture(depth, nvrhi::AllSubresources, true, 1.0f, false, 0);
        nvrhi::GraphicsState state;
        state.pipeline = pipeline;
        state.framebuffer = framebuffers[index];
        state.viewport.addViewportAndScissorRect(scene_viewport.value_or(nvrhi::Viewport(static_cast<float>(extent.width), static_cast<float>(extent.height))));
        if (scene) {
            state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(vertices).setSlot(0).setOffset(0));
            state.bindings.push_back(bindings);
        }
        commands->setGraphicsState(state);
        if (scene) {
            for (const auto& draw : draws) {
                if(!scene_visible || !draw.camera_visible)continue;
                state.pipeline=draw.cull ? culled_pipeline : pipeline;state.bindings[0]=draw.bindings;
                state.vertexBuffers[0].buffer=draw.geometry.vertices;
                state.indexBuffer=draw.geometry.indices ? nvrhi::IndexBufferBinding(draw.geometry.indices,nvrhi::Format::R32_UINT,0) : nvrhi::IndexBufferBinding();
                commands->setGraphicsState(state);
                commands->setPushConstants(&draw.constants,sizeof(draw.constants));
                if(draw.geometry.indices)commands->drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.geometry.count));
                else commands->draw(nvrhi::DrawArguments().setVertexCount(draw.geometry.count));
            }
        } else commands->draw(nvrhi::DrawArguments().setVertexCount(3));
        timestamp(3);
        if(scene && multisample_color)commands->resolveTexture(texture,nvrhi::AllSubresources,multisample_color,nvrhi::AllSubresources);
#if POIMA_EDITOR
        if(editor)render_ui(index);
#endif
        if (capture_frame) commands->copyTexture(staging, {}, texture, {});
        commands->setTextureState(texture, nvrhi::AllSubresources, nvrhi::ResourceStates::Present);
        commands->commitBarriers();timestamp(4);
        commands->close();
        const auto record_ms=elapsed_ms(record_started);
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
        validate_skin_dispatch();
        checked->runGarbageCollection();
        require(messages.errors == 0, "NVRHI reported a validation/backend error; inspect stderr.");
        ++diagnostics.completed_submissions;diagnostics.last_draws=pending_draws;
        if(diagnostics.profile_requested) {
            const auto frame_ms=elapsed_ms(frame_started);timing_sample(diagnostics.record_cpu,record_ms);timing_sample(diagnostics.render_call_cpu,frame_ms);collect_timestamps(frame_ms);
        }
        failure.complete=true;
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
    report.validation_errors = context.messages.errors;report.diagnostics=context.diagnostics;
    return report;
}
RenderReport run_render_smoke(const RenderOptions& options) { return render(options, nullptr); }
RenderReport run_render_scene(const RenderOptions& options, const SceneSnapshot& scene) { return render(options, &scene); }

#if POIMA_EDITOR
struct EditorViewport::Impl {
    RenderOptions options;
    SceneSnapshot snapshot;
    Context context;
    RenderReport result;
    Impl(const RenderOptions& requested,const SceneSnapshot& scene):options(requested),snapshot(scene) {
        require(ImGui::GetCurrentContext()!=nullptr,"Create an ImGui context before the editor viewport.");
        context.editor=true;context.initialize(options,&snapshot,true);context.prepare_ui();
        SDL_SetWindowTitle(context.window,"Poima Editor");
        result.hardware=context.hardware;result.gpu_name=context.gpu_name;result.samples=context.samples;
        result.width=context.extent.width;result.height=context.extent.height;
        result.detail="Native editor viewport; serialized Vulkan presentation, no frame-time qualification.";
    }
};
EditorViewport::EditorViewport(const RenderOptions& options,const SceneSnapshot& scene):impl_(std::make_unique<Impl>(options,scene)) {}
EditorViewport::~EditorViewport()=default;
void* EditorViewport::native_window() const { return impl_->context.window; }
std::array<std::uint32_t,2> EditorViewport::extent() const { return {impl_->context.extent.width,impl_->context.extent.height}; }
void EditorViewport::resize() { impl_->context.swapchain_dirty=true; }
bool EditorViewport::draw(const SceneSnapshot& scene,EditorRect viewport,const ImDrawData* ui,bool capture) {
    auto& state=*impl_;auto& context=state.context;
    try {
        require(!context.renderer_fault && context.messages.errors==0,"Renderer fault; recreate the editor viewport before drawing again.");
        require(ImGui::GetCurrentContext()==context.ui_context,"The editor viewport's ImGui context must remain current until teardown.");
        require(!capture || !state.options.capture.empty(),"Editor capture requires a configured output path.");
        for(float value:{viewport.x,viewport.y,viewport.width,viewport.height})require(std::isfinite(value),"Editor viewport rectangle must be finite.");
        float scale_x=1,scale_y=1,origin_x=0,origin_y=0;
        if(ui) {
            scale_x=ui->FramebufferScale.x;scale_y=ui->FramebufferScale.y;origin_x=ui->DisplayPos.x;origin_y=ui->DisplayPos.y;
            require(std::isfinite(scale_x) && std::isfinite(scale_y) && scale_x>0 && scale_y>0 && std::isfinite(origin_x) && std::isfinite(origin_y),"Invalid editor UI scale/origin.");
        }
        int width=0,height=0;require(SDL_GetWindowSizeInPixels(context.window,&width,&height),SDL_GetError());
        if(width<=0 || height<=0 || (SDL_GetWindowFlags(context.window)&SDL_WINDOW_MINIMIZED))return false;
        if(context.swapchain_dirty || static_cast<std::uint32_t>(width)!=context.extent.width || static_cast<std::uint32_t>(height)!=context.extent.height) {
            auto resized=state.options;resized.width=static_cast<std::uint32_t>(width);resized.height=static_cast<std::uint32_t>(height);
            if(!context.rebuild(resized))return false;
        }
        auto x=[&](double value){return static_cast<float>(std::clamp((value-origin_x)*scale_x,0.0,static_cast<double>(context.extent.width)));};
        auto y=[&](double value){return static_cast<float>(std::clamp((value-origin_y)*scale_y,0.0,static_cast<double>(context.extent.height)));};
        const float left=x(viewport.x),right=x(static_cast<double>(viewport.x)+std::max(viewport.width,0.0f));
        const float top=y(viewport.y),bottom=y(static_cast<double>(viewport.y)+std::max(viewport.height,0.0f));
        context.scene_visible=right>left && bottom>top;
        context.scene_viewport=context.scene_visible ? std::optional<nvrhi::Viewport>(nvrhi::Viewport(left,right,top,bottom,0,1)) : std::nullopt;
        state.snapshot=scene;context.scene=&state.snapshot;
        // Preparation can upload resources using this command list. Its failure
        // may occur after opening/submitting work, so conservatively invalidate
        // this graphics lifetime rather than attempting an unsafe retry.
        try { context.update_scene();context.prepare_ui_frame(ui); }
        catch(...) { context.renderer_fault=true;throw; }
        if(!context.frame(capture))return false;
        ++state.result.frames_presented;state.result.width=context.extent.width;state.result.height=context.extent.height;
        if(capture) { context.capture(state.options.capture);state.result.capture_written=true; }
        state.result.success=true;
        state.result.detail="Native editor viewport; serialized Vulkan presentation, no frame-time qualification.";
        return true;
    } catch(const std::exception& error) { state.result.success=false;state.result.detail=error.what();throw; }
}
RenderReport EditorViewport::report() const {
    auto result=impl_->result;result.validation_errors=impl_->context.messages.errors;result.diagnostics=impl_->context.diagnostics;return result;
}
EditorViewportResources EditorViewport::resources() const {
    const auto& c=impl_->context;
    return {c.geometry_cache.size(),c.material_cache.size(),c.texture_cache.size(),c.skin_instances.size(),c.texture_bytes,c.skin_bytes};
}
#endif

PlayerReport run_player(const PlayerOptions& options, Runtime& runtime) {
    PlayerReport result;
    auto& report=result.render;
    result.initial_tick=runtime.inspect().tick;
    Context context;
    SceneSnapshot snapshot;
    PlayerClock clock;
    BoundPlayerInput input(options.input_profile ? *options.input_profile : default_gamepad_input_profile());
    struct GamepadBinding { GamepadHost* host=nullptr; ~GamepadBinding() { if(host)try { host->stop(); }catch(...) {} } } gamepad_binding;
    auto* gamepads=options.replay ? nullptr : options.gamepad_host.get();
    result.gamepad_json=options.replay ? "{\"mode\":\"replay\",\"assigned\":null}" : "{\"mode\":\"disabled\",\"assigned\":null}";
    std::unique_ptr<PlayerAudio> audio;
    try {
        snapshot=runtime.snapshot(options.camera);
        context.initialize(options.render,&snapshot,true);
        SDL_SetWindowTitle(context.window,options.replay ? "Poima player — recorded input replay" : "Poima player — configured controls — Esc exits, Tab pauses, click or gamepad Start resumes");
        if(options.audio)audio=std::make_unique<PlayerAudio>(runtime,options.camera);
        bool focused=(SDL_GetWindowFlags(context.window)&SDL_WINDOW_INPUT_FOCUS)!=0;
        bool captured=!options.replay && focused;
        bool active=captured;
        if(gamepads) { gamepad_binding.host=gamepads;gamepads->start(input,options.gamepad_selection);gamepads->activate(active); }
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
                    focused=false; captured=false; active=false; input.clear();if(gamepads)gamepads->activate(false);
                    if(!options.replay) SDL_SetWindowRelativeMouseMode(context.window,false);
                }
                if(event.type==SDL_EVENT_WINDOW_FOCUS_GAINED) focused=true;
                if(event.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) context.swapchain_dirty=true;
                if(options.replay) continue;
                if(gamepads) {
                    if(event.type==SDL_EVENT_GAMEPAD_ADDED) { gamepads->added(event.gdevice.which);continue; }
                    if(event.type==SDL_EVENT_GAMEPAD_REMOVED) { gamepads->removed(event.gdevice.which);continue; }
                    if(event.type==SDL_EVENT_GAMEPAD_REMAPPED) { gamepads->remapped(event.gdevice.which);continue; }
                    if(event.type==SDL_EVENT_GAMEPAD_AXIS_MOTION) { gamepads->axis(event.gaxis.which,event.gaxis.axis,event.gaxis.value);continue; }
                    if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type==SDL_EVENT_GAMEPAD_BUTTON_UP) {
                        if(gamepads->button(event.gbutton.which,event.gbutton.button,event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN) && focused) {
                            active=!active;input.clear();gamepads->activate(active);
                            if(!active) { captured=false;SDL_SetWindowRelativeMouseMode(context.window,false); }
                        }
                        continue;
                    }
                }
                if(event.type==SDL_EVENT_KEY_DOWN && event.key.scancode==SDL_SCANCODE_TAB) {
                    captured=false;active=false;input.clear();if(gamepads)gamepads->activate(false);SDL_SetWindowRelativeMouseMode(context.window,false);
                }
                if(event.type==SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button==SDL_BUTTON_LEFT && focused && !captured) {
                    const bool resuming=!active;require(SDL_SetWindowRelativeMouseMode(context.window,true),SDL_GetError());captured=true;active=true;
                    if(resuming) { input.clear();if(gamepads)gamepads->activate(true); }continue;
                }
                if(!focused || !active) continue;
                if(event.type==SDL_EVENT_MOUSE_MOTION && captured) input.motion(event.motion.xrel,event.motion.yrel);
                if((event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat) || event.type==SDL_EVENT_KEY_UP)
                    input.control(InputControlKind::keyboard,static_cast<std::uint16_t>(event.key.scancode),event.type==SDL_EVENT_KEY_DOWN);
                if(captured && (event.type==SDL_EVENT_MOUSE_BUTTON_DOWN || event.type==SDL_EVENT_MOUSE_BUTTON_UP))
                    input.control(InputControlKind::mouse,event.button.button,event.type==SDL_EVENT_MOUSE_BUTTON_DOWN);
            }
            if(audio)audio->active(options.replay || (focused && active));
            if(quit) break;
            // Occluded FIFO swapchains can return immediately. Keep an idle
            // editor/player from spinning at thousands of frames per second.
            if(!options.replay && !(focused && active)) SDL_Delay(16);
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
                if(offset!=0) { control.look={0,0}; control.jump=false;control.use=false; }
                runtime.step(1,{control},offset==0 ? options.sequence[segment].motions : std::vector<KinematicTarget>{},offset==0 ? options.sequence[segment].sounds : std::vector<SoundCommand>{});
                if(audio)audio->advance(runtime,options.camera);
                if(++offset==options.sequence[segment].ticks) { offset=0; ++segment; }
            } else {
                const auto ticks=clock.advance(elapsed,focused && active);
                for(std::uint32_t tick=0;tick<ticks;++tick) { runtime.step(1,{input.peek(options.controller)});input.consume(options.controller);if(audio)audio->advance(runtime,options.camera); }
            }
            snapshot=runtime.snapshot(options.camera); context.update_scene();
            if(context.frame(false)) ++report.frames_presented;
            if(options.max_frames && report.frames_presented>=options.max_frames) { result.stop_reason="frame_limit"; break; }
        }
        if(audio)audio->finish(runtime,options.camera);
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
    if(gamepads)result.gamepad_json=gamepads->status_json();
    if(audio)result.audio=audio->report();
    result.final_tick=runtime.inspect().tick; result.dropped_seconds=clock.dropped_seconds();
    report.width=context.extent.width; report.height=context.extent.height; report.samples=context.samples;
    report.hardware=context.hardware; report.gpu_name=context.gpu_name; report.validation_errors=context.messages.errors;report.diagnostics=context.diagnostics;
    return result;
}
} // namespace poima
