// SPDX-License-Identifier: Apache-2.0
#include "poima/audio.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace poima {
namespace {
void check(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
std::uint32_t u32(std::span<const std::byte> b,std::size_t p) {
    check(p<=b.size() && b.size()-p>=4,"Truncated audio integer.");std::uint32_t n=0;
    for(unsigned k=0;k<4;++k)n|=std::uint32_t(std::to_integer<unsigned char>(b[p+k]))<<(8*k);
    return n;
}
std::uint16_t u16(std::span<const std::byte> b,std::size_t p) {
    check(p<=b.size() && b.size()-p>=2,"Truncated audio integer.");
    return static_cast<std::uint16_t>(std::to_integer<unsigned char>(b[p])|(std::to_integer<unsigned char>(b[p+1])<<8));
}
void put(std::string& b,std::uint32_t n,unsigned count=4) { for(unsigned k=0;k<count;++k)b.push_back(static_cast<char>((n>>(8*k))&255)); }
void valid(const AudioClip& clip) {
    check(!clip.samples.empty() && clip.samples.size()<=max_audio_clip_frames,"Audio clip needs 1..2880000 mono frames (60 seconds at 48 kHz).");
    for(float v:clip.samples)check(std::isfinite(v) && std::abs(v)<=1,"Audio clip sample must be finite and within [-1,1].");
}
}
std::shared_ptr<const AudioClip> decode_wave(std::span<const std::byte> b) {
    check(b.size()>=12 && b.size()<=32*1024*1024,"WAV source must contain 12 bytes..32 MiB.");
    check(std::memcmp(b.data(),"RIFF",4)==0 && std::memcmp(b.data()+8,"WAVE",4)==0 && u32(b,4)==b.size()-8,"Expected a complete little-endian RIFF/WAVE file.");
    std::span<const std::byte> format,data;bool has_format=false,has_data=false;
    for(std::size_t p=12;p<b.size();) {
        check(b.size()-p>=8,"Truncated WAV chunk.");const std::size_t length=u32(b,p+4);check(length<=b.size()-p-8,"Truncated WAV chunk payload.");
        auto body=b.subspan(p+8,length);
        if(std::memcmp(b.data()+p,"fmt ",4)==0) { check(!has_format,"Duplicate WAV format chunk.");format=body;has_format=true; }
        if(std::memcmp(b.data()+p,"data",4)==0) { check(!has_data,"Duplicate WAV data chunk.");data=body;has_data=true; }
        const auto advance=8+length+(length&1);check(advance<=b.size()-p,"Missing WAV chunk padding.");p+=advance;
    }
    check(has_format && has_data && format.size()>=16,"WAV needs format and data chunks.");
    const auto type=u16(format,0),channels=u16(format,2),bits=u16(format,14);
    check(channels==1 && u32(format,4)==audio_rate,"Spatial WAV import currently requires mono 48000 Hz.");
    check((type==1 && bits==16) || (type==3 && bits==32),"WAV import supports PCM16 or IEEE float32 only.");
    check(format.size()==16 || (format.size()>=18 && u16(format,16)==format.size()-18),"Invalid WAV format extension.");
    const unsigned stride=bits/8;check(u16(format,12)==stride && u32(format,8)==audio_rate*stride,"Invalid WAV block alignment/byte rate.");
    check(!data.empty() && data.size()%stride==0 && data.size()/stride<=max_audio_clip_frames,"WAV sample count exceeds clip bounds.");
    auto clip=std::make_shared<AudioClip>();clip->samples.resize(data.size()/stride);
    for(std::size_t i=0;i<clip->samples.size();++i)clip->samples[i]=type==1 ? float(std::bit_cast<std::int16_t>(u16(data,i*2)))/32768.0f : std::bit_cast<float>(u32(data,i*4));
    valid(*clip);return clip;
}
std::string encode_audio(const AudioClip& clip) {
    valid(clip);std::string result("PAUDIO1\0",8);put(result,static_cast<std::uint32_t>(clip.samples.size()));
    for(float v:clip.samples)put(result,std::bit_cast<std::uint32_t>(v==0 ? 0.0f : v));
    return result;
}
std::shared_ptr<const AudioClip> decode_audio(const std::string& encoded) {
    const auto b=std::as_bytes(std::span(encoded.data(),encoded.size()));check(b.size()>=12 && std::memcmp(b.data(),"PAUDIO1\0",8)==0,"Unsupported audio package.");
    const auto count=u32(b,8);check(count>=1 && count<=max_audio_clip_frames && b.size()==12+std::size_t(count)*4,"Audio package size mismatch.");
    auto clip=std::make_shared<AudioClip>();clip->samples.resize(count);for(std::size_t i=0;i<count;++i)clip->samples[i]=std::bit_cast<float>(u32(b,12+4*i));valid(*clip);return clip;
}
std::string audio_wave(std::span<const float> samples,std::uint16_t channels) {
    check((channels==1 || channels==2) && samples.size()%channels==0 && samples.size()<=max_audio_clip_frames*2,"Invalid audio output size/channels.");
    const auto bytes=static_cast<std::uint32_t>(samples.size()*4);
    std::string result="RIFF";put(result,50+bytes);result+="WAVEfmt ";put(result,18);put(result,3,2);put(result,channels,2);
    put(result,audio_rate);put(result,audio_rate*channels*4);put(result,channels*4,2);put(result,32,2);put(result,0,2);
    result+="fact";put(result,4);put(result,static_cast<std::uint32_t>(samples.size()/channels));result+="data";put(result,bytes);
    for(float v:samples) { check(std::isfinite(v),"Nonfinite audio output.");put(result,std::bit_cast<std::uint32_t>(v)); }return result;
}
void validate_acoustic_material(const AcousticMaterial& m) {
    for(float v:m.absorption)check(std::isfinite(v) && v>=0 && v<=1,"Acoustic absorption must be in [0,1].");
    for(float v:m.transmission)check(std::isfinite(v) && v>=0 && v<=1,"Acoustic transmission must be in [0,1].");
    check(std::isfinite(m.scattering) && m.scattering>=0 && m.scattering<=1,"Acoustic scattering must be in [0,1].");
}
}
