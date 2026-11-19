// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace poima::pose_frames {
inline constexpr std::size_t max_pairs=32;
using Vector=std::array<double,3>;
enum class Basis { regular, reflected, sheared, degenerate };
struct Axes {
    std::array<Vector,3> direction{};
    Vector lengths{};
    Basis basis=Basis::degenerate;
};
inline double dot(const Vector& left,const Vector& right) {
    return left[0]*right[0]+left[1]*right[1]+left[2]*right[2];
}
inline Axes axes(const Matrix4& matrix) {
    Axes result;
    for(std::size_t axis=0;axis<3;++axis) {
        auto& direction=result.direction[axis];
        direction={matrix[axis*4],matrix[axis*4+1],matrix[axis*4+2]};
        auto& length=result.lengths[axis];
        length=std::hypot(direction[0],direction[1],direction[2]);
        if(length>0 && std::isfinite(length))for(double& value:direction)value/=length;
    }
    if(std::any_of(result.lengths.begin(),result.lengths.end(),[](double v){return !(v>0) || !std::isfinite(v);}))
        return result;
    const auto& [x,y,z]=result.direction;
    const Vector cross={x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0]};
    const double chirality=dot(cross,z);
    if(std::abs(chirality)<=1e-8)return result;
    if(std::max({std::abs(dot(x,y)),std::abs(dot(x,z)),std::abs(dot(y,z))})>1e-6)
        result.basis=Basis::sheared;
    else result.basis=chirality>0?Basis::regular:Basis::reflected;
    return result;
}
inline const char* basis_name(Basis kind) {
    switch(kind) {
        case Basis::regular:return "regular";
        case Basis::reflected:return "reflected";
        case Basis::sheared:return "sheared";
        case Basis::degenerate:return "degenerate";
    }
    return "degenerate";
}
inline Vector project(const Axes& basis,const Vector& world) {
    return {dot(basis.direction[0],world),dot(basis.direction[1],world),dot(basis.direction[2],world)};
}
inline double rotation_error(const Axes& source,const Axes& target) {
    // Caller supplies two proper orthogonal bases. The skew term preserves
    // precision close to alignment and the trace resolves half turns.
    const auto x=project(target,source.direction[0]);
    const auto y=project(target,source.direction[1]);
    const auto z=project(target,source.direction[2]);
    const double sine=.5*std::hypot(y[2]-z[1],z[0]-x[2],x[1]-y[0]);
    const double cosine=std::clamp(.5*(x[0]+y[1]+z[2]-1),-1.,1.);
    return std::atan2(sine,cosine)*180/std::numbers::pi;
}
}
