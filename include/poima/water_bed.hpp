// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace poima::water {

// XZ coordinates in a cell are measured from its lower corner in metres / cell
// size. The bed is a fan from its measured center to the four shared edges.
template<typename T>
std::size_t fitted_water_side(T x,T z) noexcept {
    const bool near_z=z<=x && z<=1-x;
    const bool far_x=x>=z && x>=1-z;
    const bool far_z=z>=x && z>=1-x;
    return near_z ? 0 : far_x ? 1 : far_z ? 2 : 3;
}

template<typename T>
T fitted_water_fan(T center,const std::array<T,4>& corners,std::size_t side,T x,T z) noexcept {
    const std::array<T,4> along{x,z,1-x,1-z};
    const std::array<T,4> inward{z,1-x,1-z,x};
    const auto a=corners[side],b=corners[(side+1)%4];
    return a+(b-a)*along[side]+(2*center-a-b)*inward[side];
}

template<typename T>
std::array<T,2> fitted_water_levels(T center,const std::array<T,4>& corners,
    const std::array<T,4>& stages,T x,T z,T swell_center=0,
    const std::array<T,4>& swell_corners={},std::uint32_t inactive_wedges=0) noexcept {
    const auto side=fitted_water_side(x,z);
    const auto floor=fitted_water_fan(center,corners,side,x,z);
    if (inactive_wedges & (1u<<side)) return {floor,floor};
    const auto surface=stages[side]+fitted_water_fan(swell_center,swell_corners,side,x,z);
    return {floor,std::max(floor,surface)};
}

template<typename T>
std::array<T,2> fitted_water_gradient(T center,const std::array<T,4>& corners,
                                    std::size_t side,T size) noexcept {
    const auto a=corners[side],b=corners[(side+1)%4];
    const auto along=(b-a)/size,inward=(2*center-a-b)/size;
    switch (side) {
    case 0: return {along,inward};
    case 1: return {-inward,along};
    case 2: return {-along,-inward};
    default: return {inward,-along};
    }
}

struct FittedWaterBed {
    double center_m;
    // Near Z, then around the perimeter. Each stage belongs to the wedge
    // spanning the corresponding corner and the next corner.
    std::array<double,4> corners_m,stages_m;
    double swell_center_m=0;
    std::array<double,4> swell_corners_m{};
    std::uint32_t inactive_wedges=0;
};

} // namespace poima::water
