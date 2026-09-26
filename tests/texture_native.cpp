// SPDX-License-Identifier: Apache-2.0
#include "poima/assets.hpp"
#include <iostream>
#include <stdexcept>
using namespace poima;
void check(bool value) { if(!value)throw std::runtime_error("Texture mip assertion failed."); }
int main() {
    TextureMip base{2,1,{0,0,0,0,255,255,255,255}};
    auto linear=texture_mips(base,false),color=texture_mips(base,true);
    check(linear.size()==2 && linear[1].rgba==std::vector<std::uint8_t>({128,128,128,128}));
    check(color[1].rgba==std::vector<std::uint8_t>({188,188,188,128}));
    TextureMip odd{3,1,{0,0,0,255,0,0,0,255,255,255,255,255}};
    check(texture_mips(odd,false)[1].rgba==std::vector<std::uint8_t>({85,85,85,255}));
    TextureMip tall{1,5,std::vector<std::uint8_t>(20,71)};
    auto mips=texture_mips(tall,true);check(mips.size()==3 && mips[1].height==2 && mips[2].rgba==std::vector<std::uint8_t>(4,71));
    try { decode_texture(std::as_bytes(std::span("broken",6)),true);return 1; }catch(const std::runtime_error&){}
    std::cout<<"Linear/sRGB filtering, linear alpha, NPOT edge coverage and invalid decode passed.\n";
}
