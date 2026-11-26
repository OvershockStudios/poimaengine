// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/audio.hpp"
#include <phonon.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace poima::audio_detail {
inline constexpr double acoustic_surface_epsilon=.002;
using AcousticPoint=std::array<double,3>;

inline AcousticPoint point(const IPLVector3& v) { return {v.x,v.y,v.z}; }
inline AcousticPoint subtract(const AcousticPoint& a,const AcousticPoint& b) {
    return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};
}
inline AcousticPoint offset(const AcousticPoint& p,double scale,const AcousticPoint& v) {
    return {p[0]+scale*v[0],p[1]+scale*v[1],p[2]+scale*v[2]};
}
inline double dot(const AcousticPoint& a,const AcousticPoint& b) {
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
}
inline AcousticPoint cross(const AcousticPoint& a,const AcousticPoint& b) {
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}

struct AcousticShape {
    const AcousticGeometry& geometry;
    std::size_t begin,end;
    AcousticPoint low,high;
};

inline AcousticShape acoustic_shape(const AcousticGeometry& geometry,std::size_t begin,std::size_t end,
                                         std::span<const IPLVector3> vertices) {
    const auto infinity=std::numeric_limits<double>::infinity();
    AcousticShape shape{geometry,begin,end,{infinity,infinity,infinity},{-infinity,-infinity,-infinity}};
    for(const auto& vertex:vertices) {
        const auto p=point(vertex);
        for(std::size_t axis=0;axis<3;++axis) {
            shape.low[axis]=std::min(shape.low[axis],p[axis]);
            shape.high[axis]=std::max(shape.high[axis],p[axis]);
        }
    }
    return shape;
}

using AcousticTriangle=std::array<AcousticPoint,3>;
inline AcousticTriangle acoustic_triangle(std::span<const IPLVector3> vertices,const IPLTriangle& triangle) {
    return {point(vertices[triangle.indices[0]]),point(vertices[triangle.indices[1]]),point(vertices[triangle.indices[2]])};
}

inline bool contains_point(const AcousticTriangle& triangle,const AcousticPoint& p) {
    const auto ab=subtract(triangle[1],triangle[0]), ac=subtract(triangle[2],triangle[0]), ap=subtract(p,triangle[0]);
    const double aa=dot(ab,ab), cc=dot(ac,ac), bc=dot(ab,ac), determinant=aa*cc-bc*bc;
    if(determinant<=1e-20)return false;
    const double x=(dot(ap,ab)*cc-dot(ap,ac)*bc)/determinant;
    const double y=(dot(ap,ac)*aa-dot(ap,ab)*bc)/determinant;
    constexpr double edge=1e-9;
    return x>=-edge && y>=-edge && x+y<=1+edge;
}

inline bool intersects_bounds(const AcousticPoint& from,const AcousticPoint& delta,
                              const AcousticShape& shape,double begin,double end) {
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

inline bool blocks_segment(const AcousticTriangle& triangle,const AcousticPoint& from,
                           const AcousticPoint& delta,double minimum,double maximum) {
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
    return fraction>minimum && fraction<maximum;
}

inline bool acoustic_segment_blocked(const AcousticPoint& from,const AcousticPoint& to,
                                   std::span<const AcousticShape> shapes,std::span<const IPLVector3> vertices,
                                   std::span<const IPLTriangle> triangles,bool include_destination=false,
                                   bool include_origin=false) {
    const auto delta=subtract(to,from);
    const auto margin=acoustic_surface_epsilon/std::sqrt(dot(delta,delta));
    const auto maximum=include_destination ? 1+margin : 1-margin;
    const auto minimum=include_origin ? -margin : margin;
    for(const auto& shape:shapes) {
        if(!intersects_bounds(from,delta,shape,include_origin ? minimum : 0,maximum))continue;
        for(std::size_t i=shape.begin;i<shape.end;++i)
            if(blocks_segment(acoustic_triangle(vertices,triangles[i]),from,delta,minimum,maximum))return true;
    }
    return false;
}

template<class Path>
IPLDirectEffectParams acoustic_secondary_filter(const Path& path) {
    IPLDirectEffectParams params{};
    params.flags=static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION|IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION);
    params.distanceAttenuation=path.distance_gain;
    std::copy(path.bands.begin(),path.bands.end(),params.airAbsorption);
    return params;
}
}
