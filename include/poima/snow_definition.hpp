// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace poima::snow {

struct Definition {
    std::array<std::string, 2> tiles; // West, then east, from a single cooked field.
    std::uint32_t thickness_units = 0;
};

} // namespace poima::snow
