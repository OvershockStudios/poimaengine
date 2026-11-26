// SPDX-License-Identifier: Apache-2.0
#include "poima/assets.hpp"
#include <algorithm>
#include <bit>
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

std::uint32_t coverage_rank(std::uint32_t index) {
    // An invertible permutation gives an unambiguous, spatially distributed
    // tie break independent of sort order or worker scheduling.
    const auto spread=std::rotl(index*0x9e3779b1U,11);
    return spread ^ (spread >> 16);
}

void preserve_alpha_coverage(TextureMip& mip,std::uint8_t threshold,std::uint64_t covered,std::uint64_t count) {
    std::array<std::uint32_t,256> histogram{};
    const auto texels=static_cast<std::uint32_t>(mip.rgba.size()/4);
    for(std::uint32_t i=0;i<texels;++i)++histogram[mip.rgba[4*i+3]];
    const auto wanted=std::min<std::uint64_t>((covered*texels+count/2)/count,texels-histogram[0]);
    std::uint32_t present=0;
    for(unsigned i=threshold;i<256;++i)present+=histogram[i];
    if(present==wanted)return;

    // Pick the highest-alpha texels first. Zero-alpha texels are never promoted.
    std::uint32_t above=0;
    unsigned pivot=256;
    if(wanted>0)for(unsigned value=255;value>0;--value) {
        if(above+histogram[value]>=wanted) {
            pivot=value;
            break;
        }
        above+=histogram[value];
    }
    const auto keep=static_cast<std::uint32_t>(wanted-above);
    const bool split=pivot<256 && histogram[pivot]>keep;
    std::vector<std::uint32_t> ties;
    if(split)ties.reserve(histogram[pivot]);
    for(std::uint32_t i=0;i<texels;++i) {
        auto& alpha=mip.rgba[4*i+3];
        if(split && alpha==pivot) {
            ties.push_back(i);
            alpha=static_cast<std::uint8_t>(threshold-1);
        } else alpha=static_cast<std::uint8_t>(std::min(255u,alpha*threshold/pivot));
    }
    if(split) {
        const auto boundary=ties.begin()+keep;
        std::nth_element(ties.begin(),boundary,ties.end(),[](auto a,auto b) {
            return coverage_rank(a)<coverage_rank(b);
        });
        for(auto i=ties.begin();i!=boundary;++i)mip.rgba[4*(*i)+3]=threshold;
    }
}
}
bool valid_texture_sampler(const TextureMap& m) {
    auto wrap=[](int v) { return v==10497 || v==33071 || v==33648; };
    return wrap(m.wrap_s) && wrap(m.wrap_t) && (m.mag_filter==9728 || m.mag_filter==9729) &&
        (m.min_filter==9728 || m.min_filter==9729 || (m.min_filter>=9984 && m.min_filter<=9987));
}
std::vector<TextureMip> texture_mips(TextureMip base,bool srgb,std::uint8_t alpha_coverage_threshold) {
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
            double sum[4]{},weighted_rgb[3]{};
            for(auto sy=static_cast<std::uint32_t>(y0);sy<std::min(previous.height,static_cast<std::uint32_t>(std::ceil(y1)));++sy)
            for(auto sx=static_cast<std::uint32_t>(x0);sx<std::min(previous.width,static_cast<std::uint32_t>(std::ceil(x1)));++sx) {
                const double weight=(std::min(x1,double(sx+1))-std::max(x0,double(sx)))*(std::min(y1,double(sy+1))-std::max(y0,double(sy)));
                const auto offset=(std::size_t(sy)*previous.width+sx)*4;
                for(std::size_t c=0;c<4;++c) {
                    double value=previous.rgba[offset+c]/255.0;
                    if(srgb && c<3)value=linear(value);
                    sum[c]+=value*weight;
                    if(alpha_coverage_threshold && c<3)weighted_rgb[c]+=value*weight*(previous.rgba[offset+3]/255.0);
                }
            }
            for(std::size_t c=0;c<4;++c) {
                auto value=alpha_coverage_threshold && c<3 && sum[3]>0 ?
                    weighted_rgb[c]/sum[3] : sum[c]/((x1-x0)*(y1-y0));
                if(srgb && c<3)value=srgb_encode(value);
                next.rgba[(std::size_t(y)*next.width+x)*4+c]=static_cast<std::uint8_t>(std::clamp(std::lround(value*255),0l,255l));
            }
        }
        result.push_back(std::move(next));
    }
    if(alpha_coverage_threshold) {
        const auto& original=result.front().rgba;
        std::uint64_t covered=0;
        for(std::size_t i=3;i<original.size();i+=4)covered+=original[i]>=alpha_coverage_threshold;
        // Compute every correction against mip zero. Adjusted levels never
        // become the input to filtering or propagate their tie decisions.
        for(std::size_t i=1;i<result.size();++i)
            preserve_alpha_coverage(result[i],alpha_coverage_threshold,covered,original.size()/4);
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
