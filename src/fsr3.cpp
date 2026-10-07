// SPDX-License-Identifier: Apache-2.0
#include "poima/fsr3.hpp"
#include <bit>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
// Diagnostic accessor only: relative to the pinned SDK public include root.
#include <../src/components/fsr3upscaler/ffx_fsr3upscaler_private.h>
#include <type_traits>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace poima::fsr3 {
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void checked(FfxErrorCode code, const char* operation) {
    if (code != FFX_OK) throw std::runtime_error(std::string(operation)+" failed (FSR error "+std::to_string(code)+")");
}
void extent(Extent value) { require(value.width && value.height && value.width <= 16384 && value.height <= 16384, "Invalid FSR extent"); }
bool same(Extent a, Extent b) { return a.width==b.width && a.height==b.height; }
struct Device {
    // ffxGetInterfaceVK's pinned backend interprets FfxDevice as VkDeviceContext*.
    // Own this prefix instead of using ffxGetDeviceVK's process-global singleton.
    VkDeviceContext native{};
    PFN_vkGetDeviceProcAddr original_proc{};
    FfxGetDeviceCapabilitiesFunc original_capabilities{};
    FfxCreateBackendContextFunc original_create{};
    std::uint32_t subgroup_size{};
    bool backend_created{};
};
static_assert(offsetof(Device,native)==0);
// The pinned backend resolves its function table synchronously inside backend
// creation. Preserve each caller's resolver, including nested/reentrant creation.
thread_local PFN_vkGetDeviceProcAddr active_resolver=nullptr;
struct ResolverScope {
    PFN_vkGetDeviceProcAddr previous=active_resolver;
    explicit ResolverScope(PFN_vkGetDeviceProcAddr resolver){active_resolver=resolver;}
    ~ResolverScope(){active_resolver=previous;}
};
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL resolve_device_function(VkDevice device,const char* name) {
    if(!active_resolver)return nullptr;
    auto result=active_resolver(device,name);
    // Vulkan 1.1 promoted this operation. Its KHR alias need not be exposed if
    // the old extension wasn't enabled on a core 1.3 device.
    if(!result && std::strcmp(name,"vkGetBufferMemoryRequirements2KHR")==0) {
        result=active_resolver(device,"vkGetBufferMemoryRequirements2");
    }
    return result;
}
FfxErrorCode capabilities(FfxInterface* backend, FfxDeviceCapabilities* output) {
    auto& device=*static_cast<Device*>(backend->device);
    auto result=device.original_capabilities(backend,output);
    if(result!=FFX_OK) return result;
    // Physical support is not proof that an optional VkDevice feature was enabled.
    output->fp16Supported=false;
    output->waveLaneCountMin=output->waveLaneCountMax=device.subgroup_size;
    output->raytracingSupported=false;
    output->deviceCoherentMemorySupported=false;
    output->dedicatedAllocationSupported=false;
    output->bufferMarkerSupported=false;
    output->extendedSynchronizationSupported=false;
    output->shaderStorageBufferArrayNonUniformIndexing=false;
    return FFX_OK;
}
FfxErrorCode create_backend(FfxInterface* backend,FfxEffect effect,FfxEffectBindlessConfig* bindless,FfxUInt32* id) {
    auto& device=*static_cast<Device*>(backend->device);
    ResolverScope resolver(device.original_proc);
    auto result=device.original_create(backend,effect,bindless,id);
    device.backend_created=result==FFX_OK;
    return result;
}
FfxResource resource(const Image& image, Extent expected, VkFormat format, bool writable, bool depth=false) {
    require(image.image && same(image.extent,expected) && image.format==format,"FSR image format/extent mismatch");
    FfxResourceDescription desc{};
    desc.type=FFX_RESOURCE_TYPE_TEXTURE2D;desc.format=ffxGetSurfaceFormatVK(format);
    desc.width=expected.width;desc.height=expected.height;desc.depth=1;desc.mipCount=1;
    desc.usage=depth?FFX_RESOURCE_USAGE_DEPTHTARGET:writable?FFX_RESOURCE_USAGE_UAV:FFX_RESOURCE_USAGE_READ_ONLY;
    return ffxGetResourceVK(reinterpret_cast<void*>(image.image),desc,nullptr,writable?FFX_RESOURCE_STATE_UNORDERED_ACCESS:FFX_RESOURCE_STATE_COMPUTE_READ);
}
}
struct alignas(32) ScratchBlock { std::byte data[32]; };
static_assert(sizeof(ScratchBlock)==32 && alignof(ScratchBlock)==32);
struct Context::Impl {
    Device device;
    std::vector<ScratchBlock> scratch;
    FfxFsr3UpscalerContext context{};
    Extent render,output;
    std::thread::id owner=std::this_thread::get_id();
    std::uint64_t bytes{};
    bool alive{},poisoned{},last_reset{};
    std::uint64_t recorded_dispatches{};
    ~Impl(){ if(alive) ffxFsr3UpscalerContextDestroy(&context); }
};
Context::Context(VkDevice device,VkPhysicalDevice physical,PFN_vkGetDeviceProcAddr proc,Extent render,Extent output)
    :impl_(std::make_unique<Impl>()) {
    require(device&&physical&&proc,"FSR needs a live Vulkan device");extent(render);extent(output);
    require(render.width<=output.width&&render.height<=output.height,"FSR render extent exceeds output");
    auto& p=*impl_;p.render=render;p.output=output;p.device.native={device,physical,resolve_device_function};p.device.original_proc=proc;
    require(proc(device,"vkGetBufferMemoryRequirements2KHR") || proc(device,"vkGetBufferMemoryRequirements2"),"FSR Vulkan backend requires buffer memory requirements2 (core1.1 or KHR)");
    VkPhysicalDeviceSubgroupProperties subgroup{};subgroup.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};properties.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;properties.pNext=&subgroup;
    vkGetPhysicalDeviceProperties2(physical,&properties);
    require(subgroup.subgroupSize && (subgroup.supportedStages&VK_SHADER_STAGE_COMPUTE_BIT),"FSR requires compute subgroups");
    require((subgroup.supportedOperations & (VK_SUBGROUP_FEATURE_BASIC_BIT|VK_SUBGROUP_FEATURE_QUAD_BIT)) == (VK_SUBGROUP_FEATURE_BASIC_BIT|VK_SUBGROUP_FEATURE_QUAD_BIT),"FSR requires basic and quad subgroup operations");
    p.device.subgroup_size=subgroup.subgroupSize;
    auto size=ffxGetScratchMemorySizeVK(physical,1);
    p.scratch.resize((size+sizeof(ScratchBlock)-1)/sizeof(ScratchBlock));
    FfxFsr3UpscalerContextDescription desc{};
    checked(ffxGetInterfaceVK(&desc.backendInterface,reinterpret_cast<FfxDevice>(&p.device),p.scratch.data(),size,1),"FSR Vulkan interface");
    p.device.original_capabilities=desc.backendInterface.fpGetDeviceCapabilities;
    p.device.original_create=desc.backendInterface.fpCreateBackendContext;
    desc.backendInterface.fpGetDeviceCapabilities=capabilities;
    desc.backendInterface.fpCreateBackendContext=create_backend;
    desc.flags=FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
    desc.maxRenderSize={render.width,render.height};desc.maxUpscaleSize={output.width,output.height};
    auto result=ffxFsr3UpscalerContextCreate(&p.context,&desc);
    // Release partially initialized effect resources only after backend creation succeeded.
    if(result!=FFX_OK){if(p.device.backend_created)ffxFsr3UpscalerContextDestroy(&p.context);checked(result,"FSR context creation");}
    p.alive=true;
    FfxEffectMemoryUsage memory{};
    checked(ffxFsr3UpscalerContextGetGpuMemoryUsage(&p.context,&memory),"FSR memory query");p.bytes=memory.totalUsageInBytes;
}
Context::~Context()=default;
void Context::dispatch(const Dispatch& input) {
    auto& p=*impl_;require(p.owner==std::this_thread::get_id(),"FSR context used from another thread");
    require(!p.poisoned&&input.commands,"FSR context is invalid or command buffer missing");
    require(std::isfinite(input.near_plane)&&std::isfinite(input.far_plane)&&input.near_plane>0&&input.far_plane>input.near_plane,"Invalid FSR clip planes");
    require(std::isfinite(input.vertical_fov_radians)&&input.vertical_fov_radians>0&&input.vertical_fov_radians<3.14159265f,"Invalid FSR field of view");
    require(std::isfinite(input.delta_milliseconds)&&input.delta_milliseconds>=0&&input.delta_milliseconds<=1000,"Invalid FSR frame interval");
    require(std::isfinite(input.jitter_pixels[0])&&std::isfinite(input.jitter_pixels[1])&&std::abs(input.jitter_pixels[0])<=.5f&&std::abs(input.jitter_pixels[1])<=.5f,"Invalid FSR jitter");
    FfxFsr3UpscalerDispatchDescription d{};d.commandList=ffxGetCommandListVK(input.commands);
    d.color=resource(input.color,p.render,VK_FORMAT_R16G16B16A16_SFLOAT,false);
    d.depth=resource(input.depth,p.render,VK_FORMAT_D32_SFLOAT,false,true);
    d.motionVectors=resource(input.motion,p.render,VK_FORMAT_R32G32_SFLOAT,false);
    if(input.reactive.image)d.reactive=resource(input.reactive,p.render,VK_FORMAT_R8_UNORM,false);
    d.output=resource(input.output,p.output,VK_FORMAT_R16G16B16A16_SFLOAT,true);
    d.dilatedDepth=resource(input.dilated_depth,p.render,VK_FORMAT_R32_SFLOAT,true);
    d.dilatedMotionVectors=resource(input.dilated_motion,p.render,VK_FORMAT_R16G16_SFLOAT,true);
    d.reconstructedPrevNearestDepth=resource(input.reconstructed_depth,p.render,VK_FORMAT_R32_UINT,true);
    d.jitterOffset={input.jitter_pixels[0],input.jitter_pixels[1]};
    d.motionVectorScale={static_cast<float>(p.render.width),static_cast<float>(p.render.height)};
    d.renderSize={p.render.width,p.render.height};d.upscaleSize={p.output.width,p.output.height};
    d.frameTimeDelta=input.delta_milliseconds;d.preExposure=1;d.reset=input.reset;
    d.cameraNear=input.near_plane;d.cameraFar=input.far_plane;d.cameraFovAngleVertical=input.vertical_fov_radians;d.viewSpaceToMetersFactor=1;
    // No jitter cancellation, inverted depth, sharpening, automatic exposure or frame generation.
    auto result=ffxFsr3UpscalerContextDispatch(&p.context,&d);if(result!=FFX_OK)p.poisoned=true;checked(result,"FSR dispatch");
    p.last_reset=input.reset || p.recorded_dispatches==0;++p.recorded_dispatches;
}
DiagnosticResources Context::diagnostic_resources() const {
    const auto& p=*impl_;
    require(p.owner==std::this_thread::get_id() && p.alive && !p.poisoned && p.recorded_dispatches,"FSR diagnostics require a successful owner-thread dispatch");
    static_assert(FFX_FSR3UPSCALER_VERSION_MAJOR==3 && FFX_FSR3UPSCALER_VERSION_MINOR==1 && FFX_FSR3UPSCALER_VERSION_PATCH==4);
    static_assert(std::is_trivially_copyable_v<FfxFsr3UpscalerContext_Private>);
    static_assert(sizeof(FfxFsr3UpscalerContext_Private)<=sizeof(FfxFsr3UpscalerContext));
    static_assert(offsetof(FfxFsr3UpscalerContext,data)==0);
    require(ffxFsr3UpscalerGetEffectVersion()==FFX_SDK_MAKE_VERSION(3,1,4),"FSR diagnostic private layout version differs");
    // The SDK itself overlays this private struct onto public opaque storage.
    // Copy its trivially-copyable representation to an aligned local object;
    // do not create an aliasing/lifetime assumption through an application cast.
    FfxFsr3UpscalerContext_Private snapshot{};
    std::memcpy(&snapshot,&p.context,sizeof(snapshot));
    auto& backend=snapshot.contextDescription.backendInterface;
    require(backend.fpGetResource!=nullptr && backend.scratchBuffer!=nullptr,"FSR diagnostic backend unavailable");
    const FfxResourceInternal bindings[]={snapshot.srvResources[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS],
        snapshot.srvResources[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR],
        snapshot.uavResources[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR],
        snapshot.srvResources[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_INSTABILITY]};
    const FfxSurfaceFormat formats[]={FFX_SURFACE_FORMAT_R8G8B8A8_UNORM,FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,FFX_SURFACE_FORMAT_R16_FLOAT};
    const VkFormat native_formats[]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16_SFLOAT};
    DiagnosticResources result;result.recorded_dispatches=p.recorded_dispatches;result.sdk_effect_version=ffxFsr3UpscalerGetEffectVersion();
    result.effect_context_id=snapshot.effectContextId;result.next_resource_frame_index=snapshot.resourceFrameIndex;
    result.accumulation_frame_index=snapshot.constants.frameIndex;result.reset=p.last_reset;result.previous_history_used=p.recorded_dispatches>1 && !p.last_reset;
    require(bindings[1].internalIndex!=bindings[2].internalIndex,"FSR prior/current history aliases unexpectedly");
    for(std::size_t i=0;i<result.images.size();++i) {
        require(bindings[i].internalIndex>=0,"FSR diagnostic resource index is invalid");
        auto& out=result.images[i];out.sdk_resource_index=static_cast<std::uint32_t>(bindings[i].internalIndex);
        out.image.format=native_formats[i];out.image.extent=(i==1 || i==2) ? p.output : p.render;
        // The previous allocation has never been produced on the first dispatch.
        // Do not query it: fpGetResource clears undefined metadata as a side effect.
        if(i==1 && p.recorded_dispatches==1)continue;
        // All queried images have already been initialized/read/written by this
        // dispatch. Therefore GetResourceVK does not alter undefined metadata.
        const auto resource=backend.fpGetResource(&backend,bindings[i]);
        require(resource.resource && !(resource.description.flags&FFX_RESOURCE_FLAGS_UNDEFINED),"FSR diagnostic image is undefined");
        require(resource.description.type==FFX_RESOURCE_TYPE_TEXTURE2D && resource.description.format==formats[i] && resource.description.mipCount==1 && resource.description.depth==1 &&
            resource.description.width==out.image.extent.width && resource.description.height==out.image.extent.height,"FSR diagnostic format/extent changed");
        out.image.image=reinterpret_cast<VkImage>(resource.resource);
        // Exact pinned backend mappings; reject unrecognized states rather than
        // guessing a layout and corrupting the SDK's internal state tracker.
        switch(resource.state) {
        case FFX_RESOURCE_STATE_COMPUTE_READ:
        case FFX_RESOURCE_STATE_PIXEL_READ:
        case FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ:
            out.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;out.stages=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;out.access=VK_ACCESS_SHADER_READ_BIT;break;
        case FFX_RESOURCE_STATE_UNORDERED_ACCESS:
            out.layout=VK_IMAGE_LAYOUT_GENERAL;out.stages=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;out.access=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;break;
        case FFX_RESOURCE_STATE_COPY_SRC:
            out.layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;out.stages=VK_PIPELINE_STAGE_TRANSFER_BIT;out.access=VK_ACCESS_TRANSFER_READ_BIT;break;
        case FFX_RESOURCE_STATE_COPY_DEST:
            out.layout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;out.stages=VK_PIPELINE_STAGE_TRANSFER_BIT;out.access=VK_ACCESS_TRANSFER_WRITE_BIT;break;
        default:throw std::runtime_error("FSR diagnostic resource has an unsupported state");
        }
        out.available=true;
    }
    return result;
}
std::uint64_t Context::gpu_bytes()const noexcept{return impl_->bytes;}
std::uint64_t Context::scratch_bytes()const noexcept{return impl_->scratch.size()*sizeof(ScratchBlock);}
std::array<float,2> jitter(std::uint64_t index,Extent render,Extent output){
    extent(render);extent(output);require(render.width<=output.width&&render.height<=output.height,"FSR render extent exceeds output");
    const auto phases=ffxFsr3UpscalerGetJitterPhaseCount(static_cast<int>(render.width),static_cast<int>(output.width));require(phases>0,"Invalid FSR jitter phase count");
    std::array<float,2> result{};checked(ffxFsr3UpscalerGetJitterOffset(&result[0],&result[1],static_cast<int>(index%static_cast<std::uint64_t>(phases)),phases),"FSR jitter");return result;
}
} // namespace poima::fsr3
