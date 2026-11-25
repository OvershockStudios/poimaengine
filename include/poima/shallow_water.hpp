// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace poima::water {

inline constexpr std::uint32_t max_grid_side = 256;
inline constexpr double step_seconds = 1.0 / 60.0;

struct Cell {
    std::uint32_t x = 0;
    std::uint32_t z = 0;
};

struct GridConfig {
    std::uint32_t cells_x = 0;
    std::uint32_t cells_z = 0;
    double cell_size_m = 1;
    double origin_x_m = 0;
    double origin_z_m = 0;
    double relaxation_per_second = 6;
};

// Each interior face has one owner at the lower X or Z coordinate.
enum class FaceAxis { x, z };

struct FaceControl {
    double openness = 1;
    std::optional<double> sill_height_m;
};

struct CellExchange {
    Cell cell;
    double source_m3_per_second = 0;
    double drain_m3_per_second = 0;
};

struct CellSample {
    Cell cell;
    double floor_height_m;
    double depth_m;
    double surface_height_m;
    double volume_m3;

    // The surface itself is air. Points beneath the terrain bed are outside the water.
    double submersion_at(double y_m) const noexcept {
        return y_m >= floor_height_m && y_m < surface_height_m ? surface_height_m - y_m : 0;
    }
};

struct StepReport {
    std::uint64_t tick;
    double volume_before_m3;
    double supplied_m3;
    double drained_m3;
    double volume_after_m3;
    double rain_m3 = 0;
};

struct Snapshot {
    GridConfig config;
    std::uint64_t tick = 0;
    std::vector<double> floor_heights_m;
    std::vector<double> depths_m;
    std::vector<FaceControl> x_faces;
    std::vector<FaceControl> z_faces;
};

// One owner advances this localized, closed grid. Cells are row-major in X
// then Z. Exterior water transfer is explicit through the step's exchanges.
class Grid {
public:
    Grid(const GridConfig& config, std::span<const double> floor_heights_m,
         std::span<const double> initial_depths_m = {});

    const GridConfig& config() const noexcept { return config_; }
    std::uint64_t tick() const noexcept { return tick_; }
    double volume_m3() const noexcept;
    CellSample sample(Cell cell) const;
    // Finds the cell containing a world-space point in the half-open X/Z footprint.
    std::optional<CellSample> sample_world(double x_m, double z_m) const;

    FaceControl face(Cell lower_cell, FaceAxis axis) const;
    void set_face(Cell lower_cell, FaceAxis axis, FaceControl control);

    // Apply rain to unique cells in row-major order, process the authored
    // exchanges, then move water across interior faces. Rejected steps leave
    // the whole observable state intact.
    StepReport step(std::span<const CellExchange> exchanges = {},
                    std::span<const Cell> rain_cells = {}, double rain_depth_m = 0);

    Snapshot snapshot() const;
    static Grid restore(const Snapshot& snapshot);

private:
    std::size_t cell_index(Cell cell) const;
    std::size_t face_index(Cell lower_cell, FaceAxis axis) const;

    GridConfig config_;
    double cell_area_m2_;
    std::uint64_t tick_ = 0;
    std::vector<double> floor_heights_;
    std::vector<double> depths_;
    std::vector<FaceControl> x_faces_;
    std::vector<FaceControl> z_faces_;
    std::vector<double> staged_depths_;
    std::vector<double> transfer_deltas_;
    std::vector<std::size_t> exchange_indices_;
};

}
