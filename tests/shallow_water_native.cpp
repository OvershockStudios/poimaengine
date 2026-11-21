// SPDX-License-Identifier: Apache-2.0
#include "poima/shallow_water.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using poima::water::Cell;
using poima::water::CellExchange;
using poima::water::FaceAxis;
using poima::water::FaceControl;
using poima::water::Grid;
using poima::water::GridConfig;
using poima::water::Snapshot;
using poima::water::StepReport;

namespace {

void require(bool condition, const char* failure) {
    if (!condition)
        throw std::runtime_error(failure);
}

void near(double actual, double wanted, double tolerance, const char* failure) {
    require(std::abs(actual - wanted) <= tolerance, failure);
}

void balanced(const StepReport& report) {
    const double expected = report.volume_before_m3 + report.supplied_m3 - report.drained_m3;
    const double scale = std::max({1.0, report.volume_before_m3, report.supplied_m3, report.drained_m3});
    near(report.volume_after_m3, expected, 2e-14 * scale, "Step volume ledger differs.");
}

template<class Exception, class Function>
void rejects(Function operation, const char* failure) {
    try {
        operation();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(failure);
}

void same_face(const FaceControl& left, const FaceControl& right) {
    require(left.openness == right.openness && left.sill_height_m == right.sill_height_m,
            "Face state differs.");
}

void same_state(const Snapshot& left, const Snapshot& right) {
    const auto& a = left.config;
    const auto& b = right.config;
    require(a.cells_x == b.cells_x && a.cells_z == b.cells_z && a.cell_size_m == b.cell_size_m &&
            a.origin_x_m == b.origin_x_m && a.origin_z_m == b.origin_z_m &&
            a.relaxation_per_second == b.relaxation_per_second, "Grid config differs.");
    require(left.tick == right.tick && left.floor_heights_m == right.floor_heights_m &&
            left.depths_m == right.depths_m, "Grid tick, floors or depths differ.");
    require(left.x_faces.size() == right.x_faces.size() && left.z_faces.size() == right.z_faces.size(),
            "Grid face counts differ.");
    for (std::size_t index = 0; index < left.x_faces.size(); ++index)
        same_face(left.x_faces[index], right.x_faces[index]);
    for (std::size_t index = 0; index < left.z_faces.size(); ++index)
        same_face(left.z_faces[index], right.z_faces[index]);
}

void gates_redirect_a_flood() {
    const std::array<double, 12> floors{};
    const std::array<double, 12> initial{{1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0}};
    const GridConfig config{.cells_x = 4, .cells_z = 3, .relaxation_per_second = 12};
    Grid grid(config, floors, initial);
    for (std::uint32_t z = 0; z < config.cells_z; ++z)
        grid.set_face({1, z}, FaceAxis::x, {.openness = 0, .sill_height_m = .25});
    for (int step = 0; step < 32; ++step)
        balanced(grid.step());
    near(grid.sample({2, 0}).depth_m, 0, 0, "Water crossed a closed dam.");
    near(grid.sample({1, 0}).depth_m, 1, 0, "Closed dam drained the reservoir.");

    Grid north = Grid::restore(grid.snapshot());
    north.set_face({1, 0}, FaceAxis::x, {.openness = .5, .sill_height_m = .25});
    grid.set_face({1, 2}, FaceAxis::x, {.openness = .5, .sill_height_m = .25});
    balanced(north.step());
    balanced(grid.step());
    near(north.sample({2, 0}).depth_m, .075, 1e-14, "North gate routed the wrong volume.");
    near(grid.sample({2, 2}).depth_m, .075, 1e-14, "South gate routed the wrong volume.");
    near(north.sample({2, 2}).depth_m, 0, 0, "Water bypassed the north gate.");
    near(grid.sample({2, 0}).depth_m, 0, 0, "Water bypassed the south gate.");
    near(grid.sample({3, 2}).depth_m, 0, 0, "Newly delivered water moved twice in one tick.");

    for (int step = 0; step < 480; ++step)
        balanced(grid.step());
    near(grid.volume_m3(), 6, 1e-12, "Closed basin lost water during redistribution.");
    near(grid.sample({3, 0}).depth_m, .5, 2e-4, "The opened basin did not approach its shared water level.");
    near(grid.sample({0, 2}).depth_m, .5, 2e-4, "The reservoir did not approach its shared water level.");
}

void still_water_respects_dry_high_ground() {
    const std::array<double, 6> floors{{1, .5, 2, 1.25, .75, 2}};
    const std::array<double, 6> depths{{.5, 1, 0, .25, .75, 0}};
    Grid grid({.cells_x = 3, .cells_z = 2}, floors, depths);
    grid.set_face({0, 0}, FaceAxis::z, {.openness = 1, .sill_height_m = 1.75});
    for (int step = 0; step < 128; ++step)
        balanced(grid.step());
    auto same = grid.snapshot();
    require(std::equal(depths.begin(), depths.end(), same.depths_m.begin()),
            "A still lake moved or crossed dry high ground.");

    const std::array<double, 2> unequal_floors{{0, .5}};
    const std::array<double, 2> eastern_water{{0, .75}};
    Grid reverse({.cells_x = 2, .cells_z = 1, .relaxation_per_second = 12}, unequal_floors, eastern_water);
    balanced(reverse.step());
    near(reverse.sample({0, 0}).depth_m, .15, 1e-14, "A water face did not permit reverse flow.");

    const std::array<double, 2> low_water{{.25, 0}};
    Grid raised_sill({.cells_x = 2, .cells_z = 1}, unequal_floors, low_water);
    raised_sill.set_face({0, 0}, FaceAxis::x, {.openness = 1, .sill_height_m = -.5});
    balanced(raised_sill.step());
    near(raised_sill.sample({1, 0}).depth_m, 0, 0, "A sill below the floor overrode the bed height.");
}

void source_and_drain_report_actual_water() {
    const std::array<double, 1> floor{{1.25}};
    const GridConfig config{.cells_x = 1, .cells_z = 1, .cell_size_m = .5,
                            .origin_x_m = -1.25, .origin_z_m = 4.5};
    Grid grid(config, floor);
    const std::array<CellExchange, 1> fill_and_drain{{{{0, 0}, 60, 15}}};
    const auto filled = grid.step(fill_and_drain);
    balanced(filled);
    near(filled.supplied_m3, 1, 1e-14, "Supply volume is incorrect.");
    near(filled.drained_m3, .25, 1e-14, "Drain volume is incorrect.");
    near(filled.volume_after_m3, .75, 1e-14, "Net volume is incorrect.");
    const auto observed = grid.sample_world(-1, 4.75);
    require(observed.has_value(), "Interior world point was rejected.");
    near(observed->depth_m, 3, 1e-14, "Area did not scale cell depth.");
    near(observed->surface_height_m, 4.25, 1e-14, "Water surface differs from floor plus depth.");
    near(observed->volume_m3, .75, 1e-14, "Observed cell volume is incorrect.");
    require(!grid.sample_world(-.75, 4.5) && !grid.sample_world(-1.25, 5),
            "The grid footprint includes its upper boundary.");

    const std::array<CellExchange, 1> drain_all{{{{0, 0}, 0, 240}}};
    const auto drained = grid.step(drain_all);
    balanced(drained);
    near(drained.drained_m3, .75, 1e-14, "Drain removed more than the available water.");
    near(drained.volume_after_m3, 0, 0, "An exhausted cell retained water.");

    const std::array<double, 3> three_floors{};
    Grid decimal_cells({.cells_x = 3, .cells_z = 1, .cell_size_m = .3}, three_floors);
    const double decimal_boundary = .3 * 3;
    const auto inside = decimal_cells.sample_world(std::nextafter(decimal_boundary, 0), 0);
    require(!decimal_cells.sample_world(decimal_boundary, 0) &&
            inside && inside->cell.x == 2,
            "A nonbinary cell size changed the world-space outer boundary.");
    Grid offset_cells({.cells_x = 3, .cells_z = 1, .cell_size_m = .1, .origin_x_m = 4}, three_floors);
    const double inner_edge = 4 + .1;
    const auto upstream = offset_cells.sample_world(std::nextafter(inner_edge, 0), 0);
    const auto on_edge = offset_cells.sample_world(inner_edge, 0);
    require(upstream && upstream->cell.x == 0 && on_edge && on_edge->cell.x == 1,
            "A nonbinary offset seam was assigned to the wrong cell.");
}

void checkpoint_and_rejected_steps() {
    const std::array<double, 6> floors{{0, 1, .5, -.5, .25, .1}};
    const std::array<double, 6> depths{{1, 0, .3, 0, .55, .86}};
    Grid grid({.cells_x = 3, .cells_z = 2, .cell_size_m = .75, .relaxation_per_second = 10},
              floors, depths);
    grid.set_face({0, 0}, FaceAxis::z, {.openness = .3, .sill_height_m = .65});
    grid.set_face({1, 1}, FaceAxis::x, {.openness = 0, .sill_height_m = std::nullopt});
    grid.step();
    const auto checkpoint = grid.snapshot();
    const std::array<CellExchange, 2> forward{{{{0, 1}, 15, 0}, {{2, 0}, 0, 6}}};
    const std::array<CellExchange, 2> reversed{{forward[1], forward[0]}};
    auto resumed = Grid::restore(checkpoint);
    for (int step = 0; step < 64; ++step) {
        const auto a = grid.step(forward);
        const auto b = resumed.step(reversed);
        balanced(a);
        require(a.tick == b.tick && a.supplied_m3 == b.supplied_m3 && a.drained_m3 == b.drained_m3 &&
                a.volume_before_m3 == b.volume_before_m3 && a.volume_after_m3 == b.volume_after_m3,
                "Replay changed the accounting receipt.");
    }
    same_state(grid.snapshot(), resumed.snapshot());
    const auto valid = grid.snapshot();
    const std::array<CellExchange, 2> duplicate{{forward[0], forward[0]}};
    rejects<std::invalid_argument>([&] { grid.step(duplicate); }, "Duplicate exchanges were accepted.");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::array<CellExchange, 1> bad_rate{{{{0, 0}, nan, 0}}};
    rejects<std::invalid_argument>([&] { grid.step(bad_rate); }, "A nonfinite rate was accepted.");
    rejects<std::out_of_range>([&] {
        grid.set_face({2, 0}, FaceAxis::x, {.openness = 0, .sill_height_m = {}});
    }, "An exterior face accepted a gate.");
    rejects<std::invalid_argument>([&] {
        grid.set_face({0, 0}, FaceAxis::z, {.openness = 1, .sill_height_m = nan});
    }, "A nonfinite sill was accepted.");
    same_state(valid, grid.snapshot());

    auto malformed = valid;
    malformed.z_faces.pop_back();
    rejects<std::invalid_argument>([&] { Grid::restore(malformed); }, "Incomplete face state was accepted.");
    malformed = valid;
    malformed.depths_m.clear();
    rejects<std::invalid_argument>([&] { Grid::restore(malformed); }, "Missing snapshot depths were accepted.");
    malformed = valid;
    malformed.depths_m[0] = -.1;
    rejects<std::invalid_argument>([&] { Grid::restore(malformed); }, "Negative snapshot depth was accepted.");

    const std::array<double, 1> tiny_floor{};
    const std::array<double, 1> tiny_depth{{1e-12}};
    Grid tiny({.cells_x = 1, .cells_z = 1, .cell_size_m = .01}, tiny_floor, tiny_depth);
    const auto prior = tiny.snapshot();
    const std::array<CellExchange, 1> too_large{{{{0, 0}, 1'000'000, 0}}};
    rejects<std::overflow_error>([&] { tiny.step(too_large); }, "Unsupported column depth was accepted.");
    same_state(prior, tiny.snapshot());
    const std::array<CellExchange, 1> pass_through{{{{0, 0}, 1'000'000, 1'000'000}}};
    balanced(tiny.step(pass_through));
    near(tiny.sample({0, 0}).depth_m, tiny_depth[0], 0, "Balanced source and drain erased a shallow depth.");
}

void wet_and_dry_slope_conservation() {
    constexpr std::uint32_t width = 12;
    constexpr std::uint32_t height = 10;
    std::array<double, width * height> floors{};
    std::array<double, width * height> depths{};
    for (std::size_t i = 0; i < floors.size(); ++i) {
        floors[i] = static_cast<double>((i * 19) % 31) / 10;
        depths[i] = i % 3 == 0 ? static_cast<double>((i * 13) % 37) / 16 : 0;
    }
    Grid grid({.cells_x = width, .cells_z = height, .cell_size_m = .8, .relaxation_per_second = 12},
              floors, depths);
    const double initial_volume = grid.volume_m3();
    for (int step = 0; step < 1024; ++step) {
        if (step % 64 == 0) {
            const double openness = step % 128 == 0 ? 0.0 : 1.0;
            grid.set_face({7, 4}, FaceAxis::z, {.openness = openness, .sill_height_m = 1.5});
        }
        balanced(grid.step());
    }
    const auto end = grid.snapshot();
    require(std::all_of(end.depths_m.begin(), end.depths_m.end(), [](double depth) { return depth >= 0; }),
            "The closed sloped grid produced a negative depth.");
    near(grid.volume_m3(), initial_volume, initial_volume * 2e-13, "Closed slope lost water over time.");
}

}

int main() {
    try {
        gates_redirect_a_flood();
        still_water_respects_dry_high_ground();
        source_and_drain_report_actual_water();
        checkpoint_and_rejected_steps();
        wet_and_dry_slope_conservation();
        std::cout << "Localized water conserved volume, redirected across gates, and replayed from a complete snapshot.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
