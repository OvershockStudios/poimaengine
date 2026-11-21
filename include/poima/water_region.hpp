// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/water_region_definition.hpp"

#include <nlohmann/json.hpp>
#include <string_view>

namespace poima::water {

// Advances one frozen authored region at the runtime's fixed tick rate.
class Region {
public:
    explicit Region(const RegionDefinition& definition);

    const Grid& grid() const noexcept { return grid_; }
    std::span<const RegionFace> face_rules() const noexcept { return faces_; }
    const std::string& plan_sha256() const noexcept { return plan_sha256_; }
    const std::optional<StepReport>& last_report() const noexcept { return last_report_; }

    // An action toggles each assigned face once, even if it appears more
    // than once within the pressed edges for this tick.
    StepReport step(std::span<const std::string_view> pressed = {});
    nlohmann::json save() const;
    void restore(const nlohmann::json& saved, std::uint64_t expected_tick);

private:
    Grid grid_;
    std::vector<double> initial_depths_m_;
    std::vector<RegionFace> faces_;
    std::vector<CellExchange> exchanges_;
    std::string plan_sha256_;
    std::optional<StepReport> last_report_;
};

}
