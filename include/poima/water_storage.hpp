// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/water_storage_definition.hpp"

#include <nlohmann/json.hpp>
#include <array>
#include <string_view>

namespace poima::water {

nlohmann::json storage_definition_json(const StorageDefinition& definition);

class WaterStorage {
public:
    WaterStorage(const StorageDefinition& definition, const Grid& grid);
    double volume_m3() const noexcept;
    double capacity_m3() const noexcept;

    struct Step {
        std::array<double, max_water_reservoirs> volumes{};
        std::array<double, max_water_ports> openness{}, transfers{};
        std::array<CellVolumeChange, max_water_ports> cell_changes{};
        std::size_t cell_change_count = 0;
    };
    Step stage(const Grid& grid, std::span<const std::string_view> pressed,
               std::span<const double> occupation_m) const;
    void commit(const Step& next) noexcept;
    nlohmann::json inspect() const;
    nlohmann::json save() const;
    void restore(const nlohmann::json& state, std::uint64_t tick);

private:
    struct Port {
        StoragePortDefinition definition;
        std::size_t from, to;
    };
    std::vector<ReservoirDefinition> reservoirs_;
    std::vector<Cell> cells_;
    std::vector<Port> ports_;
    std::array<double, max_water_reservoirs> volumes_{};
    std::array<double, max_water_ports> openness_{}, transfers_{};
};

}
