// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/scene.hpp"
#include <nlohmann/json.hpp>

namespace poima {
inline nlohmann::json solar_lighting_json(const SolarLightingSample& sample) {
    return {{"version",1},{"world_milliseconds",sample.world_milliseconds},{"toward_sun",sample.toward_sun},
        {"sun_gain",sample.sun_gain},{"sun_tint",sample.sun_tint},{"sky_gain",sample.sky_gain},{"ambient",sample.ambient}};
}
}
