// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/water_region_definition.hpp"
#include "poima/water_bed.hpp"

#include <nlohmann/json.hpp>
#include <string_view>

namespace poima::water {

nlohmann::json surface_ripples_json(const SurfaceRipples& ripples);
nlohmann::json surface_wave_layers_json(std::span<const SurfaceRipples> layers);
nlohmann::json listener_audio_json(const ListenerAudio& profile);
nlohmann::json optics_json(const WaterOptics& profile);

// Advances one frozen authored region at the runtime's fixed tick rate.
class Region {
public:
    explicit Region(const RegionDefinition& definition);

    const Grid& grid() const noexcept { return grid_; }
    std::span<const RegionFace> face_rules() const noexcept { return faces_; }
    const std::string& plan_sha256() const noexcept { return plan_sha256_; }
    const std::optional<StepReport>& last_report() const noexcept { return last_report_; }
    std::span<const Cell> rain_cells() const noexcept { return rain_cells_; }
    const std::optional<SurfaceRipples>& surface_ripples() const noexcept { return surface_ripples_; }
    std::span<const SurfaceRipples> surface_wave_layers() const noexcept { return surface_wave_layers_; }
    const std::optional<ListenerAudio>& listener_audio() const noexcept { return listener_audio_; }
    const std::optional<WaterOptics>& optics() const noexcept { return optics_; }
    FittedWaterBed fitted_bed(Cell cell) const;
    std::optional<CellSample> sample_world(double x_m,double z_m) const;
    std::uint64_t cumulative_rain_nm() const noexcept { return cumulative_rain_nm_; }
    double cumulative_rain_m3() const noexcept;

    // An action toggles each assigned face once, even if it appears more
    // than once within the pressed edges for this tick.
    StepReport step(std::span<const std::string_view> pressed = {},
                    std::uint32_t rain_depth_nm = 0,
                    std::optional<std::uint64_t> first_quantum = {});
    nlohmann::json save() const;
    void restore(const nlohmann::json& saved, std::uint64_t expected_tick,
                 std::optional<std::uint64_t> next_quantum = {});

private:
    Grid grid_;
    std::vector<double> initial_depths_m_;
    std::vector<RegionFace> faces_;
    std::vector<CellExchange> exchanges_;
    std::string plan_sha256_;
    std::optional<StepReport> last_report_;
    std::vector<Cell> rain_cells_;
    std::optional<SurfaceRipples> surface_ripples_;
    std::vector<SurfaceRipples> surface_wave_layers_;
    std::optional<ListenerAudio> listener_audio_;
    std::optional<WaterOptics> optics_;
    std::vector<double> shore_corner_heights_m_;
    std::uint64_t cumulative_rain_nm_ = 0;
    std::uint32_t last_rain_nm_ = 0;
    std::optional<std::uint64_t> first_rain_quantum_;
    double rain_volume_m3(std::uint64_t depth_nm) const noexcept;
};

}
