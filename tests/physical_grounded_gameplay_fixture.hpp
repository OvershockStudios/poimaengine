// SPDX-License-Identifier: Apache-2.0
// C++ test-module metadata only. No managed or Native AOT compiler claim.
#pragma once
#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
namespace grounded_test {
struct CallbackState {
    std::int64_t calls=0,mode=0,checks=0,writes=0,passive_reads=0;
    double expected_yaw=0,endpoint_gap=0,requested_weight=0;
};
inline nlohmann::json schema(){
    using J=nlohmann::json;J fields=J::array();
    auto add=[&](const char* name,const char* kind,std::size_t offset){fields.push_back({{"name",name},{"kind",kind},{"offset",offset},{"bytes",8}});};
    add("calls","int64",offsetof(CallbackState,calls));add("mode","int64",offsetof(CallbackState,mode));
    add("checks","int64",offsetof(CallbackState,checks));add("writes","int64",offsetof(CallbackState,writes));
    add("passive_reads","int64",offsetof(CallbackState,passive_reads));add("expected_yaw","float64",offsetof(CallbackState,expected_yaw));
    add("endpoint_gap","float64",offsetof(CallbackState,endpoint_gap));add("requested_weight","float64",offsetof(CallbackState,requested_weight));
    return {{"identity","Poima.Tests.CppGroundedPhysicalCallback"},{"bytes",sizeof(CallbackState)},{"fields",fields}};
}
inline nlohmann::json requirements(){
    return {{"call_version",1},{"call_bytes",80},{"services_version",7},{"services_bytes",424},
        {"features",{"baseline_v7","animation_ik_v1","animation_ik_end_orientation_v1","animation_grounding_control_v1","physical_rig_v1"}}};
}
}
