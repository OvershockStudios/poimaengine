// SPDX-License-Identifier: Apache-2.0
#include "poima/scene.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using V=std::array<double,3>;
void check(bool yes,const char* why) { if(!yes)throw std::runtime_error(why); }
V project(const poima::Matrix4& m,V p) {
    const double w=m[3]*p[0]+m[7]*p[1]+m[11]*p[2]+m[15];check(w>0,"Point behind projection.");
    V r{};for(std::size_t i=0;i<3;++i)r[i]=(m[i]*p[0]+m[4+i]*p[1]+m[8+i]*p[2]+m[12+i])/w;return r;
}
void close(double a,double b) { check(std::isfinite(a) && std::abs(a-b)<1e-6,"Projection value mismatch."); }
int main() {
    try {
        poima::SceneSnapshot scene;scene.camera_world=poima::identity_matrix();scene.near_plane=.1;scene.far_plane=100;
        scene.lighting.preview=false;poima::SceneLight source;source.light.shadow.enabled=true;source.light.shadow.distance=80;source.light.kind=poima::LightKind::point;scene.lighting.lights.push_back(source);
        auto views=poima::shadow_views(scene,1.5);check(views.size()==6,"Point needs six faces.");
        const V axes[]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        for(std::size_t i=0;i<6;++i) {
            const auto p=project(views[i].view_projection,{axes[i][0]*4,axes[i][1]*4,axes[i][2]*4});close(p[0],0);close(p[1],0);check(p[2]>0 && p[2]<1,"Point face depth range.");
            const auto edge=project(views[i].view_projection,{axes[i][0]*80,axes[i][1]*80,axes[i][2]*80});close(edge[2],1);
        }
        scene.lighting.lights[0].light.kind=poima::LightKind::spot;scene.lighting.lights[0].direction={-1,0,0};
        views=poima::shadow_views(scene,1);check(views.size()==1,"Spot needs one view.");auto p=project(views[0].view_projection,{-4,0,0});close(p[0],0);close(p[1],0);
        scene.lighting.lights[0].light.kind=poima::LightKind::directional;scene.lighting.lights[0].direction={0,-1,0};
        views=poima::shadow_views(scene,1.5);check(views.size()==4,"Directional needs four cascades.");double previous=.1;
        const double tangent=std::tan(scene.vertical_fov*3.141592653589793/360);
        for(const auto& view:views) {
            close(view.split_near,previous);check(view.split_far>previous,"Cascade splits must increase.");
            for(double depth:{view.split_near,view.split_far})for(double x:{-1.,1.})for(double y:{-1.,1.}) {
                const auto v=project(view.view_projection,{x*depth*tangent*1.5,y*depth*tangent,-depth});
                check(std::abs(v[0])<=1 && std::abs(v[1])<=1 && v[2]>=0 && v[2]<=1,"Cascade clips a receiver frustum corner.");
            }
            previous=view.split_far;
        }
        close(previous,80);
        scene.camera_world[12]=.00001;const auto shifted=poima::shadow_views(scene,1.5);
        for(std::size_t i=0;i<4;++i)for(std::size_t k:{0u,1u,4u,5u,8u,9u,12u,13u})close(shifted[i].view_projection[k],views[i].view_projection[k]);
        scene.lighting.lights[0].light.enabled=false;check(poima::shadow_views(scene,1).empty(),"Disabled light allocated shadows.");
        poima::validate_shadow_budget(16,1024);poima::validate_shadow_budget(8,2048);
        for(const auto counts:{17u,9u}) { bool rejected=false;try { poima::validate_shadow_budget(counts,counts==17 ? 1024 : 2048); }catch(const std::runtime_error&) { rejected=true; }check(rejected,"Shadow budget overflow accepted."); }
        std::cout<<"Six point faces, rotated spot, directional frustum coverage, stable snapping and budgets passed.\n";
    }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
