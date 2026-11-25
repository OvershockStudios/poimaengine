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

template<class Visit>
void for_each_face(const GridConfig& config,Visit visit) {
    for (std::uint32_t z=0;z<config.cells_z;++z) {
        for (std::uint32_t x=0;x<config.cells_x;++x) {
            const auto index=static_cast<std::size_t>(z)*config.cells_x+x;
            if (x+1<config.cells_x) visit(index,index+1,FaceAxis::x,index-z);
            if (z+1<config.cells_z) visit(index,index+config.cells_x,FaceAxis::z,index);
        }
    }
}

double transferred_depth(double lower_floor,double upper_floor,double lower_depth,double upper_depth,
                         const FaceControl& control,double fraction) {
    if (control.openness==0) return 0;
    double sill=std::max(lower_floor,upper_floor);
    if (control.sill_height_m) sill=std::max(sill,*control.sill_height_m);
    const auto lower_head=std::max(0.,lower_depth-(sill-lower_floor));
    const auto upper_head=std::max(0.,upper_depth-(sill-upper_floor));
    return (lower_head-upper_head)*fraction*control.openness;
}

double body_stage_rise(double depth,double occupation) {
    // At 12 Hz relaxation the four-face explicit step permits at most
    // 1.25 times the original stage-to-depth slope.
    return std::min(.25*depth,occupation);
}

double pressure_transfer(double lower_floor,double upper_floor,double lower_depth,double upper_depth,
                         const FaceControl& face,double fraction,std::span<const double> occupation,
                         std::size_t lower,std::size_t upper) {
    if (occupation.empty())
        return transferred_depth(lower_floor,upper_floor,lower_depth,upper_depth,face,fraction);
    // Body occupancy raises the stage and the available head. Bound each
    // outgoing face by its share of the donor's actual water.
    lower_depth=std::max(0.,lower_depth);
    upper_depth=std::max(0.,upper_depth);
    const auto flow=transferred_depth(lower_floor,upper_floor,lower_depth+body_stage_rise(lower_depth,occupation[lower]),
        upper_depth+body_stage_rise(upper_depth,occupation[upper]),face,fraction);
    return std::clamp(flow,-upper_depth*fraction*face.openness,lower_depth*fraction*face.openness);
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
           std::span<const double> initial_depths_m,bool track_transfers)
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
    if (track_transfers) {
        last_transfers_=FaceTransfers{std::vector<double>(x_faces_.size()),std::vector<double>(z_faces_.size())};
        staged_transfers_=last_transfers_;
    }
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
    const auto surface=occupied_depths_.empty() ? floor+depth : floor+(depth+body_stage_rise(depth,occupied_depths_[index]));
    return {cell, floor, depth, surface, depth * cell_area_m2_};
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

double Grid::face_discharge_m3_s(Cell lower_cell,FaceAxis axis) const {
    const auto index=face_index(lower_cell,axis);
    if (!last_transfers_) return 0;
    const auto& transfers=axis==FaceAxis::x ? last_transfers_->x_m : last_transfers_->z_m;
    return transfers[index]*(cell_area_m2_/step_seconds);
}

double Grid::occupied_depth_m(Cell cell) const {
    const auto index=cell_index(cell);
    return occupied_depths_.empty() ? 0. : occupied_depths_[index];
}

StepReport Grid::step(std::span<const CellExchange> exchanges,
                      std::span<const Cell> rain_cells, double rain_depth_m,std::span<const double> occupied_depths_m) {
    if (tick_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Shallow-water tick limit reached.");
    if (exchanges.size() > depths_.size())
        throw std::invalid_argument("Too many shallow-water exchanges.");
    if (!occupied_depths_m.empty() && (occupied_depths_m.size()!=depths_.size() ||
            !std::ranges::all_of(occupied_depths_m,[](double depth){return bounded(depth,0,max_height_m); })))
        throw std::invalid_argument("Invalid shallow-water occupation field.");
    staged_occupied_depths_.assign(occupied_depths_m.begin(),occupied_depths_m.end());

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
    for_each_face(config_,[&](std::size_t lower,std::size_t upper,FaceAxis axis,std::size_t index) {
        const auto& control=(axis==FaceAxis::x ? x_faces_ : z_faces_)[index];
        const double flow=pressure_transfer(floor_heights_[lower],floor_heights_[upper],
            staged_depths_[lower],staged_depths_[upper],control,fraction,occupied_depths_m,lower,upper);
        if (staged_transfers_)
            (axis==FaceAxis::x ? staged_transfers_->x_m : staged_transfers_->z_m)[index]=flow;
        transfer_deltas_[lower] -= flow;
        transfer_deltas_[upper] += flow;
    });

    Sum after;
    for (std::size_t i = 0; i < depths_.size(); ++i) {
        const double depth = staged_depths_[i] + transfer_deltas_[i];
        if (!bounded(depth, 0, max_height_m))
            throw std::overflow_error("Shallow-water transfer exceeds the supported depth.");
        staged_depths_[i] = depth;
        after.add(depth);
    }
    depths_.swap(staged_depths_);
    occupied_depths_.swap(staged_occupied_depths_);
    last_transfers_.swap(staged_transfers_);
    return {++tick_, before.value() * cell_area_m2_, supplied.value() * cell_area_m2_,
            drained.value() * cell_area_m2_, after.value() * cell_area_m2_,
            rain_depth_m * static_cast<double>(rain_cells.size()) * cell_area_m2_};
}

Snapshot Grid::snapshot() const {
    return {config_, tick_, floor_heights_, depths_, x_faces_, z_faces_, last_transfers_, occupied_depths_};
}

Grid Grid::restore(const Snapshot& snapshot) {
    Grid grid(snapshot.config, snapshot.floor_heights_m, snapshot.depths_m, snapshot.last_transfers.has_value());
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
    if (!snapshot.occupied_depths_m.empty()) {
        const auto bound=grid.tick_ ? max_height_m : 0.;
        if (snapshot.occupied_depths_m.size()!=grid.depths_.size() ||
            !std::ranges::all_of(snapshot.occupied_depths_m,[&](double value){return bounded(value,0,bound);}))
            throw std::invalid_argument("Shallow-water occupation state is incomplete or out of bounds.");
        grid.occupied_depths_=snapshot.occupied_depths_m;
    }
    // The generic grid permits changing controls between ticks. At this layer
    // the completed transfers can belong to the preceding control state.
    if (snapshot.last_transfers) {
        const auto& transfers=*snapshot.last_transfers;
        const auto valid=[&](double value) {
            const auto bound=grid.tick_ ? max_height_m*(grid.config_.relaxation_per_second*step_seconds) : 0.;
            return bounded(value,-bound,bound);
        };
        if (transfers.x_m.size()!=grid.x_faces_.size() || transfers.z_m.size()!=grid.z_faces_.size() ||
            !std::ranges::all_of(transfers.x_m,valid) || !std::ranges::all_of(transfers.z_m,valid))
            throw std::invalid_argument("Shallow-water transfer state is incomplete or out of bounds.");
        grid.last_transfers_=transfers;
    }
    return grid;
}

void Grid::restore_transfers(FaceTransfers transfers) {
    if (!last_transfers_ || transfers.x_m.size()!=x_faces_.size() || transfers.z_m.size()!=z_faces_.size())
        throw std::invalid_argument("Saved water transfers differ from the grid.");
    const auto fraction=config_.relaxation_per_second*step_seconds;
    const auto max_transfer=tick_ ? max_height_m*fraction : 0.;
    auto previous=depths_;
    for_each_face(config_,[&](std::size_t lower,std::size_t upper,FaceAxis axis,std::size_t index) {
        const auto flow=(axis==FaceAxis::x ? transfers.x_m : transfers.z_m)[index];
        const auto& control=(axis==FaceAxis::x ? x_faces_ : z_faces_)[index];
        if (!bounded(flow,-max_transfer,max_transfer) || (control.openness==0 && flow!=0))
            throw std::invalid_argument("Saved water transfer exceeds its face or tick bounds.");
        previous[lower]+=flow;
        previous[upper]-=flow;
    });
    if (tick_>0) for_each_face(config_,[&](std::size_t lower,std::size_t upper,FaceAxis axis,std::size_t index) {
        const auto flow=(axis==FaceAxis::x ? transfers.x_m : transfers.z_m)[index];
        const auto& control=(axis==FaceAxis::x ? x_faces_ : z_faces_)[index];
        const auto tolerance=32*std::numeric_limits<double>::epsilon()*std::max({
            1.,std::abs(previous[lower]),std::abs(previous[upper]),depths_[lower],depths_[upper]});
        if (!bounded(previous[lower],-tolerance,max_height_m+tolerance) ||
            !bounded(previous[upper],-tolerance,max_height_m+tolerance) ||
            std::abs(flow-pressure_transfer(floor_heights_[lower],floor_heights_[upper],
                previous[lower],previous[upper],control,fraction,occupied_depths_,lower,upper))>tolerance)
            throw std::invalid_argument("Saved water transfers do not match the regional flow step.");
    });
    last_transfers_=std::move(transfers);
}

}
