// SPDX-License-Identifier: Apache-2.0
#include "poima/scene.hpp"
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace poima {
namespace {
void require(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
}
void validate_light(const Light& l) {
    require(l.kind==LightKind::directional || l.kind==LightKind::point || l.kind==LightKind::spot,"Invalid light kind.");
    for(auto x:l.color)require(std::isfinite(x) && x>=0 && x<=1,"Light color must be linear RGB in [0,1].");
    require(std::isfinite(l.intensity) && l.intensity>=0 && l.intensity<=1e9f,"Light intensity must be in [0,1e9].");
    require(std::isfinite(l.range) && l.range>=0 && l.range<=1e9f,"Light range must be in [0,1e9]; zero means unbounded.");
    require(l.kind!=LightKind::directional || l.range==0,"Directional lights cannot have a range.");
    require(std::isfinite(l.inner_angle) && std::isfinite(l.outer_angle) && l.inner_angle>=0 && l.inner_angle<l.outer_angle && l.outer_angle<=90,"Spot half-angles need 0 <= inner < outer <= 90 degrees.");
    const auto& s=l.shadow;
    if(s.angular_size_degrees) {
        require(l.kind==LightKind::directional,"Shadow angular_size_degrees applies only to directional lights.");
        require(std::isfinite(*s.angular_size_degrees) && *s.angular_size_degrees>=0 && *s.angular_size_degrees<=20,"Shadow angular_size_degrees must be in [0,20] degrees.");
    }
    require(std::isfinite(s.near_plane) && s.near_plane>=.001f && s.near_plane<=100,"Shadow near must be in [0.001,100] meters.");
    require(std::isfinite(s.distance) && s.distance>s.near_plane && s.distance<=100000,"Shadow distance must exceed near and be at most 100000 meters.");
    require(std::isfinite(s.bias) && s.bias>=0 && s.bias<=.1f && std::isfinite(s.normal_bias) && s.normal_bias>=0 && s.normal_bias<=1,"Shadow bias must be in [0,0.1]; normal_bias in [0,1] meters.");
    if(s.enabled) {
        require(l.kind!=LightKind::spot || l.outer_angle<=89.5f,"Shadowed spot outer_angle must be at most 89.5 degrees.");
        require(l.kind==LightKind::directional || l.range==0 || l.range>s.near_plane,"Shadowed local light range must exceed shadow near.");
    }
}
void validate_environment(const LightingEnvironment& e) {
    for(auto x:e.ambient)require(std::isfinite(x) && x>=0 && x<=1e6f,"Ambient fill must be in [0,1e6].");
    require(std::isfinite(e.exposure) && e.exposure>=0 && e.exposure<=1e6f,"Exposure multiplier must be in [0,1e6].");
    const auto& bloom=e.bloom;
    require(std::isfinite(bloom.intensity) && bloom.intensity>=0 && bloom.intensity<=1,
            "Bloom intensity must be finite and in [0,1].");
    require(std::isfinite(bloom.threshold) && bloom.threshold>=0 && bloom.threshold<=65504,
            "Bloom threshold must be finite and in [0,65504].");
    require(std::isfinite(bloom.radius) && bloom.radius>=2 && bloom.radius<=32,
            "Bloom radius must be finite and in [2,32] source pixels.");
    if(e.output_grade) {
        const auto& grade=*e.output_grade;
        require(std::isfinite(grade.input_max) && grade.input_max>=.125f && grade.input_max<=64,
                "Output grade input_max must be finite and in [0.125,64].");
        require(std::isfinite(grade.strength) && grade.strength>=0 && grade.strength<=1,
                "Output grade strength must be finite and in [0,1].");
        require(grade.knots.front()==std::array<float,3>{},"Output grade first knot must be [0,0,0].");
        for(const auto& knot:grade.knots)for(float channel:knot)
            require(std::isfinite(channel) && channel>=0 && channel<=16,
                    "Output grade knot channels must be finite and in [0,16].");
    }
    if(e.screen_reflections) {
        const auto& reflections=*e.screen_reflections;
        require(std::isfinite(reflections.strength) && reflections.strength>=0 && reflections.strength<=1,
                "Screen reflections strength must be finite and in [0,1].");
        require(std::isfinite(reflections.max_distance_m) && reflections.max_distance_m>=.25f && reflections.max_distance_m<=32,
                "Screen reflections max_distance_m must be finite and in [0.25,32].");
        require(std::isfinite(reflections.thickness_m) && reflections.thickness_m>=.01f && reflections.thickness_m<=.5f,
                "Screen reflections thickness_m must be finite and in [0.01,0.5].");
    }
    if(e.height_fog) {
        const auto& fog=*e.height_fog;
        const auto finite_between=[](float value,float minimum,float maximum) {
            return std::isfinite(value) && value>=minimum && value<=maximum;
        };
        require(finite_between(fog.density_per_m,0,.75f),"Height fog density_per_m must be finite and in [0,0.75].");
        require(finite_between(fog.base_height_m,-8192,8192),"Height fog base_height_m must be finite and in [-8192,8192].");
        require(finite_between(fog.height_falloff_per_m,0,4),"Height fog height_falloff_per_m must be finite and in [0,4].");
        require(finite_between(fog.start_distance_m,0,300),"Height fog start_distance_m must be finite and in [0,300].");
        require(finite_between(fog.max_distance_m,.1f,512) && fog.max_distance_m>fog.start_distance_m,
            "Height fog max_distance_m must be finite, in [0.1,512], and greater than start_distance_m.");
        for(float channel:fog.ambient_radiance)
            require(finite_between(channel,0,32),"Height fog ambient_radiance must be finite linear RGB in [0,32].");
        require(finite_between(fog.anisotropy,-.8f,.8f),"Height fog anisotropy must be finite and in [-0.8,0.8].");
        require(fog.spot_lights.size()<=2,"Height fog accepts at most two spot lights.");
        for(const auto& spot:fog.spot_lights) {
            require(!spot.entity.empty(),"Height fog spot light needs an entity ID.");
            require(finite_between(spot.scattering,0,1),"Height fog spot light scattering must be finite and in [0,1].");
        }
        require(fog.spot_lights.size()<2 || fog.spot_lights.front().entity!=fog.spot_lights.back().entity,
            "Height fog spot light entities must be distinct.");
    }
    require(e.shadow_filter==ShadowFilter::pcf3_point || e.shadow_filter==ShadowFilter::pcf3_fractional || e.shadow_filter==ShadowFilter::pcss_directional,"Invalid environment shadow filter.");
    require(e.shadow_resolution==256 || e.shadow_resolution==512 || e.shadow_resolution==1024 || e.shadow_resolution==2048,"Shadow resolution must be 256, 512, 1024 or 2048.");
    for(const auto& color:{e.sky.zenith,e.sky.horizon,e.sky.ground})
        for(auto x:color)require(std::isfinite(x) && x>=0 && x<=1,"Sky colors must be finite linear RGB in [0,1].");
    require(std::isfinite(e.sky.horizon_falloff) && e.sky.horizon_falloff>=.1f && e.sky.horizon_falloff<=16,"Sky horizon falloff must be in [0.1,16].");
    require(std::isfinite(e.sky.sun_size_degrees) && e.sky.sun_size_degrees>=.1f && e.sky.sun_size_degrees<=20,"Sky sun angular diameter must be in [0.1,20] degrees.");
    require(std::isfinite(e.sky.sun_intensity) && e.sky.sun_intensity>=0 && e.sky.sun_intensity<=1e6f,"Sky sun intensity must be in [0,1e6].");
    if(e.ibl) {
        const auto& ibl=*e.ibl;
        require(std::isfinite(ibl.diffuse_intensity) && ibl.diffuse_intensity>=0 && ibl.diffuse_intensity<=64 &&
                std::isfinite(ibl.specular_intensity) && ibl.specular_intensity>=0 && ibl.specular_intensity<=64,
                "IBL diffuse_intensity and specular_intensity must be in [0,64].");
        require(!ibl.enabled || e.sky.enabled,"Enabled IBL requires an enabled procedural sky.");
    }
}
std::size_t shadow_view_count(const Light& l) { return l.enabled && l.shadow.enabled ? (l.kind==LightKind::directional ? 4 : l.kind==LightKind::point ? 6 : 1) : 0; }
void validate_shadow_budget(std::size_t views,std::uint32_t resolution) {
    LightingEnvironment e;e.shadow_resolution=resolution;validate_environment(e);
    require(views<=max_shadow_views,"Scene exceeds the initial 16 shadow-view limit (directional=4, point=6, spot=1).");
    require(views*std::size_t(resolution)*resolution*4<=max_shadow_bytes,"Shadow depth storage exceeds the initial 128 MiB limit.");
}
void append_light(SceneLighting& state,const std::string& id,const Light& light,const Matrix4& world) {
    validate_light(light);state.preview=false;if(!light.enabled)return;
    require(state.lights.size()<max_scene_lights,"A scene exceeds the enabled-light limit.");
    SceneLight result;result.entity_id=id;result.light=light;result.position={world[12],world[13],world[14]};
    for(auto x:result.position)require(std::isfinite(x),"Invalid light world position.");
    if(light.kind!=LightKind::point) {
        const double length=std::hypot(world[8],world[9],world[10]);require(std::isfinite(length) && length>0,"Light world direction is degenerate.");
        result.direction={-world[8]/length,-world[9]/length,-world[10]/length};
    }
    state.lights.push_back(std::move(result));
}
void finalize_lighting(SceneLighting& state) {
    validate_environment(state.environment);
    if(state.preview) {
        state.lights.clear();SceneLight light;light.entity_id="preview";light.light.kind=LightKind::directional;light.light.intensity=std::numbers::pi_v<float>;
        const auto length=std::hypot(.4,.8,.6);light.direction={.4/length,-.8/length,-.6/length};state.lights.push_back(light);state.environment.ambient={.035f,.035f,.035f};
    }
    std::size_t views=0;for(const auto& light:state.lights)views+=shadow_view_count(light.light);
    validate_shadow_budget(views,state.environment.shadow_resolution);
}
}
