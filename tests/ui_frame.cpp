// SPDX-License-Identifier: Apache-2.0
#include "poima/ui.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
namespace {
void check(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
UiFrame triangle() {
    UiFrame frame;frame.width=640;frame.height=480;frame.revision=9;
    frame.vertices={{10,10,0,0,{128,0,0,128}},{50,10,1,0,{128,0,0,128}},{10,50,0,1,{128,0,0,128}}};
    frame.indices={0,1,2};UiDraw draw;draw.index_count=3;draw.scissor={0,0,640,480};frame.draws.push_back(draw);return frame;
}
template<class F> void rejected(F modify,const char* why) {
    auto frame=triangle();modify(frame);bool failed=false;
    try {validate_ui_frame(frame);}catch(const std::invalid_argument&) {failed=true;}
    check(failed,why);
}
}
int main() {
    try {
        auto source=triangle();auto frozen=freeze_ui_frame(source);source.vertices[0].x=99;
        check(frozen->vertices[0].x==10 && frozen->revision==9,"Frozen frame retained caller storage.");
        check(frozen->draws[0].texture==ui_white_texture && frozen->draws[0].transform[15]==1,"Draw defaults changed.");
        auto valid=triangle();valid.textures.push_back({1,1,{0,64,0,128}});valid.draws[0].texture=0;
        valid.draws[0].translation={20,30};valid.draws[0].transform[0]=2;valid.draws[0].scissor={10,20,10,20};validate_ui_frame(valid);
        valid.width=8192;valid.height=8192;validate_ui_frame(valid);
        rejected([](auto& f){f.width=0;},"Zero extent accepted.");
        rejected([](auto& f){f.height=8193;},"Oversized frame accepted.");
        rejected([](auto& f){f.vertices[0].x=std::numeric_limits<float>::quiet_NaN();},"NaN vertex accepted.");
        rejected([](auto& f){f.vertices[0].u=std::numeric_limits<float>::infinity();},"Infinite UV accepted.");
        rejected([](auto& f){f.vertices[0].color={255,0,0,128};},"Straight alpha vertex accepted.");
        rejected([](auto& f){f.indices[2]=3;},"Out-of-range absolute index accepted.");
        rejected([](auto& f){f.draws[0].first_index=UINT32_MAX;},"Overflowing draw start accepted.");
        rejected([](auto& f){f.draws[0].index_count=2;},"Incomplete triangle accepted.");
        rejected([](auto& f){f.draws[0].texture=0;},"Missing texture accepted.");
        rejected([](auto& f){f.draws[0].scissor[2]=641;},"Outside scissor accepted.");
        rejected([](auto& f){f.draws[0].scissor={10,0,9,50};},"Reversed scissor accepted.");
        rejected([](auto& f){f.draws[0].transform[0]=std::numeric_limits<float>::infinity();},"Infinite matrix accepted.");
        rejected([](auto& f){f.draws[0].translation[0]=std::numeric_limits<float>::quiet_NaN();},"NaN translation accepted.");
        rejected([](auto& f){f.draws[0].transform[15]=0;},"Singular perspective accepted.");
        rejected([](auto& f){f.draws[0].transform[3]=-.05f;},"Perspective crossing accepted.");
        rejected([](auto& f){f.draws[0].transform[0]=1e6f;},"Transformed coordinate overflow accepted.");
        rejected([](auto& f){f.textures.push_back({4097,1,{}});},"Oversized texture accepted.");
        rejected([](auto& f){f.textures.push_back({0,1,{}});},"Zero texture accepted.");
        rejected([](auto& f){f.textures.push_back({1,1,{255,255,255}});},"Truncated texture accepted.");
        rejected([](auto& f){f.textures.push_back({1,1,{1,0,0,0}});},"Transparent RGB texture accepted.");
        rejected([](auto& f){f.vertices.resize(max_ui_vertices+1);},"Vertex budget ignored.");
        rejected([](auto& f){f.indices.resize(max_ui_indices+1);},"Index budget ignored.");
        rejected([](auto& f){f.draws.resize(max_ui_draws+1);},"Draw budget ignored.");
        rejected([](auto& f){f.textures.resize(max_ui_textures+1);},"Texture count budget ignored.");
        rejected([](auto& f){f.textures.push_back({4096,4096,std::vector<std::uint8_t>(max_ui_texture_bytes,0)});f.textures.push_back({1,1,{0,0,0,0}});},"Total texture byte budget ignored.");
        rejected([](auto& f){f.indices.resize(max_ui_indices,0);f.draws[0].index_count=max_ui_indices;f.draws.push_back(f.draws[0]);},"Repeated draw work budget ignored.");
        const auto red=ui_linear_color({128,0,0,128});
        check(std::abs(red[0]-128.0f/255)<1e-6f && red[1]==0,"Premultiplied red was decoded before unpremultiplication.");
        const auto gray=ui_linear_color({64,64,64,128});
        check(std::abs(gray[0]-.21404114f*(128.0f/255))<1e-6f,"sRGB gray conversion differs.");
        check(ui_linear_color({0,0,0,0})==std::array<float,4>{},"Zero alpha conversion differs.");
        auto empty=UiFrame{};empty.width=1;empty.height=1;validate_ui_frame(empty);
        std::cout<<"UI packets: ownership, geometry/texture budgets, transforms, scissor and linear premultiplied color passed.\n";return 0;
    }catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
