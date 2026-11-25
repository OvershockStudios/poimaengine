// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/water_storage_definition.hpp"

#include <nlohmann/json.hpp>
#include <array>
#include <string_view>

namespace poima::water {

nlohmann::json storage_definition_json(const StorageDefinition& definition);

struct ReservoirSample {
    std::string_view id;
    ReservoirFootprint footprint;
    double floor_height_m, surface_height_m;

    double submersion_at(double height) const noexcept {
        return height >= floor_height_m && height < surface_height_m ? surface_height_m - height : 0.;
    }
};

struct WaterSheetSample {
    std::string_view id;
    std::array<double,3> lip_m;
    std::array<double,2> forward_xz;
    double width_m, length_m, drop_per_m2, vertical_depth_m, launch_speed_m_s;
    std::optional<double> aeration{};
};

struct ImpactFoamSample {
    std::array<double,2> center_xz_m, forward_xz;
    ReservoirFootprint footprint;
    double radius_m, core, fringe;
};

class WaterStorage {
public:
    WaterStorage(const StorageDefinition& definition, const Grid& grid);
    double volume_m3() const noexcept;
    double capacity_m3() const noexcept;
    std::optional<double> transfer_m3(std::string_view port_id) const noexcept;
    std::span<const std::size_t> placed() const noexcept { return placed_; }
    ReservoirSample sample(std::size_t i) const;
    std::optional<ReservoirSample> sample_world(double x_m, double z_m) const;
    std::vector<WaterSheetSample> sheets(const Grid& grid) const;
    bool has_impact_foam() const noexcept { return !foam_ports_.empty(); }
    std::vector<ImpactFoamSample> foam() const;

    struct Step {
        std::array<double, max_water_reservoirs> volumes{};
        std::array<double, max_water_ports> openness{}, transfers{};
        std::array<CellVolumeChange, max_water_ports> cell_changes{};
        std::size_t cell_change_count = 0;
    };
    Step stage(const Grid& grid, std::span<const std::string_view> pressed,
               std::span<const double> occupation_m) const;
    void commit(const Step& next,const Grid& grid) noexcept;
    nlohmann::json inspect(const Grid* grid=nullptr) const;
    nlohmann::json save() const;
    void restore(const nlohmann::json& state, std::uint64_t tick,const Grid& grid);

private:
    struct Port {
        StoragePortDefinition definition;
        std::size_t from, to;
    };
    std::vector<ReservoirDefinition> reservoirs_;
    std::vector<std::size_t> placed_;
    std::vector<Cell> cells_;
    std::vector<Port> ports_;
    std::vector<std::size_t> sheet_ports_;
    struct Foam {
        std::size_t port;
        ReservoirFootprint footprint;
        double decay, spread, core=0, fringe=0;
    };
    std::vector<Foam> foam_ports_;
    std::array<double,2> receiving_levels(const Port& port,const Grid& grid,
        const std::array<double,max_water_reservoirs>& volumes) const noexcept;
    void apply_transfer(const Step& next) noexcept;
    std::optional<WaterSheetSample> sheet_sample(const Grid& grid,std::size_t port) const;
    std::array<double, max_water_reservoirs> volumes_{};
    std::array<double, max_water_ports> openness_{}, transfers_{};
};

}
