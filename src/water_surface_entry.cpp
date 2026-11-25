// SPDX-License-Identifier: Apache-2.0
#include "poima/water_region.hpp"

#include <cmath>
#include <limits>

namespace poima::water {
namespace {

using Position = std::array<double,3>;

struct GridAxis {
    double start, delta, origin, size;
    int count;

    double edge(int index) const { return origin+index*size; }

    bool clip(double& enter, double& exit) const {
        if (delta==0) return start>=origin && start<edge(count);
        const auto a=(origin-start)/delta, b=(edge(count)-start)/delta;
        enter=std::max(enter,std::min(a,b));
        exit=std::min(exit,std::max(a,b));
        return enter<=exit;
    }

    int cell_at(double fraction) const {
        const auto position=std::fma(fraction,delta,start);
        auto index=static_cast<int>(std::clamp(std::floor((position-origin)/size),0.,double(count-1)));
        // Use the same world-space boundaries as Grid::sample_world.
        if (index+1<count && position>=edge(index+1)) ++index;
        else if (index>0 && position<edge(index)) --index;
        return index;
    }

    double exit_at(int index) const {
        if (delta==0) return std::numeric_limits<double>::infinity();
        return (edge(index+(delta>0 ? 1 : 0))-start)/delta;
    }
};

Position along(const Position& from,const Position& delta,double fraction) {
    return {std::fma(fraction,delta[0],from[0]),std::fma(fraction,delta[1],from[1]),
        std::fma(fraction,delta[2],from[2])};
}

}

std::optional<SurfaceEntry> Region::surface_entry(const Position& from,const Position& to) const {
    const auto bounded=[](double axis) { return std::isfinite(axis) && std::abs(axis)<=1e6; };
    if (!std::ranges::all_of(from,bounded) || !std::ranges::all_of(to,bounded)) return std::nullopt;
    auto hit=terrain_surface_entry(from,to);
    if (!storage_ || to[1]>=from[1]) return hit;
    const Position delta{to[0]-from[0],to[1]-from[1],to[2]-from[2]};
    for (const auto i : storage_->placed()) {
        const auto sample=storage_->sample(i);
        if (sample.surface_height_m<=sample.floor_height_m) continue;
        const auto fraction=(sample.surface_height_m-from[1])/delta[1];
        if (fraction<0 || fraction>1 || (hit && fraction>=hit->fraction)) continue;
        auto point=along(from,delta,fraction);
        if (!sample.footprint.contains(point[0],point[2])) continue;
        point[1]=sample.surface_height_m;
        hit=SurfaceEntry{fraction,point,{0,1,0},std::nullopt,sample.id};
    }
    return hit;
}

std::optional<SurfaceEntry> Region::terrain_surface_entry(const Position& from,const Position& to) const {
    const Position delta{to[0]-from[0],to[1]-from[1],to[2]-from[2]};
    const auto& config=grid_.config();
    const auto size=config.cell_size_m;
    const GridAxis x{from[0],delta[0],config.origin_x_m,size,static_cast<int>(config.cells_x)};
    const GridAxis z{from[2],delta[2],config.origin_z_m,size,static_cast<int>(config.cells_z)};
    std::optional<SurfaceEntry> offshore;
    if (optics_ && optics_->offshore && delta[1]<0) {
        const auto stage=optics_->offshore->surface_height_m;
        const auto t=(stage-from[1])/delta[1];
        if (t>=0 && t<=1) {
            auto point=along(from,delta,t);
            if (!grid_.sample_world(point[0],point[2]) && offshore_levels(point[0],point[2])) {
                point[1]=stage;
                offshore=SurfaceEntry{t,point,{0,1,0},std::nullopt};
            }
        }
    }
    double enter=0, exit=offshore ? offshore->fraction : 1.;
    if (!x.clip(enter,exit) || !z.clip(enter,exit)) return offshore;
    int ix=x.cell_at(enter),iz=z.cell_at(enter);
    // Visit crossed cells in order. Each move passes at least one grid line.
    while (ix>=0 && ix<x.count && iz>=0 && iz<z.count && enter<=exit) {
        const auto x_exit=x.exit_at(ix),z_exit=z.exit_at(iz);
        const auto end=std::min({x_exit,z_exit,exit});
        const Cell cell{static_cast<std::uint32_t>(ix),static_cast<std::uint32_t>(iz)};
        if (shore_corner_heights_m_.empty()) {
            const auto sample=grid_.sample(cell);
            if (sample.depth_m>0 && delta[1]<0) {
                const auto t=(sample.surface_height_m-from[1])/delta[1];
                if (t>=enter && t<=end) {
                    auto point=along(from,delta,t);
                    point[1]=sample.surface_height_m;
                    return SurfaceEntry{t,point,{0,1,0},cell};
                }
            }
        } else {
            const auto bed=fitted_bed(cell);
            const auto start=along(from,delta,enter);
            const auto cell_x=x.edge(ix),cell_z=z.edge(iz);
            const auto u=(start[0]-cell_x)/size,v=(start[2]-cell_z)/size;
            std::optional<SurfaceEntry> closest;
            for (std::size_t side=0;side<4;++side) {
                if (bed.inactive_wedges & (1u<<side)) continue;
                const auto gradient=fitted_water_gradient(bed.swell_center_m,bed.swell_corners_m,side,size);
                const auto slope=delta[1]-gradient[0]*delta[0]-gradient[1]*delta[2];
                if (slope>=0) continue;
                const auto stage=bed.stages_m[side]+fitted_water_fan(bed.swell_center_m,bed.swell_corners_m,side,u,v);
                const auto t=enter+(stage-start[1])/slope;
                if (!(t>=enter && t<=end) || (closest && t>=closest->fraction)) continue;
                auto point=along(from,delta,t);
                const auto s=std::clamp((point[0]-cell_x)/size,0.,1.);
                const auto r=std::clamp((point[2]-cell_z)/size,0.,1.);
                if (fitted_water_side(s,r)!=side) continue;
                const auto top=bed.stages_m[side]+fitted_water_fan(bed.swell_center_m,bed.swell_corners_m,side,s,r);
                if (top<=fitted_water_fan(bed.center_m,bed.corners_m,side,s,r)) continue;
                const auto scale=1/std::hypot(gradient[0],1.,gradient[1]);
                point[1]=top;
                closest=SurfaceEntry{t,point,{-gradient[0]*scale,scale,-gradient[1]*scale},cell};
            }
            if (closest) return closest;
        }
        if (x_exit>end && z_exit>end) break;
        if (x_exit==end) ix+=delta[0]>0 ? 1 : -1;
        if (z_exit==end) iz+=delta[2]>0 ? 1 : -1;
        enter=std::max(enter,end);
    }
    return offshore;
}

} // namespace poima::water
