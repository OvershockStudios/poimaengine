// SPDX-License-Identifier: Apache-2.0
#include "particle_state_json.hpp"
#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <utility>

namespace poima::particles {
namespace {
using Json = nlohmann::json;
constexpr auto format = "poima.particle-state";

void require(bool valid, const char* message) {
    if (!valid) throw std::invalid_argument(message);
}

void fields(const Json& value, std::initializer_list<const char*> names, std::initializer_list<const char*> optional = {}) {
    require(value.is_object(), "Expected a particle snapshot object.");
    auto count = names.size();
    for (const auto* name : optional) if (value.contains(name)) ++count;
    require(value.size() == count, "Unexpected particle snapshot object fields.");
    for (const auto* name : names)
        require(value.contains(name), "Missing required particle snapshot field.");
}

void array(const Json& value, std::size_t maximum) {
    require(value.is_array() && value.size() <= maximum, "Particle snapshot array exceeds its shape or limit.");
}

std::uint64_t unsigned_integer(const Json& value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        return static_cast<std::uint64_t>(value.get<std::int64_t>());
    throw std::invalid_argument("Particle snapshot integer must be an unsigned integer.");
}

template<class Integer>
Integer bounded_integer(const Json& value, Integer maximum = std::numeric_limits<Integer>::max()) {
    const auto result = unsigned_integer(value);
    require(result <= maximum, "Particle snapshot integer exceeds its bound.");
    return static_cast<Integer>(result);
}

bool boolean(const Json& value) {
    require(value.is_boolean(), "Particle snapshot flag must be Boolean.");
    return value.get<bool>();
}

template<class Float>
Float number(const Json& value) {
    require(value.is_number(), "Particle snapshot scalar must be a number.");
    const auto real = value.get<double>();
    require(std::isfinite(real) && std::abs(real) <= std::numeric_limits<Float>::max(),
        "Particle snapshot scalar exceeds its finite range.");
    const auto result = static_cast<Float>(real);
    require(static_cast<double>(result) == real, "Particle snapshot scalar is not exactly representable.");
    return result;
}

template<class Float, std::size_t Size>
std::array<Float, Size> vector(const Json& value) {
    require(value.is_array() && value.size() == Size, "Particle snapshot vector has the wrong length.");
    std::array<Float, Size> result{};
    for (std::size_t i = 0; i < Size; ++i) result[i] = number<Float>(value[i]);
    return result;
}

const char* space_name(Space space) {
    switch (space) {
    case Space::Local: return "local";
    case Space::World: return "world";
    }
    throw std::invalid_argument("Unknown particle snapshot space.");
}

Space space(const Json& value) {
    if (value == "local") return Space::Local;
    if (value == "world") return Space::World;
    throw std::invalid_argument("Particle snapshot space must be local or world.");
}

Json pose_json(const Pose& value) {
    return {{"position_m", value.position_m}, {"rotation", value.rotation}};
}

Pose pose(const Json& value) {
    fields(value, {"position_m", "rotation"});
    return {vector<double, 3>(value.at("position_m")), vector<double, 4>(value.at("rotation"))};
}

Json range_json(const VectorRange& value) {
    return {{"minimum", value.minimum}, {"maximum", value.maximum}};
}

VectorRange range(const Json& value) {
    fields(value, {"minimum", "maximum"});
    return {vector<double, 3>(value.at("minimum")), vector<double, 3>(value.at("maximum"))};
}

Json counts_json(const Counts& value) {
    Json result = {{"live", value.live}, {"spawned", value.spawned}, {"dropped", value.dropped},
        {"expired", value.expired}, {"removed", value.removed}};
    if (value.collided) result["collided"] = value.collided;
    return result;
}

Counts counts(const Json& value) {
    fields(value, {"live", "spawned", "dropped", "expired", "removed"}, {"collided"});
    Counts result{bounded_integer(value.at("live"), snapshot_particle_limit), unsigned_integer(value.at("spawned")),
        unsigned_integer(value.at("dropped")), unsigned_integer(value.at("expired")), unsigned_integer(value.at("removed"))};
    if (value.contains("collided")) {
        result.collided = unsigned_integer(value.at("collided"));
        require(result.collided != 0, "Zero collision counts must be omitted from particle snapshots.");
    }
    return result;
}

Json status_json(const Status& value) {
    return {{"limits", {{"emitters", value.limits.emitters}, {"particles", value.limits.particles}}},
        {"tick", value.tick}, {"emitter_count", value.emitter_count}, {"particles", counts_json(value.particles)}};
}

Status status(const Json& value) {
    fields(value, {"limits", "tick", "emitter_count", "particles"});
    const auto& limits = value.at("limits");
    fields(limits, {"emitters", "particles"});
    return {{bounded_integer(limits.at("emitters"), snapshot_emitter_limit),
            bounded_integer(limits.at("particles"), snapshot_particle_limit)},
        unsigned_integer(value.at("tick")), bounded_integer(value.at("emitter_count"), snapshot_emitter_limit),
        counts(value.at("particles"))};
}

Json config_json(const EmitterConfig& value) {
    Json result = {{"seed", value.seed}, {"spawn_space", space_name(value.spawn_space)},
        {"simulation_space", space_name(value.simulation_space)}, {"particles_per_second", value.particles_per_second},
        {"lifetime_ticks", value.lifetime_ticks}, {"live_limit", value.live_limit}, {"offset_m", range_json(value.offset_m)},
        {"velocity_m_s", range_json(value.velocity_m_s)}, {"acceleration_m_s2", value.acceleration_m_s2},
        {"size_m", value.size_m}, {"linear_rgba", value.linear_rgba}, {"emitting", value.emitting}};
    if (value.expire_on_contact) result["expire_on_contact"] = true;
    return result;
}

EmitterConfig config(const Json& value) {
    fields(value, {"seed", "spawn_space", "simulation_space", "particles_per_second", "lifetime_ticks", "live_limit",
        "offset_m", "velocity_m_s", "acceleration_m_s2", "size_m", "linear_rgba", "emitting"}, {"expire_on_contact"});
    EmitterConfig result;
    result.seed = unsigned_integer(value.at("seed"));
    result.spawn_space = space(value.at("spawn_space"));
    result.simulation_space = space(value.at("simulation_space"));
    result.particles_per_second = bounded_integer<std::uint32_t>(value.at("particles_per_second"));
    result.lifetime_ticks = bounded_integer<std::uint32_t>(value.at("lifetime_ticks"));
    result.live_limit = bounded_integer(value.at("live_limit"), snapshot_particle_limit);
    result.offset_m = range(value.at("offset_m"));
    result.velocity_m_s = range(value.at("velocity_m_s"));
    result.acceleration_m_s2 = vector<double, 3>(value.at("acceleration_m_s2"));
    result.size_m = number<float>(value.at("size_m"));
    result.linear_rgba = vector<float, 4>(value.at("linear_rgba"));
    result.emitting = boolean(value.at("emitting"));
    if (value.contains("expire_on_contact")) {
        require(boolean(value.at("expire_on_contact")), "Disabled collision must be omitted from particle snapshots.");
        result.expire_on_contact = true;
    }
    return result;
}

Json emitter_status_json(const EmitterStatus& value) {
    return {{"particles", counts_json(value.particles)}, {"pose", pose_json(value.pose)},
        {"pending_burst", value.pending_burst}, {"rate_phase", value.rate_phase}, {"emitting", value.emitting}};
}

EmitterStatus emitter_status(const Json& value) {
    fields(value, {"particles", "pose", "pending_burst", "rate_phase", "emitting"});
    return {counts(value.at("particles")), pose(value.at("pose")),
        bounded_integer<std::uint32_t>(value.at("pending_burst")), bounded_integer<std::uint32_t>(value.at("rate_phase")),
        boolean(value.at("emitting"))};
}
}

Json snapshot_json(const Snapshot& state) {
    require(state.emitters.size() <= snapshot_emitter_limit && state.particles.size() <= snapshot_particle_limit &&
        state.status.limits.emitters <= snapshot_emitter_limit && state.status.limits.particles <= snapshot_particle_limit,
        "Particle snapshot exceeds the runtime profile limits.");
    auto emitters = Json::array();
    emitters.get_ref<Json::array_t&>().reserve(state.emitters.size());
    for (const auto& emitter : state.emitters)
        emitters.push_back({{"id", emitter.id}, {"config", config_json(emitter.config)},
            {"next_pose", pose_json(emitter.next_pose)}, {"status", emitter_status_json(emitter.status)}});

    auto particles = Json::array();
    particles.get_ref<Json::array_t&>().reserve(state.particles.size());
    for (const auto& particle : state.particles)
        particles.push_back({{"id", particle.id}, {"slot", particle.slot}, {"position_m", particle.position},
            {"velocity_m_s", particle.velocity}, {"age_ticks", particle.age}});

    return {{"format", format}, {"version", 1u}, {"status", status_json(state.status)},
        {"emitters", std::move(emitters)}, {"particles", std::move(particles)}};
}

Snapshot parse_snapshot_json(const Json& value) {
    fields(value, {"format", "version", "status", "emitters", "particles"});
    require(value.at("format") == format && unsigned_integer(value.at("version")) == 1,
        "Unsupported particle snapshot format or version.");
    const auto& emitters = value.at("emitters");
    const auto& particles = value.at("particles");
    array(emitters, snapshot_emitter_limit);
    array(particles, snapshot_particle_limit);

    Snapshot result;
    result.status = status(value.at("status"));
    result.emitters.reserve(emitters.size());
    for (const auto& emitter : emitters) {
        fields(emitter, {"id", "config", "next_pose", "status"});
        result.emitters.push_back({unsigned_integer(emitter.at("id")), config(emitter.at("config")),
            pose(emitter.at("next_pose")), emitter_status(emitter.at("status"))});
    }
    result.particles.reserve(particles.size());
    for (const auto& particle : particles) {
        fields(particle, {"id", "slot", "position_m", "velocity_m_s", "age_ticks"});
        result.particles.push_back({unsigned_integer(particle.at("id")),
            bounded_integer(particle.at("slot"), snapshot_emitter_limit - 1),
            vector<double, 3>(particle.at("position_m")), vector<double, 3>(particle.at("velocity_m_s")),
            bounded_integer<std::uint32_t>(particle.at("age_ticks"))});
    }
    return result;
}
}
