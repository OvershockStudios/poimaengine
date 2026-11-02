// SPDX-License-Identifier: Apache-2.0
// Shared ONLY by the native test executable and its compiled C++ ABI fixture.
// This fixture is not a C# or Native AOT implementation/qualification claim.
#pragma once
#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <string>

namespace physical_ik_test {
struct CallbackState {
    std::int64_t calls=0,writes=0,passive_reads=0,mode=0,staged_same=0;
    double expected_yaw=0,end_gap=0,target_gap=0;
};
inline nlohmann::json schema() {
    using J=nlohmann::json;
    J fields=J::array();
    auto field=[&](const char* name,const char* kind,std::size_t offset) {
        fields.push_back({{"name",name},{"kind",kind},{"offset",offset},{"bytes",8}});
    };
    field("calls","int64",offsetof(CallbackState,calls));
    field("writes","int64",offsetof(CallbackState,writes));
    field("passive_reads","int64",offsetof(CallbackState,passive_reads));
    field("mode","int64",offsetof(CallbackState,mode));
    field("staged_same","int64",offsetof(CallbackState,staged_same));
    field("expected_yaw","float64",offsetof(CallbackState,expected_yaw));
    field("end_gap","float64",offsetof(CallbackState,end_gap));
    field("target_gap","float64",offsetof(CallbackState,target_gap));
    return {{"identity","Poima.Tests.CppPhysicalIkCallback"},{"bytes",sizeof(CallbackState)},{"fields",fields}};
}
inline nlohmann::json requirements() {
    return {{"call_version",1},{"call_bytes",80},{"services_version",7},{"services_bytes",424},
        {"features",{"baseline_v7","animation_ik_v1","physical_rig_v1"}}};
}
}
