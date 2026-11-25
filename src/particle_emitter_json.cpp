// SPDX-License-Identifier: Apache-2.0
#include "particle_emitter_json.hpp"
#include "poima/input_action.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <stdexcept>

namespace poima {
namespace {
using Json = nlohmann::json;
constexpr std::uint32_t live_capacity = 4096;
constexpr double vector_limit = 1'000'000;
constexpr double min_size = .001, max_size = 100;

void require(bool condition, const char* reason) {
    if (!condition) throw std::invalid_argument(reason);
}

void fields(const Json& value, std::initializer_list<std::string_view> required,
    std::initializer_list<std::string_view> optional = {}) {
    require(value.is_object(), "Particle emitter fields require objects.");
    for (auto key : required) require(value.contains(key), "Missing required particle emitter field.");
    for (const auto& [key, item] : value.items()) {
        (void)item;
        require(std::find(required.begin(), required.end(), key) != required.end() ||
            std::find(optional.begin(), optional.end(), key) != optional.end(), "Unknown particle emitter field.");
    }
}

double number(const Json& value, double minimum, double maximum) {
    require(value.is_number(), "Expected a numeric particle emitter value.");
    const auto parsed = value.get<double>();
    require(std::isfinite(parsed) && parsed >= minimum && parsed <= maximum, "Particle emitter numeric value is out of bounds.");
    return parsed == 0 ? 0.0 : parsed;
}

std::uint32_t integer(const Json& value, std::uint32_t minimum, std::uint32_t maximum) {
    require(value.is_number_integer(), "Expected an integer particle emitter value.");
    return static_cast<std::uint32_t>(number(value, minimum, maximum));
}

particles::Vec3 vector(const Json& value, double minimum, double maximum) {
    require(value.is_array() && value.size() == 3, "Particle emitter vector needs exactly three numbers.");
    return {number(value[0], minimum, maximum), number(value[1], minimum, maximum), number(value[2], minimum, maximum)};
}

particles::VectorRange vector_range(const Json& value) {
    fields(value, {"minimum", "maximum"});
    const auto lower = vector(value.at("minimum"), -vector_limit, vector_limit);
    const auto upper = vector(value.at("maximum"), -vector_limit, vector_limit);
    for (std::size_t axis = 0; axis < lower.size(); ++axis)
        require(lower[axis] <= upper[axis], "Particle emitter vector range must increase on each axis.");
    return {lower, upper};
}

particles::Space space(const Json& value) {
    require(value == "local" || value == "world", "Particle space must be local or world.");
    return value == "local" ? particles::Space::Local : particles::Space::World;
}

std::vector<RuntimeParticleCurveKnot> parse_curve(const Json& value, double max_multiplier) {
    require(value.is_array() && value.size() >= 2 && value.size() <= max_runtime_particle_curve_knots,
        "Particle sprite lifetime curves require 2..8 knots.");
    std::vector<RuntimeParticleCurveKnot> result;
    result.reserve(value.size());
    for (const auto& knot : value) {
        require(knot.is_array() && knot.size() == 2, "Particle sprite curve knots require [phase, multiplier].");
        const auto phase = number(knot[0], 0, 1);
        require(result.empty() || phase > result.back().phase, "Particle sprite curve phases must strictly increase.");
        result.push_back({phase, number(knot[1], 0, max_multiplier)});
    }
    require(result.front().phase == 0 && result.back().phase == 1, "Particle sprite curves require phases 0 and 1 at the endpoints.");
    return result;
}

RuntimeParticleFlipbook parse_flipbook(const Json& value) {
    fields(value, {"columns", "rows", "frame_count", "ticks_per_frame"}, {"first_frame", "loop"});
    RuntimeParticleFlipbook result;
    result.columns = integer(value.at("columns"), 1, max_runtime_particle_flipbook_side);
    result.rows = integer(value.at("rows"), 1, max_runtime_particle_flipbook_side);
    const auto cells = result.columns * result.rows;
    if (value.contains("first_frame")) result.first_frame = integer(value.at("first_frame"), 0, cells - 1);
    result.frame_count = integer(value.at("frame_count"), 1, cells - result.first_frame);
    result.ticks_per_frame = integer(value.at("ticks_per_frame"), 1, max_runtime_particle_frame_ticks);
    if (value.contains("loop")) {
        require(value.at("loop").is_boolean(), "Particle sprite flipbook loop must be Boolean.");
        result.loop = value.at("loop").get<bool>();
    }
    return result;
}

Json object(Json properties, Json required) {
    return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}};
}
Json range(const char* type, double minimum, double maximum) {
    return {{"type", type}, {"minimum", minimum}, {"maximum", maximum}};
}
Json vec_schema(double minimum, double maximum) {
    return {{"type", "array"}, {"items", range("number", minimum, maximum)}, {"minItems", 3}, {"maxItems", 3}};
}
Json curve_schema(double max_multiplier) {
    const Json knot = {{"type", "array"}, {"minItems", 2}, {"maxItems", 2},
        {"prefixItems", {range("number", 0, 1), range("number", 0, max_multiplier)}}};
    return {{"type", "array"}, {"items", knot}, {"minItems", 2}, {"maxItems", max_runtime_particle_curve_knots},
        {"description", "Linear [phase, multiplier] knots with phases strictly increasing from exactly 0 to 1. Phase is committed age divided by lifetime. Omission uses multiplier 1."}};
}
}

RuntimeParticleEmitter parse_particle_emitter(const Json& value) {
    fields(value, {"spawn_space", "simulation_space", "lifetime_ticks", "size_m", "linear_rgb"},
        {"seed", "particles_per_second", "live_limit", "emitting", "offset_m", "velocity_m_s", "acceleration_m_s2",
            "burst", "sprite", "rain", "surface_collision", "impact_burst"});
    RuntimeParticleEmitter result;
    auto& config = result.config;
    config.spawn_space = space(value.at("spawn_space"));
    config.simulation_space = space(value.at("simulation_space"));
    config.lifetime_ticks = integer(value.at("lifetime_ticks"), 1, particles::max_lifetime_ticks);
    config.size_m = static_cast<float>(number(value.at("size_m"), min_size, max_size));
    const auto color = vector(value.at("linear_rgb"), 0, 1);
    for (std::size_t channel = 0; channel < color.size(); ++channel) config.linear_rgba[channel] = static_cast<float>(color[channel]);
    if (value.contains("seed")) {
        const auto& seed = value.at("seed");
        require(seed.is_string(), "Particle seed must be a sixteen-digit lowercase hexadecimal string.");
        const auto& text = seed.get_ref<const std::string&>();
        require(text.size() == 16 && text.find_first_not_of("0123456789abcdef") == std::string::npos,
            "Particle seed must be a sixteen-digit lowercase hexadecimal string.");
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), config.seed, 16);
        require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), "Invalid particle seed.");
    }
    if (value.contains("particles_per_second"))
        config.particles_per_second = integer(value.at("particles_per_second"), 0, particles::max_rate);
    if (value.contains("live_limit")) config.live_limit = integer(value.at("live_limit"), 0, live_capacity);
    if (value.contains("surface_collision")) {
        require(value.at("surface_collision") == "expire" && config.simulation_space == particles::Space::World &&
            config.live_limit >= 1 && config.live_limit <= particles::max_contact_particles_per_emitter,
            "Particle surface collision must be expire with world simulation and a live limit of 1..64.");
        config.expire_on_contact = true;
    }
    if (value.contains("impact_burst")) {
        const auto& burst = value.at("impact_burst");
        fields(burst, {"emitter", "count"});
        require(config.expire_on_contact && burst.at("emitter").is_string(),
            "An impact burst needs surface collision and an emitter ID.");
        auto target = burst.at("emitter").get<std::string>();
        require(target.size() == 32 && target.find_first_not_of("0123456789abcdef") == std::string::npos &&
            target.find_first_not_of('0') != std::string::npos, "Impact burst emitter must be a nonzero entity ID.");
        result.impact_burst = RuntimeParticleImpactBurst{std::move(target),
            integer(burst.at("count"), 1, max_particle_impact_burst)};
    }
    if (value.contains("emitting")) {
        require(value.at("emitting").is_boolean(), "Particle emitting must be Boolean.");
        config.emitting = value.at("emitting").get<bool>();
    }
    if (value.contains("offset_m")) config.offset_m = vector_range(value.at("offset_m"));
    if (value.contains("velocity_m_s")) config.velocity_m_s = vector_range(value.at("velocity_m_s"));
    if (value.contains("acceleration_m_s2"))
        config.acceleration_m_s2 = vector(value.at("acceleration_m_s2"), -vector_limit, vector_limit);
    if (value.contains("burst")) {
        const auto& burst = value.at("burst");
        fields(burst, {"action", "count"});
        require(burst.at("action").is_string(), "Particle burst action must be a named input string.");
        result.burst_action = burst.at("action").get<std::string>();
        require(valid_input_action_name(result.burst_action), "Particle burst action is not a valid input name.");
        result.burst_count = integer(burst.at("count"), 1, particles::max_burst);
    }
    if (value.contains("sprite")) {
        const auto& sprite = value.at("sprite");
        fields(sprite, {"texture"}, {"opacity", "depth_fade_m", "size_over_life", "opacity_over_life", "flipbook"});
        const auto& texture = sprite.at("texture");
        require(texture.is_string(), "Particle sprite texture must be a lowercase cooked image SHA-256.");
        const auto& digest = texture.get_ref<const std::string&>();
        require(digest.size() == 64 && digest.find_first_not_of("0123456789abcdef") == std::string::npos,
            "Particle sprite texture must be a lowercase cooked image SHA-256.");
        auto& style = result.sprite.emplace();
        style.texture_sha256 = digest;
        if (sprite.contains("opacity"))
            config.linear_rgba[3] = static_cast<float>(number(sprite.at("opacity"), 0, 1));
        if (sprite.contains("depth_fade_m"))
            style.depth_fade_m = static_cast<float>(number(sprite.at("depth_fade_m"), 0, 10));
        if (sprite.contains("size_over_life")) style.size_over_life = parse_curve(sprite.at("size_over_life"), 8);
        if (sprite.contains("opacity_over_life")) style.opacity_over_life = parse_curve(sprite.at("opacity_over_life"), 1);
        if (sprite.contains("flipbook")) style.flipbook = parse_flipbook(sprite.at("flipbook"));
    }
    if (value.contains("rain")) {
        const auto& rain = value.at("rain");
        fields(rain, {"nm_per_particle", "max_particles_per_tick"});
        require(config.spawn_space == particles::Space::World && config.simulation_space == particles::Space::World &&
            config.particles_per_second == 0 && !result.burst_count,
            "Rain emitters require world spaces and only accept weather-driven emission.");
        result.rain = RuntimeParticleRainProfile{
            integer(rain.at("nm_per_particle"), 1, wetness::max_rain_nm),
            integer(rain.at("max_particles_per_tick"), 1, max_rain_particles_per_tick)};
    }
    return result;
}

Json particle_emitter_schema() {
    const Json spaces = {{"enum", {"local", "world"}}};
    const auto v = vec_schema(-vector_limit, vector_limit);
    const auto limits = object({{"minimum", v}, {"maximum", v}}, {"minimum", "maximum"});
    const auto burst = object({{"action", {{"type", "string"}, {"pattern", "^[a-z][a-z0-9_.-]{0,63}$"}}},
        {"count", range("integer", 1, particles::max_burst)}}, {"action", "count"});
    constexpr auto max_cells = max_runtime_particle_flipbook_side * max_runtime_particle_flipbook_side;
    auto flipbook = object({{"columns", range("integer", 1, max_runtime_particle_flipbook_side)},
        {"rows", range("integer", 1, max_runtime_particle_flipbook_side)}, {"frame_count", range("integer", 1, max_cells)},
        {"ticks_per_frame", range("integer", 1, max_runtime_particle_frame_ticks)}, {"first_frame", range("integer", 0, max_cells - 1)},
        {"loop", {{"type", "boolean"}, {"default", true}}}}, {"columns", "rows", "frame_count", "ticks_per_frame"});
    flipbook["properties"]["first_frame"]["default"] = 0;
    flipbook["description"] = "Row-major frames from the top left. The frame range must fit in the grid, which must divide the cooked texture's base dimensions into positive whole texels.";
    auto sprite = object({{"texture", {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}},
        {"opacity", range("number", 0, 1)}, {"depth_fade_m", range("number", 0, 10)},
        {"size_over_life", curve_schema(8)}, {"opacity_over_life", curve_schema(1)}, {"flipbook", std::move(flipbook)}}, {"texture"});
    sprite["properties"]["opacity"]["default"] = 1;
    sprite["properties"]["depth_fade_m"]["default"] = 0;
    auto rain = object({{"nm_per_particle", range("integer", 1, wetness::max_rain_nm)},
        {"max_particles_per_tick", range("integer", 1, max_rain_particles_per_tick)}},
        {"nm_per_particle", "max_particles_per_tick"});
    rain["description"] = "Authored world-space rain source driven by accepted weather doses. Shelter is sampled at the emitter origin each active tick.";
    auto impact_burst = object({{"emitter", {{"type", "string"}, {"pattern", "^[0-9a-f]{32}$"},
        {"not", {{"const", "00000000000000000000000000000000"}}}}},
        {"count", range("integer", 1, max_particle_impact_burst)}}, {"emitter", "count"});
    impact_burst["description"] = "Offer a same-tick burst to another world-space emitter at each valid surface contact.";
    auto result = object({{"spawn_space", spaces}, {"simulation_space", spaces}, {"lifetime_ticks", range("integer", 1, particles::max_lifetime_ticks)},
        {"size_m", range("number", min_size, max_size)}, {"linear_rgb", vec_schema(0, 1)},
        {"seed", {{"type", "string"}, {"pattern", "^[0-9a-f]{16}$"}, {"default", "0000000000000000"}}},
        {"particles_per_second", range("integer", 0, particles::max_rate)}, {"live_limit", range("integer", 0, live_capacity)},
        {"emitting", {{"type", "boolean"}, {"default", true}}}, {"offset_m", limits},
        {"velocity_m_s", limits}, {"acceleration_m_s2", v}, {"burst", burst}, {"sprite", std::move(sprite)},
        {"rain", std::move(rain)}, {"surface_collision", {{"const", "expire"},
            {"description", "Retire world particles on a swept centre contact with a static or kinematic collider."}}},
        {"impact_burst", std::move(impact_burst)}},
        {"spawn_space", "simulation_space", "lifetime_ticks", "size_m", "linear_rgb"});
    result["description"] = "Fixed-tick billboards with optional translucent cooked image sprites. Spawn space selects birth axes. Simulation space selects whether live particles follow the source. Burst actions use named pressed edges. Authored rain uses world spaces, zero continuous rate and no explicit bursts.";
    result["if"] = {{"required", {"surface_collision"}}};
    result["then"] = {{"required", {"live_limit"}}, {"properties", {{"simulation_space", {{"const", "world"}}},
        {"live_limit", range("integer", 1, particles::max_contact_particles_per_emitter)}}}};
    result["dependentRequired"] = {{"impact_burst", {"surface_collision"}}};
    return result;
}

Json particle_observation_schema() {
    auto result = object({{"session_id", {{"type", "string"}, {"pattern", "^[0-9a-f]{32}$"}}},
        {"tick", {{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991ULL}}},
        {"after", {{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}}},
        {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}, {"default", 32}}}}, {"session_id", "tick"});
    result["description"] = "Read committed counts, source poses and ordered world-space particles. Sprite rows include sampled presentation. Authored rain adds its current window and last sampled shelter, phase and scheduled births. Continue after the returned decimal ID at the same tick.";
    return result;
}
} // namespace poima
