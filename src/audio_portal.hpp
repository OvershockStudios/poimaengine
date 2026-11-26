// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "audio_visibility.hpp"
#include <tuple>

namespace poima::audio_detail {
inline constexpr std::size_t max_portal_triangles=8192, portal_probe_budget=512;
inline constexpr std::size_t portal_candidates=4;
inline constexpr double max_portal_distance_m=100;

inline double length(const AcousticPoint& point) { return std::sqrt(dot(point,point)); }

struct PortalAperture {
    const AudioPortalInstance& portal;
    AcousticPoint center, right, up, normal;

    explicit PortalAperture(const AudioPortalInstance& value)
        : portal(value), center{value.world[12],value.world[13],value.world[14]},
          right{value.world[0],value.world[1],value.world[2]}, up{value.world[4],value.world[5],value.world[6]},
          normal(cross(right,up)) {}

    bool crosses(const AcousticPoint& from,const AcousticPoint& to) const {
        return dot(subtract(from,center),normal)*dot(subtract(to,center),normal)<0;
    }
    AcousticPoint point(std::uint32_t sample) const {
        const auto x=(double(sample%3)-1)*portal.opening.half_extents[0]*(2./3);
        const auto y=(double(sample/3)-1)*portal.opening.half_extents[1]*(2./3);
        return offset(offset(center,x,right),y,up);
    }
};

inline AudioPortalPath measured_portal_path(const AudioPath& direct,const Matrix4& listener,
    AudioPortalCrossing crossing,double distance,double aperture_gain) {
    AudioPortalPath path;
    static_cast<AudioPortalCrossing&>(path)=std::move(crossing);
    path.distance=distance;
    path.distance_gain=static_cast<float>(1/std::max(1.,distance));
    path.propagation_delay_samples=static_cast<std::uint32_t>(std::llround(distance/343*audio_rate));
    const auto toward=subtract(path.point,direct.listener);
    const auto last_leg=length(toward);
    const double excess=std::max(0.,distance-direct.distance);
    constexpr std::array frequencies{400.,2500.,10000.};
    for(std::size_t axis=0;axis<3;++axis) {
        const AcousticPoint basis{listener[axis*4],listener[axis*4+1],listener[axis*4+2]};
        path.direction[axis]=dot(basis,toward)/last_leg;
        path.bands[axis]=static_cast<float>(aperture_gain/std::sqrt(1+4*frequencies[axis]*excess/343));
    }
    return path;
}

inline std::optional<AudioPortalPath> portal_path(const AudioPath& direct,const Matrix4& listener,
    std::span<const AudioPortalInstance> portals,std::span<const AcousticShape> shapes,
    std::span<const IPLVector3> vertices,std::span<const IPLTriangle> triangles,
    std::size_t& remaining_probes,bool& budget_exceeded) {
    if(direct.occlusion!=0 || direct.distance>=max_portal_distance_m)return {};
    struct Candidate {
        const AudioPortalInstance* portal=nullptr;
        double center_distance=0;
    };
    std::array<Candidate,max_audio_portals> candidates;
    std::size_t count=0;
    for(const auto& portal:portals) {
        if(portal.opening.gain==0)continue;
        const PortalAperture aperture(portal);
        if(!aperture.crosses(direct.source,direct.listener))continue;
        candidates[count++]={&portal,length(subtract(direct.source,aperture.center))+
            length(subtract(direct.listener,aperture.center))};
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
        const PortalAperture portal(*candidate.portal);
        const auto& opening=candidate.portal->opening;
        std::uint32_t visible=0, sample=0;
        AcousticPoint closest{};
        double distance=max_portal_distance_m;
        for(std::uint32_t probe=0;probe<9;++probe) {
            const auto aperture=portal.point(probe);
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
        const double aperture_gain=opening.gain*std::sqrt(visible/9.);
        auto path=measured_portal_path(direct,listener,{candidate.portal->entity,sample,visible,closest},
            distance,aperture_gain);
        const double score=path.distance_gain*(path.bands[0]+path.bands[1]+path.bands[2]);
        if(score>best_score) { best_score=score;best=std::move(path); }
    }
    return best;
}
}
