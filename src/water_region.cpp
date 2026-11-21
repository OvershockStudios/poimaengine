// SPDX-License-Identifier: Apache-2.0
#include "poima/water_region.hpp"

#include "poima/input_action.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace poima::water {
namespace {

using Json = nlohmann::json;

constexpr char snapshot_format[] = "poima.water-region-state";
constexpr double max_depth_m = 1'000'000;
constexpr double max_exchange_rate = 1'000'000;

bool unit_interval(double value) noexcept {
    return value >= 0 && value <= 1;
}

bool lowercase_sha256(std::string_view value) noexcept {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const char digit) {
        return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
    });
}

Grid initial_grid(const RegionDefinition& definition) {
    if (definition.config.cells_x > max_region_side || definition.config.cells_z > max_region_side ||
        definition.faces.size() > max_region_faces || definition.exchanges.size() > max_region_exchanges)
        throw std::invalid_argument("Authored water region exceeds its size or rule limits.");
    if (!lowercase_sha256(definition.plan_sha256))
        throw std::invalid_argument("Water region requires a lowercase SHA-256 plan identity.");
    return Grid(definition.config, definition.floor_heights_m, definition.initial_depths_m);
}

auto face_key(const RegionFace& rule) noexcept {
    return std::tuple{rule.cell.z, rule.cell.x, rule.axis};
}

auto exchange_key(const CellExchange& rule) noexcept {
    return std::pair{rule.cell.z, rule.cell.x};
}

bool exact_fields(const Json& object, std::initializer_list<const char*> fields) {
    return object.is_object() && object.size() == fields.size() &&
        std::all_of(fields.begin(), fields.end(), [&](const char* name) { return object.contains(name); });
}

std::uint64_t unsigned_integer(const Json& value) {
    if (value.is_number_unsigned())
        return value.get<std::uint64_t>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        return static_cast<std::uint64_t>(value.get<std::int64_t>());
    throw std::invalid_argument("Water snapshot version and tick must be unsigned integers.");
}

double finite_nonnegative(const Json& value) {
    if (value.is_number()) {
        const double number = value.get<double>();
        if (std::isfinite(number) && number >= 0)
            return number;
    }
    throw std::invalid_argument("Water snapshot values must be finite nonnegative numbers.");
}

Json report_json(const std::optional<StepReport>& report) {
    if (!report)
        return nullptr;
    return {{"tick", report->tick}, {"volume_before_m3", report->volume_before_m3},
        {"supplied_m3", report->supplied_m3}, {"drained_m3", report->drained_m3},
        {"volume_after_m3", report->volume_after_m3}};
}

std::optional<StepReport> validated_report(const Json& report, const Grid& grid,
                                          std::span<const CellExchange> exchanges) {
    if (grid.tick() == 0) {
        if (!report.is_null())
            throw std::invalid_argument("Initial water snapshot cannot have a step receipt.");
        return std::nullopt;
    }
    if (!exact_fields(report, {"tick", "volume_before_m3", "supplied_m3", "drained_m3", "volume_after_m3"}))
        throw std::invalid_argument("Water snapshot has an incomplete step receipt.");
    const StepReport result{unsigned_integer(report.at("tick")),
        finite_nonnegative(report.at("volume_before_m3")), finite_nonnegative(report.at("supplied_m3")),
        finite_nonnegative(report.at("drained_m3")), finite_nonnegative(report.at("volume_after_m3"))};
    if (result.tick != grid.tick() || result.volume_after_m3 != grid.volume_m3())
        throw std::invalid_argument("Water step receipt does not match the restored tick and volume.");

    double supply_limit = 0;
    double drain_limit = 0;
    for (const auto& exchange : exchanges) {
        supply_limit += exchange.source_m3_per_second * step_seconds;
        drain_limit += exchange.drain_m3_per_second * step_seconds;
    }
    const auto& config = grid.config();
    const double cell_area = config.cell_size_m * config.cell_size_m;
    const double before_limit = max_depth_m * config.cells_x * config.cells_z * cell_area;
    const double scale = std::max({1.0, before_limit, supply_limit, drain_limit});
    const double rate_rounding = 64 * std::numeric_limits<double>::epsilon() * scale;
    if (result.volume_before_m3 > before_limit + rate_rounding ||
        result.supplied_m3 > supply_limit + (supply_limit > 0 ? rate_rounding : 0) ||
        result.drained_m3 > drain_limit + (drain_limit > 0 ? rate_rounding : 0))
        throw std::invalid_argument("Water step receipt exceeds the region's volume or exchange rates.");

    const double actual_scale = std::max({1.0, result.volume_before_m3, result.volume_after_m3,
        result.supplied_m3, result.drained_m3});
    const double volume_rounding = 64 * std::numeric_limits<double>::epsilon() * actual_scale;
    const double expected_after = result.volume_before_m3 + result.supplied_m3 - result.drained_m3;
    if (std::abs(result.volume_after_m3 - expected_after) > volume_rounding)
        throw std::invalid_argument("Water step receipt does not balance.");
    return result;
}

}

Region::Region(const RegionDefinition& definition)
    : grid_(initial_grid(definition)), initial_depths_m_(definition.initial_depths_m),
      faces_(definition.faces), exchanges_(definition.exchanges), plan_sha256_(definition.plan_sha256) {
    const auto& config = grid_.config();
    if (initial_depths_m_.empty())
        initial_depths_m_.resize(static_cast<std::size_t>(config.cells_x) * config.cells_z);

    std::ranges::sort(faces_, {}, face_key);
    for (std::size_t index = 0; index < faces_.size(); ++index) {
        const auto& rule = faces_[index];
        if ((index > 0 && face_key(faces_[index - 1]) == face_key(rule)) ||
            (!rule.action.empty() && !valid_input_action_name(rule.action)) || !unit_interval(rule.action_openness))
            throw std::invalid_argument("Invalid or duplicate authored water face rule.");
        grid_.set_face(rule.cell, rule.axis, rule.initial);
    }

    std::ranges::sort(exchanges_, {}, exchange_key);
    for (std::size_t index = 0; index < exchanges_.size(); ++index) {
        const auto& rule = exchanges_[index];
        if (rule.cell.x >= config.cells_x || rule.cell.z >= config.cells_z ||
            (index > 0 && exchange_key(exchanges_[index - 1]) == exchange_key(rule)) ||
            !(rule.source_m3_per_second >= 0 && rule.source_m3_per_second <= max_exchange_rate) ||
            !(rule.drain_m3_per_second >= 0 && rule.drain_m3_per_second <= max_exchange_rate))
            throw std::invalid_argument("Invalid or duplicate authored water exchange.");
    }
}

StepReport Region::step(std::span<const std::string_view> pressed) {
    struct ChangedFace { std::size_t index; FaceControl previous; };
    std::array<ChangedFace, max_region_faces> changes;
    std::size_t changed = 0;
    try {
        for (std::size_t index = 0; index < faces_.size(); ++index) {
            const auto& rule = faces_[index];
            if (rule.action.empty() || std::ranges::find(pressed, rule.action) == pressed.end())
                continue;
            const auto previous = grid_.face(rule.cell, rule.axis);
            auto updated = rule.initial;
            if (previous.openness == rule.initial.openness)
                updated.openness = rule.action_openness;
            grid_.set_face(rule.cell, rule.axis, updated);
            changes[changed++] = {index, previous};
        }
        last_report_ = grid_.step(exchanges_);
    } catch (...) {
        // Only previously validated face values enter this buffer. Grid::step
        // leaves cells and its tick untouched when it rejects a step.
        while (changed > 0) {
            const auto& change = changes[--changed];
            const auto& rule = faces_[change.index];
            grid_.set_face(rule.cell, rule.axis, change.previous);
        }
        throw;
    }
    return *last_report_;
}

Json Region::save() const {
    Json action_faces = Json::array();
    for (const auto& rule : faces_)
        if (!rule.action.empty())
            action_faces.push_back(grid_.face(rule.cell, rule.axis).openness);
    auto snapshot = grid_.snapshot();
    return {{"format", snapshot_format}, {"version", 1}, {"plan_sha256", plan_sha256_},
        {"tick", snapshot.tick}, {"depths_m", std::move(snapshot.depths_m)},
        {"action_face_openness", std::move(action_faces)}, {"last_report", report_json(last_report_)}};
}

void Region::restore(const Json& saved, std::uint64_t expected_tick) {
    if (!exact_fields(saved, {"format", "version", "plan_sha256", "tick", "depths_m", "action_face_openness", "last_report"}) ||
        saved.at("format") != snapshot_format || unsigned_integer(saved.at("version")) != 1 ||
        saved.at("plan_sha256") != plan_sha256_ || unsigned_integer(saved.at("tick")) != expected_tick)
        throw std::invalid_argument("Water snapshot format, plan identity or tick does not match.");

    auto snapshot = grid_.snapshot();
    const auto& depths = saved.at("depths_m");
    if (!depths.is_array() || depths.size() != snapshot.depths_m.size())
        throw std::invalid_argument("Water snapshot depth count does not match the grid.");
    std::transform(depths.begin(), depths.end(), snapshot.depths_m.begin(), finite_nonnegative);
    if (expected_tick == 0 && snapshot.depths_m != initial_depths_m_)
        throw std::invalid_argument("Initial water snapshot depths do not match the plan.");
    snapshot.tick = expected_tick;
    auto candidate = Grid::restore(snapshot);

    const auto& openings = saved.at("action_face_openness");
    const auto action_count = static_cast<std::size_t>(std::ranges::count_if(faces_, [](const RegionFace& rule) {
        return !rule.action.empty();
    }));
    if (!openings.is_array() || openings.size() != action_count)
        throw std::invalid_argument("Water snapshot face count does not match the plan.");
    std::size_t index = 0;
    for (const auto& rule : faces_) {
        if (rule.action.empty())
            continue;
        const double openness = finite_nonnegative(openings.at(index++));
        auto control = rule.initial;
        if (openness != rule.initial.openness) {
            if (expected_tick == 0 || openness != rule.action_openness)
                throw std::invalid_argument("Water snapshot face openness is outside the plan's states.");
            control.openness = rule.action_openness;
        }
        candidate.set_face(rule.cell, rule.axis, control);
    }
    const auto receipt = validated_report(saved.at("last_report"), candidate, exchanges_);
    grid_ = std::move(candidate);
    last_report_ = receipt;
}

}
