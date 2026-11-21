// SPDX-License-Identifier: Apache-2.0
#include "poima/shallow_water.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace poima::water {
namespace {

constexpr double max_height_m = 1'000'000;
constexpr double max_exchange_rate = 1'000'000;
// With four neighbors and at most 0.2 of the depth sent to each, a donor
// retains at least 20% before receiving any flow. No dry-cell clamp is needed.
constexpr double max_relaxation_rate = 12;
constexpr std::size_t no_exchange = std::numeric_limits<std::size_t>::max();

bool bounded(double value, double minimum, double maximum) {
    return value >= minimum && value <= maximum;
}

std::size_t checked_cell_count(const GridConfig& config) {
    if (config.cells_x == 0 || config.cells_x > max_grid_side ||
        config.cells_z == 0 || config.cells_z > max_grid_side ||
        !bounded(config.cell_size_m, .01, 256) ||
        !bounded(config.origin_x_m, -max_height_m, max_height_m) ||
        !bounded(config.origin_z_m, -max_height_m, max_height_m) ||
        !bounded(config.relaxation_per_second, 0, max_relaxation_rate))
        throw std::invalid_argument("Invalid shallow-water grid dimensions or configuration.");
    return static_cast<std::size_t>(config.cells_x) * config.cells_z;
}

void validate_face(FaceControl control) {
    if (!bounded(control.openness, 0, 1) ||
        (control.sill_height_m && !bounded(*control.sill_height_m, -max_height_m, max_height_m)))
        throw std::invalid_argument("Invalid shallow-water face control.");
}

class Sum {
public:
    void add(double value) noexcept {
        const double corrected = value - error_;
        const double total = total_ + corrected;
        error_ = (total - total_) - corrected;
        total_ = total;
    }
    double value() const noexcept { return total_; }
private:
    double total_ = 0;
    double error_ = 0;
};

}

Grid::Grid(const GridConfig& config, std::span<const double> floor_heights_m,
           std::span<const double> initial_depths_m)
    : config_(config), cell_area_m2_(config.cell_size_m * config.cell_size_m) {
    const auto cells = checked_cell_count(config);
    if (floor_heights_m.size() != cells || (!initial_depths_m.empty() && initial_depths_m.size() != cells))
        throw std::invalid_argument("Shallow-water heights and depths must match the cell count.");
    for (const double floor : floor_heights_m)
        if (!bounded(floor, -max_height_m, max_height_m))
            throw std::invalid_argument("Shallow-water floor height is out of bounds.");
    for (const double depth : initial_depths_m)
        if (!bounded(depth, 0, max_height_m))
            throw std::invalid_argument("Shallow-water initial depth is out of bounds.");

    floor_heights_.assign(floor_heights_m.begin(), floor_heights_m.end());
    if (initial_depths_m.empty())
        depths_.resize(cells);
    else
        depths_.assign(initial_depths_m.begin(), initial_depths_m.end());
    x_faces_.resize(static_cast<std::size_t>(config.cells_x - 1) * config.cells_z);
    z_faces_.resize(static_cast<std::size_t>(config.cells_z - 1) * config.cells_x);
    staged_depths_.resize(cells);
    transfer_deltas_.resize(cells);
    exchange_indices_.resize(cells);
}

std::size_t Grid::cell_index(Cell cell) const {
    if (cell.x >= config_.cells_x || cell.z >= config_.cells_z)
        throw std::out_of_range("Shallow-water cell is outside the grid.");
    return static_cast<std::size_t>(cell.z) * config_.cells_x + cell.x;
}

std::size_t Grid::face_index(Cell lower_cell, FaceAxis axis) const {
    const auto cell = cell_index(lower_cell);
    switch (axis) {
    case FaceAxis::x:
        if (lower_cell.x < config_.cells_x - 1)
            return cell - lower_cell.z;
        break;
    case FaceAxis::z:
        if (lower_cell.z < config_.cells_z - 1)
            return cell;
        break;
    }
    throw std::out_of_range("Shallow-water face must connect two adjacent cells.");
}

double Grid::volume_m3() const noexcept {
    Sum total;
    for (const double depth : depths_)
        total.add(depth);
    return total.value() * cell_area_m2_;
}

CellSample Grid::sample(Cell cell) const {
    const auto index = cell_index(cell);
    const double floor = floor_heights_[index];
    const double depth = depths_[index];
    return {cell, floor, depth, floor + depth, depth * cell_area_m2_};
}

std::optional<CellSample> Grid::sample_world(double x_m, double z_m) const {
    const double size = config_.cell_size_m;
    const double end_x = config_.origin_x_m + config_.cells_x * size;
    const double end_z = config_.origin_z_m + config_.cells_z * size;
    if (!(x_m >= config_.origin_x_m && x_m < end_x && z_m >= config_.origin_z_m && z_m < end_z))
        return std::nullopt;
    const auto containing_index = [size](double coordinate, double origin, std::uint32_t count) {
        auto index = std::min(static_cast<std::uint32_t>((coordinate - origin) / size), count - 1);
        // Subtracting an offset origin can round the quotient across a seam.
        // Compare with the world coordinates of the neighboring cell edges.
        if (index + 1 < count && coordinate >= origin + (index + 1) * size)
            ++index;
        else if (index > 0 && coordinate < origin + index * size)
            --index;
        return index;
    };
    return sample({containing_index(x_m, config_.origin_x_m, config_.cells_x),
                   containing_index(z_m, config_.origin_z_m, config_.cells_z)});
}

FaceControl Grid::face(Cell lower_cell, FaceAxis axis) const {
    const auto index = face_index(lower_cell, axis);
    return (axis == FaceAxis::x ? x_faces_ : z_faces_)[index];
}

void Grid::set_face(Cell lower_cell, FaceAxis axis, FaceControl control) {
    validate_face(control);
    const auto index = face_index(lower_cell, axis);
    (axis == FaceAxis::x ? x_faces_ : z_faces_)[index] = control;
}

StepReport Grid::step(std::span<const CellExchange> exchanges,
                      std::span<const Cell> rain_cells, double rain_depth_m) {
    if (tick_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Shallow-water tick limit reached.");
    if (exchanges.size() > depths_.size())
        throw std::invalid_argument("Too many shallow-water exchanges.");

    if (!bounded(rain_depth_m, 0, max_height_m) || (rain_depth_m > 0 && rain_cells.empty()))
        throw std::invalid_argument("Invalid shallow-water rain depth or coverage.");
    std::size_t previous_rain = no_exchange;
    for (const Cell cell : rain_cells) {
        const auto index = cell_index(cell);
        if (previous_rain != no_exchange && previous_rain >= index)
            throw std::invalid_argument("Shallow-water rain cells must be unique and ordered.");
        previous_rain = index;
    }

    std::fill(exchange_indices_.begin(), exchange_indices_.end(), no_exchange);
    for (std::size_t i = 0; i < exchanges.size(); ++i) {
        const auto& exchange = exchanges[i];
        const auto index = cell_index(exchange.cell);
        if (exchange_indices_[index] != no_exchange ||
            !bounded(exchange.source_m3_per_second, 0, max_exchange_rate) ||
            !bounded(exchange.drain_m3_per_second, 0, max_exchange_rate))
            throw std::invalid_argument("Invalid or duplicate shallow-water exchange.");
        exchange_indices_[index] = i;
    }

    Sum before;
    Sum supplied;
    Sum drained;
    const double depth_per_rate = step_seconds / cell_area_m2_;
    auto rain = rain_cells.begin();
    for (std::size_t i = 0; i < depths_.size(); ++i) {
        double depth = depths_[i];
        before.add(depth);
        if (rain != rain_cells.end() && i == static_cast<std::size_t>(rain->z) * config_.cells_x + rain->x) {
            depth += rain_depth_m;
            if (!bounded(depth, 0, max_height_m))
                throw std::overflow_error("Shallow-water rain exceeds the supported depth.");
            ++rain;
        }
        staged_depths_[i] = depth;
        const auto input = exchange_indices_[i];
        if (input == no_exchange)
            continue;
        const auto& exchange = exchanges[input];
        // Passing the coincident supply through the drain without storing it
        // avoids a large add/subtract pair that could erase a shallow depth.
        const double pass_through_rate = std::min(exchange.source_m3_per_second, exchange.drain_m3_per_second);
        const double after_drain = depth - std::min(depth, (exchange.drain_m3_per_second - pass_through_rate) * depth_per_rate);
        const double after_exchange = after_drain + (exchange.source_m3_per_second - pass_through_rate) * depth_per_rate;
        if (!bounded(after_exchange, 0, max_height_m))
            throw std::overflow_error("Shallow-water exchange exceeds the supported depth.");
        const double pass_through_depth = pass_through_rate * depth_per_rate;
        supplied.add(pass_through_depth);
        supplied.add(after_exchange - after_drain);
        drained.add(pass_through_depth);
        drained.add(depth - after_drain);
        staged_depths_[i] = after_exchange;
    }

    std::fill(transfer_deltas_.begin(), transfer_deltas_.end(), 0);
    const double fraction = config_.relaxation_per_second * step_seconds;
    const auto transfer = [&](std::size_t lower, std::size_t upper, const FaceControl& control) {
        if (control.openness == 0)
            return;
        double sill = std::max(floor_heights_[lower], floor_heights_[upper]);
        if (control.sill_height_m)
            sill = std::max(sill, *control.sill_height_m);
        const double lower_head = std::max(0.0, staged_depths_[lower] - (sill - floor_heights_[lower]));
        const double upper_head = std::max(0.0, staged_depths_[upper] - (sill - floor_heights_[upper]));
        const double flow = (lower_head - upper_head) * fraction * control.openness;
        transfer_deltas_[lower] -= flow;
        transfer_deltas_[upper] += flow;
    };
    for (std::uint32_t z = 0; z < config_.cells_z; ++z) {
        for (std::uint32_t x = 0; x < config_.cells_x; ++x) {
            const auto index = static_cast<std::size_t>(z) * config_.cells_x + x;
            if (x < config_.cells_x - 1)
                transfer(index, index + 1, x_faces_[index - z]);
            if (z < config_.cells_z - 1)
                transfer(index, index + config_.cells_x, z_faces_[index]);
        }
    }

    Sum after;
    for (std::size_t i = 0; i < depths_.size(); ++i) {
        const double depth = staged_depths_[i] + transfer_deltas_[i];
        if (!bounded(depth, 0, max_height_m))
            throw std::overflow_error("Shallow-water transfer exceeds the supported depth.");
        staged_depths_[i] = depth;
        after.add(depth);
    }
    depths_.swap(staged_depths_);
    return {++tick_, before.value() * cell_area_m2_, supplied.value() * cell_area_m2_,
            drained.value() * cell_area_m2_, after.value() * cell_area_m2_,
            rain_depth_m * static_cast<double>(rain_cells.size()) * cell_area_m2_};
}

Snapshot Grid::snapshot() const {
    return {config_, tick_, floor_heights_, depths_, x_faces_, z_faces_};
}

Grid Grid::restore(const Snapshot& snapshot) {
    Grid grid(snapshot.config, snapshot.floor_heights_m, snapshot.depths_m);
    if (snapshot.depths_m.size() != grid.depths_.size() ||
        snapshot.x_faces.size() != grid.x_faces_.size() ||
        snapshot.z_faces.size() != grid.z_faces_.size())
        throw std::invalid_argument("Shallow-water snapshot state is incomplete.");
    for (const auto& control : snapshot.x_faces)
        validate_face(control);
    for (const auto& control : snapshot.z_faces)
        validate_face(control);
    grid.tick_ = snapshot.tick;
    grid.x_faces_ = snapshot.x_faces;
    grid.z_faces_ = snapshot.z_faces;
    return grid;
}

}
