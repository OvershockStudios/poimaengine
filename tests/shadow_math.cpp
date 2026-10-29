// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using V=std::array<double,3>;
void check(bool yes,const char* why) { if(!yes)throw std::runtime_error(why); }
V project(const poima::Matrix4& m,V p) {
    const double w=m[3]*p[0]+m[7]*p[1]+m[11]*p[2]+m[15];check(w>0,"Point behind projection.");
    V r{};for(std::size_t i=0;i<3;++i)r[i]=(m[i]*p[0]+m[4+i]*p[1]+m[8+i]*p[2]+m[12+i])/w;return r;
}
void close(double a,double b) { check(std::isfinite(a) && std::abs(a-b)<1e-6,"Projection value mismatch."); }
void same_views(const std::vector<poima::ShadowView>& a,const std::vector<poima::ShadowView>& b) {
    check(a.size()==b.size(),"Shadow view count changed.");
    for(std::size_t i=0;i<a.size();++i)
        check(a[i].view_projection==b[i].view_projection && a[i].light_index==b[i].light_index &&
              a[i].split_near==b[i].split_near && a[i].split_far==b[i].split_far &&
              a[i].depth_span==b[i].depth_span && a[i].world_units_per_texel==b[i].world_units_per_texel,
              "Equivalent filter selection changed exact shadow views.");
}
void contact_shadow_views() {
    using poima::ShadowFilter;
    poima::SceneSnapshot scene;scene.camera_world=poima::identity_matrix();scene.near_plane=.1;scene.far_plane=100;
    scene.lighting.preview=false;poima::SceneLight source;source.light.kind=poima::LightKind::directional;
    source.light.shadow.enabled=true;source.light.shadow.distance=80;source.direction={0,-1,0};scene.lighting.lights.push_back(source);
    const auto legacy=poima::shadow_views(scene,1.5);
    // Frozen pre-PCSS axis-aligned reference, independent of the planner under test.
    // Rounded radii and snapped translations make these exact across the old modes.
    const poima::Matrix4 expected[]={
        {-0x1.a7e567109f95ap-4,0,0,0,0,0,-0x1.49e7be04d9ff0p-7,0,0,0x1.a7e567109f95ap-4,0,0,0,0x1.c000000000000p-2,0x1.ce30d6c3083f5p-1,1},
        {-0x1.a946e12586411p-5,0,0,0,0,0,-0x1.1476b77efbcc8p-7,0,0,0x1.a946e12586411p-5,0,0,0,0x1.4f00000000000p-1,0x1.acca32af5d5fdp-1,1},
        {-0x1.cd57bede87b06p-6,0,0,0,0,0,-0x1.b1ef3218095c0p-8,0,0,0x1.cd57bede87b06p-6,0,0,0,0x1.6700000000000p-1,0x1.879abfa782eccp-1,1},
        {-0x1.7840ec14a8352p-7,0,0,0,0,0,-0x1.01d5277f156c4p-8,0,0,0x1.7840ec14a8352p-7,0,0,0,0x1.4800000000001p-1,0x1.50929c57b6b1dp-1,1},
    };
    check(legacy.size()==4,"Legacy directional cascade count changed.");
    for(std::size_t i=0;i<4;++i)check(legacy[i].view_projection==expected[i],"Frozen legacy matrix changed.");
    same_views(legacy,poima::shadow_views(scene,1.5,ShadowFilter::pcf3_point));
    same_views(legacy,poima::shadow_views(scene,1.5,ShadowFilter::pcf3_fractional));
    scene.lighting.environment.shadow_filter=ShadowFilter::pcf3_fractional;
    same_views(legacy,poima::shadow_views(scene,1.5));
    const auto contact=poima::shadow_views(scene,1.5,ShadowFilter::pcss_directional);
    check(scene.lighting.environment.shadow_filter==ShadowFilter::pcf3_fractional,"Effective override mutated authored environment.");
    scene.lighting.environment.shadow_filter=ShadowFilter::pcss_directional;
    same_views(contact,poima::shadow_views(scene,1.5));
    same_views(legacy,poima::shadow_views(scene,1.5,ShadowFilter::pcf3_point));
    same_views(legacy,poima::shadow_views(scene,1.5,ShadowFilter::pcf3_fractional));
    auto& settings=scene.lighting.lights[0].light.shadow;
    settings.angular_size_degrees=0;
    same_views(legacy,poima::shadow_views(scene,1.5));
    for(float angle:{std::numeric_limits<float>::min(),.53f,20.f}) {
        settings.angular_size_degrees=angle;same_views(contact,poima::shadow_views(scene,1.5));
        same_views(legacy,poima::shadow_views(scene,1.5,ShadowFilter::pcf3_fractional));
    }
    settings.angular_size_degrees.reset();
    for(auto resolution:{256u,512u,1024u,2048u}) {
        scene.lighting.environment.shadow_resolution=resolution;
        const auto hard=poima::shadow_views(scene,1.5,ShadowFilter::pcf3_fractional);
        const auto soft=poima::shadow_views(scene,1.5);
        for(std::size_t i=0;i<4;++i) {
            const auto& v=soft[i];const auto& m=v.view_projection;
            // Recover distances from the actual transform instead of repeating its fit.
            const double half_width=1/std::hypot(m[0],m[4],m[8]);
            const double depth_span=1/std::hypot(m[2],m[6],m[10]);
            close(v.world_units_per_texel,2*half_width/resolution);close(v.depth_span,depth_span);
            close(v.depth_span,2*half_width+settings.distance);
            check(v.depth_span>v.split_far-v.split_near,"Camera splits substituted for orthographic depth span.");
            close(soft[i].world_units_per_texel/hard[i].world_units_per_texel,double(resolution-4)/(resolution-36));
            check(v.split_near==hard[i].split_near && v.split_far==hard[i].split_far,"Filter changed camera cascade splits.");
            const auto a=project(m,{0,0,0}),b=project(m,{0,-depth_span,0});close(b[2]-a[2],1);
            // An unexpanded bounding sphere must retain 18 texels minus at most
            // half a texel of snapping, including the bilinear footprint at 17.5.
            const double radius=half_width*(resolution-36)/resolution;
            const double previous_width=i ? hard[i-1].split_far-hard[i-1].split_near : 0;
            const double fit_near=std::max(scene.near_plane,v.split_near-.1*previous_width);
            const double center_z=-(fit_near+v.split_far)*.5;
            for(V edge:{V{radius,0,center_z},V{-radius,0,center_z},V{0,0,center_z+radius},V{0,0,center_z-radius}}) {
                const auto p=project(m,edge);
                check((1-std::abs(p[0]))*resolution*.5>=17.5-1e-9 &&
                      (1-std::abs(p[1]))*resolution*.5>=17.5-1e-9,"Contact filter guard clips sphere support.");
            }
            const double tangent=std::tan(scene.vertical_fov*3.141592653589793/360);
            for(double d:{fit_near,v.split_far})for(double x:{-1.,1.})for(double y:{-1.,1.}) {
                const auto p=project(m,{x*d*tangent*1.5,y*d*tangent,-d});
                check(std::abs(p[0])<1 && std::abs(p[1])<1 && p[2]>=0 && p[2]<=1,"Contact cascade clips a receiver corner.");
            }
        }
        scene.camera_world[12]=soft[0].world_units_per_texel*.01;
        const auto shifted=poima::shadow_views(scene,1.5);
        for(std::size_t i=0;i<4;++i)for(std::size_t k:{0u,1u,4u,5u,8u,9u,12u,13u})
            check(shifted[i].view_projection[k]==soft[i].view_projection[k],"Contact cascade lost subtexel snapping.");
        scene.camera_world=poima::identity_matrix();
    }
    scene.near_plane=2;scene.far_plane=50;settings.distance=120;
    const auto bounded=poima::shadow_views(scene,1);
    close(bounded.front().split_near,2);close(bounded.back().split_far,50);
    for(const auto& v:bounded)close(v.depth_span,v.world_units_per_texel*scene.lighting.environment.shadow_resolution+120);
    settings.near_plane=10;same_views(bounded,poima::shadow_views(scene,1));
    scene.near_plane=130;scene.far_plane=150;check(poima::shadow_views(scene,1).empty(),"Out-of-coverage directional camera allocated cascades.");
    scene.near_plane=.1;scene.far_plane=100;settings.near_plane=.05f;settings.distance=80;
    scene.camera_world=poima::local_matrix({3,2,4},{0,std::sin(.2),0,std::cos(.2)},{1,1,1});
    auto& direction=scene.lighting.lights[0].direction;const double length=std::hypot(.4,.8,.6);direction={.4/length,-.8/length,-.6/length};
    for(const auto& v:poima::shadow_views(scene,1.5)) {
        const auto& m=v.view_projection;
        close(v.depth_span,1/std::hypot(m[2],m[6],m[10]));
        close(v.world_units_per_texel,2/(scene.lighting.environment.shadow_resolution*std::hypot(m[0],m[4],m[8])));
        const auto a=project(m,{0,0,0}),b=project(m,{direction[0]*v.depth_span,direction[1]*v.depth_span,direction[2]*v.depth_span});
        close(b[2]-a[2],1);
    }
    for(auto kind:{poima::LightKind::point,poima::LightKind::spot}) {
        scene.lighting.lights[0].light.kind=kind;
        const auto local=poima::shadow_views(scene,1,ShadowFilter::pcf3_fractional);
        same_views(local,poima::shadow_views(scene,1));
        for(const auto& v:local)check(v.depth_span==0 && v.world_units_per_texel==0,"Local light published directional metrics.");
    }
    bool invalid_override=false;try { poima::shadow_views(scene,1,static_cast<ShadowFilter>(3)); }catch(const std::runtime_error&) { invalid_override=true; }
    check(invalid_override,"Unknown effective filter override accepted.");
    for(auto resolution:{128u,4096u}) {
        scene.lighting.environment.shadow_resolution=resolution;bool rejected=false;
        try { poima::shadow_views(scene,1); }catch(const std::runtime_error&) { rejected=true; }
        check(rejected,"Unsupported contact shadow resolution accepted.");
    }
}
int main() {
    try {
        poima::LightingEnvironment environment;
        check(environment.shadow_filter==poima::ShadowFilter::pcf3_point,"Legacy environment filter changed.");
        poima::validate_environment(environment);
        environment.shadow_filter=poima::ShadowFilter::pcf3_fractional;poima::validate_environment(environment);
        environment.shadow_filter=poima::ShadowFilter::pcss_directional;poima::validate_environment(environment);
        environment.shadow_filter=static_cast<poima::ShadowFilter>(3);
        bool invalid_filter=false;try { poima::validate_environment(environment); }catch(const std::runtime_error&) { invalid_filter=true; }
        check(invalid_filter,"Unknown native environment filter accepted.");
        poima::RenderOptions options;check(!options.shadow_filter.has_value(),"Default rendering must inherit the environment.");
        options.shadow_filter=poima::ShadowFilter::pcf3_point;
        check(options.shadow_filter.has_value(),"Explicit point override lost its presence.");
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
        contact_shadow_views();
        scene.lighting.lights[0].light.enabled=false;check(poima::shadow_views(scene,1).empty(),"Disabled light allocated shadows.");
        poima::validate_shadow_budget(16,1024);poima::validate_shadow_budget(8,2048);
        for(const auto counts:{17u,9u}) { bool rejected=false;try { poima::validate_shadow_budget(counts,counts==17 ? 1024 : 2048); }catch(const std::runtime_error&) { rejected=true; }check(rejected,"Shadow budget overflow accepted."); }
        std::cout<<"Point/spot projections, exact legacy cascades, contact depth/texel/guard/overrides, stable snapping and budgets passed.\n";
    }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
