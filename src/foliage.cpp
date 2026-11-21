// SPDX-License-Identifier: Apache-2.0
#include "poima/foliage.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace poima::foliage {
namespace {

constexpr double coordinate_limit_m = 1'000'000;
constexpr double minimum_cell_size_m = 0.01;

bool in_range(double value, double minimum, double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

std::uint64_t cell_key(InstanceId id) {
    return (std::uint64_t{id.row} << 32) | id.column;
}

// SplitMix64 finalizer, using defined unsigned overflow for each seed lane.
std::uint64_t mix(std::uint64_t value) {
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

enum class Channel : std::uint64_t { Density = 1, X, Z, Species, Yaw, Scale, Variation, Priority };

std::uint64_t sample_bits(std::uint64_t key, Channel channel) {
    return mix(key + 0x9e3779b97f4a7c15ULL * static_cast<std::uint64_t>(channel));
}

double sample_unit(std::uint64_t key, Channel channel) {
    const auto bits = static_cast<std::uint32_t>(sample_bits(key, channel) >> 40);
    // The open interval keeps jittered candidates away from the exact cell edge.
    return (static_cast<double>(bits) + 0.5) * 0x1.0p-24;
}

double up_at_slope(double degrees) {
    if (degrees == 0) return 1;
    if (degrees == 90) return 0;
    return std::cos(degrees * (std::numbers::pi / 180));
}

struct WeightedSpecies {
    Species species;
    double minimum_up;
    double maximum_up;
    std::uint64_t weight_end = 0;
};

std::vector<WeightedSpecies> prepare_species(std::span<const Species> definitions) {
    if (definitions.empty() || definitions.size() > max_scatter_species)
        throw std::invalid_argument("Foliage scatter requires 1 to 64 species.");
    std::vector<WeightedSpecies> result;
    result.reserve(definitions.size());
    for (const auto& species : definitions) {
        if (species.id == 0 || species.weight == 0 || species.weight > 1'000'000 ||
            species.variation_count == 0 || species.variation_count > 256 ||
            !in_range(species.minimum_slope_degrees, 0, 90) ||
            !in_range(species.maximum_slope_degrees, species.minimum_slope_degrees, 90) ||
            !in_range(species.minimum_scale, 0.001, 100) ||
            !in_range(species.maximum_scale, species.minimum_scale, 100)) {
            throw std::invalid_argument("Foliage species has an invalid ID, weight, variation count, slope or scale range.");
        }
        result.push_back({species, up_at_slope(species.maximum_slope_degrees),
                          up_at_slope(species.minimum_slope_degrees)});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.species.id < b.species.id;
    });
    std::uint64_t total = 0;
    std::uint64_t previous_id = 0;
    for (auto& entry : result) {
        if (entry.species.id == previous_id)
            throw std::invalid_argument("Foliage species IDs must be unique.");
        previous_id = entry.species.id;
        total += entry.species.weight;
        entry.weight_end = total;
    }
    return result;
}

struct Candidate {
    Instance instance;
    std::uint64_t priority;
};

void apply_spacing(const ScatterArea& area, double spacing,
                   std::vector<Candidate>& candidates, ScatterResult& result) {
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return cell_key(a.instance.id) < cell_key(b.instance.id);
    });
    using Bucket = std::pair<std::int64_t, std::int64_t>;
    std::map<Bucket, std::vector<std::size_t>> occupied;
    const double distance_squared = spacing * spacing;
    for (const auto& candidate : candidates) {
        const auto& instance = candidate.instance;
        const auto& position = instance.position_m;
        const auto bucket_x = static_cast<std::int64_t>(std::floor((position[0] - area.origin_x_m) / spacing));
        const auto bucket_z = static_cast<std::int64_t>(std::floor((position[2] - area.origin_z_m) / spacing));
        bool crowded = false;
        // The extra neighboring bucket covers rounding at exact bucket edges.
        for (std::int64_t z = bucket_z - 2; z <= bucket_z + 2 && !crowded; ++z) {
            for (std::int64_t x = bucket_x - 2; x <= bucket_x + 2 && !crowded; ++x) {
                const auto found = occupied.find({x, z});
                if (found == occupied.end()) continue;
                for (const std::size_t placed : found->second) {
                    const auto& neighbor = result.instances[placed].position_m;
                    const double dx = position[0] - neighbor[0];
                    const double dz = position[2] - neighbor[2];
                    if (dx * dx + dz * dz < distance_squared) {
                        crowded = true;
                        break;
                    }
                }
            }
        }
        if (crowded) {
            ++result.counts.spacing_rejected;
            continue;
        }
        occupied[{bucket_x, bucket_z}].push_back(result.instances.size());
        result.instances.push_back(instance);
    }
    std::sort(result.instances.begin(), result.instances.end(), [](const auto& a, const auto& b) {
        return cell_key(a.id) < cell_key(b.id);
    });
}

} // namespace

ScatterResult scatter(const ScatterArea& area, const ScatterRecipe& recipe,
                      std::span<const Species> species, const SurfaceSampler& surface) {
    const std::uint64_t cell_count = std::uint64_t{area.columns} * area.rows;
    if (area.id == 0 || cell_count == 0 || cell_count > max_scatter_cells ||
        !in_range(area.cell_size_m, minimum_cell_size_m, 10'000) ||
        !in_range(area.origin_x_m, -coordinate_limit_m, coordinate_limit_m) ||
        !in_range(area.origin_z_m, -coordinate_limit_m, coordinate_limit_m) ||
        !in_range(area.origin_x_m + area.columns * area.cell_size_m, -coordinate_limit_m, coordinate_limit_m) ||
        !in_range(area.origin_z_m + area.rows * area.cell_size_m, -coordinate_limit_m, coordinate_limit_m)) {
        throw std::invalid_argument("Foliage scatter area exceeds its ID, grid or world-coordinate limits.");
    }
    if (!in_range(recipe.density, 0, 1) ||
        (recipe.minimum_spacing_m != 0 && !in_range(recipe.minimum_spacing_m, 0.001, 2'000'000)) ||
        !surface) {
        throw std::invalid_argument("Foliage scatter requires a bounded density, spacing and surface sampler.");
    }
    const auto weighted = prepare_species(species);
    const std::uint64_t seed = mix(recipe.seed ^ mix(area.id));
    std::vector<Candidate> candidates;
    ScatterResult result;
    result.counts.cells = static_cast<std::size_t>(cell_count);
    const bool spaced = recipe.minimum_spacing_m > 0;
    if (spaced) candidates.reserve(result.counts.cells);

    for (std::uint32_t row = 0; row < area.rows; ++row) {
        for (std::uint32_t column = 0; column < area.columns; ++column) {
            const InstanceId id{area.id, column, row};
            const std::uint64_t key = mix(seed ^ cell_key(id));
            if (sample_unit(key, Channel::Density) >= recipe.density) {
                ++result.counts.density_rejected;
                continue;
            }
            const double x = area.origin_x_m + (column + sample_unit(key, Channel::X)) * area.cell_size_m;
            const double z = area.origin_z_m + (row + sample_unit(key, Channel::Z)) * area.cell_size_m;
            const auto point = surface(x, z);
            if (!point) {
                ++result.counts.surface_missing;
                continue;
            }
            if (!in_range(point->height_m, -coordinate_limit_m, coordinate_limit_m) ||
                !in_range(point->normal_up, 0, 1)) {
                throw std::invalid_argument("Foliage surface sample has an invalid height or normal.");
            }
            const auto ticket = sample_bits(key, Channel::Species) % weighted.back().weight_end;
            const auto selected = std::upper_bound(weighted.begin(), weighted.end(), ticket,
                [](std::uint64_t value, const auto& entry) { return value < entry.weight_end; });
            if (point->normal_up < selected->minimum_up || point->normal_up > selected->maximum_up) {
                ++result.counts.slope_rejected;
                continue;
            }
            const auto& definition = selected->species;
            const Instance instance{
                id, definition.id,
                static_cast<std::uint32_t>(sample_bits(key, Channel::Variation) % definition.variation_count),
                {x, point->height_m, z},
                2 * std::numbers::pi * sample_unit(key, Channel::Yaw),
                std::lerp(definition.minimum_scale, definition.maximum_scale, sample_unit(key, Channel::Scale)),
            };
            if (spaced)
                candidates.push_back({instance, sample_bits(key, Channel::Priority)});
            else
                result.instances.push_back(instance);
        }
    }
    if (spaced) {
        result.instances.reserve(candidates.size());
        apply_spacing(area, recipe.minimum_spacing_m, candidates, result);
    }
    return result;
}

std::string entity_id(InstanceId id) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(32, '0');
    const std::array<std::uint64_t, 2> parts{id.area, cell_key(id)};
    for (std::size_t part = 0; part < parts.size(); ++part) {
        for (std::size_t digit = 0; digit < 16; ++digit) {
            result[part * 16 + digit] = digits[(parts[part] >> ((15 - digit) * 4)) & 15];
        }
    }
    return result;
}

} // namespace poima::foliage
