// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "runtime_physical_pose.hpp"
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace poima {
namespace detail {
template<std::size_t N>bool handoff_array_same_bits(const std::array<double,N>& left,const std::array<double,N>& right) {
    for(std::size_t i=0;i<N;++i)if(std::bit_cast<std::uint64_t>(left[i])!=std::bit_cast<std::uint64_t>(right[i]))return false;
    return true;
}
}
// Portable, named witnesses for a one-way motor suspension. Support is the
// frozen last locomotion observation, not a query of the corpse's environment.
// Raw IDs retain their full native index/generation bits, including retired
// support IDs. The enclosing ledger validates their historical lineage.
struct RuntimeControllerSupportState {
    std::uint32_t ground_state=3;
    std::uint32_t ground_body_id=std::numeric_limits<std::uint32_t>::max();
    std::array<double,3> normal{},velocity{};
    bool operator==(const RuntimeControllerSupportState& other) const {
        return ground_state==other.ground_state && ground_body_id==other.ground_body_id &&
            detail::handoff_array_same_bits(normal,other.normal) && detail::handoff_array_same_bits(velocity,other.velocity);
    }
};
struct RuntimeControllerProxyState {
    std::uint32_t body_id=std::numeric_limits<std::uint32_t>::max();
    std::array<double,3> position{},linear_velocity{},angular_velocity{};
    std::array<double,4> rotation{0,0,0,1};
    bool active=false;
    bool operator==(const RuntimeControllerProxyState& other) const {
        return body_id==other.body_id && active==other.active && detail::handoff_array_same_bits(position,other.position) &&
            detail::handoff_array_same_bits(linear_velocity,other.linear_velocity) && detail::handoff_array_same_bits(angular_velocity,other.angular_velocity) &&
            detail::handoff_array_same_bits(rotation,other.rotation);
    }
};
struct RuntimeControllerSuppression {
    std::uint64_t takeover_tick=0;
    RuntimePhysicalActorFollowReference follow;
    RuntimeControllerSupportState support;
    RuntimeControllerProxyState proxy;
};
struct RuntimeControllerHandoffState {
    std::string actor;
    // Absent while locomotion owns the controller. Present only after the
    // physical owner takes over; this contract provides no return to locomotion.
    std::optional<RuntimeControllerSuppression> passive;
};
} // namespace poima
