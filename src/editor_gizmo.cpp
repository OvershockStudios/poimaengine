// SPDX-License-Identifier: Apache-2.0
#include "poima/editor_gizmo.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace poima {
namespace {
using V3=std::array<double,3>;
using V2=std::array<double,2>;
constexpr double pi=std::numbers::pi_v<double>;
constexpr double handle_pixels=80;
constexpr std::size_t ring_segments=96;
V3 add(V3 a,V3 b) { for(std::size_t i=0;i<3;++i)a[i]+=b[i];return a; }
V3 sub(V3 a,V3 b) { for(std::size_t i=0;i<3;++i)a[i]-=b[i];return a; }
V3 mul(V3 a,double s) { for(auto& v:a)v*=s;return a; }
double dot(V3 a,V3 b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
V3 cross(V3 a,V3 b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
bool finite(V3 a) { return std::all_of(a.begin(),a.end(),[](double v){return std::isfinite(v)&&std::abs(v)<=1e12;}); }
std::optional<V3> normalized(V3 a) {
    if(!finite(a))return {};
    const double n=std::hypot(a[0],a[1],a[2]);
    if(n<1e-12||!std::isfinite(n))return {};
    return mul(a,1/n);
}
double distance(V2 a,V2 b) { return std::hypot(a[0]-b[0],a[1]-b[1]); }
double segment_distance(V2 p,V2 a,V2 b) {
    const V2 v{b[0]-a[0],b[1]-a[1]};
    const double n=v[0]*v[0]+v[1]*v[1];
    const double t=n>1e-12?std::clamp(((p[0]-a[0])*v[0]+(p[1]-a[1])*v[1])/n,0.0,1.0):0;
    return distance(p,{a[0]+t*v[0],a[1]+t*v[1]});
}
bool valid_axis(EditorGizmoAxis axis) { const auto i=static_cast<unsigned>(axis);return i<3; }
}

struct EditorGizmoData {
    V3 origin{},eye{},right{},up{},back{};
    std::array<V3,3> axes{},ring_u{},ring_v{},ring_normal{};
    std::array<bool,3> visible{};
    double width=0,height=0,focal=0,near_plane=0,radius=0;
    EditorGizmoMode mode=EditorGizmoMode::move;
    std::optional<EditorGizmoAxis> selected;
};

namespace {
std::optional<V2> project(const EditorGizmoData& d,V3 world) {
    const auto p=sub(world,d.eye);
    const double depth=-dot(p,d.back);
    if(!std::isfinite(depth)||depth<=d.near_plane)return {};
    V2 result{d.width*.5+d.focal*dot(p,d.right)/depth,
              d.height*.5-d.focal*dot(p,d.up)/depth};
    // Permit modest offscreen handles, but never publish unbounded coordinates.
    if(!std::isfinite(result[0])||!std::isfinite(result[1])||
       std::abs(result[0])>d.width*16||std::abs(result[1])>d.height*16)return {};
    return result;
}
std::optional<V3> ray(const EditorGizmoData& d,double x,double y) {
    if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>1e8||std::abs(y)>1e8)return {};
    return normalized(add(add(mul(d.right,(x-d.width*.5)/d.focal),
                              mul(d.up,(d.height*.5-y)/d.focal)),mul(d.back,-1)));
}
std::optional<double> axis_parameter(const EditorGizmoData& d,std::size_t axis,double x,double y) {
    const auto r=ray(d,x,y);if(!r)return {};
    const auto w=sub(d.origin,d.eye);
    const double b=dot(d.axes[axis],*r),denom=1-b*b;
    if(denom<1e-5)return {};
    const double parameter=(b*dot(*r,w)-dot(d.axes[axis],w))/denom;
    const double ray_distance=dot(*r,w)+b*parameter;
    if(!std::isfinite(parameter)||std::abs(parameter)>1e12||ray_distance<=d.near_plane)return {};
    return parameter;
}
std::optional<double> ring_angle(const EditorGizmoData& d,std::size_t axis,double x,double y) {
    const auto r=ray(d,x,y);if(!r)return {};
    const double denom=dot(*r,d.ring_normal[axis]);
    if(std::abs(denom)<.01)return {};
    const double t=dot(sub(d.origin,d.eye),d.ring_normal[axis])/denom;
    if(!std::isfinite(t)||t<=d.near_plane)return {};
    const auto p=sub(add(d.eye,mul(*r,t)),d.origin);
    const auto& u=d.ring_u[axis];const auto& v=d.ring_v[axis];
    const double uu=dot(u,u),vv=dot(v,v),uv=dot(u,v),det=uu*vv-uv*uv;
    if(det<1e-8*uu*vv)return {};
    const double pu=dot(p,u),pv=dot(p,v);
    // Solve the plane's Gram system. Dot products alone yield the wrong angle
    // when inherited nonuniform scale or shear makes the ring an affine ellipse.
    const double a=(vv*pu-uv*pv)/det,b=(uu*pv-uv*pu)/det;
    if(!std::isfinite(a)||!std::isfinite(b)||std::hypot(a,b)<d.radius*.02)return {};
    return std::atan2(b,a);
}
void triangle(EditorGizmo& out,const EditorGizmoData& d,V2 a,V2 b,V2 c,const std::array<float,4>& color) {
    // Sutherland-Hodgman viewport clipping. A triangle becomes at most a bounded
    // convex polygon; fan triangulation preserves the overlay triangle contract.
    std::vector<V2> polygon{a,b,c};
    for(unsigned edge=0;edge<4&&!polygon.empty();++edge) {
        const std::size_t component=edge/2;
        const double boundary=(edge%2)==0?0:(component==0?d.width:d.height);
        const auto inside=[&](V2 p){return (edge%2)==0?p[component]>=boundary:p[component]<=boundary;};
        std::vector<V2> clipped;clipped.reserve(8);
        auto previous=polygon.back();bool previous_inside=inside(previous);
        for(const auto current:polygon) {
            const bool current_inside=inside(current);
            if(current_inside!=previous_inside) {
                const double t=(boundary-previous[component])/(current[component]-previous[component]);
                V2 p{previous[0]+t*(current[0]-previous[0]),previous[1]+t*(current[1]-previous[1])};
                p[component]=boundary;clipped.push_back(p);
            }
            if(current_inside)clipped.push_back(current);
            previous=current;previous_inside=current_inside;
        }
        polygon=std::move(clipped);
    }
    for(std::size_t i=2;i<polygon.size();++i)
        for(const auto p:{polygon[0],polygon[i-1],polygon[i]})
            out.triangles.push_back({static_cast<float>(std::clamp(p[0]/d.width,0.0,1.0)),
                static_cast<float>(std::clamp(p[1]/d.height,0.0,1.0)),color});
}
void line(EditorGizmo& out,const EditorGizmoData& d,V2 a,V2 b,double thickness,const std::array<float,4>& color) {
    const double n=distance(a,b);if(n<1e-5)return;
    const V2 offset{-(b[1]-a[1])/n*thickness*.5,(b[0]-a[0])/n*thickness*.5};
    const V2 p{a[0]+offset[0],a[1]+offset[1]},q{a[0]-offset[0],a[1]-offset[1]},
             r{b[0]+offset[0],b[1]+offset[1]},s{b[0]-offset[0],b[1]-offset[1]};
    triangle(out,d,p,q,r,color);triangle(out,d,r,q,s,color);
}
}

EditorGizmo make_editor_gizmo(const SceneSnapshot& camera,std::uint32_t width,std::uint32_t height,
    const V3& origin,const std::array<V3,3>& basis,EditorGizmoMode mode,std::optional<EditorGizmoAxis> selected) {
    EditorGizmo out;
    if(width==0||height==0||width>32768||height>32768||!finite(origin)||
       !std::isfinite(camera.vertical_fov)||camera.vertical_fov<1||camera.vertical_fov>175||
       !std::isfinite(camera.near_plane)||camera.near_plane<=0||!rigid_transform(camera.camera_world)||
       !std::all_of(camera.camera_world.begin(),camera.camera_world.end(),[](double v){return std::isfinite(v)&&std::abs(v)<=1e12;})||
       std::abs(camera.camera_world[3])>1e-9||std::abs(camera.camera_world[7])>1e-9||
       std::abs(camera.camera_world[11])>1e-9||std::abs(camera.camera_world[15]-1)>1e-9||
       (mode!=EditorGizmoMode::move&&mode!=EditorGizmoMode::rotate&&mode!=EditorGizmoMode::scale))return out;
    auto data=std::make_shared<EditorGizmoData>();auto& d=*data;
    d.origin=origin;d.width=width;d.height=height;d.mode=mode;d.near_plane=camera.near_plane;d.selected=selected;
    for(std::size_t i=0;i<3;++i){d.eye[i]=camera.camera_world[12+i];d.right[i]=camera.camera_world[i];d.up[i]=camera.camera_world[4+i];d.back[i]=camera.camera_world[8+i];}
    if(!finite(d.eye)||dot(cross(d.right,d.up),d.back)<.999999)return out;
    d.focal=d.height*.5/std::tan(camera.vertical_fov*pi/360);
    const auto center=project(d,origin);if(!center)return out;
    d.radius=-dot(sub(origin,d.eye),d.back)*handle_pixels/d.focal;
    if(!std::isfinite(d.radius)||d.radius<=1e-12||d.radius>1e12)return out;
    constexpr std::array<std::array<float,4>,3> colors{{{.95f,.22f,.20f,1},{.30f,.85f,.30f,1},{.25f,.50f,1,1}}};
    constexpr std::array<float,4> highlight{1,.84f,.20f,1};
    for(std::size_t i=0;i<3;++i) {
        const auto axis=normalized(basis[i]);if(!axis)continue;d.axes[i]=*axis;
        auto& handle=out.handles[i];
        if(mode==EditorGizmoMode::rotate) {
            auto u=basis[(i+1)%3],v=basis[(i+2)%3];
            if(!finite(u)||!finite(v))continue;
            // A shared normalization preserves ellipse shape, angular meaning
            // and handedness while keeping the longest basis radius nominally
            // 80 pixels. Never normalize these two columns independently.
            const double length=std::max(std::hypot(u[0],u[1],u[2]),std::hypot(v[0],v[1],v[2]));
            if(!std::isfinite(length)||length<1e-12)continue;
            u=mul(u,1/length);v=mul(v,1/length);
            const double uu=dot(u,u),vv=dot(v,v),uv=dot(u,v);
            if(uu<1e-10||vv<1e-10||uu*vv-uv*uv<1e-8*uu*vv)continue;
            const auto normal=normalized(cross(u,v));if(!normal)continue;
            const auto view=normalized(sub(d.eye,origin));
            if(!view||std::abs(dot(*view,*normal))<.08)continue;
            d.ring_u[i]=u;d.ring_v[i]=v;d.ring_normal[i]=*normal;
            for(std::size_t j=0;j<=ring_segments;++j) {
                const double angle=2*pi*static_cast<double>(j)/static_cast<double>(ring_segments);
                const auto point=project(d,add(origin,mul(add(mul(d.ring_u[i],std::cos(angle)),mul(d.ring_v[i],std::sin(angle))),d.radius)));
                if(!point){handle.screen_points.clear();break;}
                handle.screen_points.push_back(*point);
            }
            if(handle.screen_points.empty())continue;
        } else {
            const auto end=project(d,add(origin,mul(*axis,d.radius)));
            if(!end||distance(*center,*end)<12||!axis_parameter(d,i,(*center)[0],(*center)[1]))continue;
            // Exclude the ambiguous center from hit-testing and line rendering.
            const double length=distance(*center,*end);
            V2 start{(*center)[0]+((*end)[0]-(*center)[0])*8/length,(*center)[1]+((*end)[1]-(*center)[1])*8/length};
            handle.screen_points={start,*end};
        }
        handle.visible=true;d.visible[i]=true;
    }
    // Paint selected last, so visual ordering and hit-testing agree at crossings.
    std::array<std::size_t,3> order{0,1,2};
    if(selected&&valid_axis(*selected)) {
        const auto index=static_cast<std::size_t>(*selected);
        std::size_t at=0;for(std::size_t i=0;i<3;++i)if(i!=index)order[at++]=i;order[2]=index;
    }
    for(const auto i:order) {
        const auto& handle=out.handles[i];if(!handle.visible)continue;
        const bool active=selected&&static_cast<std::size_t>(*selected)==i;
        const auto color=active?highlight:colors[i];
        for(std::size_t j=1;j<handle.screen_points.size();++j)line(out,d,handle.screen_points[j-1],handle.screen_points[j],active?4:3,color);
        if(mode==EditorGizmoMode::rotate)continue;
        const auto a=handle.screen_points.front(),b=handle.screen_points.back();
        const double n=distance(a,b);const V2 v{(b[0]-a[0])/n,(b[1]-a[1])/n};
        if(mode==EditorGizmoMode::move) {
            triangle(out,d,b,{b[0]-v[0]*12-v[1]*5,b[1]-v[1]*12+v[0]*5},
                {b[0]-v[0]*12+v[1]*5,b[1]-v[1]*12-v[0]*5},color);
        } else {
            const V2 p{b[0]-5,b[1]-5},q{b[0]+5,b[1]-5},r{b[0]-5,b[1]+5},s{b[0]+5,b[1]+5};
            triangle(out,d,p,q,r,color);triangle(out,d,r,q,s,color);
        }
    }
    out.data=std::move(data);return out;
}

std::optional<EditorGizmoAxis> hit_test_editor_gizmo(const EditorGizmo& gizmo,double x,double y,double tolerance_pixels) {
    if(!gizmo.data||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(tolerance_pixels)||tolerance_pixels<0||tolerance_pixels>64)return {};
    double best=tolerance_pixels;std::optional<EditorGizmoAxis> result;
    for(std::size_t i=0;i<3;++i)if(gizmo.handles[i].visible) {
        const auto& points=gizmo.handles[i].screen_points;
        for(std::size_t j=1;j<points.size();++j) {
            const double dist=segment_distance({x,y},points[j-1],points[j]);
            if(dist<best||(!result&&dist<=best)||
                (result&&std::abs(dist-best)<1e-9&&gizmo.data->selected==static_cast<EditorGizmoAxis>(i)))
            {best=dist;result=static_cast<EditorGizmoAxis>(i);}
        }
    }
    return result;
}

std::optional<EditorGizmoDrag> begin_editor_gizmo_drag(const EditorGizmo& gizmo,EditorGizmoAxis axis,double x,double y) {
    if(!gizmo.data||!valid_axis(axis))return {};
    const auto i=static_cast<std::size_t>(axis);const auto& d=*gizmo.data;
    if(!d.visible[i])return {};
    const auto value=d.mode==EditorGizmoMode::rotate?ring_angle(d,i,x,y):axis_parameter(d,i,x,y);
    if(!value)return {};
    return EditorGizmoDrag{gizmo.data,axis,*value,*value,0};
}

std::optional<double> update_editor_gizmo_drag(EditorGizmoDrag& drag,double x,double y) {
    if(!drag.data||!valid_axis(drag.axis))return {};
    const auto i=static_cast<std::size_t>(drag.axis);const auto& d=*drag.data;
    if(!d.visible[i])return {};
    const auto value=d.mode==EditorGizmoMode::rotate?ring_angle(d,i,x,y):axis_parameter(d,i,x,y);
    if(!value)return {};
    if(d.mode==EditorGizmoMode::rotate) {
        const double angle=drag.angle+std::remainder(*value-drag.last_angle,2*pi);
        if(!std::isfinite(angle)||std::abs(angle)>1e9)return {};
        drag.angle=angle;drag.last_angle=*value;return angle;
    }
    const double delta=*value-drag.anchor;
    if(d.mode==EditorGizmoMode::move)return delta;
    const double ratio=1+delta/d.radius;
    if(!std::isfinite(ratio)||ratio<.001||ratio>1000)return {};
    return ratio;
}
}
