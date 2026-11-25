// SPDX-License-Identifier: Apache-2.0
#include "poima/water_wakes.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace poima::water {

WakeField::WakeField(SurfaceWakes profile,const GridConfig& config)
    : profile_(profile),config_(config),decay_(std::exp(-profile.damping_per_second*step_seconds)),
      propagation_(std::pow(profile.speed_m_s*step_seconds/config.cell_size_m,2)),
      height_(std::size_t(config.cells_x)*config.cells_z),vertical_(height_.size()),
      next_height_(height_.size()),next_vertical_(height_.size()),
      corners_(std::size_t(config.cells_x+1)*(config.cells_z+1)),
      x_connection_(height_.size()),z_connection_(height_.size()) {}

std::size_t WakeField::index(Cell cell) const noexcept {
    return std::size_t(cell.z)*config_.cells_x+cell.x;
}

double WakeField::height(Cell cell) const noexcept { return height_[index(cell)]; }

double WakeField::corner(std::uint32_t x,std::uint32_t z) const noexcept {
    return corners_[std::size_t(z)*(config_.cells_x+1)+x];
}

double WakeField::connection(Cell lower,FaceAxis axis) const noexcept {
    return (axis==FaceAxis::x ? x_connection_ : z_connection_)[index(lower)];
}

void WakeField::refresh_connections(const Grid& grid) noexcept {
    const auto weight=[&](Cell a,Cell b,FaceAxis axis) {
        const auto gate=grid.face(a,axis);
        if (gate.openness==0) return 0.;
        const auto left=grid.sample(a),right=grid.sample(b);
        const auto sill=std::max({left.floor_height_m,right.floor_height_m,
            gate.sill_height_m.value_or(left.floor_height_m)});
        return std::min(left.surface_height_m,right.surface_height_m)>sill ? gate.openness : 0.;
    };
    for (std::uint32_t z=0;z<config_.cells_z;++z) for (std::uint32_t x=0;x<config_.cells_x;++x) {
        const Cell cell{x,z};
        const auto i=index(cell);
        x_connection_[i]=x+1<config_.cells_x ? weight(cell,{x+1,z},FaceAxis::x) : 0;
        z_connection_[i]=z+1<config_.cells_z ? weight(cell,{x,z+1},FaceAxis::z) : 0;
    }
}

void WakeField::refresh_corners() noexcept {
    for (std::uint32_t z=1;z<config_.cells_z;++z) for (std::uint32_t x=1;x<config_.cells_x;++x) {
        const Cell nw{x-1,z-1},ne{x,z-1},sw{x-1,z},se{x,z};
        // Anchor corners at seams with a closed face or a dry neighbor.
        // The same corner is then used on both sides of the seam.
        const bool connected=connection(nw,FaceAxis::x)>0 && connection(sw,FaceAxis::x)>0 &&
            connection(nw,FaceAxis::z)>0 && connection(ne,FaceAxis::z)>0;
        corners_[std::size_t(z)*(config_.cells_x+1)+x]=connected
            ? (height(nw)+height(ne)+height(sw)+height(se))*.25 : 0;
    }
}

void WakeField::advance(const Grid& grid,std::span<const WakeDriver> drivers) noexcept {
    refresh_connections(grid);
    for (std::size_t i=0;i<height_.size();++i) next_vertical_[i]=vertical_[i]*decay_;
    const auto transfer=[&](Cell lower,Cell upper,FaceAxis axis) {
        const auto a=index(lower),b=index(upper);
        const auto delta=decay_*propagation_*connection(lower,axis)*(height_[b]-height_[a]);
        next_vertical_[a]+=delta;
        next_vertical_[b]-=delta;
    };
    for (std::uint32_t z=0;z<config_.cells_z;++z) for (std::uint32_t x=0;x<config_.cells_x;++x) {
        if (x+1<config_.cells_x) transfer({x,z},{x+1,z},FaceAxis::x);
        if (z+1<config_.cells_z) transfer({x,z},{x,z+1},FaceAxis::z);
    }

    // Scatter an equal crest and trough across connected faces. Bilinear
    // weights blend the driver smoothly as it moves past cell centers.
    const auto size=config_.cell_size_m;
    for (const auto& driver : drivers) {
        const auto x=(driver.x_m-config_.origin_x_m)/size,z=(driver.z_m-config_.origin_z_m)/size;
        if (!(x>=0 && x<config_.cells_x && z>=0 && z<config_.cells_z)) continue;
        const auto base_x=static_cast<int>(std::floor(x-.5)),base_z=static_cast<int>(std::floor(z-.5));
        const auto u=x-.5-base_x,v=z-.5-base_z;
        const auto speed=std::hypot(driver.speed_x_m_s,driver.speed_z_m_s);
        const auto gain=profile_.body_gain*step_seconds*std::min(1.,10/std::max(speed,1e-12));
        for (int j=0;j<2;++j) for (int i=0;i<2;++i) {
            const auto ix=base_x+i,iz=base_z+j;
            if (ix<0 || iz<0 || ix>=int(config_.cells_x) || iz>=int(config_.cells_z)) continue;
            const Cell own{std::uint32_t(ix),std::uint32_t(iz)};
            const auto weight=(i ? u : 1-u)*(j ? v : 1-v)*gain;
            const auto drive=[&](double velocity,FaceAxis axis,int neighbor_x,int neighbor_z) {
                if (velocity==0 || neighbor_x<0 || neighbor_z<0 ||
                    neighbor_x>=int(config_.cells_x) || neighbor_z>=int(config_.cells_z)) return;
                const Cell other{std::uint32_t(neighbor_x),std::uint32_t(neighbor_z)};
                const auto delta=weight*std::abs(velocity)*connection(velocity>0 ? own : other,axis);
                next_vertical_[index(own)]-=delta;
                next_vertical_[index(other)]+=delta;
            };
            drive(driver.speed_x_m_s,FaceAxis::x,ix+(driver.speed_x_m_s>0 ? 1 : -1),iz);
            drive(driver.speed_z_m_s,FaceAxis::z,ix,iz+(driver.speed_z_m_s>0 ? 1 : -1));
        }
    }
    double peak=0;
    for (std::uint32_t z=0;z<config_.cells_z;++z) for (std::uint32_t x=0;x<config_.cells_x;++x) {
        const auto i=index({x,z});
        if (grid.sample({x,z}).depth_m==0) {
            next_vertical_[i]=next_height_[i]=0;
            continue;
        }
        next_height_[i]=height_[i]*decay_+next_vertical_[i];
        peak=std::max(peak,std::abs(next_height_[i]));
    }
    if (peak>profile_.max_amplitude_m) {
        const auto scale=profile_.max_amplitude_m/peak;
        for (std::size_t i=0;i<height_.size();++i) {
            next_height_[i]*=scale;
            next_vertical_[i]*=scale;
        }
    }
    height_.swap(next_height_);
    vertical_.swap(next_vertical_);
    refresh_corners();
}

void WakeField::restore(const Grid& grid,std::vector<double> height,std::vector<double> vertical) {
    if (height.size()!=height_.size() || vertical.size()!=vertical_.size())
        throw std::invalid_argument("Saved wake samples must match the water grid.");
    for (std::uint32_t z=0;z<config_.cells_z;++z) for (std::uint32_t x=0;x<config_.cells_x;++x) {
        const auto i=index({x,z});
        const auto limit=grid.tick()==0 || grid.sample({x,z}).depth_m==0 ? 0 : profile_.max_amplitude_m;
        if (!(std::abs(height[i])<=limit*(1+1e-12) && std::abs(vertical[i])<=limit*(2+1e-12)))
            throw std::invalid_argument("Saved wake height or velocity exceeds its committed bounds.");
    }
    height_=std::move(height);
    vertical_=std::move(vertical);
    refresh_connections(grid);
    refresh_corners();
}

}
