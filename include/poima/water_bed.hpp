// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace poima::water {

// XZ coordinates in a cell are measured from its lower corner in metres / cell
// size. The bed is a fan from its measured center to the four shared edges.
template<typename T>
std::array<T,2> fitted_water_levels(T center,const std::array<T,4>& corners,
                                  const std::array<T,4>& stages,T x,T z) noexcept {
    const bool near_z=z<=x && z<=1-x;
    const bool far_x=x>=z && x>=1-z;
    const bool far_z=z>=x && z>=1-x;
    const std::size_t side=near_z ? 0 : far_x ? 1 : far_z ? 2 : 3;
    const std::array<T,4> along{x,z,1-x,1-z};
    const std::array<T,4> inward{z,1-x,1-z,x};
    const auto a=corners[side],b=corners[(side+1)%4];
    const auto floor=a+(b-a)*along[side]+(2*center-a-b)*inward[side];
    return {floor,std::max(floor,stages[side])};
}

struct FittedWaterBed {
    double center_m;
    // Near Z, then around the perimeter. Each stage belongs to the wedge
    // spanning the corresponding corner and the next corner.
    std::array<double,4> corners_m,stages_m;
};

} // namespace poima::water
