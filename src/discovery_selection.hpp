// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <nlohmann/json.hpp>

namespace poima::discovery_detail {

inline constexpr std::size_t selection_limit=12;
inline constexpr std::size_t selection_name_bytes=128;
inline constexpr std::size_t selection_result_bytes=24*1024;
inline constexpr std::array named_selection_fields{"methods","components","sections"};

inline nlohmann::json selection_parameters_schema() {
    using nlohmann::json;
    const json name={{"type","string"},{"minLength",1},{"maxLength",selection_name_bytes}};
    const json names={{"type","array"},{"minItems",1},{"maxItems",selection_limit},{"uniqueItems",true},{"items",name}};
    const json mutation={{"type","object"},{"additionalProperties",false},{"required",{"operation"}},
        {"properties",{{"operation",name},{"type",name}}}};
    return {{"type","object"},{"additionalProperties",false},{"minProperties",1},
        {"description","Select 1..12 total focused schemas from this owner; names have a 128-byte UTF-8 limit. "
            "Transaction operation selectors narrow the existing guarded world.transact schema. "
            "The result is bounded to 24 KiB; request a smaller or more focused selection if needed."},
        {"properties",{{"methods",names},{"components",names},{"sections",names},
            {"mutations",{{"type","array"},{"minItems",1},{"maxItems",selection_limit},{"uniqueItems",true},{"items",mutation}}}}}};
}

}
