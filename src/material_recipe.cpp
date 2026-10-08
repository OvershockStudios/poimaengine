// SPDX-License-Identifier: Apache-2.0
#include "poima/material_recipe.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace poima::materials {
namespace {
using Json=nlohmann::json;
void check(bool value,const char* text){if(!value)throw Error(-32602,text);}
std::uint32_t integer(const Json& value,std::uint32_t low,std::uint32_t high){
    check(value.is_number_integer() && !value.is_boolean(),"Recipe count must be an integer.");
    if(value.is_number_unsigned()){const auto x=value.get<std::uint64_t>();check(x>=low && x<=high,"Recipe count exceeds bounds.");return static_cast<std::uint32_t>(x);}
    const auto x=value.get<std::int64_t>();check(x>=low && x<=high,"Recipe count exceeds bounds.");return static_cast<std::uint32_t>(x);
}
double number(const Json& v,double low,double high){check(v.is_number() && !v.is_boolean(),"Recipe parameter must be numeric.");const auto x=v.get<double>();check(std::isfinite(x) && x>=low && x<=high,"Recipe parameter exceeds finite bounds.");return x;}
std::array<std::uint8_t,3> color(const Json& v){
    check(v.is_string(),"Recipe color must be lowercase #rrggbb.");const auto& s=v.get_ref<const std::string&>();
    check(s.size()==7 && s[0]=='#' && s.find_first_not_of("0123456789abcdef",1)==std::string::npos,"Recipe color must be lowercase #rrggbb.");
    std::array<std::uint8_t,3> c{};for(unsigned i=0;i<3;++i)c[i]=static_cast<std::uint8_t>(std::stoul(s.substr(1+i*2,2),nullptr,16));return c;
}
std::string color_text(const std::array<std::uint8_t,3>& c){const char* h="0123456789abcdef";std::string s="#";for(auto x:c){s+=h[x>>4];s+=h[x&15];}return s;}
double linear(double x){return x<=.04045 ? x/12.92 : std::pow((x+.055)/1.055,2.4);}
double encoded(double x){return x<=.0031308 ? x*12.92 : 1.055*std::pow(x,1/2.4)-.055;}
std::uint32_t hash(std::uint32_t seed,std::uint32_t x,std::uint32_t y){auto n=seed^(x*0x9e3779b9u)^(y*0x85ebca6bu);n^=n>>16;n*=0x7feb352du;n^=n>>15;n*=0x846ca68bu;n^=n>>16;return n;}
double random(std::uint32_t seed,std::uint32_t x,std::uint32_t y){return double(hash(seed,x,y)&0xffffffu)/16777215.0*2-1;}
double smooth(double x){x=std::clamp(x,0.0,1.0);return x*x*(3-2*x);}
double wrap(double x){return x-std::floor(x);}
double noise(std::uint32_t seed,double u,double v){
    constexpr std::uint32_t grid=32;const double x=wrap(u)*grid,y=wrap(v)*grid;const auto ix=static_cast<std::uint32_t>(x),iy=static_cast<std::uint32_t>(y);
    const double sx=smooth(x-ix),sy=smooth(y-iy);
    return std::lerp(std::lerp(random(seed,ix,iy),random(seed,(ix+1)%grid,iy),sx),std::lerp(random(seed,ix,(iy+1)%grid),random(seed,(ix+1)%grid,(iy+1)%grid),sx),sy);
}
void cancelled(const std::atomic_bool* flag){if(flag && flag->load(std::memory_order_relaxed))throw Cancelled();}
std::uint8_t byte(double value){return static_cast<std::uint8_t>(std::clamp(std::lround(value*255),0l,255l));}
}
Recipe parse_recipe(const Json& value){
    check(value.is_object(),"Recipe must be an object.");
    const std::set<std::string> allowed{"format","kind","seed","width","height","tile_width_m","tile_height_m","rows","columns","joint_width_m","joint_depth_m","bevel_m","color","joint_color","roughness","roughness_variation","color_variation","grain_m"};
    for(const auto& [key,unused]:value.items()){(void)unused;check(allowed.contains(key),"Unknown material recipe field.");}
    check(value.contains("format") && value.at("format")=="poima.material.recipe.v1","Unsupported material recipe format.");
    check(value.contains("kind") && (value.at("kind")=="brick" || value.at("kind")=="plaster"),"Recipe kind must be brick or plaster.");
    Recipe r;r.brick=value.at("kind")=="brick";
    if(!r.brick){r.color={184,176,158};r.roughness=.9;r.grain=.0001;for(const auto* k:{"rows","columns","joint_width_m","joint_depth_m","bevel_m","joint_color"})check(!value.contains(k),"Brick-only field in plaster recipe.");}
    if(value.contains("seed"))r.seed=integer(value.at("seed"),0,std::numeric_limits<std::uint32_t>::max());
    if(value.contains("width"))r.width=integer(value.at("width"),64,512);
    if(value.contains("height"))r.height=integer(value.at("height"),64,512);
    if(value.contains("rows"))r.rows=integer(value.at("rows"),2,64);
    if(value.contains("columns"))r.columns=integer(value.at("columns"),1,64);
    check(!r.brick || r.rows%2==0,"Staggered brick rows must be even for vertical periodicity.");
    auto set=[&](const char* key,double& target,double low,double high){if(value.contains(key))target=number(value.at(key),low,high);};
    set("tile_width_m",r.tile_width,.1,100);set("tile_height_m",r.tile_height,.1,100);
    set("joint_width_m",r.joint_width,0,.2);set("joint_depth_m",r.joint_depth,0,.05);set("bevel_m",r.bevel,.000001,.2);
    set("roughness",r.roughness,.045,1);set("roughness_variation",r.roughness_variation,0,.5);set("color_variation",r.color_variation,0,.5);set("grain_m",r.grain,0,.01);
    if(value.contains("color"))r.color=color(value.at("color"));
    if(value.contains("joint_color"))r.joint_color=color(value.at("joint_color"));
    if(r.brick){const auto cell=std::min(r.tile_width/r.columns,r.tile_height/r.rows);check(r.joint_width+2*r.bevel<cell,"Mortar and bevel must fit each brick cell.");}
    return r;
}
Json canonical_recipe(const Recipe& r){
    Json out={{"format","poima.material.recipe.v1"},{"kind",r.brick?"brick":"plaster"},{"seed",r.seed},{"width",r.width},{"height",r.height},
        {"tile_width_m",r.tile_width},{"tile_height_m",r.tile_height},{"color",color_text(r.color)},{"roughness",r.roughness},{"roughness_variation",r.roughness_variation},{"color_variation",r.color_variation},{"grain_m",r.grain}};
    if(r.brick)out.update({{"rows",r.rows},{"columns",r.columns},{"joint_width_m",r.joint_width},{"joint_depth_m",r.joint_depth},{"bevel_m",r.bevel},{"joint_color",color_text(r.joint_color)}});
    return out;
}
std::string recipe_id(const Recipe& r){const auto text=Json{{"evaluator",evaluator},{"recipe",canonical_recipe(r)}}.dump();return sha256(std::as_bytes(std::span(text.data(),text.size())));}
Surface sample_surface(const Recipe& r,double u,double v){
    check(std::isfinite(u) && std::isfinite(v) && std::abs(u)<=1e6 && std::abs(v)<=1e6,"Surface coordinates exceed bounds.");
    const double n=noise(r.seed^0x3c6ef372u,u,v);double coverage=1,cell=0;
    if(r.brick){const double y=wrap(v)*r.rows;const auto row=static_cast<std::uint32_t>(y);const double x=wrap(u)*r.columns+double(row%2)*.5;
        const auto column=static_cast<std::uint32_t>(std::floor(x))%r.columns;
        const auto dx=std::min(wrap(x),1-wrap(x))*r.tile_width/r.columns,dy=std::min(wrap(y),1-wrap(y))*r.tile_height/r.rows;
        coverage=smooth((std::min(dx,dy)-r.joint_width*.5)/r.bevel);cell=random(r.seed,column,row);
    }
    const double variation=.65*cell+.35*n;
    Surface out;for(unsigned c=0;c<3;++c){const auto base=linear(r.color[c]/255.0)*std::max(0.0,1+r.color_variation*variation);
        out.color[c]=std::clamp(std::lerp(linear(r.joint_color[c]/255.0),base,coverage),0.0,1.0);}
    out.roughness=std::clamp(r.roughness+r.roughness_variation*(.65*cell*coverage+.35*n),.045,1.0);
    out.height=(r.brick?coverage*r.joint_depth:0)+n*r.grain*(.2+.8*coverage);return out;
}
std::array<double,3> height_normal(double u,double v){check(std::isfinite(u) && std::isfinite(v) && std::abs(u)<=1e6 && std::abs(v)<=1e6,"Height slope exceeds finite bounds.");const auto length=std::sqrt(u*u+v*v+1);return {-u/length,-v/length,1/length};}
Baked bake(const Recipe& input,const std::atomic_bool* cancellation){
    // Canonicalize through the admission path before allocating any pixels.
    const auto r=parse_recipe(canonical_recipe(input));cancelled(cancellation);
    Baked result;result.recipe=r;const std::size_t count=std::size_t(r.width)*r.height;
    std::vector<double> heights(count);for(auto& image:result.images){TextureMip base;base.width=r.width;base.height=r.height;base.rgba.resize(count*4);image.mips.push_back(std::move(base));}
    result.images[0].srgb=true;
    for(std::uint32_t y=0;y<r.height;++y){cancelled(cancellation);for(std::uint32_t x=0;x<r.width;++x){const auto i=std::size_t(y)*r.width+x;const auto surface=sample_surface(r,(x+.5)/r.width,(y+.5)/r.height);heights[i]=surface.height;
        auto& color_pixels=result.images[0].mips[0].rgba;for(unsigned c=0;c<3;++c)color_pixels[i*4+c]=byte(encoded(surface.color[c]));color_pixels[i*4+3]=255;
        auto& packed=result.images[2].mips[0].rgba;packed[i*4]=0;packed[i*4+1]=byte(surface.roughness);packed[i*4+2]=0;packed[i*4+3]=255;
    }}
    for(std::uint32_t y=0;y<r.height;++y){cancelled(cancellation);for(std::uint32_t x=0;x<r.width;++x){const auto height=[&](std::uint32_t px,std::uint32_t py){return heights[std::size_t(py)*r.width+px];};
        const auto u=(height((x+1)%r.width,y)-height((x+r.width-1)%r.width,y))*r.width/(2*r.tile_width);
        const auto v=(height(x,(y+1)%r.height)-height(x,(y+r.height-1)%r.height))*r.height/(2*r.tile_height);
        const auto normal=height_normal(u,v);auto& pixels=result.images[1].mips[0].rgba;const auto i=(std::size_t(y)*r.width+x)*4;
        for(unsigned c=0;c<3;++c)pixels[i+c]=byte(normal[c]*.5+.5);
        pixels[i+3]=255;
    }}
    for(auto& image:result.images){cancelled(cancellation);image.mips=texture_mips(std::move(image.mips[0]),image.srgb);}
    cancelled(cancellation);return result;
}
}
