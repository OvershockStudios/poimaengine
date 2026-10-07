// SPDX-License-Identifier: Apache-2.0
#include "poima/capture_image.hpp"
#include "poima/assets.hpp"
#include <lodepng.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace poima {
namespace {
void require(bool ok,const char* message){if(!ok)throw std::invalid_argument(message);}
std::uint32_t u32(std::span<const std::byte> b,std::size_t p){
    require(p<=b.size() && b.size()-p>=4,"Truncated BMP field.");
    return std::to_integer<std::uint32_t>(b[p]) | (std::to_integer<std::uint32_t>(b[p+1])<<8) |
        (std::to_integer<std::uint32_t>(b[p+2])<<16) | (std::to_integer<std::uint32_t>(b[p+3])<<24);
}
std::uint16_t u16(std::span<const std::byte> b,std::size_t p){
    require(p<=b.size() && b.size()-p>=2,"Truncated BMP field.");
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(b[p]) | (std::to_integer<unsigned>(b[p+1])<<8));
}
}
CaptureImage capture_image(std::span<const std::byte> bmp,std::uint32_t max_edge){
    require(max_edge>=128 && max_edge<=2048,"Capture max_edge must be 128..2048.");
    require(bmp.size()>=54 && bmp.size()<=128ull*1024*1024,"BMP size is outside capture limits.");
    require(u16(bmp,0)==0x4d42 && u32(bmp,6)==0,"Invalid BMP file header.");
    const auto file_size=u32(bmp,2),offset=u32(bmp,10),dib=u32(bmp,14);
    require(file_size==bmp.size(),"BMP file size differs from payload.");
    require(dib==40 || dib==52 || dib==56 || dib==108 || dib==124,"Unsupported BMP DIB header.");
    require(std::uint64_t(14)+dib<=bmp.size() && offset>=14+dib && offset<=bmp.size(),"Invalid BMP pixel offset.");
    const auto width=u32(bmp,18),raw_height=u32(bmp,22);
    const bool top_down=(raw_height&0x80000000u)!=0;
    const std::uint64_t height=top_down ? std::uint64_t(0x100000000ull)-raw_height : raw_height;
    require(width>=1 && width<=4096 && height>=1 && height<=4096 && std::uint64_t(width)*height<=16ull*1024*1024,"BMP dimensions exceed capture limits.");
    const auto bits=u16(bmp,28);const auto compression=u32(bmp,30);
    require(u16(bmp,26)==1 && ((bits==24 && compression==0) || (bits==32 && (compression==0 || compression==3))),"Unsupported BMP planes, depth or compression.");
    if(compression==3){
        require(dib>=52 || offset>=66,"BMP bitfield masks are missing.");
        require(u32(bmp,54)==0x00ff0000 && u32(bmp,58)==0x0000ff00 && u32(bmp,62)==0x000000ff,"BMP RGB masks are unsupported.");
        if(dib>=56) {
            const auto alpha=u32(bmp,66);require(alpha==0 || alpha==0xff000000,"BMP alpha mask is unsupported.");
        }
    }
    // Extended engine screenshots must explicitly use sRGB, without an ICC
    // profile. Calibrated/profile-based BMP color cannot be reinterpreted safely.
    if(dib>=108)require(u32(bmp,70)==0x73524742,"Unsupported BMP color space.");
    if(dib==124)require(u32(bmp,126)==0 && u32(bmp,130)==0,"BMP color profiles are unsupported.");
    const std::uint64_t stride=(std::uint64_t(width)*bits+31)/32*4;
    const auto payload=stride*height;
    require(payload<=bmp.size()-offset,"BMP pixels are truncated.");
    const auto declared=u32(bmp,34);
    require(declared==0 || declared==payload,"BMP image size differs from row layout.");
    CaptureImage result;result.source_width=width;result.source_height=static_cast<std::uint32_t>(height);
    result.width=width;result.height=result.source_height;
    const auto edge=std::max(result.width,result.height);
    if(edge>max_edge){
        result.width=std::max(1u,static_cast<std::uint32_t>((std::uint64_t(width)*max_edge+edge/2)/edge));
        result.height=std::max(1u,static_cast<std::uint32_t>((height*max_edge+edge/2)/edge));
    }
    auto pixel=[&](std::uint32_t x,std::uint32_t y,unsigned channel){
        const auto row=top_down ? y : result.source_height-1-y;
        const auto index=offset+std::uint64_t(row)*stride+std::uint64_t(x)*(bits/8)+(2-channel);
        return std::to_integer<unsigned char>(bmp[static_cast<std::size_t>(index)]);
    };
    std::vector<unsigned char> rgb(std::size_t(result.width)*result.height*3);
    if(result.width==width && result.height==height){
        for(std::uint32_t y=0;y<result.height;++y)for(std::uint32_t x=0;x<result.width;++x)
            for(unsigned c=0;c<3;++c)rgb[(std::size_t(y)*result.width+x)*3+c]=pixel(x,y,c);
    }else{
        std::array<double,256> linear{};
        for(unsigned i=0;i<256;++i){const double s=i/255.;linear[i]=s<=.04045 ? s/12.92 : std::pow((s+.055)/1.055,2.4);}
        const double sx=double(width)/result.width,sy=double(height)/result.height;
        for(std::uint32_t y=0;y<result.height;++y)for(std::uint32_t x=0;x<result.width;++x){
            const double left=x*sx,right=(x+1)*sx,top=y*sy,bottom=(y+1)*sy;
            std::array<double,3> sum{};double area=0;
            for(auto iy=static_cast<std::uint32_t>(std::floor(top));iy<std::min(result.source_height,static_cast<std::uint32_t>(std::ceil(bottom)));++iy)
                for(auto ix=static_cast<std::uint32_t>(std::floor(left));ix<std::min(width,static_cast<std::uint32_t>(std::ceil(right)));++ix){
                    const double weight=(std::min(right,double(ix+1))-std::max(left,double(ix)))*(std::min(bottom,double(iy+1))-std::max(top,double(iy)));
                    area+=weight;for(unsigned c=0;c<3;++c)sum[c]+=linear[pixel(ix,iy,c)]*weight;
                }
            for(unsigned c=0;c<3;++c){const double value=std::clamp(sum[c]/area,0.,1.);
                const double s=value<=.0031308 ? value*12.92 : 1.055*std::pow(value,1/2.4)-.055;
                rgb[(std::size_t(y)*result.width+x)*3+c]=static_cast<unsigned char>(std::clamp(std::lround(s*255),0l,255l));}
        }
    }
    std::vector<unsigned char> png;
    const auto status=lodepng::encode(png,rgb,result.width,result.height,LCT_RGB,8);
    if(status)throw std::runtime_error("Capture PNG encoding failed: "+std::to_string(status));
    result.png.resize(png.size());std::memcpy(result.png.data(),png.data(),png.size());result.source_sha256=sha256(bmp);
    return result;
}
}
