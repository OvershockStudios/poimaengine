// SPDX-License-Identifier: Apache-2.0
#include "poima/foliage.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>

namespace {

using namespace poima::foliage;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Test>
void require_invalid(Test test) {
    try {
        test();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Invalid foliage input was admitted.");
}

void check_counts(const ScatterResult& result) {
    const auto& counts = result.counts;
    require(counts.cells == result.instances.size() + counts.density_rejected + counts.surface_missing +
                            counts.slope_rejected + counts.spacing_rejected,
            "Scatter counts do not explain every cell.");
}

std::optional<SurfaceSample> flat(double, double) {
    return SurfaceSample{2, 1};
}

double xz_distance_squared(const Instance& a, const Instance& b) {
    const double dx = a.position_m[0] - b.position_m[0];
    const double dz = a.position_m[2] - b.position_m[2];
    return dx * dx + dz * dz;
}

void repeatable_instances() {
    const ScatterArea area{0x0123456789abcdefULL, -10, -20, 0.5, 19, 13};
    const ScatterRecipe recipe{2349, 0.75, 0};
    const Species grass{3, 5, 4, 0, 45, 0.75, 1.25};
    const Species fern{9, 2, 3, 0, 45, 0.5, 1};
    const auto height = [](double x, double z) { return 2 + 0.3 * x - 0.4 * z; };
    const auto plane = [&](double x, double z) -> std::optional<SurfaceSample> {
        return SurfaceSample{height(x, z), 1 / std::sqrt(1 + 0.3 * 0.3 + 0.4 * 0.4)};
    };
    const auto result = scatter(area, recipe, std::array{grass, fern}, plane);
    require(result == scatter(area, recipe, std::array{fern, grass}, plane),
            "Species order changed the scatter.");
    require(result == scatter(area, recipe, std::array{grass, fern}, plane),
            "The same recipe did not repeat exactly.");
    require(entity_id({area.id, 4, 7}) == "0123456789abcdef0000000700000004",
            "Authoring ID did not encode the namespace, row and column.");
    require(result.counts.density_rejected > 0 && result.instances.size() > 0,
            "Density filter did not retain and reject candidates.");
    check_counts(result);
    const auto complete = scatter(area, {recipe.seed, 1, 0}, std::array{grass, fern}, plane);
    require(complete.instances.size() == area.columns * area.rows,
            "Full density lost valid candidates.");
    std::set<std::uint64_t> seen_species;
    std::set<std::string> seen_ids;
    std::size_t prior = 0;
    bool first = true;
    for (const auto& instance : result.instances) {
        const auto i = instance.id.row * area.columns + instance.id.column;
        require(first || i > prior, "Output is not in row-major order.");
        prior = i;
        first = false;
        require(instance == complete.instances.at(i),
                "Density altered the retained instance identity or variation.");
        require(instance.position_m[1] == height(instance.position_m[0], instance.position_m[2]),
                "Placement height diverged from the supplied surface.");
        const double low_x = area.origin_x_m + instance.id.column * area.cell_size_m;
        const double low_z = area.origin_z_m + instance.id.row * area.cell_size_m;
        require(instance.position_m[0] > low_x && instance.position_m[0] < low_x + area.cell_size_m &&
                instance.position_m[2] > low_z && instance.position_m[2] < low_z + area.cell_size_m,
                "Jittered placement left its cell.");
        require(seen_ids.insert(entity_id(instance.id)).second, "Duplicate foliage identity.");
        const Species& preset = instance.species == grass.id ? grass : fern;
        require(instance.species == preset.id && instance.variation < preset.variation_count &&
                instance.uniform_scale >= preset.minimum_scale &&
                instance.uniform_scale <= preset.maximum_scale &&
                instance.yaw_radians >= 0 && instance.yaw_radians < 2 * std::numbers::pi,
                "Foliage variation escaped its authored bounds.");
        seen_species.insert(instance.species);
    }
    require(seen_species.size() == 2, "Weighted recipe did not exercise both species.");
    auto reseed = recipe;
    reseed.seed += 1;
    reseed.density = 1;
    const auto rerolled = scatter(area, reseed, std::array{grass, fern}, plane);
    require(rerolled.instances.size() == complete.instances.size(), "Reseeding changed the full lattice size.");
    for (std::size_t i = 0; i < complete.instances.size(); ++i)
        require(rerolled.instances[i].id == complete.instances[i].id, "Reseeding changed the cell identity.");
    require(rerolled.instances != complete.instances, "Reseeding failed to vary placement.");
}

void surface_filters() {
    const ScatterArea area{7, 0, 0, 1, 8, 8};
    const Species species{10, 1, 1, 0, 25};
    const auto surface = [](double x, double z) -> std::optional<SurfaceSample> {
        if (x < 2) return std::nullopt;
        if (x < 4) return SurfaceSample{z, 0.5};
        return SurfaceSample{z, 1};
    };
    const auto result = scatter(area, {}, std::array{species}, surface);
    require(result.counts.surface_missing == 16 && result.counts.slope_rejected == 16 &&
            result.instances.size() == 32, "Surface and slope diagnostics disagreed with independent cell regions.");
    check_counts(result);
    std::size_t queries = 0;
    const auto count_surface = [&](double x, double z) {
        ++queries;
        return flat(x, z);
    };
    const auto empty = scatter(area, {1, 0, 0}, std::array{species}, count_surface);
    require(queries == 0 && empty.instances.empty() && empty.counts.density_rejected == 64,
            "Zero density still queried or accepted a surface.");
    check_counts(empty);

    const Species vertical_only{11, 1, 1, 90, 90};
    const auto vertical = [](double, double) { return SurfaceSample{1, 0}; };
    require(scatter(area, {}, std::array{vertical_only}, vertical).instances.size() == 64,
            "The inclusive 90 degree limit missed a vertical normal.");
    const Species horizontal_only{12, 1, 1, 0, 0};
    require(scatter(area, {}, std::array{horizontal_only}, flat).instances.size() == 64,
            "The inclusive zero degree limit missed a horizontal normal.");
}

void spacing_and_limits() {
    const ScatterArea area{42, -999999.5, 999996, 0.1, 25, 26};
    const std::array species{Species{1}, Species{2}};
    const auto unspaced = scatter(area, {213, 1, 0}, species, flat);
    constexpr double spacing = 0.19;
    const auto spaced = scatter(area, {213, 1, spacing}, species, flat);
    require(spaced == scatter(area, {213, 1, spacing}, species, flat),
            "Seeded priority or spacing did not replay exactly.");
    require(spaced.counts.spacing_rejected > 0 && spaced.instances.size() > 1,
            "The spacing fixture did not exercise both outcomes.");
    check_counts(spaced);
    for (std::size_t i = 0; i < spaced.instances.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            require(xz_distance_squared(spaced.instances[i], spaced.instances[j]) >= spacing * spacing,
                    "Spatial buckets missed a pair inside the spacing radius.");
        }
    }
    for (const auto& candidate : unspaced.instances) {
        require(std::any_of(spaced.instances.begin(), spaced.instances.end(), [&](const auto& placed) {
                    return placed == candidate || xz_distance_squared(placed, candidate) < spacing * spacing;
                }),
                "Spacing suppressed an otherwise valid candidate with no accepted neighbor.");
    }

    const ScatterArea maximum{20, -32768, 0, 1, 65536, 1};
    require(scatter(maximum, {1, 0, 0}, species, flat).counts.cells == max_scatter_cells,
            "The maximum supported grid was not admitted.");
    auto invalid_area = maximum;
    invalid_area.rows = 2;
    require_invalid([&] { (void)scatter(invalid_area, {}, species, flat); });
    invalid_area = area;
    invalid_area.origin_z_m = 1'000'000;
    require_invalid([&] { (void)scatter(invalid_area, {}, species, flat); });
    require_invalid([&] { (void)scatter(area, {1, 1, 0.0005}, species, flat); });
    require_invalid([&] { (void)scatter(area, {}, std::array{Species{7}, Species{7}}, flat); });
    require_invalid([&] { (void)scatter(area, {}, std::array{Species{2, 1, 257}}, flat); });
    require_invalid([&] {
        const auto invalid_sample = [](double, double) {
            return SurfaceSample{std::numeric_limits<double>::infinity(), 1};
        };
        (void)scatter(area, {}, species, invalid_sample);
    });
}

} // namespace

int main() {
    try {
        repeatable_instances();
        surface_filters();
        spacing_and_limits();
        std::cout << "Foliage scatter passed seeded variation, surface filters, spacing and bounds.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
