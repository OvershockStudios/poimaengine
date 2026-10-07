// SPDX-License-Identifier: Apache-2.0
#include "poima/capture_image.hpp"
#include "poima/assets.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <fstream>
#include <iterator>
using namespace poima;
namespace {
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void put(std::vector<std::byte>& b,std::size_t at,std::uint32_t value,unsigned count=4){for(unsigned i=0;i<count;++i)b[at+i]=std::byte((value>>(i*8))&255);}
std::vector<std::byte> bmp(unsigned w,unsigned h,unsigned bits,bool top,bool fields=false,unsigned dib=40){
    const unsigned offset=14+dib+(fields&&dib==40 ? 16:0),stride=(w*(bits/8)+3)&~3u;
    std::vector<std::byte> b(offset+stride*h);put(b,0,0x4d42,2);put(b,2,static_cast<unsigned>(b.size()));put(b,10,offset);put(b,14,dib);
    put(b,18,w);put(b,22,top ? 0u-h:h);put(b,26,1,2);put(b,28,bits,2);put(b,30,fields?3:0);put(b,34,stride*h);
    if(fields){put(b,54,0xff0000);put(b,58,0xff00);put(b,62,0xff);if(dib!=52)put(b,66,0xff000000);}
    if(dib>=108)put(b,70,0x73524742);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        const auto at=offset+(top?y:h-1-y)*stride+x*(bits/8);
        b[at]=std::byte((x*13+y*7)%256);b[at+1]=std::byte((x*3+y*23)%256);b[at+2]=std::byte((x*41+y*5)%256);
        if(bits==32)b[at+3]=std::byte((x+y)%256); // Deliberately not opaque: alpha must be ignored.
    }
    return b;
}
void exact(unsigned bits,bool top,bool fields=false,unsigned dib=40){
    auto bytes=bmp(3,2,bits,top,fields,dib);auto image=capture_image(bytes);
    check(image.source_width==3&&image.source_height==2&&image.width==3&&image.height==2,"Wrong source/output extent");
    check(image.source_sha256==sha256(bytes),"Source digest differs");
    auto decoded=decode_texture(image.png,true);check(!decoded->mips.empty(),"PNG decoder returned no pixels");const auto& mip=decoded->mips[0];
    check(mip.width==3&&mip.height==2,"PNG extent differs");
    for(unsigned y=0;y<2;++y)for(unsigned x=0;x<3;++x){auto at=(y*3+x)*4;
        check(mip.rgba[at]==(x*41+y*5)%256&&mip.rgba[at+1]==(x*3+y*23)%256&&mip.rgba[at+2]==(x*13+y*7)%256&&mip.rgba[at+3]==255,"Lossless PNG orientation/color/alpha differs");}
}
void invalid(){
    auto good=bmp(3,2,24,false);
    auto rejects=[&](const std::function<void(std::vector<std::byte>&)>& mutate){auto b=good;mutate(b);bool failed=false;try{(void)capture_image(b);}catch(const std::exception&){failed=true;}check(failed,"Malformed BMP accepted");};
    rejects([](auto& b){b.resize(20);});rejects([](auto& b){put(b,0,0,2);});rejects([](auto& b){put(b,6,1);});
    rejects([](auto& b){put(b,2,0);});rejects([](auto& b){put(b,10,53);});rejects([](auto& b){put(b,10,0xffffffff);});
    rejects([](auto& b){put(b,14,12);});rejects([](auto& b){put(b,14,124);});
    for(auto width:{0u,4097u,0xffffffffu})rejects([&](auto& b){put(b,18,width);});
    for(auto height:{0u,4097u,0x80000000u})rejects([&](auto& b){put(b,22,height);});
    rejects([](auto& b){put(b,26,2,2);});rejects([](auto& b){put(b,28,16,2);});rejects([](auto& b){put(b,30,1);});
    rejects([](auto& b){put(b,30,3);});rejects([](auto& b){put(b,34,1);});
    rejects([](auto& b){b.pop_back();put(b,2,static_cast<unsigned>(b.size()));});
    for(auto mask:{0u,0x000000ffu}){auto b=bmp(3,2,32,false,true);put(b,54,mask);bool failed=false;try{(void)capture_image(b);}catch(const std::exception&){failed=true;}check(failed,"Invalid RGB mask accepted");}
    {auto b=bmp(3,2,32,false,true,56);put(b,66,0xff);bool failed=false;try{(void)capture_image(b);}catch(const std::exception&){failed=true;}check(failed,"Overlapping alpha mask accepted");}
    for(auto dib:{108u,124u})for(auto space:{0u,0x4c494e4bu,0x4d424544u}){
        auto b=bmp(3,2,32,false,true,dib);put(b,70,space);bool failed=false;
        try{(void)capture_image(b);}catch(const std::exception&){failed=true;}check(failed,"Unsupported BMP color space accepted");
    }
    for(auto field:{126u,130u}){
        auto b=bmp(3,2,32,false,true,124);put(b,field,1);bool failed=false;
        try{(void)capture_image(b);}catch(const std::exception&){failed=true;}check(failed,"BMP profile declaration accepted");
    }
    // BITMAPINFOHEADER has three external RGB masks. Additional bytes before
    // pixel data are a gap, not a declared alpha mask.
    {auto b=bmp(3,2,32,false,true);put(b,66,0x12345678);const auto image=capture_image(b);check(image.width==3&&image.height==2,"BMP mask gap treated as alpha");}
    for(auto limit:{0u,127u,2049u}){bool failed=false;try{(void)capture_image(good,limit);}catch(const std::exception&){failed=true;}check(failed,"Invalid max_edge accepted");}
}
void reduction(){
    auto bytes=bmp(256,128,24,true);
    const unsigned stride=256*3;
    // Each output pixel covers equally weighted white/black source pixels.
    for(unsigned y=0;y<128;++y)for(unsigned x=0;x<256;++x)for(unsigned c=0;c<3;++c)bytes[54+y*stride+x*3+c]=std::byte(x%2 ? 255:0);
    const auto image=capture_image(bytes,128);check(image.width==128&&image.height==64&&image.source_width==256&&image.source_height==128,"Reduction aspect differs");
    const auto decoded=decode_texture(image.png,true);const auto& pixels=decoded->mips[0].rgba;
    for(std::size_t i=0;i<pixels.size();i+=4)check(pixels[i]==188&&pixels[i+1]==188&&pixels[i+2]==188&&pixels[i+3]==255,"Reduction is not linear-light area average");
    auto narrow=capture_image(bmp(1,257,24,false),128);check(narrow.width==1&&narrow.height==128,"Thin-image reduction failed");
    auto odd=capture_image(bmp(257,129,32,true),128);check(odd.width==128&&odd.height==64,"Odd-size aspect rounding failed");
}
}
int main(int argc,char** argv){try{
    check(argc<=2,"Usage: capture-image-test [ARCHIVED_BMP]");
    if(argc==2){
        std::ifstream file(argv[1],std::ios::binary);check(bool(file),"Cannot open archived BMP");
        const std::string bytes((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
        const auto input=std::as_bytes(std::span(bytes.data(),bytes.size()));
        const auto image=capture_image(input,2048);const auto decoded=decode_texture(image.png,true);
        check(decoded->mips[0].width==image.width&&decoded->mips[0].height==image.height,"Archived PNG extent differs");
        if(image.width==image.source_width&&image.height==image.source_height){
            const auto read32=[&](std::size_t at){std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t(static_cast<unsigned char>(bytes[at+i]))<<(8*i);return value;};
            const auto offset=read32(10),raw_height=read32(22);const auto bits=static_cast<unsigned char>(bytes[28]);
            const auto stride=(image.width*(bits/8)+3)&~3u;const bool top=(raw_height&0x80000000u)!=0;
            const auto& output=decoded->mips[0].rgba;
            for(unsigned y=0;y<image.height;++y)for(unsigned x=0;x<image.width;++x){
                const auto source=offset+(top?y:image.height-1-y)*stride+x*(bits/8),target=(y*image.width+x)*4;
                check(static_cast<unsigned char>(bytes[source+2])==output[target]&&static_cast<unsigned char>(bytes[source+1])==output[target+1]&&static_cast<unsigned char>(bytes[source])==output[target+2]&&output[target+3]==255,"Archived RGB changed during conversion");
            }
        }
        std::cout<<"Archived BMP: source "<<image.source_width<<'x'<<image.source_height<<", output "<<image.width<<'x'<<image.height<<", sha256 "<<image.source_sha256<<" (no new GPU execution).\n";
    }
for(bool top:{false,true}){exact(24,top);exact(32,top);for(auto dib:{40u,52u,56u,108u,124u})exact(32,top,true,dib);}invalid();reduction();std::cout<<"Capture BMP/PNG codec: "<<checks<<" checks passed (synthetic CPU fixtures; no GPU qualification).\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
