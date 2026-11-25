// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "audio_visibility.hpp"
#include <tuple>

namespace poima::audio_detail {
inline constexpr std::size_t max_portal_triangles=8192, portal_probe_budget=512;
inline constexpr std::size_t portal_candidates=4;
inline constexpr double max_portal_distance_m=100;

inline double length(const AcousticPoint& point) { return std::sqrt(dot(point,point)); }

inline std::optional<AudioPortalPath> portal_path(const AudioPath& direct,const Matrix4& listener,
    std::span<const AudioPortalInstance> portals,std::span<const AcousticShape> shapes,
    std::span<const IPLVector3> vertices,std::span<const IPLTriangle> triangles,
    std::size_t& remaining_probes,bool& budget_exceeded) {
    if(direct.occlusion!=0 || direct.distance>=max_portal_distance_m)return {};
    struct Candidate {
        const AudioPortalInstance* portal=nullptr;
        AcousticPoint center, right, up;
        double center_distance=0;
    };
    std::array<Candidate,max_audio_portals> candidates;
    std::size_t count=0;
    for(const auto& portal:portals) {
        if(portal.opening.gain==0)continue;
        const auto& m=portal.world;
        Candidate c{&portal,{m[12],m[13],m[14]},{m[0],m[1],m[2]},{m[4],m[5],m[6]},0};
        const auto normal=cross(c.right,c.up);
        const auto from=subtract(direct.source,c.center), to=subtract(direct.listener,c.center);
        if(dot(from,normal)*dot(to,normal)>=0)continue;
        c.center_distance=length(from)+length(to);
        candidates[count++]=c;
    }
    std::sort(candidates.begin(),candidates.begin()+static_cast<std::ptrdiff_t>(count),
        [](const Candidate& left,const Candidate& right) {
            return std::tie(left.center_distance,left.portal->entity)<std::tie(right.center_distance,right.portal->entity);
        });
    std::optional<AudioPortalPath> best;
    double best_score=0;
    for(std::size_t i=0;i<std::min(count,portal_candidates);++i) {
        if(remaining_probes<9) { budget_exceeded=true;break; }
        remaining_probes-=9;
        const auto& candidate=candidates[i];
        const auto& opening=candidate.portal->opening;
        std::uint32_t visible=0, sample=0;
        AcousticPoint closest{};
        double distance=max_portal_distance_m;
        for(std::uint32_t probe=0;probe<9;++probe) {
            const auto x=(double(probe%3)-1)*opening.half_extents[0]*(2./3);
            const auto y=(double(probe/3)-1)*opening.half_extents[1]*(2./3);
            const auto aperture=offset(offset(candidate.center,x,candidate.right),y,candidate.up);
            const auto from_length=length(subtract(aperture,direct.source));
            const auto to_length=length(subtract(aperture,direct.listener));
            if(from_length<=acoustic_surface_epsilon || to_length<=acoustic_surface_epsilon ||
               acoustic_segment_blocked(direct.source,aperture,shapes,vertices,triangles,true) ||
               acoustic_segment_blocked(direct.listener,aperture,shapes,vertices,triangles,true))continue;
            ++visible;
            if(from_length+to_length<distance) {
                distance=from_length+to_length;
                closest=aperture;
                sample=probe;
            }
        }
        if(distance==max_portal_distance_m)continue;
        AudioPortalPath path;
        path.entity=candidate.portal->entity;
        path.sample=sample;
        path.visible_samples=visible;
        path.point=closest;
        path.distance=distance;
        path.distance_gain=static_cast<float>(1/std::max(1.,distance));
        path.propagation_delay_samples=static_cast<std::uint32_t>(std::llround(distance/343*audio_rate));
        const auto toward=subtract(closest,direct.listener);
        const auto last_leg=length(toward);
        const double aperture_gain=opening.gain*std::sqrt(visible/9.);
        const double excess=std::max(0.,distance-direct.distance);
        constexpr std::array frequencies{400.,2500.,10000.};
        for(std::size_t axis=0;axis<3;++axis) {
            const AcousticPoint basis{listener[axis*4],listener[axis*4+1],listener[axis*4+2]};
            path.direction[axis]=dot(basis,toward)/last_leg;
            path.bands[axis]=static_cast<float>(aperture_gain/std::sqrt(1+4*frequencies[axis]*excess/343));
        }
        const double score=path.distance_gain*(path.bands[0]+path.bands[1]+path.bands[2]);
        if(score>best_score) { best_score=score;best=std::move(path); }
    }
    return best;
}
}
