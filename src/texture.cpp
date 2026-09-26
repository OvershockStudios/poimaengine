// SPDX-License-Identifier: Apache-2.0
#include "poima/assets.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {
constexpr std::size_t allocation_limit=128*1024*1024;
thread_local std::size_t image_allocated=0;
struct alignas(std::max_align_t) ImageAllocation { std::size_t size; };
void* image_malloc(std::size_t size) {
    if(size>allocation_limit-image_allocated)return nullptr;
    auto* p=static_cast<ImageAllocation*>(std::malloc(sizeof(ImageAllocation)+size));
    if(!p)return nullptr;
    p->size=size;image_allocated+=size;return p+1;
}
void image_free(void* ptr) {
    if(!ptr)return;
    auto* p=static_cast<ImageAllocation*>(ptr)-1;image_allocated-=p->size;std::free(p);
}
void* image_realloc(void* ptr,std::size_t size) {
    if(!ptr)return image_malloc(size);
    auto* old=static_cast<ImageAllocation*>(ptr)-1;
    if(size>allocation_limit-(image_allocated-old->size))return nullptr;
    const auto previous=old->size;
    auto* p=static_cast<ImageAllocation*>(std::realloc(old,sizeof(ImageAllocation)+size));
    if(!p)return nullptr;
    p->size=size;image_allocated=image_allocated-previous+size;return p+1;
}
}
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS 4096
#define STBI_MALLOC image_malloc
#define STBI_REALLOC image_realloc
#define STBI_FREE image_free
#include <stb/stb_image.h>

namespace poima {
namespace {
void require(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
double linear(double x) { return x<=0.04045 ? x/12.92 : std::pow((x+0.055)/1.055,2.4); }
double srgb_encode(double x) { return x<=0.0031308 ? x*12.92 : 1.055*std::pow(x,1/2.4)-0.055; }
}
bool valid_texture_sampler(const TextureMap& m) {
    auto wrap=[](int v) { return v==10497 || v==33071 || v==33648; };
    return wrap(m.wrap_s) && wrap(m.wrap_t) && (m.mag_filter==9728 || m.mag_filter==9729) &&
        (m.min_filter==9728 || m.min_filter==9729 || (m.min_filter>=9984 && m.min_filter<=9987));
}
std::vector<TextureMip> texture_mips(TextureMip base,bool srgb) {
    require(base.width && base.height && base.width<=4096 && base.height<=4096 &&
        std::size_t(base.width)*base.height<=4*1024*1024 && base.rgba.size()==std::size_t(base.width)*base.height*4,"Texture dimensions/data exceed limits.");
    std::vector<TextureMip> result;result.push_back(std::move(base));
    while(result.back().width>1 || result.back().height>1) {
        const auto& previous=result.back();TextureMip next;
        next.width=std::max(1u,previous.width/2);next.height=std::max(1u,previous.height/2);next.rgba.resize(std::size_t(next.width)*next.height*4);
        // Area box filtering includes every edge texel of non-power-of-two images.
        for(std::uint32_t y=0;y<next.height;++y)for(std::uint32_t x=0;x<next.width;++x) {
            const double x0=double(x)*previous.width/next.width,x1=double(x+1)*previous.width/next.width;
            const double y0=double(y)*previous.height/next.height,y1=double(y+1)*previous.height/next.height;
            double sum[4]{};
            for(auto sy=static_cast<std::uint32_t>(y0);sy<std::min(previous.height,static_cast<std::uint32_t>(std::ceil(y1)));++sy)
            for(auto sx=static_cast<std::uint32_t>(x0);sx<std::min(previous.width,static_cast<std::uint32_t>(std::ceil(x1)));++sx) {
                const double weight=(std::min(x1,double(sx+1))-std::max(x0,double(sx)))*(std::min(y1,double(sy+1))-std::max(y0,double(sy)));
                for(std::size_t c=0;c<4;++c) {
                    double value=previous.rgba[(std::size_t(sy)*previous.width+sx)*4+c]/255.0;
                    if(srgb && c<3)value=linear(value);
                    sum[c]+=value*weight;
                }
            }
            for(std::size_t c=0;c<4;++c) {
                auto value=sum[c]/((x1-x0)*(y1-y0));if(srgb && c<3)value=srgb_encode(value);
                next.rgba[(std::size_t(y)*next.width+x)*4+c]=static_cast<std::uint8_t>(std::clamp(std::lround(value*255),0l,255l));
            }
        }
        result.push_back(std::move(next));
    }
    return result;
}
std::shared_ptr<const TextureImage> decode_texture(std::span<const std::byte> bytes,bool srgb) {
    require(!bytes.empty() && bytes.size()<=32*1024*1024,"Encoded image exceeds the 32 MiB limit.");
    const auto* data=reinterpret_cast<const stbi_uc*>(bytes.data());const auto length=static_cast<int>(bytes.size());
    int w=0,h=0,channels=0;
    require(stbi_info_from_memory(data,length,&w,&h,&channels)!=0,"Invalid or unsupported PNG/JPEG image.");
    require(w>0 && h>0 && w<=4096 && h<=4096 && std::size_t(w)*std::size_t(h)<=4*1024*1024,"Image exceeds dimension/pixel budget.");
    require(!stbi_is_16_bit_from_memory(data,length),"16-bit images are not supported by the initial RGBA8 texture profile.");
    std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> decoded(stbi_load_from_memory(data,length,&w,&h,&channels,4),stbi_image_free);
    require(bool(decoded),"PNG/JPEG decoding failed or exceeded the allocation budget.");
    TextureMip base;base.width=static_cast<std::uint32_t>(w);base.height=static_cast<std::uint32_t>(h);
    base.rgba.assign(decoded.get(),decoded.get()+std::size_t(w)*std::size_t(h)*4);
    auto result=std::make_shared<TextureImage>();result->srgb=srgb;result->mips=texture_mips(std::move(base),srgb);return result;
}
}
