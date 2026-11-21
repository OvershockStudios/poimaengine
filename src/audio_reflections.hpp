// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/audio.hpp"
#include <phonon.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <tuple>

namespace poima::audio_detail {
// Bound the reference solver separately from Steam Audio's direct simulation.
// All enabled acoustic geometry still participates in occlusion within this budget.
inline constexpr std::size_t max_reflection_triangles=16384, max_reflection_candidates=8;
inline constexpr double max_reflection_distance=100, reflection_surface_epsilon=.002;
using ReflectionPoint=std::array<double,3>;

inline ReflectionPoint point(const IPLVector3& v) { return {v.x,v.y,v.z}; }
inline ReflectionPoint subtract(const ReflectionPoint& a,const ReflectionPoint& b) {
    return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};
}
inline ReflectionPoint offset(const ReflectionPoint& p,double scale,const ReflectionPoint& v) {
    return {p[0]+scale*v[0],p[1]+scale*v[1],p[2]+scale*v[2]};
}
inline double dot(const ReflectionPoint& a,const ReflectionPoint& b) {
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
}
inline ReflectionPoint cross(const ReflectionPoint& a,const ReflectionPoint& b) {
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}

struct ReflectionShape {
    const AcousticGeometry& geometry;
    std::size_t begin,end;
    ReflectionPoint low,high;
};

inline ReflectionShape reflection_shape(const AcousticGeometry& geometry,std::size_t begin,std::size_t end,
                                         std::span<const IPLVector3> vertices) {
    const auto infinity=std::numeric_limits<double>::infinity();
    ReflectionShape shape{geometry,begin,end,{infinity,infinity,infinity},{-infinity,-infinity,-infinity}};
    for(const auto& vertex:vertices) {
        const auto p=point(vertex);
        for(std::size_t axis=0;axis<3;++axis) {
            shape.low[axis]=std::min(shape.low[axis],p[axis]);
            shape.high[axis]=std::max(shape.high[axis],p[axis]);
        }
    }
    return shape;
}

using ReflectionTriangle=std::array<ReflectionPoint,3>;
inline ReflectionTriangle reflection_triangle(std::span<const IPLVector3> vertices,const IPLTriangle& triangle) {
    return {point(vertices[triangle.indices[0]]),point(vertices[triangle.indices[1]]),point(vertices[triangle.indices[2]])};
}

inline bool contains_point(const ReflectionTriangle& triangle,const ReflectionPoint& p) {
    const auto ab=subtract(triangle[1],triangle[0]), ac=subtract(triangle[2],triangle[0]), ap=subtract(p,triangle[0]);
    const double aa=dot(ab,ab), cc=dot(ac,ac), bc=dot(ab,ac), determinant=aa*cc-bc*bc;
    if(determinant<=1e-20)return false;
    const double x=(dot(ap,ab)*cc-dot(ap,ac)*bc)/determinant;
    const double y=(dot(ap,ac)*aa-dot(ap,ab)*bc)/determinant;
    constexpr double edge=1e-9;
    return x>=-edge && y>=-edge && x+y<=1+edge;
}

inline bool intersects_bounds(const ReflectionPoint& from,const ReflectionPoint& delta,const ReflectionShape& shape) {
    double begin=0,end=1;
    for(std::size_t axis=0;axis<3;++axis) {
        if(std::abs(delta[axis])<1e-12) {
            if(from[axis]<shape.low[axis] || from[axis]>shape.high[axis])return false;
            continue;
        }
        const double a=(shape.low[axis]-from[axis])/delta[axis], b=(shape.high[axis]-from[axis])/delta[axis];
        begin=std::max(begin,std::min(a,b));end=std::min(end,std::max(a,b));
        if(begin>end)return false;
    }
    return true;
}

inline bool blocks_segment(const ReflectionTriangle& triangle,const ReflectionPoint& from,
                           const ReflectionPoint& delta,double end_margin) {
    const auto ab=subtract(triangle[1],triangle[0]), ac=subtract(triangle[2],triangle[0]);
    const auto p=cross(delta,ac);
    const double determinant=dot(ab,p);
    if(std::abs(determinant)<=1e-12)return false;
    const auto offset_from_triangle=subtract(from,triangle[0]);
    const double u=dot(offset_from_triangle,p)/determinant;
    constexpr double edge=1e-9;
    if(u<-edge || u>1+edge)return false;
    const auto q=cross(offset_from_triangle,ab);
    const double v=dot(delta,q)/determinant;
    if(v<-edge || u+v>1+edge)return false;
    const double fraction=dot(ac,q)/determinant;
    return fraction>end_margin && fraction<1-end_margin;
}

inline bool reflection_leg_blocked(const ReflectionPoint& from,const ReflectionPoint& to,
                                   std::span<const ReflectionShape> shapes,std::span<const IPLVector3> vertices,
                                   std::span<const IPLTriangle> triangles) {
    const auto delta=subtract(to,from);
    const auto margin=reflection_surface_epsilon/std::sqrt(dot(delta,delta));
    for(const auto& shape:shapes) {
        if(!intersects_bounds(from,delta,shape))continue;
        for(std::size_t i=shape.begin;i<shape.end;++i)
            if(blocks_segment(reflection_triangle(vertices,triangles[i]),from,delta,margin))return true;
    }
    return false;
}

inline std::optional<AudioReflection> mirror_path(const AudioPath& direct,const Matrix4& listener,
                                                  const ReflectionShape& shape,std::uint32_t face,
                                                  std::span<const IPLVector3> vertices,std::span<const IPLTriangle> triangles) {
    const auto first=shape.begin+face*2;
    const auto triangle=reflection_triangle(vertices,triangles[first]);
    auto normal=cross(subtract(triangle[1],triangle[0]),subtract(triangle[2],triangle[0]));
    const auto magnitude=std::sqrt(dot(normal,normal));
    if(magnitude<=1e-12)return {};
    const auto& m=shape.geometry.world;
    const ReflectionPoint center{m[12],m[13],m[14]};
    const auto outward=dot(normal,subtract(triangle[0],center));
    if(outward==0)return {};
    normal=offset({},outward<0 ? -1/magnitude : 1/magnitude,normal);
    const auto source_side=dot(normal,subtract(direct.source,triangle[0]));
    const auto listener_side=dot(normal,subtract(direct.listener,triangle[0]));
    if(source_side<=reflection_surface_epsilon || listener_side<=reflection_surface_epsilon)return {};
    const auto image=offset(direct.source,-2*source_side,normal);
    const auto line=subtract(image,direct.listener);
    const auto distance=std::sqrt(dot(line,line));
    if(distance>max_reflection_distance)return {};
    const auto bounce=offset(direct.listener,listener_side/(listener_side+source_side),line);
    if(!contains_point(triangle,bounce) &&
       !contains_point(reflection_triangle(vertices,triangles[first+1]),bounce))return {};

    AudioReflection result;
    result.entity=shape.geometry.entity;result.face=face;result.point=bounce;result.distance=distance;
    result.propagation_delay_samples=static_cast<std::uint32_t>(std::llround(distance/343*audio_rate));
    result.distance_gain=static_cast<float>(1/std::max(1.,distance));
    const auto toward=subtract(bounce,direct.listener);
    const auto length=std::sqrt(dot(toward,toward));
    for(std::size_t axis=0;axis<3;++axis) {
        const ReflectionPoint basis{listener[axis*4],listener[axis*4+1],listener[axis*4+2]};
        result.direction[axis]=dot(basis,toward)/length;
        result.bands[axis]=std::sqrt((1-shape.geometry.material.absorption[axis])*(1-shape.geometry.material.scattering));
    }
    return result;
}

inline std::optional<AudioReflection> first_reflection(const AudioPath& direct,const Matrix4& listener,
                                                       std::span<const ReflectionShape> shapes,
                                                       std::span<const IPLVector3> vertices,
                                                       std::span<const IPLTriangle> triangles) {
    // Only finite faces of acoustic proxy boxes create reflections. Baked mesh
    // triangles still block either leg of a candidate.
    std::array<AudioReflection,max_reflection_candidates> candidates;
    std::size_t count=0;
    const auto score=[](const AudioReflection& value) {
        return value.distance_gain*(value.bands[0]+value.bands[1]+value.bands[2]);
    };
    const auto stronger=[&](const AudioReflection& left,const AudioReflection& right) {
        return std::tuple(-score(left),left.distance,std::string_view(left.entity),left.face)<
               std::tuple(-score(right),right.distance,std::string_view(right.entity),right.face);
    };
    for(const auto& shape:shapes) {
        if(shape.geometry.mesh || shape.geometry.material.scattering==1)continue;
        for(std::uint32_t face=0;face<6;++face) {
            auto candidate=mirror_path(direct,listener,shape,face,vertices,triangles);
            if(!candidate || score(*candidate)==0)continue;
            auto begin=candidates.begin(),end=begin+static_cast<std::ptrdiff_t>(count);
            const auto slot=std::lower_bound(begin,end,*candidate,stronger);
            if(slot==candidates.end())continue;
            if(count==candidates.size())--end;
            else ++count;
            std::move_backward(slot,end,end+1);
            *slot=std::move(*candidate);
        }
    }
    for(std::size_t i=0;i<count;++i) {
        auto& candidate=candidates[i];
        if(!reflection_leg_blocked(direct.source,candidate.point,shapes,vertices,triangles) &&
           !reflection_leg_blocked(direct.listener,candidate.point,shapes,vertices,triangles))return std::move(candidate);
    }
    return {};
}

inline IPLDirectEffectParams reflection_filter(const AudioReflection& reflection) {
    IPLDirectEffectParams params{};
    params.flags=static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION|IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION);
    params.distanceAttenuation=reflection.distance_gain;
    std::copy(reflection.bands.begin(),reflection.bands.end(),params.airAbsorption);
    return params;
}
}
