// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/water_region_definition.hpp"

namespace poima::water {

class WakeField {
public:
    WakeField(SurfaceWakes profile,const GridConfig& config);
    const SurfaceWakes& profile() const noexcept { return profile_; }
    double height(Cell cell) const noexcept;
    double corner(std::uint32_t x,std::uint32_t z) const noexcept;
    std::span<const double> heights() const noexcept { return height_; }
    std::span<const double> vertical_steps() const noexcept { return vertical_; }
    void advance(const Grid& grid,std::span<const WakeDriver> drivers) noexcept;
    void restore(const Grid& grid,std::vector<double> height,std::vector<double> vertical);

private:
    SurfaceWakes profile_;
    GridConfig config_;
    double decay_, propagation_;
    std::vector<double> height_,vertical_,next_height_,next_vertical_,corners_,x_connection_,z_connection_;
    double connection(Cell lower,FaceAxis axis) const noexcept;
    void refresh_connections(const Grid& grid) noexcept;
    void refresh_corners() noexcept;
    std::size_t index(Cell cell) const noexcept;
};

}
