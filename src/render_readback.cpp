// SPDX-License-Identifier: Apache-2.0
#include "render_readback.hpp"

#include "poima/fsr3.hpp"
#include "poima/profiler.hpp"
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace poima::render_readback {
namespace {

void require(bool condition, const std::string& detail) {
    if (!condition) throw std::runtime_error(detail);
}

void save_bmp_exclusive(SDL_Surface* surface,const std::string& path) {
    require(!path.empty() && path.find('\0')==std::string::npos,"Invalid exclusive capture path.");
    require(surface && surface->w>0 && surface->h>0,"Invalid BMP capture surface.");
    constexpr std::uint64_t budget=512ull*1024*1024;
    const auto pixels=std::uint64_t(surface->w)*std::uint64_t(surface->h);
    require(pixels<=(budget-4096)/4,"BMP exceeds the 512 MiB capture budget.");
    std::unique_ptr<SDL_IOStream,decltype(&SDL_CloseIO)> memory(SDL_IOFromDynamicMem(),SDL_CloseIO);
    require(bool(memory),std::string("BMP memory stream: ")+SDL_GetError());
    // SDL otherwise grows this stream in 1 KiB chunks as each row is written.
    // Reserve a bounded full-frame chunk without changing the BMP serializer.
    require(SDL_SetNumberProperty(SDL_GetIOProperties(memory.get()),SDL_PROP_IOSTREAM_DYNAMIC_CHUNKSIZE_NUMBER,
        static_cast<Sint64>(pixels*4+4096)),std::string("BMP allocation chunk: ")+SDL_GetError());
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

} // namespace

nvrhi::StagingTextureHandle allocate_capture(nvrhi::IDevice& device,
    const nvrhi::TextureDesc& source, std::uint32_t width, std::uint32_t height) {
    require(source.width==width && source.height==height && source.sampleCount==1,
        "Capture source must match the single-sample swapchain extent.");
    require(std::uint64_t(source.width)*source.height<=128u*1024u*1024u/4u,
        "Capture surface exceeds the 128 MiB RGBA staging budget.");
    nvrhi::TextureDesc desc;desc.width=source.width;desc.height=source.height;
    desc.format=source.format;desc.isRenderTarget=false;desc.isShaderResource=false;
    desc.debugName="Native owner capture staging";
    // No simulation, swapchain rebuild, history reset or command recording:
    // this is an owned readback allocation for the current surface only.
    auto prepared=device.createStagingTexture(desc,nvrhi::CpuAccessMode::Read);
    require(bool(prepared),"NVRHI capture staging texture creation failed.");
    return prepared;
}

void publish_capture(nvrhi::IDevice& device, nvrhi::IStagingTexture* staging,
    std::uint32_t width, std::uint32_t height, bool bgra, bool exclusive,
    const std::string& path) {
    profiling::Scope profile_scope("renderer.capture_readback_write");
    std::size_t row_pitch = 0;
    void* pixels = device.mapStagingTexture(staging, {}, nvrhi::CpuAccessMode::Read, &row_pitch);
    require(pixels != nullptr, "GPU capture readback mapping failed.");
    SDL_Surface* image = SDL_CreateSurfaceFrom(static_cast<int>(width), static_cast<int>(height),
        bgra ? SDL_PIXELFORMAT_BGRA32 : SDL_PIXELFORMAT_RGBA32, pixels, static_cast<int>(row_pitch));
    if(exclusive) {
        try { require(image!=nullptr,std::string("BMP capture surface: ")+SDL_GetError());save_bmp_exclusive(image,path); }
        catch(...) { if(image)SDL_DestroySurface(image);device.unmapStagingTexture(staging);throw; }
        SDL_DestroySurface(image);device.unmapStagingTexture(staging);return;
    }
    const bool saved = image && SDL_SaveBMP(image, path.c_str());
    const std::string detail = saved ? "" : std::string("BMP capture: ") + SDL_GetError();
    if (image) SDL_DestroySurface(image);
    device.unmapStagingTexture(staging);
    require(saved, detail);
}

void allocate_product_probes(nvrhi::IDevice& device, std::uint32_t count,
    bool ambient_occlusion, std::array<nvrhi::StagingTextureHandle, 8>& staging) {
    const std::array<nvrhi::Format,8> probe_formats={nvrhi::Format::D32,nvrhi::Format::RGBA16_FLOAT,nvrhi::Format::RG32_FLOAT,nvrhi::Format::R8_UNORM,nvrhi::Format::RGBA16_FLOAT,nvrhi::Format::RGBA16_FLOAT,nvrhi::Format::R16_FLOAT,nvrhi::Format::R16_FLOAT};
    for(std::size_t i=0;i<(ambient_occlusion ? 8u : 6u);++i) {
        // Pinned NVRHI Vulkan treats staging X as a byte offset. Use row origins instead;
        // four texels keep even R8 rows aligned to the required four-byte copy offset.
        nvrhi::TextureDesc desc;desc.width=4;desc.height=count;desc.format=probe_formats[i];desc.debugName="Sparse scene product capture";
        staging[i]=device.createStagingTexture(desc,nvrhi::CpuAccessMode::Read);
        require(bool(staging[i]),"Scene product probe staging allocation failed.");
    }
}

void read_product_probes(nvrhi::IDevice& device, std::span<SceneProductSample> samples_out,
    std::span<const nvrhi::StagingTextureHandle> staging, nvrhi::IBuffer* fsr_history,
    const fsr3::DiagnosticResources& fsr_resources) {
    auto half=[](std::uint16_t bits) {
        const float sign=(bits&0x8000u) ? -1.0f : 1.0f;
        const unsigned exponent=(bits>>10)&31u,mantissa=bits&1023u;
        if(exponent==31)return mantissa ? std::numeric_limits<float>::quiet_NaN() : sign*std::numeric_limits<float>::infinity();
        return sign*std::ldexp(static_cast<float>(exponent ? mantissa+1024u : mantissa),exponent ? static_cast<int>(exponent)-25 : -24);
    };
    for(std::size_t product=0;product<staging.size();++product) {
        std::size_t pitch=0;
        const auto* mapped=static_cast<const unsigned char*>(device.mapStagingTexture(staging[product],{},nvrhi::CpuAccessMode::Read,&pitch));
        require(mapped!=nullptr,"Scene product probe mapping failed.");
        for(std::size_t i=0;i<samples_out.size();++i) {
            const auto* pixel=mapped+i*pitch;auto& sample=samples_out[i];
            if(product==0)std::memcpy(&sample.depth,pixel,4);
            else if(product==1) {
                std::uint16_t components[4]{};std::memcpy(components,pixel,8);
                for(std::size_t k=0;k<3;++k)sample.shading_normal[k]=half(components[k]);
                sample.surface_valid=half(components[3])>.5f;
            } else if(product==2)std::memcpy(sample.motion.data(),pixel,8);
            else if(product==3) {
                sample.motion_validity_code=*pixel;
                sample.motion_valid=*pixel>=surface_mark_reactive_code;
            }
            else if(product>=6) {std::uint16_t value{};std::memcpy(&value,pixel,2);(product==6 ? sample.raw_ambient_visibility : sample.ambient_visibility)=half(value);}
            else {std::uint16_t components[4]{};std::memcpy(components,pixel,8);auto& color=product==4 ? sample.raw_hdr : sample.resolved_hdr;for(std::size_t k=0;k<3;++k)color[k]=half(components[k]);}
        }
        device.unmapStagingTexture(staging[product]);
    }
    if(fsr_history) {
        const auto* mapped=static_cast<const unsigned char*>(device.mapBuffer(fsr_history,nvrhi::CpuAccessMode::Read));
        require(mapped!=nullptr,"FSR history readback mapping failed.");
        for(std::size_t i=0;i<samples_out.size();++i) {
            Fsr3HistorySample history_sample;const auto* pixel=mapped+i*64;
            for(std::size_t c=0;c<4;++c)history_sample.masks[c]=pixel[c]/255.f;
            history_sample.previous_history_available=fsr_resources.images[1].available;
            history_sample.previous_history_used=fsr_resources.previous_history_used;
            history_sample.sdk_dispatch_sequence=fsr_resources.recorded_dispatches;
            for(std::size_t r=0;r<4;++r)history_sample.sdk_resource_indices[r]=fsr_resources.images[r].sdk_resource_index;
            for(std::size_t r=1;r<=2;++r)if(fsr_resources.images[r].available) {
                std::uint16_t values[4]{};std::memcpy(values,pixel+r*16,8);
                auto& color=r==1 ? history_sample.previous_history : history_sample.current_history;
                for(std::size_t c=0;c<4;++c)color[c]=half(values[c]);
            }
            std::uint16_t luma=0;std::memcpy(&luma,pixel+48,2);history_sample.luma_instability=half(luma);
            samples_out[i].fsr3_history=history_sample;
        }
        device.unmapBuffer(fsr_history);
    }
    for(const auto& sample:samples_out) {
        require(std::isfinite(sample.depth),"Scene depth probe is nonfinite.");
        for(float value:sample.shading_normal)require(std::isfinite(value),"Scene normal probe is nonfinite.");
        for(float value:sample.motion)require(std::isfinite(value),"Scene motion probe is nonfinite.");
        for(float value:sample.raw_hdr)require(std::isfinite(value),"Raw HDR probe is nonfinite.");
        require(std::isfinite(sample.raw_ambient_visibility) && sample.raw_ambient_visibility>=0 && sample.raw_ambient_visibility<=1 && std::isfinite(sample.ambient_visibility) && sample.ambient_visibility>=0 && sample.ambient_visibility<=1,"Ambient visibility probe is invalid.");
        for(float value:sample.resolved_hdr)require(std::isfinite(value),"Resolved HDR probe is nonfinite.");
    }
}

} // namespace poima::render_readback
