// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"
#include "poima/scene.hpp"
#include "poima/player.hpp"
#include "poima/input_profile.hpp"
#include "poima/assets.hpp"
#include "poima/animation.hpp"
#include "poima/editor_viewport.hpp"
#include "poima/hosted_viewport.hpp"
#include "poima/profiler.hpp"
#if POIMA_GAME_UI
#include "poima/ui_presenter.hpp"
#endif
#if POIMA_EDITOR
#include <imgui.h>
#include "poima/editor_ui_vs.hpp"
#include "poima/editor_ui_ps.hpp"
#endif
#include "poima/skinning_cs.hpp"
#include "poima/editor_overlay_vs.hpp"
#include "poima/editor_overlay_ps.hpp"
#include "poima/game_ui_vs.hpp"
#include "poima/game_ui_ps.hpp"
#include <set>
#include <map>
#include "poima/scene_vs.hpp"
#include "poima/scene_ps.hpp"
#include "poima/sky_vs.hpp"
#include "poima/sky_ps.hpp"
#include "poima/shadow_vs.hpp"
#include "poima/smoke_vs.hpp"
#include "poima/smoke_ps.hpp"

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
// Compile-time enforcement: every Poima Vulkan-Hpp call supplies its dispatcher.
#define VULKAN_HPP_NO_DEFAULT_DISPATCHER
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
#include <mutex>
#include <stdexcept>
#include <thread>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
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

void save_bmp_exclusive(SDL_Surface* surface,const std::string& path) {
    require(!path.empty() && path.find('\0')==std::string::npos,"Invalid exclusive capture path.");
    std::unique_ptr<SDL_IOStream,decltype(&SDL_CloseIO)> memory(SDL_IOFromDynamicMem(),SDL_CloseIO);
    require(bool(memory),std::string("BMP memory stream: ")+SDL_GetError());
    require(SDL_SaveBMP_IO(surface,memory.get(),false),std::string("BMP serialization: ")+SDL_GetError());
    const auto size=SDL_GetIOSize(memory.get());
    require(size>0 && size<=512*1024*1024,"BMP exceeds the 512 MiB capture budget.");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    require(SDL_SeekIO(memory.get(),0,SDL_IO_SEEK_SET)==0 && SDL_ReadIO(memory.get(),bytes.data(),bytes.size())==bytes.size(),"Cannot read serialized BMP.");
    const auto output=std::filesystem::path(std::u8string(path.begin(),path.end()));
    // The filesystem performs the absence check and creation in one operation.
    // A competing creator cannot have its file truncated between preflight and
    // publication. Write failure can leave our own partial new file; report it.
#ifdef _WIN32
    HANDLE file=CreateFileW(output.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(file!=INVALID_HANDLE_VALUE,"Capture destination exists or cannot be exclusively created.");
    try {
        std::size_t offset=0;
        while(offset<bytes.size()) {
            const auto length=static_cast<DWORD>(std::min(bytes.size()-offset,std::size_t(1024*1024)));DWORD written=0;
            require(WriteFile(file,bytes.data()+offset,length,&written,nullptr) && written>0,"BMP write failed.");offset+=written;
        }
        require(FlushFileBuffers(file),"BMP flush failed.");
        const bool closed=CloseHandle(file)!=0;file=INVALID_HANDLE_VALUE;require(closed,"BMP close failed.");
    }catch(...) { if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);throw; }
#else
    int file=::open(output.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    require(file>=0,"Capture destination exists or cannot be exclusively created.");
    try {
        std::size_t offset=0;
        while(offset<bytes.size()) {
            const auto written=::write(file,bytes.data()+offset,bytes.size()-offset);
            if(written<0 && errno==EINTR)continue;
            require(written>0,"BMP write failed.");offset+=static_cast<std::size_t>(written);
        }
        require(::fsync(file)==0,"BMP flush failed.");
        const bool closed=::close(file)==0;file=-1;require(closed,"BMP close failed.");
    }catch(...) { if(file>=0)::close(file);throw; }
#endif
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

// NVRHI's pinned static backend uses Vulkan-Hpp's global dispatcher. Keep it
// loaded with INSTANCE-derived loader trampolines, never device-specific entry
// points: these dispatch correctly for every device descended from this shared
// instance. Poima itself uses an explicit per-Context device dispatcher below.
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
        require(dispatch.vkEnumerateInstanceVersion && vk::enumerateInstanceVersion(dispatch)>=VK_API_VERSION_1_3,
            "The renderer experiment needs a Vulkan 1.3 loader.");
        const vk::ApplicationInfo app("Poima",1,"Poima",1,VK_API_VERSION_1_3);
        vk::InstanceCreateInfo info;
        info.pApplicationInfo=&app;info.enabledExtensionCount=count;info.ppEnabledExtensionNames=names;
        instance=vk::createInstance(info,nullptr,dispatch);
        dispatch.init(instance);
        // NVRHI_BUILD_SHARED is forced OFF by render_smoke.cmake. Its static
        // backend does not reinitialize this dispatcher when creating a device.
        // Assignment occurs only when no previous Context/device remains alive.
        VULKAN_HPP_DEFAULT_DISPATCHER=dispatch;
    }
    ~SharedInstance() { if(instance)instance.destroy(nullptr,dispatch); }
};

std::shared_ptr<SharedInstance> acquire_instance(PFN_vkGetInstanceProcAddr get,const char* const* names,std::uint32_t count) {
    static std::mutex mutex;
    static std::weak_ptr<SharedInstance> active;
    std::lock_guard lock(mutex);
    if(auto existing=active.lock()) {
        require(existing->owner==std::this_thread::get_id(),"Live graphics contexts require their common UI thread.");
        require(existing->entry==get && existing->extensions.size()==count,"Live graphics contexts require the same Vulkan loader/platform.");
        for(std::uint32_t i=0;i<count;++i)
            require(existing->extensions[i]==names[i],"Live graphics contexts require matching Vulkan instance extensions.");
        return existing;
    }
    auto created=std::make_shared<SharedInstance>(get,names,count);active=created;return created;
}
struct DrawConstants {
    float model[3][4];
    float normal[3][4];
    float base_metallic[4];
    float emissive_roughness[4];
};
static_assert(sizeof(DrawConstants)==128);
struct SkyConstants {
    float right_tan[4],up_tan[4],forward_srgb[4],zenith_exposure[4];
    float horizon_falloff[4],ground_radius[4],sun_intensity[4],sun_color[4];
};
static_assert(sizeof(SkyConstants)==128);
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
    std::shared_ptr<SharedInstance> shared_instance;
    vk::detail::DispatchLoaderDynamic dispatch;
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
    SkyConstants sky_constants{};
    bool sky_enabled=false;
    nvrhi::ShaderHandle sky_vs,sky_ps;
    nvrhi::BindingLayoutHandle sky_layout;
    nvrhi::BindingSetHandle sky_bindings;
    nvrhi::GraphicsPipelineHandle sky_pipeline;
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
    bool timestamp_prepared=false,timestamp_recording=false;
    std::map<std::pair<const MeshAsset*,const MaterialTextures*>,nvrhi::BindingSetHandle> material_cache;
    std::map<const TextureImage*,nvrhi::TextureHandle> texture_cache;
    std::map<std::array<int,4>,nvrhi::SamplerHandle> sampler_cache;
    std::shared_ptr<const TextureImage> white_image;
    std::size_t texture_bytes=0;
    bool hardware = false;
    std::string gpu_name;
    bool swapchain_dirty=false;
    bool shadow_ready=false,skin_pipeline_ready=false,renderer_fault=false;
    bool editor=false,hosted=false,scene_visible=true,capture_exclusive=false;
    std::optional<nvrhi::Viewport> scene_viewport;
    std::vector<EditorOverlayVertex> overlay_data;
    nvrhi::ShaderHandle overlay_vs,overlay_ps;
    nvrhi::InputLayoutHandle overlay_input;
    nvrhi::BindingLayoutHandle overlay_layout;
    nvrhi::BindingSetHandle overlay_bindings;
    nvrhi::GraphicsPipelineHandle overlay_pipeline;
    nvrhi::BufferHandle overlay_vertices;
    std::vector<nvrhi::FramebufferHandle> overlay_framebuffers;
    std::shared_ptr<const UiFrame> game_ui_frame;
    std::shared_ptr<const UiFrame> game_ui_source,game_ui_layout_frame,game_ui_composed_frame;
    std::array<std::int32_t,4> game_ui_layout_rect{};
    std::array<std::uint32_t,2> game_ui_layout_extent{};
#if POIMA_GAME_UI
    std::unique_ptr<UiPresenter> game_ui_presenter;
#endif
    nvrhi::ShaderHandle game_ui_vs,game_ui_ps;
    nvrhi::InputLayoutHandle game_ui_input;
    nvrhi::BindingLayoutHandle game_ui_layout;
    nvrhi::GraphicsPipelineHandle game_ui_pipeline;
    nvrhi::SamplerHandle game_ui_sampler;
    nvrhi::BufferHandle game_ui_vertices,game_ui_indices;
    std::size_t game_ui_vertex_capacity=0,game_ui_index_capacity=0;
    std::vector<nvrhi::TextureHandle> game_ui_textures;
    std::vector<nvrhi::BindingSetHandle> game_ui_bindings;
    std::vector<nvrhi::FramebufferHandle> game_ui_framebuffers;
#if POIMA_EDITOR
    nvrhi::ShaderHandle ui_vs,ui_ps;
    nvrhi::InputLayoutHandle ui_input;
    nvrhi::BindingLayoutHandle ui_layout;
    nvrhi::BindingSetHandle ui_bindings,ui_scene_bindings;
    nvrhi::GraphicsPipelineHandle ui_pipeline;
    nvrhi::TextureHandle ui_font,ui_scene;
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
            try { device.waitIdle(dispatch); } catch (...) { /* Preserve the original diagnostic. */ }
        }
        commands = nullptr;
        game_ui_framebuffers.clear();game_ui_pipeline=nullptr;game_ui_bindings.clear();game_ui_textures.clear();
        game_ui_vertices=nullptr;game_ui_indices=nullptr;game_ui_sampler=nullptr;game_ui_input=nullptr;
        game_ui_layout=nullptr;game_ui_vs=nullptr;game_ui_ps=nullptr;game_ui_frame.reset();
        overlay_framebuffers.clear();overlay_pipeline=nullptr;overlay_bindings=nullptr;overlay_layout=nullptr;
        overlay_vertices=nullptr;overlay_input=nullptr;overlay_vs=nullptr;overlay_ps=nullptr;
#if POIMA_EDITOR
        if(ui_context && ImGui::GetCurrentContext()==ui_context) {
            auto& io=ImGui::GetIO();
            if(io.BackendRendererName && std::strcmp(io.BackendRendererName,"poima_nvrhi")==0) {
                if(io.Fonts->TexID==static_cast<ImTextureID>(1))io.Fonts->SetTexID(0);
                io.BackendFlags&=~ImGuiBackendFlags_RendererHasVtxOffset;io.BackendRendererName=nullptr;
            }
        }
        ui_framebuffers.clear();ui_pipeline=nullptr;ui_bindings=nullptr;ui_scene_bindings=nullptr;ui_layout=nullptr;
        ui_vertices=nullptr;ui_indices=nullptr;ui_font=nullptr;ui_sampler=nullptr;ui_input=nullptr;ui_vs=nullptr;ui_ps=nullptr;
#endif
        skin_instances.clear();skin_sources.clear();skin_pipeline=nullptr;skin_layout=nullptr;skin_shader=nullptr;skin_errors=nullptr;skin_readback=nullptr;
        shadow_pipeline=nullptr;shadow_bindings=nullptr;shadow_layout=nullptr;shadow_shader=nullptr;shadow_framebuffers.clear();shadow_texture=nullptr;
        pipeline = nullptr; culled_pipeline=nullptr;
        sky_pipeline=nullptr;sky_bindings=nullptr;sky_layout=nullptr;sky_vs=nullptr;sky_ps=nullptr;
        vertex_shader = nullptr;
        pixel_shader = nullptr;
        staging = nullptr;
        bindings = nullptr;
        binding_layout = nullptr;
        input_layout = nullptr;
        vertices = nullptr; frame_buffer=nullptr; draws.clear(); geometry_cache.clear(); material_cache.clear();texture_cache.clear();sampler_cache.clear();
        framebuffers.clear();
#if POIMA_EDITOR
        ui_scene=nullptr;
#endif
        images.clear();
        depth = nullptr;
        multisample_color = nullptr;
        checked = nullptr;
        native = nullptr;
        if (device) {
            for (auto semaphore : finished) device.destroySemaphore(semaphore,nullptr,dispatch);
            if (acquired) device.destroySemaphore(acquired,nullptr,dispatch);
            if (timestamp_pool) device.destroyQueryPool(timestamp_pool,nullptr,dispatch);
            if (swapchain) device.destroySwapchainKHR(swapchain,nullptr,dispatch);
            device.destroy(nullptr,dispatch);
        }
        if (surface) instance.destroySurfaceKHR(surface,nullptr,dispatch);
        instance=nullptr;
        shared_instance.reset(); // Last device/surface is gone before instance and SDL loader teardown.
        if (window) SDL_DestroyWindow(window);
        if (sdl_initialized) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    void initialize(const RenderOptions& options, const SceneSnapshot* source, bool player=false,void* external_window=nullptr) {
        require(!source || !source->ui || !source->logical_ui,"A scene cannot supply both a UI packet and a logical UI presentation.");
#if !POIMA_GAME_UI
        require(!source || !source->logical_ui || source->logical_ui->elements.empty(),"This renderer was built without native game UI presentation support.");
#endif
        if(source && source->ui)validate_ui_frame(*source->ui);
        scene = source;capture_exclusive=options.capture_exclusive;
        hosted=external_window!=nullptr;
        diagnostics.culling=options.culling;diagnostics.profile_requested=options.profile;
        samples = scene ? options.samples : 1;
        SDL_SetMainReady();
        const bool initialized_video = SDL_Init(SDL_INIT_VIDEO);
        require(initialized_video, std::string("SDL video initialization: ") + SDL_GetError());
        sdl_initialized = true;
        if(external_window) {
#ifdef _WIN32
            const auto properties=SDL_CreateProperties();require(properties!=0,SDL_GetError());
            const bool configured=SDL_SetPointerProperty(properties,SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER,external_window)
                && SDL_SetBooleanProperty(properties,SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN,true);
            if(configured)window=SDL_CreateWindowWithProperties(properties);
            SDL_DestroyProperties(properties);
            require(configured,std::string("SDL native window properties: ")+SDL_GetError());
#else
            throw std::runtime_error("Hosted viewport currently supports Windows child HWNDs only.");
#endif
        } else window = SDL_CreateWindow(scene ? "Poima — authored scene preview" : "Poima — Vulkan/NVRHI foundation", static_cast<int>(options.width),
            static_cast<int>(options.height), SDL_WINDOW_VULKAN | (player ? SDL_WINDOW_RESIZABLE : 0));
        require(window != nullptr, std::string("SDL window: ") + SDL_GetError());
        const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        require(get != nullptr, "SDL could not load the Vulkan entry point.");
        std::uint32_t extension_count = 0;
        const auto extension_names = SDL_Vulkan_GetInstanceExtensions(&extension_count);
        require(extension_names != nullptr, std::string("SDL Vulkan extensions: ") + SDL_GetError());
        shared_instance=acquire_instance(get,extension_names,extension_count);
        instance=shared_instance->instance;
        dispatch=shared_instance->dispatch;
        VkSurfaceKHR raw_surface = VK_NULL_HANDLE;
        const bool created_surface = SDL_Vulkan_CreateSurface(window, static_cast<VkInstance>(instance), nullptr, &raw_surface);
        require(created_surface,
            std::string("SDL Vulkan surface: ") + SDL_GetError());
        surface = raw_surface;

        const auto devices = instance.enumeratePhysicalDevices(dispatch);
        int best_score = -1;
        for (std::size_t index = 0; index < devices.size(); ++index) {
            if (options.gpu >= 0 && index != static_cast<std::size_t>(options.gpu)) continue;
            const auto candidate = devices[index];
            const auto props = candidate.getProperties(dispatch);
            const bool is_hardware = props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu ||
                props.deviceType == vk::PhysicalDeviceType::eIntegratedGpu;
            if ((!is_hardware && !options.allow_software) || props.apiVersion < VK_API_VERSION_1_3) continue;
            const auto extensions = candidate.enumerateDeviceExtensionProperties(nullptr,dispatch);
            const bool swapchain_supported = std::any_of(extensions.begin(), extensions.end(), [](const auto& item) {
                return std::string_view(item.extensionName.data()) == VK_KHR_SWAPCHAIN_EXTENSION_NAME;
            });
            if (!swapchain_supported) continue;
            vk::PhysicalDeviceVulkan13Features features13;
            vk::PhysicalDeviceVulkan12Features features12;
            features12.pNext = &features13;
            vk::PhysicalDeviceFeatures2 features;
            features.pNext = &features12;
            candidate.getFeatures2(&features,dispatch);
            if (!features12.timelineSemaphore || !features13.synchronization2 || !features13.dynamicRendering) continue;
            const auto queues = candidate.getQueueFamilyProperties(dispatch);
            for (std::uint32_t family = 0; family < queues.size(); ++family) {
                if (!(queues[family].queueFlags & vk::QueueFlagBits::eGraphics) || !candidate.getSurfaceSupportKHR(family,surface,dispatch)) continue;
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
        device = physical.createDevice(device_info,nullptr,dispatch);
        dispatch.init(device);
        queue = device.getQueue(queue_family,0,dispatch);

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

        const auto limits = physical.getProperties(dispatch).limits;
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
        if(timestamp_prepared)return;
        timestamp_prepared=true;
        const auto limits=physical.getProperties(dispatch).limits;
        diagnostics.timestamp_period_ns=limits.timestampPeriod;
        diagnostics.timestamp_valid_bits=physical.getQueueFamilyProperties(dispatch).at(queue_family).timestampValidBits;
        if(!(limits.timestampPeriod>0) || diagnostics.timestamp_valid_bits==0) {
            diagnostics.gpu_timing_detail="Selected graphics queue does not support timestamps.";return;
        }
        timestamp_pool=device.createQueryPool(vk::QueryPoolCreateInfo({},vk::QueryType::eTimestamp,5),nullptr,dispatch);
        diagnostics.gpu_timestamps=true;
        diagnostics.gpu_timing_detail="64-bit graphics-queue timestamps; approximate pass intervals, not presentation latency or game frame time.";
    }
    vk::CommandBuffer native_commands() {
        const auto object=commands->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
        require(object.pointer!=nullptr,"Native Vulkan command buffer unavailable.");
        return vk::CommandBuffer(static_cast<VkCommandBuffer>(object.pointer));
    }
    void timestamp(std::uint32_t index) {
        if(timestamp_recording)native_commands().writeTimestamp(index==0 ? vk::PipelineStageFlagBits::eTopOfPipe : vk::PipelineStageFlagBits::eBottomOfPipe,timestamp_pool,index,dispatch);
    }
    void collect_timestamps(double cpu_interval_ms) {
        if(!timestamp_recording)return;
        std::array<std::uint64_t,5> values{};
        const auto status=device.getQueryPoolResults(timestamp_pool,0,5,sizeof(values),values.data(),sizeof(values[0]),vk::QueryResultFlagBits::e64,dispatch);
        const auto bits=diagnostics.timestamp_valid_bits;
        const double wrap_ms=std::ldexp(diagnostics.timestamp_period_ns*1e-6,static_cast<int>(bits));
        if(status!=vk::Result::eSuccess || cpu_interval_ms>=wrap_ms) {
            ++diagnostics.gpu_samples_dropped;profiling::counter("gpu.samples_dropped",diagnostics.gpu_samples_dropped);return;
        }
        const auto mask=bits==64 ? ~std::uint64_t(0) : (std::uint64_t(1)<<bits)-1;
        auto ms=[&](std::size_t a,std::size_t b) { return static_cast<double>((values[b]-values[a])&mask)*diagnostics.timestamp_period_ns*1e-6; };
        if(diagnostics.profile_requested) {
            timing_sample(diagnostics.skinning_gpu,ms(0,1));timing_sample(diagnostics.shadow_gpu,ms(1,2));timing_sample(diagnostics.opaque_gpu,ms(2,3));
            timing_sample(diagnostics.post_gpu,ms(3,4));timing_sample(diagnostics.total_gpu,ms(0,4));
        }
        // GPU values are queue durations observed after completion, not CPU
        // timeline timestamps. Never fabricate cross-clock synchronization.
        auto sample=[&](std::string_view name,std::size_t a,std::size_t b) {
            const long double ns=static_cast<long double>((values[b]-values[a])&mask)*diagnostics.timestamp_period_ns;
            if(std::isfinite(ns) && ns>=0 && ns<static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
                profiling::counter(name,static_cast<std::uint64_t>(ns),profiling::Kind::gpu);
        };
        sample("gpu.skinning.ns",0,1);sample("gpu.shadows.ns",1,2);sample("gpu.opaque.ns",2,3);
        sample("gpu.post.ns",3,4);sample("gpu.total.ns",0,4);
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
        const auto limits=physical.getProperties(dispatch).limits;
        require(resolution<=limits.maxImageDimension2D && layers<=limits.maxImageArrayLayers,"Shadow texture dimensions are unavailable on this GPU.");
        const auto flags=physical.getFormatProperties(vk::Format::eD32Sfloat,dispatch).optimalTilingFeatures;
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
        require(bool(physical.getQueueFamilyProperties(dispatch).at(queue_family).queueFlags & vk::QueueFlagBits::eCompute),"Selected graphics queue cannot run skinning compute.");
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
    void prepare_sky() {
        if(!sky_layout) {
            sky_vs=create_embedded_shader(checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main"),poima_sky_vs);
            sky_ps=create_embedded_shader(checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main"),poima_sky_ps);
            sky_layout=checked->createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0,sizeof(SkyConstants))));
            require(sky_vs && sky_ps && sky_layout,"Procedural sky shader/layout creation failed.");
            sky_bindings=checked->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0,sizeof(SkyConstants))),sky_layout);
            require(bool(sky_bindings),"Procedural sky bindings creation failed.");
        }
        if(!sky_pipeline) {
            nvrhi::GraphicsPipelineDesc description;description.VS=sky_vs;description.PS=sky_ps;description.bindingLayouts.push_back(sky_layout);
            description.renderState.depthStencilState.depthTestEnable=false;description.renderState.depthStencilState.depthWriteEnable=false;
            description.renderState.rasterState.cullMode=nvrhi::RasterCullMode::None;description.renderState.rasterState.scissorEnable=true;
            sky_pipeline=checked->createGraphicsPipeline(description,framebuffers.front()->getFramebufferInfo());
            require(bool(sky_pipeline),"Procedural sky pipeline creation failed.");
        }
    }
    void render_sky(std::uint32_t image_index) {
        if(!scene || !scene_visible || !sky_enabled)return;
        nvrhi::GraphicsState state;state.pipeline=sky_pipeline;state.framebuffer=framebuffers.at(image_index);state.bindings.push_back(sky_bindings);
        state.viewport.addViewportAndScissorRect(scene_viewport.value_or(nvrhi::Viewport(static_cast<float>(extent.width),static_cast<float>(extent.height))));
        commands->setGraphicsState(state);commands->setPushConstants(&sky_constants,sizeof(sky_constants));
        commands->draw(nvrhi::DrawArguments().setVertexCount(3));
    }
    void update_scene() {
        validate_game_ui();
        profiling::Scope profile_scope("renderer.prepare");
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
        const auto& sky=lighting.environment.sky;sky_enabled=sky.enabled;sky_constants={};
        if(sky_enabled) {
            require(rigid_transform(scene->camera_world),"Procedural sky requires a rigid camera transform.");
            prepare_sky();
            for(std::size_t k=0;k<3;++k) {
                sky_constants.right_tan[k]=number(scene->camera_world[k]);
                sky_constants.up_tan[k]=number(scene->camera_world[4+k]);
                sky_constants.forward_srgb[k]=number(-scene->camera_world[8+k]);
                sky_constants.zenith_exposure[k]=sky.zenith[k];sky_constants.horizon_falloff[k]=sky.horizon[k];sky_constants.ground_radius[k]=sky.ground[k];
            }
            const double tan_y=std::tan(scene->vertical_fov*0.0087266462599716478846);
            sky_constants.right_tan[3]=number(tan_y*aspect);sky_constants.up_tan[3]=number(tan_y);
            sky_constants.forward_srgb[3]=frame_constants.camera[3];sky_constants.zenith_exposure[3]=lighting.environment.exposure;
            sky_constants.horizon_falloff[3]=sky.horizon_falloff;
            sky_constants.ground_radius[3]=static_cast<float>(2*std::sin(sky.sun_size_degrees*0.0043633231299858239423));
            // A deterministic reference, never whichever directional light happens
            // to sort first. Runtime snapshots supply the live light orientation.
            for(const auto& light:lighting.lights)if(!sky.sun.empty() && light.entity_id==sky.sun && light.light.enabled && light.light.kind==LightKind::directional) {
                const double length=std::hypot(light.direction[0],light.direction[1],light.direction[2]);
                require(std::isfinite(length) && length>0,"Sky sun direction is degenerate.");
                validate_light(light.light);
                for(std::size_t k=0;k<3;++k) { sky_constants.sun_intensity[k]=number(-light.direction[k]/length);sky_constants.sun_color[k]=light.light.color[k]; }
                sky_constants.sun_intensity[3]=sky.sun_intensity;break;
            }
        }
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
        profiling::counter("renderer.objects",pending_draws.objects);
        profiling::counter("renderer.camera_draws",pending_draws.camera_draws);
        profiling::counter("renderer.camera_triangles",pending_draws.camera_triangles);
        profiling::counter("renderer.shadow_draws",pending_draws.shadow_draws);
        profiling::counter("renderer.shadow_triangles",pending_draws.shadow_triangles);
        profiling::counter("renderer.geometries",geometry_cache.size());
        profiling::counter("renderer.materials",material_cache.size());
        profiling::counter("renderer.images",texture_cache.size());
        profiling::counter("renderer.skin_instances",skin_instances.size());
        profiling::counter("renderer.texture_payload_bytes",texture_bytes);
        profiling::counter("renderer.skin_buffer_bytes",skin_bytes);
    }

    bool rebuild(const RenderOptions& options) {
        swapchain_dirty=true;
        device.waitIdle(dispatch);
        commands=nullptr;
        sky_pipeline=nullptr;
        overlay_framebuffers.clear();overlay_pipeline=nullptr;
        game_ui_framebuffers.clear();game_ui_pipeline=nullptr;
#if POIMA_EDITOR
        ui_framebuffers.clear();ui_scene_bindings=nullptr;
#endif
        framebuffers.clear(); images.clear(); depth=nullptr; multisample_color=nullptr; staging=nullptr;
#if POIMA_EDITOR
        ui_scene=nullptr;
#endif
        checked->runGarbageCollection();
        for(auto semaphore:finished) device.destroySemaphore(semaphore,nullptr,dispatch);
        finished.clear(); initialized.clear();
        device.destroySemaphore(acquired,nullptr,dispatch); acquired=nullptr;
        device.destroySwapchainKHR(swapchain,nullptr,dispatch); swapchain=nullptr;
        const auto previous_format=format;
        if(!create_swapchain(options)) { swapchain_dirty=true; return false; }
        if(format!=previous_format) { renderer_fault=true;throw std::runtime_error("Surface format changed; recreate the renderer session."); }
        commands=checked->createCommandList();
        require(static_cast<bool>(commands),"Player command-list recreation failed.");
        swapchain_dirty=false;
        return true;
    }

    bool create_swapchain(const RenderOptions& options) {
        const auto caps = physical.getSurfaceCapabilitiesKHR(surface,dispatch);
        // Native minimize/restore can race SDL's queued size notification.
        // Zero surface extent is a suspended window, not a device failure.
        if(caps.currentExtent.width==0 || caps.currentExtent.height==0) return false;
        const auto formats = physical.getSurfaceFormatsKHR(surface,dispatch);
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
        // Editor IPC may request its first screenshot long after startup. Keep
        // transfer support and one staging image ready without requiring a
        // configured final-capture path; ordinary frames still skip the copy.
        const bool capture_enabled=editor || hosted || !options.capture.empty();
        const auto required_usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
            (capture_enabled ? vk::ImageUsageFlagBits::eTransferSrc : vk::ImageUsageFlags{});
        require((caps.supportedUsageFlags & required_usage) == required_usage, "Surface lacks required render/capture image usage.");
        extent = caps.currentExtent;
        if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
            extent.width = std::clamp(options.width, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(options.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        require(extent.width > 0 && extent.height > 0, "The window has an empty rendering extent.");
        validate_game_ui();
        if(editor || hosted)require(std::uint64_t(extent.width)*extent.height<=128u*1024u*1024u/4u,
            "GUI surface exceeds the 128 MiB RGBA staging budget.");
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
        swapchain = device.createSwapchainKHR(swapchain_info,nullptr,dispatch);
        nvrhi::TextureDesc texture_desc;
        texture_desc.width = extent.width;
        texture_desc.height = extent.height;
        texture_desc.format = nvrhi_format;
        texture_desc.isRenderTarget = true;
        texture_desc.isShaderResource = false;
#if POIMA_EDITOR
        if(editor) {
            // One full-window image keeps Scene UVs stable within this frame,
            // including docking, floating panels and framebuffer DPI scaling.
            // The existing extent limit bounds this additional image to 128 MiB.
            auto scene_image_desc=texture_desc;
            scene_image_desc.isShaderResource=true;
            scene_image_desc.initialState=nvrhi::ResourceStates::ShaderResource;
            scene_image_desc.keepInitialState=true;
            scene_image_desc.debugName="Editor Scene compositing image";
            ui_scene=checked->createTexture(scene_image_desc);
            require(bool(ui_scene),"Editor Scene compositing image creation failed.");
            if(ui_layout)create_ui_scene_bindings();
        }
#endif
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
        for (const auto image : device.getSwapchainImagesKHR(swapchain,dispatch)) {
            auto texture = checked->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image,
                nvrhi::Object(static_cast<VkImage>(image)), texture_desc);
            require(static_cast<bool>(texture), "NVRHI swapchain image wrapping failed.");
            images.push_back(texture);
            auto* scene_target=texture.Get();
#if POIMA_EDITOR
            if(editor)scene_target=ui_scene.Get();
#endif
            auto framebuffer_desc = nvrhi::FramebufferDesc().addColorAttachment(multisample_color ? multisample_color.Get() : scene_target);
            if (depth) framebuffer_desc.setDepthAttachment(depth);
            auto framebuffer = checked->createFramebuffer(framebuffer_desc);
            require(static_cast<bool>(framebuffer), "NVRHI framebuffer creation failed.");
            framebuffers.push_back(framebuffer);
            auto game_ui_framebuffer=checked->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(scene_target));
            require(bool(game_ui_framebuffer),"Game UI composition framebuffer creation failed.");
            game_ui_framebuffers.push_back(game_ui_framebuffer);
            if(hosted) {
                auto overlay_framebuffer=checked->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
                require(bool(overlay_framebuffer),"Hosted overlay framebuffer creation failed.");overlay_framebuffers.push_back(overlay_framebuffer);
            }
#if POIMA_EDITOR
            if(editor) {
                auto ui_framebuffer=checked->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
                require(bool(ui_framebuffer),"Editor UI framebuffer creation failed.");ui_framebuffers.push_back(ui_framebuffer);
            }
#endif
            finished.push_back(device.createSemaphore({},nullptr,dispatch));
        }
        initialized.resize(images.size(), false);
        acquired = device.createSemaphore({},nullptr,dispatch);
        if (capture_enabled) {
            texture_desc.isRenderTarget = false;
            staging = checked->createStagingTexture(texture_desc, nvrhi::CpuAccessMode::Read);
            require(static_cast<bool>(staging), "NVRHI capture staging texture creation failed.");
        }
        return true;
    }

    void capture(const std::string& path) {
        profiling::Scope profile_scope("renderer.capture_readback_write");
        std::size_t row_pitch = 0;
        void* pixels = checked->mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, &row_pitch);
        require(pixels != nullptr, "GPU capture readback mapping failed.");
        const bool bgra = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
        SDL_Surface* image = SDL_CreateSurfaceFrom(static_cast<int>(extent.width), static_cast<int>(extent.height),
            bgra ? SDL_PIXELFORMAT_BGRA32 : SDL_PIXELFORMAT_RGBA32, pixels, static_cast<int>(row_pitch));
        if(editor || capture_exclusive) {
            try { require(image!=nullptr,std::string("BMP capture surface: ")+SDL_GetError());save_bmp_exclusive(image,path); }
            catch(...) { if(image)SDL_DestroySurface(image);checked->unmapStagingTexture(staging);throw; }
            SDL_DestroySurface(image);checked->unmapStagingTexture(staging);return;
        }
        const bool saved = image && SDL_SaveBMP(image, path.c_str());
        const std::string detail = saved ? "" : std::string("BMP capture: ") + SDL_GetError();
        if (image) SDL_DestroySurface(image);
        checked->unmapStagingTexture(staging);
        require(saved, detail);
    }

    void validate_game_ui() const {
        if(!scene)return;
        require(!scene->ui || !scene->logical_ui,"A scene cannot supply both a UI packet and a logical UI presentation.");
#if !POIMA_GAME_UI
        require(!scene->logical_ui || scene->logical_ui->elements.empty(),"This renderer was built without native game UI presentation support.");
#endif
        if(!scene || !scene->ui)return;
        validate_ui_frame(*scene->ui);
        require(scene->ui->width==extent.width && scene->ui->height==extent.height,"Game UI packet extent differs from the render target; relayout before drawing.");
        if(!scene->ui->draws.empty())require(format==vk::Format::eB8G8R8A8Srgb || format==vk::Format::eR8G8B8A8Srgb,
            "Game UI requires an sRGB composition attachment; UNORM composition is not supported.");
    }
    void resolve_game_ui() {
        game_ui_source=scene ? scene->ui : nullptr;
#if POIMA_GAME_UI
        if(!scene || !scene->logical_ui || scene->logical_ui->elements.empty()) {
            game_ui_presenter.reset();game_ui_layout_frame.reset();game_ui_composed_frame.reset();return;
        }
        if(!scene_visible) {game_ui_source.reset();return;}
        const auto view=scene_viewport.value_or(nvrhi::Viewport(static_cast<float>(extent.width),static_cast<float>(extent.height)));
        const std::array<std::int32_t,4> rect{static_cast<std::int32_t>(std::floor(view.minX)),static_cast<std::int32_t>(std::floor(view.minY)),
            static_cast<std::int32_t>(std::ceil(view.maxX)),static_cast<std::int32_t>(std::ceil(view.maxY))};
        if(rect[0]>=rect[2] || rect[1]>=rect[3]) {game_ui_source.reset();return;}
        if(!game_ui_presenter)game_ui_presenter=std::make_unique<UiPresenter>();
        const float scale=std::clamp(SDL_GetWindowDisplayScale(window),.25f,8.f);
        const auto layout=game_ui_presenter->frame(scene->logical_ui,static_cast<std::uint32_t>(rect[2]-rect[0]),static_cast<std::uint32_t>(rect[3]-rect[1]),scale);
        const std::array<std::uint32_t,2> target_extent{extent.width,extent.height};
        if(game_ui_layout_frame!=layout || game_ui_layout_rect!=rect || game_ui_layout_extent!=target_extent) {
            auto packet=*layout;packet.width=extent.width;packet.height=extent.height;
            for(auto& draw:packet.draws) {
                // Translate homogeneous coordinates after the layout transform.
                for(std::size_t col=0;col<4;++col) {
                    draw.transform[col*4]+=static_cast<float>(rect[0])*draw.transform[col*4+3];
                    draw.transform[col*4+1]+=static_cast<float>(rect[1])*draw.transform[col*4+3];
                }
                draw.scissor[0]+=rect[0];draw.scissor[2]+=rect[0];draw.scissor[1]+=rect[1];draw.scissor[3]+=rect[1];
            }
            game_ui_source=freeze_ui_frame(std::move(packet));game_ui_composed_frame=game_ui_source;game_ui_layout_frame=layout;game_ui_layout_rect=rect;game_ui_layout_extent=target_extent;
        }else game_ui_source=game_ui_composed_frame;
#endif
    }
    void reset_game_ui_input() {
#if POIMA_GAME_UI
        if(game_ui_presenter)game_ui_presenter->reset_input();
#endif
    }
    UiInputResult dispatch_game_ui_input(const UiInput& event,bool activate=true) {
#if POIMA_GAME_UI
        if(game_ui_presenter && game_ui_source && scene_visible) {
            auto local=event;
            local.x-=static_cast<float>(game_ui_layout_rect[0]);
            local.y-=static_cast<float>(game_ui_layout_rect[1]);
            return game_ui_presenter->input(local,activate);
        }
#else
        (void)event;(void)activate;
#endif
        return {};
    }
    void prepare_game_ui() {
        validate_game_ui();
        resolve_game_ui();
        const auto& packet=game_ui_source;
        if(packet) {
            validate_ui_frame(*packet);
            require(packet->width==extent.width && packet->height==extent.height,"Resolved UI packet extent differs from target.");
            if(!packet->draws.empty())require(format==vk::Format::eB8G8R8A8Srgb || format==vk::Format::eR8G8B8A8Srgb,"Game UI requires an sRGB composition attachment; UNORM composition is not supported.");
        }
        if(!packet || packet->draws.empty()) {
            game_ui_frame.reset();game_ui_bindings.clear();game_ui_textures.clear();game_ui_vertices=nullptr;game_ui_indices=nullptr;
            game_ui_vertex_capacity=game_ui_index_capacity=0;return;
        }
        try {
        if(!game_ui_layout) {
            game_ui_vs=create_embedded_shader(checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main"),poima_game_ui_vs);
            game_ui_ps=create_embedded_shader(checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main"),poima_game_ui_ps);
            const nvrhi::VertexAttributeDesc attributes[]={
                nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(UiVertex,x)).setElementStride(sizeof(UiVertex)),
                nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(UiVertex,u)).setElementStride(sizeof(UiVertex)),
                nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGBA8_UNORM).setOffset(offsetof(UiVertex,color)).setElementStride(sizeof(UiVertex))};
            game_ui_input=checked->createInputLayout(attributes,3,game_ui_vs);
            game_ui_layout=checked->createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0,80)).addItem(nvrhi::BindingLayoutItem::Texture_SRV(0)).addItem(nvrhi::BindingLayoutItem::Sampler(0)));
            nvrhi::SamplerDesc sampler;sampler.addressU=sampler.addressV=nvrhi::SamplerAddressMode::Clamp;sampler.minFilter=sampler.magFilter=true;
            game_ui_sampler=checked->createSampler(sampler);
            require(game_ui_vs && game_ui_ps && game_ui_input && game_ui_layout && game_ui_sampler,"Game UI shader/layout creation failed.");
        }
        if(!game_ui_pipeline) {
            nvrhi::GraphicsPipelineDesc description;description.VS=game_ui_vs;description.PS=game_ui_ps;
            description.inputLayout=game_ui_input;description.bindingLayouts.push_back(game_ui_layout);
            description.renderState.depthStencilState.depthTestEnable=false;description.renderState.depthStencilState.depthWriteEnable=false;
            description.renderState.rasterState.cullMode=nvrhi::RasterCullMode::None;description.renderState.rasterState.scissorEnable=true;
            auto& blend=description.renderState.blendState.targets[0];blend.blendEnable=true;
            blend.srcBlend=blend.srcBlendAlpha=nvrhi::BlendFactor::One;
            blend.destBlend=blend.destBlendAlpha=nvrhi::BlendFactor::InvSrcAlpha;
            game_ui_pipeline=checked->createGraphicsPipeline(description,game_ui_framebuffers.front()->getFramebufferInfo());
            require(bool(game_ui_pipeline),"Game UI composition pipeline creation failed.");
        }
        if(game_ui_frame==packet)return;
        // Reuse capacity and unchanged atlas slots across layout packets. Retain
        // only the current packet/resources; replacement is bounded by two packets.
        auto buffer=[&](nvrhi::BufferHandle& target,std::size_t& capacity,std::size_t bytes,bool vertex) {
            if(target && capacity>=bytes)return;
            nvrhi::BufferDesc desc;desc.byteSize=std::max<std::size_t>(bytes,4);desc.isVertexBuffer=vertex;desc.isIndexBuffer=!vertex;
            desc.initialState=vertex ? nvrhi::ResourceStates::VertexBuffer : nvrhi::ResourceStates::IndexBuffer;desc.keepInitialState=true;
            auto result=checked->createBuffer(desc);require(bool(result),"Game UI buffer creation failed.");target=std::move(result);capacity=desc.byteSize;
        };
        buffer(game_ui_vertices,game_ui_vertex_capacity,packet->vertices.size()*sizeof(UiVertex),true);
        buffer(game_ui_indices,game_ui_index_capacity,packet->indices.size()*sizeof(std::uint32_t),false);
        std::vector<nvrhi::TextureHandle> textures;std::vector<nvrhi::BindingSetHandle> texture_bindings;
        textures.reserve(packet->textures.size()+1);texture_bindings.reserve(packet->textures.size()+1);
        bool uploads=false;
        auto upload_texture=[&](const UiTexture& source) {
            // Linear premultiplied half floats ensure filtering occurs in linear light.
            std::vector<std::uint16_t> pixels;pixels.reserve(source.rgba.size());
            auto half=[](float value)->std::uint16_t {
                if(value==0)return 0;
                if(value<0x1p-14f)return static_cast<std::uint16_t>(std::lround(std::ldexp(value,24)));
                int exponent=0;const float mantissa=std::frexp(value,&exponent);
                return static_cast<std::uint16_t>((exponent+14)*1024+std::lround((mantissa*2-1)*1024));
            };
            for(std::size_t i=0;i<source.rgba.size();i+=4) {
                const auto c=ui_linear_color({source.rgba[i],source.rgba[i+1],source.rgba[i+2],source.rgba[i+3]});
                for(auto value:c)pixels.push_back(half(value));
            }
            nvrhi::TextureDesc desc;desc.width=source.width;desc.height=source.height;desc.format=nvrhi::Format::RGBA16_FLOAT;
            desc.initialState=nvrhi::ResourceStates::ShaderResource;desc.keepInitialState=true;
            auto texture=checked->createTexture(desc);require(bool(texture),"Game UI texture creation failed.");
            auto binding=checked->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0,80))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(0,texture)).addItem(nvrhi::BindingSetItem::Sampler(0,game_ui_sampler)),game_ui_layout);
            require(bool(binding),"Game UI texture binding failed.");
            if(!uploads) {commands->open();uploads=true;}
            commands->writeTexture(texture,0,0,pixels.data(),std::size_t(source.width)*8);
            textures.push_back(std::move(texture));texture_bindings.push_back(std::move(binding));
        };
        for(std::size_t i=0;i<packet->textures.size();++i) {
            const auto& texture=packet->textures[i];
            if(game_ui_frame && i<game_ui_frame->textures.size() && texture.width==game_ui_frame->textures[i].width &&
                texture.height==game_ui_frame->textures[i].height && texture.rgba==game_ui_frame->textures[i].rgba) {
                textures.push_back(game_ui_textures[i]);texture_bindings.push_back(game_ui_bindings[i]);
            }else upload_texture(texture);
        }
        if(game_ui_frame) {textures.push_back(game_ui_textures.back());texture_bindings.push_back(game_ui_bindings.back());}
        else upload_texture(UiTexture{1,1,{255,255,255,255}});
        if(uploads) {
            commands->close();checked->executeCommandList(commands);require(checked->waitForIdle(),"Game UI texture upload failed.");
        }
        game_ui_textures.swap(textures);game_ui_bindings.swap(texture_bindings);
        game_ui_frame=packet;
        } catch(...) {
            // Allocation/resource failures can interrupt an open upload command
            // list or submission. Retrying this Context cannot safely recover it.
            renderer_fault=true;throw;
        }
    }
    void render_game_ui(std::uint32_t image_index) {
        if(!game_ui_frame)return;
        const auto& frame=*game_ui_frame;
        if(!frame.vertices.empty())commands->writeBuffer(game_ui_vertices,frame.vertices.data(),frame.vertices.size()*sizeof(UiVertex));
        if(!frame.indices.empty())commands->writeBuffer(game_ui_indices,frame.indices.data(),frame.indices.size()*sizeof(std::uint32_t));
        for(const auto& draw:frame.draws) {
            const auto& clip=draw.scissor;if(!draw.index_count || clip[0]==clip[2] || clip[1]==clip[3])continue;
            nvrhi::GraphicsState state;state.pipeline=game_ui_pipeline;state.framebuffer=game_ui_framebuffers.at(image_index);
            state.bindings.push_back(game_ui_bindings.at(draw.texture==ui_white_texture ? frame.textures.size() : draw.texture));
            state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(game_ui_vertices));
            state.indexBuffer=nvrhi::IndexBufferBinding(game_ui_indices,nvrhi::Format::R32_UINT,0);
            state.viewport.addViewport(nvrhi::Viewport(static_cast<float>(extent.width),static_cast<float>(extent.height)));
            state.viewport.addScissorRect(nvrhi::Rect(clip[0],clip[2],clip[1],clip[3]));
            std::array<float,20> constants{};std::copy(draw.transform.begin(),draw.transform.end(),constants.begin());
            constants[16]=draw.translation[0];constants[17]=draw.translation[1];constants[18]=static_cast<float>(frame.width);constants[19]=static_cast<float>(frame.height);
            commands->setGraphicsState(state);commands->setPushConstants(constants.data(),sizeof(constants));
            commands->drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.index_count).setStartIndexLocation(draw.first_index));
        }
    }
    void prepare_overlay() {
        if(!hosted || overlay_data.empty())return;
        if(!overlay_vertices) {
            overlay_vs=create_embedded_shader(checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main"),poima_editor_overlay_vs);
            overlay_ps=create_embedded_shader(checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main"),poima_editor_overlay_ps);
            const nvrhi::VertexAttributeDesc attributes[]={
                nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(EditorOverlayVertex,x)).setElementStride(sizeof(EditorOverlayVertex)),
                nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(offsetof(EditorOverlayVertex,color)).setElementStride(sizeof(EditorOverlayVertex))};
            overlay_input=checked->createInputLayout(attributes,2,overlay_vs);
            overlay_layout=checked->createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All).addItem(nvrhi::BindingLayoutItem::PushConstants(0,16)));
            require(overlay_vs && overlay_ps && overlay_input && overlay_layout,"Hosted overlay shader/layout creation failed.");
            overlay_bindings=checked->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0,16)),overlay_layout);
            nvrhi::BufferDesc buffer;buffer.byteSize=max_editor_overlay_vertices*sizeof(EditorOverlayVertex);buffer.isVertexBuffer=true;
            buffer.initialState=nvrhi::ResourceStates::VertexBuffer;buffer.keepInitialState=true;buffer.debugName="Hosted editor overlay vertices";
            overlay_vertices=checked->createBuffer(buffer);
            require(overlay_bindings && overlay_vertices,"Hosted overlay bindings/buffer creation failed.");
        }
        if(!overlay_pipeline) {
            nvrhi::GraphicsPipelineDesc description;description.VS=overlay_vs;description.PS=overlay_ps;description.inputLayout=overlay_input;description.bindingLayouts.push_back(overlay_layout);
            description.renderState.depthStencilState.depthTestEnable=false;description.renderState.depthStencilState.depthWriteEnable=false;
            description.renderState.rasterState.cullMode=nvrhi::RasterCullMode::None;description.renderState.rasterState.scissorEnable=true;
            auto& blend=description.renderState.blendState.targets[0];blend.blendEnable=true;
            blend.srcBlend=nvrhi::BlendFactor::SrcAlpha;blend.destBlend=nvrhi::BlendFactor::InvSrcAlpha;
            blend.srcBlendAlpha=nvrhi::BlendFactor::One;blend.destBlendAlpha=nvrhi::BlendFactor::InvSrcAlpha;
            overlay_pipeline=checked->createGraphicsPipeline(description,overlay_framebuffers.front()->getFramebufferInfo());
            require(bool(overlay_pipeline),"Hosted overlay pipeline creation failed.");
        }
    }
    void render_overlay(std::uint32_t image_index) {
        if(!hosted || overlay_data.empty())return;
        commands->writeBuffer(overlay_vertices,overlay_data.data(),overlay_data.size()*sizeof(EditorOverlayVertex));
        nvrhi::GraphicsState state;state.pipeline=overlay_pipeline;state.framebuffer=overlay_framebuffers.at(image_index);state.bindings.push_back(overlay_bindings);
        state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(overlay_vertices));
        state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(extent.width),static_cast<float>(extent.height)));
        const float constants[4]={(format==vk::Format::eB8G8R8A8Srgb || format==vk::Format::eR8G8B8A8Srgb) ? 1.0f : 0.0f,0,0,0};
        commands->setGraphicsState(state);commands->setPushConstants(constants,sizeof(constants));
        commands->draw(nvrhi::DrawArguments().setVertexCount(static_cast<std::uint32_t>(overlay_data.size())));
    }

#if POIMA_EDITOR
    void create_ui_scene_bindings() {
        ui_scene_bindings=checked->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0,32))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(0,ui_scene)).addItem(nvrhi::BindingSetItem::Sampler(0,ui_sampler)),ui_layout);
        require(bool(ui_scene_bindings),"Editor Scene compositing bindings failed.");
    }
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
        create_ui_scene_bindings();
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
                require(command.GetTexID()==static_cast<ImTextureID>(1) || command.GetTexID()==static_cast<ImTextureID>(2),
                    "Editor UI accepts only the font atlas (1) and Scene image (2).");
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
        float constants[8]={sx,sy,-1.0f-ui_data->DisplayPos.x*sx,1.0f-ui_data->DisplayPos.y*sy,
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
                const bool scene_image=draw.GetTexID()==static_cast<ImTextureID>(2);
                constants[5]=scene_image ? 1.0f : 0.0f;
                nvrhi::GraphicsState state;state.pipeline=ui_pipeline;state.framebuffer=ui_framebuffers.at(image_index);state.bindings.push_back(scene_image ? ui_scene_bindings : ui_bindings);
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
        profiling::Scope profile_scope("render.frame");
        require(!renderer_fault,"Renderer synchronization failed; recreate the renderer session.");
        const auto frame_started=SteadyClock::now();
        if(!swapchain) { swapchain_dirty=true; return false; }
        prepare_game_ui();
        // Query storage belongs to this Context. Create it before acquiring an
        // image; stopped traces leave it allocated but perform no query work.
        if(profiling::active() && !timestamp_prepared)prepare_timestamps();
        timestamp_recording=bool(timestamp_pool) && (diagnostics.profile_requested || profiling::active());
        // A finite acquire timeout bounds the experiment if presentation stalls.
        vk::ResultValue<std::uint32_t> next(vk::Result::eSuccess,0);
        try { profiling::Scope acquire_scope("render.acquire");next=device.acquireNextImageKHR(swapchain, (editor || hosted) ? 16'000'000ULL : 5'000'000'000ULL,acquired,{},dispatch); }
        catch(const vk::OutOfDateKHRError&) { swapchain_dirty=true; return false; }
        if((editor || hosted) && (next.result==vk::Result::eTimeout || next.result==vk::Result::eNotReady))return false;
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
        double record_ms=0;
        {
        profiling::Scope record_scope("render.record");
        const auto record_started=SteadyClock::now();commands->open();
        if(timestamp_recording)native_commands().resetQueryPool(timestamp_pool,0,5,dispatch);
        timestamp(0);
        if(scene) { commands->writeBuffer(frame_buffer,&frame_constants,sizeof(frame_constants));dispatch_skinning(); }
        timestamp(1);
        if(scene)render_shadows();
        timestamp(2);
        commands->beginTrackingTextureState(texture, nvrhi::AllSubresources,
            initialized[index] ? nvrhi::ResourceStates::Present : nvrhi::ResourceStates::Common);
        auto* scene_target=texture.Get();
#if POIMA_EDITOR
        if(editor)scene_target=ui_scene.Get();
#endif
        commands->clearTextureFloat(multisample_color ? multisample_color.Get() : scene_target, nvrhi::AllSubresources, nvrhi::Color(0.025f, 0.035f, 0.055f, 1.0f));
        if (depth) commands->clearDepthStencilTexture(depth, nvrhi::AllSubresources, true, 1.0f, false, 0);
        // Sky covers only the actual Scene viewport. It leaves depth untouched,
        // so opaque geometry and its MSAA edge samples naturally cover it.
        render_sky(index);
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
        if(scene && multisample_color)commands->resolveTexture(scene_target,nvrhi::AllSubresources,multisample_color,nvrhi::AllSubresources);
        render_game_ui(index);
        render_overlay(index);
#if POIMA_EDITOR
        if(editor) {
            // Composite only through the Scene image command. Window/dock
            // backgrounds and floating panels now obey ImGui's draw order.
            commands->clearTextureFloat(texture,nvrhi::AllSubresources,nvrhi::Color(0.025f,0.035f,0.055f,1.0f));
            render_ui(index);
        }
#endif
        if (capture_frame) commands->copyTexture(staging, {}, texture, {});
        commands->setTextureState(texture, nvrhi::AllSubresources, nvrhi::ResourceStates::Present);
        commands->commitBarriers();timestamp(4);
        commands->close();
        record_ms=elapsed_ms(record_started);
        }
        {
        profiling::Scope submit_scope("render.submit");
        native->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, finished[index], 0);
        checked->executeCommandList(commands);
        }
        vk::PresentInfoKHR present_info;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &finished[index];
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain;
        present_info.pImageIndices = &index;
        vk::Result presented;
        try { profiling::Scope present_scope("render.present");presented=queue.presentKHR(present_info,dispatch); }
        catch(const vk::OutOfDateKHRError&) { presented=vk::Result::eErrorOutOfDateKHR; }
        swapchain_dirty = next.result==vk::Result::eSuboptimalKHR || presented==vk::Result::eSuboptimalKHR || presented==vk::Result::eErrorOutOfDateKHR;
        require(presented == vk::Result::eSuccess || presented == vk::Result::eSuboptimalKHR || presented==vk::Result::eErrorOutOfDateKHR,
            "Vulkan presentation failed.");
        initialized[index] = true;
        // Deliberately serialized for this correctness test, not a frame-time benchmark.
        { profiling::Scope wait_scope("render.wait");require(checked->waitForIdle(), "NVRHI device wait failed."); }
        validate_skin_dispatch();
        checked->runGarbageCollection();
        require(messages.errors == 0, "NVRHI reported a validation/backend error; inspect stderr.");
        ++diagnostics.completed_submissions;diagnostics.last_draws=pending_draws;
        if(diagnostics.profile_requested) {
            const auto frame_ms=elapsed_ms(frame_started);timing_sample(diagnostics.record_cpu,record_ms);timing_sample(diagnostics.render_call_cpu,frame_ms);
        }
        if(timestamp_recording)collect_timestamps(elapsed_ms(frame_started));
        profiling::counter("renderer.completed_submissions",diagnostics.completed_submissions);
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

struct HostedViewport::Impl {
    RenderOptions options;
    SceneSnapshot snapshot;
    Context context;
    RenderReport result;
    void* hwnd=nullptr;
    std::thread::id owner=std::this_thread::get_id();
    bool ready=false,ui_presented=false;
    Impl(const RenderOptions& requested,const SceneSnapshot& scene,void* window):options(requested),snapshot(scene),hwnd(window) {
#ifdef _WIN32
        require(hwnd && IsWindow(static_cast<HWND>(hwnd)),"Hosted viewport requires a valid externally owned HWND.");
        require(GetWindowThreadProcessId(static_cast<HWND>(hwnd),nullptr)==GetCurrentThreadId(),"Hosted viewport must be created on its HWND's owning thread.");
        require((GetWindowLongPtrW(static_cast<HWND>(hwnd),GWL_STYLE)&WS_CHILD)!=0,"Hosted viewport requires a child HWND.");
        require(options.samples==1 || options.samples==2 || options.samples==4 || options.samples==8,"Hosted viewport samples must be 1, 2, 4 or 8.");
        options.capture_exclusive=true;result.samples=options.samples;
        result.detail="Hosted Vulkan viewport awaits a visible, nonempty child window.";
#else
        throw std::runtime_error("Hosted viewport currently supports Windows child HWNDs only.");
#endif
    }
    void check_thread() const { require(std::this_thread::get_id()==owner,"Hosted viewport calls require its creating thread."); }
    std::array<std::uint32_t,2> client_extent() const {
#ifdef _WIN32
        const auto window=static_cast<HWND>(hwnd);require(IsWindow(window),"Hosted HWND was destroyed before its viewport.");
        RECT rectangle{};require(GetClientRect(window,&rectangle),"Cannot query hosted HWND client extent.");
        if(rectangle.right<=rectangle.left || rectangle.bottom<=rectangle.top)return {};
        const auto width=static_cast<std::uint32_t>(rectangle.right-rectangle.left),height=static_cast<std::uint32_t>(rectangle.bottom-rectangle.top);
        require(std::uint64_t(width)*height<=128u*1024u*1024u/4u,"Hosted surface exceeds the 128 MiB RGBA staging budget.");
        return {width,height};
#else
        return {};
#endif
    }
    std::array<std::uint32_t,2> drawable_extent() const {
#ifdef _WIN32
        const auto window=static_cast<HWND>(hwnd);require(IsWindow(window),"Hosted HWND was destroyed before its viewport.");
        if(!IsWindowVisible(window) || IsIconic(GetAncestor(window,GA_ROOT)))return {};
#endif
        return client_extent();
    }
};
HostedViewport::HostedViewport(const RenderOptions& options,const SceneSnapshot& scene,void* hwnd):impl_(std::make_unique<Impl>(options,scene,hwnd)) {}
HostedViewport::~HostedViewport()=default;
std::array<std::uint32_t,2> HostedViewport::extent() const {
    impl_->check_thread();return impl_->client_extent();
}
void HostedViewport::resize() { impl_->check_thread();impl_->context.swapchain_dirty=true;reset_ui_input(); }
void HostedViewport::reset_ui_input() {
    impl_->check_thread();impl_->ui_presented=false;impl_->context.reset_game_ui_input();
}
UiInputResult HostedViewport::ui_input(const std::shared_ptr<const ui::Presentation>& current,const UiInput& event) {
    auto& state=*impl_;state.check_thread();
    const auto size=state.drawable_extent();
    if(!state.ui_presented || !current || state.context.renderer_fault
        || state.context.swapchain_dirty || !size[0] || !size[1]
        || size[0]!=state.result.width || size[1]!=state.result.height) {
        reset_ui_input();return {};
    }
    return state.context.dispatch_game_ui_input(event,current==state.snapshot.logical_ui);
}
void HostedViewport::set_overlay(const std::vector<EditorOverlayVertex>& triangles) {
    impl_->check_thread();
    require(triangles.size()<=max_editor_overlay_vertices && triangles.size()%3==0,"Hosted overlay needs a triangle list with at most 65536 vertices.");
    for(const auto& vertex:triangles) {
        require(std::isfinite(vertex.x) && std::isfinite(vertex.y) && vertex.x>=0 && vertex.x<=1 && vertex.y>=0 && vertex.y<=1,"Hosted overlay coordinates must be finite and in [0,1].");
        for(float channel:vertex.color)require(std::isfinite(channel) && channel>=0 && channel<=1,"Hosted overlay colors must be finite linear RGBA in [0,1].");
    }
    auto copy=triangles;impl_->context.overlay_data.swap(copy);
}
bool HostedViewport::draw(const SceneSnapshot& scene,bool capture) { return draw_frame(scene,capture ? &impl_->options.capture : nullptr); }
bool HostedViewport::draw_capture(const SceneSnapshot& scene,const std::string& path) { return draw_frame(scene,&path); }
bool HostedViewport::draw_frame(const SceneSnapshot& scene,const std::string* capture_path) {
    auto& state=*impl_;auto& context=state.context;state.check_thread();
    state.ui_presented=false;
    try {
        require(!context.renderer_fault && context.messages.errors==0,"Hosted renderer fault; recreate the viewport before drawing again.");
        require(!capture_path || (!capture_path->empty() && capture_path->find('\0')==std::string::npos),"Hosted capture requires a nonempty, NUL-free output path.");
        // Avalonia dispatches native messages. Do not run SDL's message pump
        // inside its dispatcher or consume events belonging to another window.
        // Discard only this wrapper's queued SDL copies of native GUI events.
        if(context.window)SDL_FilterEvents([](void* window,SDL_Event* event)->bool { return SDL_GetWindowFromEvent(event)!=window; },context.window);
        const auto size=state.drawable_extent();if(size[0]==0 || size[1]==0) {context.reset_game_ui_input();return false;}
        state.snapshot=scene;context.scene=&state.snapshot;
        auto options=state.options;options.width=size[0];options.height=size[1];
        try {
            if(!state.ready) {
                context.initialize(options,&state.snapshot,false,state.hwnd);state.ready=true;context.swapchain_dirty=false;
            } else if(context.swapchain_dirty || context.extent.width!=size[0] || context.extent.height!=size[1]) {
                if(!context.rebuild(options))return false;
            }
            context.update_scene();
            context.prepare_overlay();
        } catch(...) { context.renderer_fault=true;throw; }
        if(!context.frame(capture_path!=nullptr)) {context.reset_game_ui_input();return false;}
        ++state.result.frames_presented;state.result.width=context.extent.width;state.result.height=context.extent.height;
        state.ui_presented=true;
        state.result.hardware=context.hardware;state.result.gpu_name=context.gpu_name;
        if(capture_path) { context.capture(*capture_path);state.result.capture_written=true; }
        state.result.success=true;state.result.detail="Native Vulkan child viewport; serialized presentation, no frame-time qualification.";
        return true;
    } catch(const std::exception& error) { state.result.success=false;state.result.detail=error.what();throw; }
}
RenderReport HostedViewport::report() const {
    impl_->check_thread();auto result=impl_->result;result.validation_errors=impl_->context.messages.errors;result.diagnostics=impl_->context.diagnostics;return result;
}

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
    return draw_frame(scene,viewport,ui,capture ? &impl_->options.capture : nullptr);
}
bool EditorViewport::draw_capture(const SceneSnapshot& scene,EditorRect viewport,const ImDrawData* ui,const std::string& capture_path) {
    return draw_frame(scene,viewport,ui,&capture_path);
}
bool EditorViewport::draw_frame(const SceneSnapshot& scene,EditorRect viewport,const ImDrawData* ui,const std::string* capture_path) {
    auto& state=*impl_;auto& context=state.context;
    try {
        require(!context.renderer_fault && context.messages.errors==0,"Renderer fault; recreate the editor viewport before drawing again.");
        require(ImGui::GetCurrentContext()==context.ui_context,"The editor viewport's ImGui context must remain current until teardown.");
        require(!capture_path || (!capture_path->empty() && capture_path->find('\0')==std::string::npos),"Editor capture requires a nonempty, NUL-free output path.");
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
        if(!context.frame(capture_path!=nullptr))return false;
        ++state.result.frames_presented;state.result.width=context.extent.width;state.result.height=context.extent.height;
        if(capture_path) { context.capture(*capture_path);state.result.capture_written=true; }
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

PlayerReport run_player(const PlayerOptions& options, PlayerSession& session) {
    profiling::SourceScope player_source(profiling::Source::player);
    profiling::Scope player_scope("player.run");
    PlayerReport result;
    auto& report=result.render;
    result.initial_tick=session.tick();
    result.initial_session=result.final_session=session.identity();
    Context context;
    SceneSnapshot snapshot;
    PlayerClock clock;
    BoundPlayerInput input(options.input_profile ? *options.input_profile : default_gamepad_input_profile());
    struct GamepadBinding { GamepadHost* host=nullptr; ~GamepadBinding() { if(host)try { host->stop(); }catch(...) {} } } gamepad_binding;
    auto* gamepads=options.replay ? nullptr : options.gamepad_host.get();
    result.gamepad_json=options.replay ? "{\"mode\":\"replay\",\"assigned\":null}" : "{\"mode\":\"disabled\",\"assigned\":null}";
    std::unique_ptr<PlayerAudio> audio;
    try {
        require((options.controller.empty() && !options.replay) || session.controller_valid(options.controller),"Player requires an active CharacterController when selected; replay requires a controller.");
        snapshot=session.snapshot(options.camera);
        context.initialize(options.render,&snapshot,true);
        SDL_SetWindowTitle(context.window,options.replay ? "Poima player — recorded input replay" : "Poima player — configured controls — Esc exits, Tab pauses, click or gamepad Start resumes");
        if(options.audio)audio=std::make_unique<PlayerAudio>(session.audio_state(options.camera));
        bool focused=(SDL_GetWindowFlags(context.window)&SDL_WINDOW_INPUT_FOCUS)!=0;
        bool captured=!options.replay && focused && !options.controller.empty() && (!snapshot.logical_ui || snapshot.logical_ui->modal.empty());
        bool active=!options.replay && focused;
        bool ui_ready=false,ui_presented=false;
        auto ui_modal=[&] { return snapshot.logical_ui && !snapshot.logical_ui->modal.empty(); };
        auto ui_available=[&] { return snapshot.logical_ui && !snapshot.logical_ui->elements.empty(); };
        if(gamepads) { gamepad_binding.host=gamepads;gamepads->start(input,options.gamepad_selection);gamepads->activate(active); }
        if(captured) require(SDL_SetWindowRelativeMouseMode(context.window,true),SDL_GetError());
        bool quit=false;
        std::size_t segment=0;
        std::uint32_t offset=0;
        auto previous=SDL_GetTicksNS();
        auto after_advance=[&](bool save_serviced) {
            if(save_serviced)previous=SDL_GetTicksNS();
            const auto current=session.identity();
            profiling::SessionScope current_session(current);
            if(current==result.final_session) {
                if(audio)audio->advance(session.audio_state(options.camera));
                return false;
            }
            result.final_session=current;++result.runtime_replacements;
            // Runtime ownership changed only after the committed owner advance.
            // Never reuse catch-up ticks, pending edges or DSP from that timeline.
            captured=false;active=false;input.clear();
            ui_ready=false;ui_presented=false;context.reset_game_ui_input();
            if(gamepads)gamepads->activate(false);
            require(SDL_SetWindowRelativeMouseMode(context.window,false),SDL_GetError());
            clock.advance(0,false);previous=SDL_GetTicksNS();
            if(audio)audio->discard_pending();
            require(options.controller.empty() || session.controller_valid(options.controller),"Restored runtime no longer has the selected CharacterController; choose a valid player before resuming.");
            snapshot=session.snapshot(options.camera);
            if(audio)audio->reset(session.audio_state(options.camera));
            if(options.replay) { quit=true;result.stop_reason="runtime_replaced"; }
            else if(result.runtime_replacements>=32) { quit=true;result.stop_reason="runtime_replacement_limit"; }
            return true;
        };
        auto ui_event=[&](const UiInput& event) {
            if(!ui_presented || !ui_available())return ui_modal();
            auto physical=event;
            if(event.kind==UiInputKind::pointer_move || event.kind==UiInputKind::pointer_down || event.kind==UiInputKind::pointer_up || event.kind==UiInputKind::pointer_wheel) {
                int logical_width=0,logical_height=0;
                require(SDL_GetWindowSize(context.window,&logical_width,&logical_height),SDL_GetError());
                if(logical_width<=0 || logical_height<=0) {context.reset_game_ui_input();ui_ready=false;ui_presented=false;return ui_modal();}
                physical.x*=static_cast<float>(context.extent.width)/static_cast<float>(logical_width);
                physical.y*=static_cast<float>(context.extent.height)/static_cast<float>(logical_height);
            }
            const auto response=context.dispatch_game_ui_input(physical,ui_ready);
            if(response.activated) {
                const auto accepted=session.control(result.final_session,snapshot.logical_ui->revision,*response.activated);
                const bool replaced=after_advance(accepted.save_serviced);
                if(!replaced && accepted.intent!=RuntimeControlIntent::none) {
                    active=accepted.intent==RuntimeControlIntent::resume;
                    captured=false;input.clear();clock.advance(0,false);previous=SDL_GetTicksNS();
                    SDL_SetWindowRelativeMouseMode(context.window,false);
                    if(gamepads)gamepads->activate(active);
                }
                snapshot=session.snapshot(options.camera);
                // Same-session queued events still target the visible old UI.
                // Until redraw, consume its regions without invoking Control.
                ui_ready=false;context.reset_game_ui_input();
            }
            return response.consumed;
        };
        while(!quit) {
            profiling::SessionScope frame_session(result.final_session);
            profiling::Scope frame_scope("player.frame",static_cast<std::int64_t>(session.tick()));
            if(!options.replay && ui_modal()) {
                if(captured)SDL_SetWindowRelativeMouseMode(context.window,false);
                captured=false;input.clear();
            }
            const bool navigable=snapshot.logical_ui && std::any_of(snapshot.logical_ui->elements.begin(),snapshot.logical_ui->elements.end(),
                [](const auto& row) {return row.eligible && row.element.kind==ui::Kind::button;});
            if(gamepads)gamepads->ui_active(focused && (ui_modal() || navigable) && !captured);
            SDL_Event event;
            while(SDL_PollEvent(&event)) {
                if(event.type==SDL_EVENT_QUIT || event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED) { quit=true; result.stop_reason="window_closed"; }
                if(event.type==SDL_EVENT_KEY_DOWN && event.key.scancode==SDL_SCANCODE_ESCAPE) {
                    if(!options.replay && ui_modal()) {ui_event({UiInputKind::cancel});continue;}
                    quit=true; result.stop_reason="escape";
                }
                if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST || event.type==SDL_EVENT_WINDOW_MINIMIZED) {
                    focused=false; captured=false; active=false; input.clear();if(gamepads)gamepads->activate(false);
                    ui_ready=false;ui_presented=false;context.reset_game_ui_input();if(gamepads)gamepads->ui_active(false);
                    if(!options.replay) SDL_SetWindowRelativeMouseMode(context.window,false);
                }
                if(event.type==SDL_EVENT_WINDOW_FOCUS_GAINED) focused=true;
                if(event.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type==SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                    context.swapchain_dirty=true;ui_ready=false;ui_presented=false;context.reset_game_ui_input();
                }
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
                if(focused && !captured && ui_available()) {
                    std::optional<UiInput> routed;
                    if(event.type==SDL_EVENT_MOUSE_MOTION)routed=UiInput{UiInputKind::pointer_move,event.motion.x,event.motion.y};
                    else if((event.type==SDL_EVENT_MOUSE_BUTTON_DOWN || event.type==SDL_EVENT_MOUSE_BUTTON_UP) && event.button.button==SDL_BUTTON_LEFT)
                        routed=UiInput{event.type==SDL_EVENT_MOUSE_BUTTON_DOWN ? UiInputKind::pointer_down : UiInputKind::pointer_up,event.button.x,event.button.y};
                    else if(event.type==SDL_EVENT_MOUSE_WHEEL)routed=UiInput{UiInputKind::pointer_wheel,event.wheel.mouse_x,event.wheel.mouse_y,
                        std::clamp(event.wheel.direction==SDL_MOUSEWHEEL_FLIPPED ? event.wheel.y : -event.wheel.y,-100.f,100.f)};
                    else if(event.type==SDL_EVENT_KEY_DOWN && event.key.scancode==SDL_SCANCODE_TAB) {
                        if(event.key.repeat)continue;
                        routed=UiInput{(event.key.mod & SDL_KMOD_SHIFT) ? UiInputKind::focus_previous : UiInputKind::focus_next};
                    } else if((event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_KEY_UP) && event.key.scancode==SDL_SCANCODE_RETURN) {
                        if(event.type==SDL_EVENT_KEY_DOWN && event.key.repeat)continue;
                        routed=UiInput{event.type==SDL_EVENT_KEY_DOWN ? UiInputKind::accept_down : UiInputKind::accept_up};
                    }
                    if(routed && ui_event(*routed))continue;
                    if(ui_modal())continue;
                }
                if(event.type==SDL_EVENT_KEY_DOWN && event.key.scancode==SDL_SCANCODE_TAB) {
                    captured=false;active=false;input.clear();if(gamepads)gamepads->activate(false);SDL_SetWindowRelativeMouseMode(context.window,false);
                }
                if(event.type==SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button==SDL_BUTTON_LEFT && focused && !captured && !options.controller.empty()) {
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
            if(gamepads) {
                const auto batch=gamepads->drain_ui_events();
                if(batch.reset)context.reset_game_ui_input();
                for(std::uint32_t i=0;i<batch.count && focused && !captured;++i) {
                    const auto action=batch.events[i];
                    const auto kind=action==GamepadUiAction::next ? UiInputKind::focus_next : action==GamepadUiAction::previous ? UiInputKind::focus_previous :
                        action==GamepadUiAction::accept_down ? UiInputKind::accept_down : action==GamepadUiAction::accept_up ? UiInputKind::accept_up : UiInputKind::cancel;
                    ui_event({kind});
                }
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
                const bool save_serviced=session.advance({control},offset==0 ? options.sequence[segment].motions : std::vector<KinematicTarget>{},offset==0 ? options.sequence[segment].sounds : std::vector<SoundCommand>{});
                if(++offset==options.sequence[segment].ticks) { offset=0; ++segment; }
                after_advance(save_serviced);
            } else {
                const auto ticks=clock.advance(elapsed,focused && active);
                for(std::uint32_t tick=0;tick<ticks;++tick) {
                    const bool save_serviced=session.advance(options.controller.empty() ? std::vector<RuntimeInput>{} : std::vector<RuntimeInput>{input.peek(options.controller)});
                    if(after_advance(save_serviced))break;
                    if(!options.controller.empty())input.consume(options.controller);
                    // A gameplay tick can open a modal during catch-up. Clear
                    // held input before the next tick, not after all eight.
                    snapshot.logical_ui=session.ui_presentation();
                    if(ui_modal()) {
                        captured=false;input.clear();SDL_SetWindowRelativeMouseMode(context.window,false);
                        if(gamepads)gamepads->ui_active(focused);
                    }
                }
            }
            profiling::SessionScope render_session(result.final_session);
            profiling::Scope presentation_scope("player.presentation",static_cast<std::int64_t>(session.tick()));
            snapshot=session.snapshot(options.camera); context.update_scene();
            ui_ready=context.frame(false);ui_presented=ui_ready;
            if(ui_ready) ++report.frames_presented;
            if(!quit && options.max_frames && report.frames_presented>=options.max_frames) { result.stop_reason="frame_limit"; break; }
        }
        profiling::SessionScope final_session(result.final_session);
        if(audio)audio->finish(session.audio_state(options.camera));
        if(!options.render.capture.empty()) {
            profiling::SessionScope capture_session(result.final_session);
            profiling::Scope capture_scope("player.final_capture",static_cast<std::int64_t>(session.tick()));
            // Final artifact observes the exact final tick without simulating an
            // extra tick. Rebuild once if the surface changed during shutdown.
            snapshot=session.snapshot(options.camera); context.update_scene();
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
    result.final_tick=session.tick();result.final_session=session.identity(); result.dropped_seconds=clock.dropped_seconds();
    report.width=context.extent.width; report.height=context.extent.height; report.samples=context.samples;
    report.hardware=context.hardware; report.gpu_name=context.gpu_name; report.validation_errors=context.messages.errors;report.diagnostics=context.diagnostics;
    return result;
}
} // namespace poima
