// SPDX-License-Identifier: Apache-2.0
#include "poima/ui.hpp"
#include <cmath>
#include <stdexcept>
#include <utility>

namespace poima {
namespace {
void check(bool value,const char* message) { if(!value)throw std::invalid_argument(message); }
bool finite(float value) {return std::isfinite(value) && std::abs(value)<=1e6f;}
void color(const std::array<std::uint8_t,4>& c) {
    check(c[0]<=c[3] && c[1]<=c[3] && c[2]<=c[3],"UI colors must be premultiplied sRGB.");
}
}
std::array<float,4> ui_linear_color(const std::array<std::uint8_t,4>& c) {
    color(c);const float a=c[3]/255.0f;std::array<float,4> result{0,0,0,a};
    if(c[3])for(unsigned i=0;i<3;++i) {
        const float s=static_cast<float>(c[i])/c[3];
        result[i]=(s<=.04045f ? s/12.92f : std::pow((s+.055f)/1.055f,2.4f))*a;
    }
    return result;
}
void validate_ui_frame(const UiFrame& frame) {
    check(frame.width>=1 && frame.width<=8192 && frame.height>=1 && frame.height<=8192,"UI extent must be 1..8192 pixels.");
    check(frame.vertices.size()<=max_ui_vertices && frame.indices.size()<=max_ui_indices && frame.draws.size()<=max_ui_draws && frame.textures.size()<=max_ui_textures,"UI packet count budget exceeded.");
    for(const auto& vertex:frame.vertices) {
        check(finite(vertex.x) && finite(vertex.y) && finite(vertex.u) && finite(vertex.v),"UI vertex is nonfinite or outside the coordinate bound.");color(vertex.color);
    }
    for(auto index:frame.indices)check(index<frame.vertices.size(),"UI index is outside the vertex array.");
    std::size_t bytes=0,drawn=0;
    for(const auto& texture:frame.textures) {
        check(texture.width>=1 && texture.width<=4096 && texture.height>=1 && texture.height<=4096,"UI texture extent must be 1..4096 pixels.");
        const auto count=std::size_t(texture.width)*texture.height*4;
        check(texture.rgba.size()==count && count<=max_ui_texture_bytes-bytes,"UI texture payload length or budget is invalid.");bytes+=count;
        for(std::size_t i=0;i<count;i+=4)color({texture.rgba[i],texture.rgba[i+1],texture.rgba[i+2],texture.rgba[i+3]});
    }
    for(const auto& draw:frame.draws) {
        check(draw.first_index<=frame.indices.size() && draw.index_count<=frame.indices.size()-draw.first_index && draw.index_count%3==0,"UI draw index range is invalid.");
        check(draw.index_count<=max_ui_indices-drawn,"UI total drawn index budget exceeded.");drawn+=draw.index_count;
        check(draw.texture==ui_white_texture || draw.texture<frame.textures.size(),"UI draw texture is absent.");
        for(float value:draw.transform)check(finite(value),"UI transform is nonfinite or outside the bound.");
        for(float value:draw.translation)check(finite(value),"UI translation is nonfinite or outside the bound.");
        const auto& clip=draw.scissor;
        check(clip[0]>=0 && clip[1]>=0 && clip[2]>=clip[0] && clip[3]>=clip[1] && std::uint32_t(clip[2])<=frame.width && std::uint32_t(clip[3])<=frame.height,"UI scissor is outside the frame.");
        for(std::size_t i=draw.first_index;i<std::size_t(draw.first_index)+draw.index_count;++i) {
            const auto& v=frame.vertices[frame.indices[i]];const auto& m=draw.transform;
            const double x=double(v.x)+draw.translation[0],y=double(v.y)+draw.translation[1];
            const double w=m[3]*x+m[7]*y+m[15];
            check(std::isfinite(w) && w>=1e-6,"UI transform crosses or approaches the perspective plane.");
            for(unsigned row=0;row<3;++row) {
                const double value=(m[row]*x+m[4+row]*y+m[12+row])/w;
                check(std::isfinite(value) && std::abs(value)<=1e6,"UI transformed coordinate exceeds its bound.");
            }
        }
    }
}
std::shared_ptr<const UiFrame> freeze_ui_frame(UiFrame frame) {
    validate_ui_frame(frame);return std::make_shared<const UiFrame>(std::move(frame));
}
}
