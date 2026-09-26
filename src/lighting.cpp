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
}
void validate_environment(const LightingEnvironment& e) {
    for(auto x:e.ambient)require(std::isfinite(x) && x>=0 && x<=1e6f,"Ambient fill must be in [0,1e6].");
    require(std::isfinite(e.exposure) && e.exposure>=0 && e.exposure<=1e6f,"Exposure multiplier must be in [0,1e6].");
}
void append_light(SceneLighting& state,const std::string& id,const Light& light,const Matrix4& world) {
    validate_light(light);state.preview=false;if(!light.enabled)return;
    require(state.lights.size()<max_scene_lights,"A scene supports at most 64 enabled lights in the initial forward path.");
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
}
}
