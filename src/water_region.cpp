// SPDX-License-Identifier: Apache-2.0
#include "poima/water_region.hpp"

#include "poima/input_action.hpp"
#include "poima/surface_wetness.hpp"

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
constexpr double nm_to_m = 1e-9;
constexpr std::uint64_t max_tick_rain_nm = wetness::max_batch * wetness::max_rain_nm;
constexpr std::uint64_t max_total_rain_nm = (wetness::max_quantum + 1) * wetness::max_rain_nm;

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
    if (definition.surface_wave_layers.size() > max_surface_wave_layers)
        throw std::invalid_argument("Water expects at most four surface wave layers.");
    if (definition.surface_ripples && !definition.surface_wave_layers.empty())
        throw std::invalid_argument("Choose either surface_ripples or surface_wave_layers.");
    return Grid(definition.config, definition.floor_heights_m, definition.initial_depths_m);
}

void validate_surface_ripples(const SurfaceRipples& ripples) {
    if (!(ripples.wavelength_m >= .10 && ripples.wavelength_m <= 8.0))
        throw std::invalid_argument("Water surface ripple wavelength_m must be finite and in [0.10,8].");
    if (!(ripples.slope >= 0 && ripples.slope <= .4))
        throw std::invalid_argument("Water surface ripple slope must be finite and in [0,0.4].");
    if (ripples.period_ticks < 8 || ripples.period_ticks > 3600)
        throw std::invalid_argument("Water surface ripple period_ticks must be in [8,3600].");
    if (!(ripples.heading_degrees >= -180 && ripples.heading_degrees <= 180))
        throw std::invalid_argument("Water surface ripple heading_degrees must be finite and in [-180,180].");
}

void validate_wave_layers(std::span<const SurfaceRipples> layers) {
    double sum = 0;
    double rounding_error = 0;
    for (const auto& layer : layers) {
        validate_surface_ripples(layer);
        const double next = sum + layer.slope;
        // Carry rounding error so the slope limit does not depend on the sequence of partial sums.
        rounding_error += sum >= layer.slope ? (sum - next) + layer.slope : (layer.slope - next) + sum;
        sum = next;
    }
    if (sum + rounding_error > .4)
        throw std::invalid_argument("Water surface wave layer slopes must sum to at most 0.4.");
}

auto face_key(const RegionFace& rule) noexcept {
    return std::tuple{rule.cell.z, rule.cell.x, rule.axis};
}

auto cell_key(Cell cell) noexcept {
    return std::pair{cell.z, cell.x};
}

auto exchange_key(const CellExchange& rule) noexcept {
    return cell_key(rule.cell);
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

Json report_json(const std::optional<StepReport>& report, bool rain_enabled) {
    if (!report)
        return nullptr;
    Json result{{"tick", report->tick}, {"volume_before_m3", report->volume_before_m3},
        {"supplied_m3", report->supplied_m3}, {"drained_m3", report->drained_m3},
        {"volume_after_m3", report->volume_after_m3}};
    if (rain_enabled)
        result["rain_m3"] = report->rain_m3;
    return result;
}

std::optional<StepReport> validated_report(const Json& report, const Grid& grid,
                                          std::span<const CellExchange> exchanges,
                                          std::optional<double> rain_m3) {
    if (grid.tick() == 0) {
        if (!report.is_null())
            throw std::invalid_argument("Initial water snapshot cannot have a step receipt.");
        return std::nullopt;
    }
    if (rain_m3
            ? !exact_fields(report, {"tick", "volume_before_m3", "supplied_m3", "drained_m3", "volume_after_m3", "rain_m3"})
            : !exact_fields(report, {"tick", "volume_before_m3", "supplied_m3", "drained_m3", "volume_after_m3"}))
        throw std::invalid_argument("Water snapshot has an incomplete step receipt.");
    const StepReport result{unsigned_integer(report.at("tick")),
        finite_nonnegative(report.at("volume_before_m3")), finite_nonnegative(report.at("supplied_m3")),
        finite_nonnegative(report.at("drained_m3")), finite_nonnegative(report.at("volume_after_m3")),
        rain_m3 ? finite_nonnegative(report.at("rain_m3")) : 0.0};
    if (result.tick != grid.tick() || result.volume_after_m3 != grid.volume_m3() ||
        result.rain_m3 != rain_m3.value_or(0.0))
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
        result.supplied_m3, result.drained_m3, result.rain_m3});
    const double volume_rounding = 64 * std::numeric_limits<double>::epsilon() * actual_scale;
    const double expected_after = result.volume_before_m3 + result.supplied_m3 + result.rain_m3 - result.drained_m3;
    if (std::abs(result.volume_after_m3 - expected_after) > volume_rounding)
        throw std::invalid_argument("Water step receipt does not balance.");
    return result;
}

}

nlohmann::json surface_ripples_json(const SurfaceRipples& ripples) {
    return {{"wavelength_m", ripples.wavelength_m}, {"slope", ripples.slope},
        {"period_ticks", ripples.period_ticks}, {"heading_degrees", ripples.heading_degrees}};
}

nlohmann::json surface_wave_layers_json(std::span<const SurfaceRipples> layers) {
    auto result = Json::array();
    for (const auto& layer : layers)
        result.push_back(surface_ripples_json(layer));
    return result;
}

nlohmann::json listener_audio_json(const ListenerAudio& profile) {
    return {{"cutoff_hz", profile.cutoff_hz}, {"gain", profile.gain},
        {"full_depth_m", profile.full_depth_m}};
}

nlohmann::json optics_json(const WaterOptics& profile) {
    return {{"extinction_per_m",profile.extinction_per_m},{"scattered_radiance",profile.scattered_radiance}};
}

Region::Region(const RegionDefinition& definition)
    : grid_(initial_grid(definition)), initial_depths_m_(definition.initial_depths_m),
      faces_(definition.faces), exchanges_(definition.exchanges), plan_sha256_(definition.plan_sha256),
      rain_cells_(definition.rain_cells), surface_ripples_(definition.surface_ripples),
      surface_wave_layers_(definition.surface_wave_layers), listener_audio_(definition.listener_audio), optics_(definition.optics) {
    if (surface_ripples_)
        validate_surface_ripples(*surface_ripples_);
    validate_wave_layers(surface_wave_layers_);
    if (listener_audio_) {
        const auto& profile = *listener_audio_;
        if (!(profile.cutoff_hz >= 80 && profile.cutoff_hz <= 8000 && unit_interval(profile.gain) &&
              profile.full_depth_m >= .01 && profile.full_depth_m <= 10))
            throw std::invalid_argument("Water listener audio needs cutoff_hz in [80,8000], gain in [0,1] and full_depth_m in [0.01,10].");
    }
    if (optics_) {
        for (double value : optics_->extinction_per_m)
            if (!(value >= 0 && value <= 8))
                throw std::invalid_argument("Water extinction_per_m channels must be in [0,8].");
        for (double value : optics_->scattered_radiance)
            if (!(value >= 0 && value <= 16))
                throw std::invalid_argument("Water scattered_radiance channels must be in [0,16].");
    }

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

    if (rain_cells_.size() > initial_depths_m_.size())
        throw std::invalid_argument("Rain coverage exceeds the authored water grid.");
    std::ranges::sort(rain_cells_, {}, cell_key);
    for (std::size_t index = 0; index < rain_cells_.size(); ++index) {
        const auto cell = rain_cells_[index];
        if (cell.x >= config.cells_x || cell.z >= config.cells_z ||
            (index > 0 && cell_key(rain_cells_[index - 1]) == cell_key(cell)))
            throw std::invalid_argument("Invalid or duplicate authored water rain cell.");
    }
}

double Region::rain_volume_m3(std::uint64_t depth_nm) const noexcept {
    const double size = grid_.config().cell_size_m;
    return (static_cast<double>(depth_nm) * nm_to_m) *
        static_cast<double>(rain_cells_.size()) * (size * size);
}

double Region::cumulative_rain_m3() const noexcept {
    return rain_volume_m3(cumulative_rain_nm_);
}

StepReport Region::step(std::span<const std::string_view> pressed, std::uint32_t rain_depth_nm,
                        std::optional<std::uint64_t> first_quantum) {
    if ((first_quantum && (rain_cells_.empty() || *first_quantum > wetness::max_quantum)) ||
        (rain_depth_nm > 0 && !first_quantum) || rain_depth_nm > max_tick_rain_nm ||
        rain_depth_nm > max_total_rain_nm - cumulative_rain_nm_)
        throw std::invalid_argument("Rainfall exceeds the water region's coverage, clock or volume bounds.");
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
        const auto rain_cells = rain_depth_nm ? std::span<const Cell>(rain_cells_) : std::span<const Cell>{};
        last_report_ = grid_.step(exchanges_, rain_cells, rain_depth_nm * nm_to_m);
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
    if (!first_rain_quantum_)
        first_rain_quantum_ = first_quantum;
    last_rain_nm_ = rain_depth_nm;
    cumulative_rain_nm_ += rain_depth_nm;
    return *last_report_;
}

Json Region::save() const {
    Json action_faces = Json::array();
    for (const auto& rule : faces_)
        if (!rule.action.empty())
            action_faces.push_back(grid_.face(rule.cell, rule.axis).openness);
    auto snapshot = grid_.snapshot();
    const bool rain_enabled = !rain_cells_.empty();
    Json result{{"format", snapshot_format}, {"version", rain_enabled ? 2 : 1}, {"plan_sha256", plan_sha256_},
        {"tick", snapshot.tick}, {"depths_m", std::move(snapshot.depths_m)},
        {"action_face_openness", std::move(action_faces)}, {"last_report", report_json(last_report_, rain_enabled)}};
    if (rain_enabled)
        result["rain"] = {{"first_quantum", first_rain_quantum_ ? Json(*first_rain_quantum_) : Json(nullptr)},
            {"cumulative_depth_nm", cumulative_rain_nm_}, {"last_step_depth_nm", last_rain_nm_}};
    return result;
}

void Region::restore(const Json& saved, std::uint64_t expected_tick,
                     std::optional<std::uint64_t> next_quantum) {
    const bool rain_enabled = !rain_cells_.empty();
    const bool fields_match = rain_enabled
        ? exact_fields(saved, {"format", "version", "plan_sha256", "tick", "depths_m", "action_face_openness", "last_report", "rain"})
        : exact_fields(saved, {"format", "version", "plan_sha256", "tick", "depths_m", "action_face_openness", "last_report"});
    if (!fields_match || saved.at("format") != snapshot_format ||
        unsigned_integer(saved.at("version")) != (rain_enabled ? 2u : 1u) ||
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
    std::optional<std::uint64_t> first_quantum;
    std::uint64_t cumulative_rain_nm = 0;
    std::uint64_t last_rain_nm = 0;
    if (rain_enabled) {
        const auto& rain = saved.at("rain");
        if (!exact_fields(rain, {"first_quantum", "cumulative_depth_nm", "last_step_depth_nm"}))
            throw std::invalid_argument("Water snapshot has an incomplete rain receipt.");
        if (!rain.at("first_quantum").is_null())
            first_quantum = unsigned_integer(rain.at("first_quantum"));
        cumulative_rain_nm = unsigned_integer(rain.at("cumulative_depth_nm"));
        last_rain_nm = unsigned_integer(rain.at("last_step_depth_nm"));
        if (first_quantum.has_value() != next_quantum.has_value() ||
            (first_quantum && (!expected_tick || *first_quantum >= *next_quantum || *next_quantum > wetness::max_quantum + 1)))
            throw std::invalid_argument("Water snapshot rain receipt differs from the saved weather clock.");
        const auto quanta = first_quantum ? *next_quantum - *first_quantum : 0;
        if ((quanta && (quanta + wetness::max_batch - 1) / wetness::max_batch > expected_tick) ||
            cumulative_rain_nm > quanta * wetness::max_rain_nm ||
            last_rain_nm > max_tick_rain_nm || last_rain_nm > cumulative_rain_nm)
            throw std::invalid_argument("Water snapshot rain receipt exceeds the weather clock or tick bounds.");
    }
    const auto rain_m3 = rain_enabled ? std::optional(rain_volume_m3(last_rain_nm)) : std::nullopt;
    const auto receipt = validated_report(saved.at("last_report"), candidate, exchanges_, rain_m3);
    grid_ = std::move(candidate);
    last_report_ = receipt;
    first_rain_quantum_ = first_quantum;
    cumulative_rain_nm_ = cumulative_rain_nm;
    last_rain_nm_ = static_cast<std::uint32_t>(last_rain_nm);
}

}
