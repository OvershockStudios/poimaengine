// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "audio_visibility.hpp"
#include <string_view>
#include <tuple>

namespace poima::audio_detail {
// Bound the reference solver separately from Steam Audio's direct simulation.
// All enabled acoustic geometry still participates in occlusion within this budget.
inline constexpr std::size_t max_reflection_triangles=16384, max_reflection_candidates=8;
inline constexpr double max_reflection_distance=100;
inline std::optional<AudioReflection> mirror_path(const AudioPath& direct,const Matrix4& listener,
                                                  const AcousticShape& shape,std::uint32_t face,
                                                  std::span<const IPLVector3> vertices,std::span<const IPLTriangle> triangles) {
    const auto first=shape.begin+face*2;
    const auto triangle=acoustic_triangle(vertices,triangles[first]);
    auto normal=cross(subtract(triangle[1],triangle[0]),subtract(triangle[2],triangle[0]));
    const auto magnitude=std::sqrt(dot(normal,normal));
    if(magnitude<=1e-12)return {};
    const auto& m=shape.geometry.world;
    const AcousticPoint center{m[12],m[13],m[14]};
    const auto outward=dot(normal,subtract(triangle[0],center));
    if(outward==0)return {};
    normal=offset({},outward<0 ? -1/magnitude : 1/magnitude,normal);
    const auto source_side=dot(normal,subtract(direct.source,triangle[0]));
    const auto listener_side=dot(normal,subtract(direct.listener,triangle[0]));
    if(source_side<=acoustic_surface_epsilon || listener_side<=acoustic_surface_epsilon)return {};
    const auto image=offset(direct.source,-2*source_side,normal);
    const auto line=subtract(image,direct.listener);
    const auto distance=std::sqrt(dot(line,line));
    if(distance>max_reflection_distance)return {};
    const auto bounce=offset(direct.listener,listener_side/(listener_side+source_side),line);
    if(!contains_point(triangle,bounce) &&
       !contains_point(acoustic_triangle(vertices,triangles[first+1]),bounce))return {};

    AudioReflection result;
    result.entity=shape.geometry.entity;result.face=face;result.point=bounce;result.distance=distance;
    result.propagation_delay_samples=static_cast<std::uint32_t>(std::llround(distance/343*audio_rate));
    result.distance_gain=static_cast<float>(1/std::max(1.,distance));
    const auto toward=subtract(bounce,direct.listener);
    const auto length=std::sqrt(dot(toward,toward));
    for(std::size_t axis=0;axis<3;++axis) {
        const AcousticPoint basis{listener[axis*4],listener[axis*4+1],listener[axis*4+2]};
        result.direction[axis]=dot(basis,toward)/length;
        result.bands[axis]=std::sqrt((1-shape.geometry.material.absorption[axis])*(1-shape.geometry.material.scattering));
    }
    return result;
}

inline std::optional<AudioReflection> first_reflection(const AudioPath& direct,const Matrix4& listener,
                                                       std::span<const AcousticShape> shapes,
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
        if(!acoustic_segment_blocked(direct.source,candidate.point,shapes,vertices,triangles) &&
           !acoustic_segment_blocked(direct.listener,candidate.point,shapes,vertices,triangles))return std::move(candidate);
    }
    return {};
}

}
