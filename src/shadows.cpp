// SPDX-License-Identifier: Apache-2.0
#include "poima/scene.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace poima {
namespace {
using V=std::array<double,3>;
V add(V a,V b) { return {a[0]+b[0],a[1]+b[1],a[2]+b[2]}; }
V scale(V a,double s) { return {a[0]*s,a[1]*s,a[2]*s}; }
double dot(V a,V b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
V cross(V a,V b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
V unit(V a) { const auto length=std::hypot(a[0],a[1],a[2]);if(!(length>0) || !std::isfinite(length))throw std::runtime_error("Degenerate shadow direction.");return scale(a,1/length); }
Matrix4 view(V position,V direction) {
    const auto z=scale(unit(direction),-1);const V up=std::abs(z[1])>.99 ? V{0,0,1} : V{0,1,0};
    const auto x=unit(cross(up,z)),y=cross(z,x);
    return {x[0],y[0],z[0],0,x[1],y[1],z[1],0,x[2],y[2],z[2],0,-dot(x,position),-dot(y,position),-dot(z,position),1};
}
V point(const Matrix4& m,V p) { V r{};for(std::size_t i=0;i<3;++i)r[i]=m[i]*p[0]+m[4+i]*p[1]+m[8+i]*p[2]+m[12+i];return r; }
}
std::vector<ShadowView> shadow_views(const SceneSnapshot& scene,double aspect) {
    if(!(aspect>0) || !std::isfinite(aspect) || !rigid_transform(scene.camera_world))throw std::runtime_error("Shadow views require a rigid camera and finite positive aspect.");
    auto lighting=scene.lighting;finalize_lighting(lighting);std::vector<ShadowView> result;
    const V axes[]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for(std::size_t i=0;i<lighting.lights.size();++i) {
        const auto& source=lighting.lights[i];const auto& l=source.light;validate_light(l);if(!shadow_view_count(l))continue;
        const auto& s=l.shadow;
        if(l.kind!=LightKind::directional) {
            const double far=l.range>0 ? std::min(s.distance,l.range) : s.distance;
            const auto projection=perspective(l.kind==LightKind::point ? 90 : double(l.outer_angle)*2,1,s.near_plane,far);
            for(std::size_t face=0;face<(l.kind==LightKind::point ? 6u : 1u);++face)
                result.push_back({multiply(projection,view(source.position,l.kind==LightKind::point ? axes[face] : source.direction)),i,s.near_plane,far});
        } else {
            const double near=scene.near_plane,far=std::min(scene.far_plane,double(s.distance));if(far<=near)continue;
            const double tangent=std::tan(scene.vertical_fov*std::numbers::pi/360);
            double previous=near,previous_width=0;
            for(int cascade=0;cascade<4;++cascade) {
                const double fraction=double(cascade+1)/4;
                const double split=cascade==3 ? far : .6*near*std::pow(far/near,fraction)+.4*(near+(far-near)*fraction);
                // Overlap the preceding cascade's last tenth for cross-fading.
                const double fit_near=std::max(near,previous-.1*previous_width);
                const V center=point(scene.camera_world,{0,0,-(fit_near+split)*.5});
                double radius=0;
                for(double d:{fit_near,split})for(double x:{-1.,1.})for(double y:{-1.,1.}) {
                    const auto corner=point(scene.camera_world,{x*d*tangent*aspect,y*d*tangent,-d});
                    const auto delta=add(corner,scale(center,-1));radius=std::max(radius,std::hypot(delta[0],delta[1],delta[2]));
                }
                // A rotation-invariant bounding sphere, rounded extent and texel snapping
                // keep camera rotation/translation from continuously rescaling the map.
                radius=std::ceil(radius*16)/16;radius*=double(lighting.environment.shadow_resolution)/(lighting.environment.shadow_resolution-4);
                auto light_view=view(add(center,scale(source.direction,-(radius+s.distance))),source.direction);
                const double texel=2*radius/lighting.environment.shadow_resolution;
                light_view[12]=std::round(light_view[12]/texel)*texel;light_view[13]=std::round(light_view[13]/texel)*texel;
                const double depth=2*radius+s.distance;
                Matrix4 projection={1/radius,0,0,0,0,1/radius,0,0,0,0,-1/depth,0,0,0,0,1};
                result.push_back({multiply(projection,light_view),i,previous,split});previous_width=split-previous;previous=split;
            }
        }
    }
    validate_shadow_budget(result.size(),lighting.environment.shadow_resolution);return result;
}
}
