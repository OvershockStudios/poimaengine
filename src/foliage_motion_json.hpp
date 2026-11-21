// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/foliage_motion.hpp"
#include "world_json.hpp"

namespace poima::world_detail {
FoliageMotion foliage_motion_value(const Json& value);
}
