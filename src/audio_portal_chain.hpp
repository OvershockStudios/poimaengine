// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "audio_portal.hpp"
#include <map>
#include <queue>

namespace poima::audio_detail {
inline constexpr std::size_t chain_portal_candidates=8, samples_per_portal=9;
inline constexpr std::size_t chain_segment_budget=2048, chain_expansion_budget=256;

struct PortalChainBudget {
    std::size_t segments=chain_segment_budget, expansions=chain_expansion_budget;
    bool exceeded=false;

    bool take(std::size_t& remaining) {
        if(remaining==0) { exceeded=true;return false; }
        --remaining;
        return true;
    }
};

class PortalChainSearch {
    static constexpr std::size_t max_points=chain_portal_candidates*samples_per_portal;
    using SampleIndex=std::uint8_t;
    enum class Visibility : std::uint8_t { Unknown, Blocked, Clear };
    struct Leg {
        double length=0;
        Visibility visibility=Visibility::Unknown;
    };
    struct Route {
        std::array<SampleIndex,max_audio_portal_chain> points{};
        std::uint8_t count=0;
        unsigned visited=0;
        std::uint32_t key=0;
        double length=0, estimate=0;
    };
    struct LaterRoute {
        bool operator()(const Route& a,const Route& b) const {
            return std::tie(a.estimate,a.count,a.points)>std::tie(b.estimate,b.count,b.points);
        }
    };

    const AudioPath& direct_;
    PortalChainBudget& budget_;
    std::span<const AcousticShape> shapes_;
    std::span<const IPLVector3> vertices_;
    std::span<const IPLTriangle> triangles_;
    std::vector<PortalAperture> apertures_;
    std::array<AcousticPoint,max_points> points_{};
    std::array<Leg,max_points> from_source_{}, to_listener_{};
    std::array<std::array<Visibility,max_points>,max_points> between_{};

    static std::size_t opening_index(SampleIndex index) { return index/samples_per_portal; }
    const PortalAperture& aperture(SampleIndex index) const { return apertures_[opening_index(index)]; }
    std::size_t point_count() const { return apertures_.size()*samples_per_portal; }

    bool clear(Visibility& cached,const AcousticPoint& from,const AcousticPoint& to,bool both) {
        if(cached==Visibility::Unknown) {
            if(!budget_.take(budget_.segments))return false;
            cached=acoustic_segment_blocked(from,to,shapes_,vertices_,triangles_,true,both) ?
                Visibility::Blocked : Visibility::Clear;
        }
        return cached==Visibility::Clear;
    }

    bool from_source(SampleIndex point) {
        auto& leg=from_source_[point];
        return leg.length>acoustic_surface_epsilon && clear(leg.visibility,direct_.source,points_[point],false);
    }
    bool to_listener(SampleIndex point) {
        auto& leg=to_listener_[point];
        return leg.length>acoustic_surface_epsilon && clear(leg.visibility,direct_.listener,points_[point],false);
    }
    bool between(SampleIndex a,SampleIndex b,double distance) {
        return distance>acoustic_surface_epsilon &&
            clear(between_[std::min(a,b)][std::max(a,b)],points_[std::min(a,b)],points_[std::max(a,b)],true);
    }

    std::uint32_t visible_samples(const Route& route,std::size_t crossing) {
        const auto current=route.points[crossing];
        const bool starts=crossing==0, ends=crossing+1==route.count;
        const auto previous=route.points[starts ? 0 : crossing-1];
        const auto next=route.points[ends ? crossing : crossing+1];
        std::uint32_t visible=0;
        const auto base=opening_index(current)*samples_per_portal;
        for(std::size_t sample=0;sample<samples_per_portal;++sample) {
            const auto probe=static_cast<SampleIndex>(base+sample);
            if(!starts) {
                const auto& incoming=crossing==1 ? direct_.source : points_[route.points[crossing-2]];
                if(!aperture(previous).crosses(incoming,points_[probe]))continue;
            }
            if(!ends) {
                const auto& outgoing=crossing+2==route.count ? direct_.listener : points_[route.points[crossing+2]];
                if(!aperture(next).crosses(points_[probe],outgoing))continue;
            }
            if(!(starts ? from_source(probe) : between(previous,probe,length(subtract(points_[previous],points_[probe])))))
                continue;
            if(ends ? to_listener(probe) : between(probe,next,length(subtract(points_[probe],points_[next]))))
                ++visible;
        }
        return visible;
    }

    std::optional<AudioPortalPath> measured(const Route& route,const Matrix4& listener) {
        std::vector<AudioPortalCrossing> crossings;
        crossings.reserve(route.count);
        double gain=1;
        for(std::size_t i=0;i<route.count;++i) {
            const auto point=route.points[i];
            const auto& portal=aperture(point).portal;
            const auto visible=visible_samples(route,i);
            if(budget_.exceeded || visible==0)return {};
            gain*=portal.opening.gain*std::sqrt(visible/9.);
            crossings.push_back({portal.entity,static_cast<std::uint32_t>(point%samples_per_portal),
                visible,points_[point]});
        }
        auto path=measured_portal_path(direct_,listener,std::move(crossings.back()),route.estimate,gain);
        crossings.pop_back();
        path.approach=std::move(crossings);
        return path;
    }

public:
    PortalChainSearch(const AudioPath& direct,std::span<const AudioPortalInstance> portals,
        std::span<const AcousticShape> shapes,std::span<const IPLVector3> vertices,
        std::span<const IPLTriangle> triangles,PortalChainBudget& budget)
        : direct_(direct),budget_(budget),shapes_(shapes),vertices_(vertices),triangles_(triangles) {
        struct Candidate { const AudioPortalInstance* portal=nullptr;double estimate=0; };
        std::array<Candidate,max_audio_portals> candidates;
        std::size_t count=0;
        for(const auto& portal:portals) {
            if(portal.opening.gain==0)continue;
            const PortalAperture opening(portal);
            candidates[count++]={&portal,length(subtract(direct.source,opening.center))+
                length(subtract(direct.listener,opening.center))};
        }
        std::sort(candidates.begin(),candidates.begin()+static_cast<std::ptrdiff_t>(count),
            [](const Candidate& a,const Candidate& b) {
                return std::tie(a.estimate,a.portal->entity)<std::tie(b.estimate,b.portal->entity);
            });
        apertures_.reserve(std::min(count,chain_portal_candidates));
        for(std::size_t i=0;i<std::min(count,chain_portal_candidates);++i) {
            const auto& opening=apertures_.emplace_back(*candidates[i].portal);
            for(std::size_t sample=0;sample<samples_per_portal;++sample) {
                const auto id=i*samples_per_portal+sample;
                points_[id]=opening.point(static_cast<std::uint32_t>(sample));
                from_source_[id].length=length(subtract(points_[id],direct.source));
                to_listener_[id].length=length(subtract(points_[id],direct.listener));
            }
        }
    }

    std::optional<AudioPortalPath> find(const Matrix4& listener) {
        if(apertures_.size()<2 || budget_.exceeded)return {};
        // Straight-line distance to the listener is a lower bound on each
        // unfinished route. Expand the most promising route first.
        std::priority_queue<Route,std::vector<Route>,LaterRoute> pending;
        std::map<std::uint32_t,double> shortest;
        const auto submit=[&](Route route,const AcousticPoint& incoming) {
            const auto last=route.points[route.count-1];
            const auto& opening=aperture(last);
            const bool front=dot(subtract(incoming,opening.center),opening.normal)>0;
            // The visited openings, last sample and its entry side determine
            // the continuation. A longer route to that state is dominated.
            route.key=(route.visited<<8) | (std::uint32_t(last)<<1) | std::uint32_t(front);
            auto [known,inserted]=shortest.try_emplace(route.key,route.length);
            if(!inserted && route.length>=known->second)return;
            known->second=route.length;
            pending.push(route);
        };
        for(SampleIndex point=0;point<point_count();++point) {
            const auto distance=from_source_[point].length;
            const auto estimate=distance+to_listener_[point].length;
            if(estimate<max_portal_distance_m && from_source(point))
                submit({{point},1,1u<<opening_index(point),0,distance,estimate},direct_.source);
        }
        while(!pending.empty() && !budget_.exceeded) {
            const auto route=pending.top();
            pending.pop();
            if(route.length!=shortest.at(route.key))continue;
            if(!budget_.take(budget_.expansions))break;
            const auto last=route.points[route.count-1];
            const auto& incoming=route.count==1 ? direct_.source : points_[route.points[route.count-2]];
            if(route.count>=2 && aperture(last).crosses(incoming,direct_.listener) && to_listener(last))
                return measured(route,listener);
            if(route.count==max_audio_portal_chain)continue;
            for(SampleIndex point=0;point<point_count();++point) {
                const auto bit=1u<<opening_index(point);
                if((route.visited&bit) || !aperture(last).crosses(incoming,points_[point]))continue;
                const auto distance=length(subtract(points_[last],points_[point]));
                const auto estimate=route.length+distance+to_listener_[point].length;
                if(estimate>=max_portal_distance_m || !between(last,point,distance))continue;
                auto extended=route;
                extended.points[extended.count++]=point;
                extended.visited|=bit;
                extended.length+=distance;
                extended.estimate=estimate;
                submit(extended,points_[last]);
            }
        }
        return {};
    }
};
}
