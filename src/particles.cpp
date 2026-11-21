// SPDX-License-Identifier: Apache-2.0
#include "poima/particles.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>

namespace poima::particles {
namespace {
constexpr double tick_dt = 1.0 / ticks_per_second;
constexpr double component_limit = 1.0e9;
constexpr auto max_attempts_per_tick = max_emitters * (max_rate / ticks_per_second + max_burst);
constexpr auto last_tick = std::numeric_limits<std::uint64_t>::max() / max_attempts_per_tick;

Vec3 add(const Vec3& a, const Vec3& b) noexcept {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Vec3 scale(const Vec3& vector, double factor) noexcept {
    return {vector[0] * factor, vector[1] * factor, vector[2] * factor};
}

Vec3 cross(const Vec3& a, const Vec3& b) noexcept {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Quaternion inverse_rotation(const Quaternion& q) noexcept {
    return {-q[0], -q[1], -q[2], q[3]};
}

Vec3 rotate(const Quaternion& rotation, const Vec3& vector) noexcept {
    const Vec3 axis{rotation[0], rotation[1], rotation[2]};
    const auto tangent = scale(cross(axis, vector), 2);
    return add(vector, add(scale(tangent, rotation[3]), cross(axis, tangent)));
}

bool bounded(const Vec3& value) noexcept {
    return std::all_of(value.begin(), value.end(), [](double component) {
        return std::isfinite(component) && std::abs(component) <= component_limit;
    });
}

Pose canonical_pose(Pose pose) {
    if (!bounded(pose.position_m)) throw std::invalid_argument("Particle emitter position is outside its bounds.");
    double norm_squared = 0;
    for (const auto component : pose.rotation) norm_squared += component * component;
    if (!std::isfinite(norm_squared) || std::abs(norm_squared - 1) > 1.0e-6)
        throw std::invalid_argument("Particle emitter rotation must be a unit quaternion.");
    const auto length = std::sqrt(norm_squared);
    for (auto& component : pose.rotation) component /= length;
    return pose;
}

bool valid_space(Space value) noexcept {
    return value == Space::Local || value == Space::World;
}

bool valid_range(const VectorRange& range) noexcept {
    return bounded(range.minimum) && bounded(range.maximum) &&
        std::equal(range.minimum.begin(), range.minimum.end(), range.maximum.begin(), std::less_equal<>{});
}

void validate(const EmitterConfig& config, std::size_t capacity) {
    if (!valid_space(config.spawn_space) || !valid_space(config.simulation_space))
        throw std::invalid_argument("Unknown particle space.");
    if (config.particles_per_second > max_rate || config.lifetime_ticks == 0 ||
        config.lifetime_ticks > max_lifetime_ticks || config.live_limit > capacity)
        throw std::invalid_argument("Particle emitter rate, lifetime or live limit is outside its bounds.");
    if (!valid_range(config.offset_m) || !valid_range(config.velocity_m_s) || !bounded(config.acceleration_m_s2))
        throw std::invalid_argument("Particle emitter vector or range is outside its bounds.");
    if (!std::isfinite(config.size_m) || config.size_m <= 0 || config.size_m > component_limit)
        throw std::invalid_argument("Particle size must be positive and within bounds.");
    for (const auto value : config.linear_rgba)
        if (!std::isfinite(value) || value < 0 || value > component_limit)
            throw std::invalid_argument("Particle color is outside its bounds.");
    if (config.linear_rgba[3] > 1) throw std::invalid_argument("Particle alpha must be in [0, 1].");
}

// SplitMix64 applied to a seed and a counter. Each attempt owns six samples,
// including attempts withheld by capacity. Integer wraparound is intentional.
double unit_sample(std::uint64_t seed, std::uint64_t sample) noexcept {
    auto bits = seed + (sample + 1) * 0x9e3779b97f4a7c15ULL;
    bits = (bits ^ (bits >> 30)) * 0xbf58476d1ce4e5b9ULL;
    bits = (bits ^ (bits >> 27)) * 0x94d049bb133111ebULL;
    bits ^= bits >> 31;
    return static_cast<double>(bits >> 11) * 0x1p-53;
}

Vec3 sample(const VectorRange& range, std::uint64_t seed, std::uint64_t index) noexcept {
    Vec3 result{};
    for (std::size_t axis = 0; axis < result.size(); ++axis)
        result[axis] = std::lerp(range.minimum[axis], range.maximum[axis], unit_sample(seed, index + axis));
    return result;
}
}

struct System::Emitter {
    EmitterId id = 0;
    EmitterConfig config;
    Pose next_pose;
    EmitterStatus status;
};

struct System::Particle {
    std::uint64_t id = 0;
    std::size_t emitter = 0;
    Vec3 position{}, velocity{};
    std::uint32_t age = 0;
};

System::System(Limits limits) {
    if (limits.emitters == 0 || limits.emitters > max_emitters || limits.particles == 0 || limits.particles > max_particles)
        throw std::invalid_argument("Particle system capacity is outside its bounds.");
    status_.limits = limits;
    emitters_.resize(limits.emitters);
    particles_.reserve(limits.particles);
}

System::~System() = default;

std::size_t System::find(EmitterId id) const {
    if (id == 0) throw std::invalid_argument("Particle emitter identity must be nonzero.");
    const auto entry = std::find_if(emitters_.begin(), emitters_.end(), [id](const Emitter& emitter) { return emitter.id == id; });
    if (entry == emitters_.end()) throw std::out_of_range("Unknown particle emitter identity.");
    return static_cast<std::size_t>(entry - emitters_.begin());
}

void System::add_emitter(EmitterId id, const EmitterConfig& config, const Pose& pose) {
    if (id == 0) throw std::invalid_argument("Particle emitter identity must be nonzero.");
    auto available = emitters_.end();
    for (auto entry = emitters_.begin(); entry != emitters_.end(); ++entry) {
        if (entry->id == id) throw std::invalid_argument("Particle emitter identity is already in use.");
        if (entry->id == 0 && available == emitters_.end()) available = entry;
    }
    if (available == emitters_.end()) throw std::length_error("Particle emitter capacity is exhausted.");
    validate(config, status_.limits.particles);
    const auto normalized = canonical_pose(pose);
    *available = Emitter{id, config, normalized, EmitterStatus{.pose = normalized, .emitting = config.emitting}};
    ++status_.emitter_count;
}

void System::remove_emitter(EmitterId id) {
    const auto slot = find(id);
    auto& emitter = emitters_[slot];
    std::erase_if(particles_, [slot](const Particle& particle) { return particle.emitter == slot; });
    status_.particles.removed += emitter.status.particles.live;
    status_.particles.live -= emitter.status.particles.live;
    emitter = Emitter{};
    --status_.emitter_count;
}

void System::set_pose(EmitterId id, const Pose& pose) {
    auto& emitter = emitters_[find(id)];
    emitter.next_pose = canonical_pose(pose);
}

void System::set_emitting(EmitterId id, bool emitting) {
    emitters_[find(id)].status.emitting = emitting;
}

void System::burst(EmitterId id, std::uint32_t count) {
    auto& pending = emitters_[find(id)].status.pending_burst;
    if (count > max_burst - pending) throw std::length_error("Particle burst exceeds the per-emitter tick budget.");
    pending += count;
}

void System::spawn(std::size_t slot) {
    auto& emitter = emitters_[slot];
    const auto& config = emitter.config;
    auto& state = emitter.status;
    auto& total = status_.particles;
    std::size_t requested = state.pending_burst;
    state.pending_burst = 0;
    if (state.emitting) {
        const auto phase = state.rate_phase + config.particles_per_second;
        requested += phase / ticks_per_second;
        state.rate_phase = phase % ticks_per_second;
    }
    if (requested == 0) return;
    const auto available = status_.limits.particles - total.live;
    const auto emitter_available = config.live_limit == 0 ? available : config.live_limit - state.particles.live;
    const auto admitted = std::min({requested, available, emitter_available});
    const auto first = state.particles.spawned + state.particles.dropped;
    const auto dropped = requested - admitted;
    state.particles.dropped += dropped;
    total.dropped += dropped;

    const bool world = config.simulation_space == Space::World;
    const auto conversion = world ? state.pose.rotation : inverse_rotation(state.pose.rotation);
    for (std::size_t index = 0; index < admitted; ++index) {
        const auto start = (first + index) * 6;
        auto offset = sample(config.offset_m, config.seed, start);
        auto velocity = sample(config.velocity_m_s, config.seed, start + 3);
        if (config.spawn_space != config.simulation_space) {
            offset = rotate(conversion, offset);
            velocity = rotate(conversion, velocity);
        }
        const auto position = world ? add(state.pose.position_m, offset) : offset;
        particles_.push_back(Particle{++total.spawned, slot, position, velocity, 0});
    }
    state.particles.spawned += admitted;
    state.particles.live += admitted;
    total.live += admitted;
}

void System::step() {
    if (status_.tick == last_tick) throw std::overflow_error("Particle system tick counter is exhausted.");
    auto kept = particles_.begin();
    for (auto& particle : particles_) {
        auto& emitter = emitters_[particle.emitter];
        if (++particle.age == emitter.config.lifetime_ticks) {
            ++emitter.status.particles.expired;
            --emitter.status.particles.live;
            ++status_.particles.expired;
            --status_.particles.live;
            continue;
        }
        particle.velocity = add(particle.velocity, scale(emitter.config.acceleration_m_s2, tick_dt));
        particle.position = add(particle.position, scale(particle.velocity, tick_dt));
        *kept++ = particle;
    }
    particles_.erase(kept, particles_.end());
    for (std::size_t slot = 0; slot < emitters_.size(); ++slot) {
        auto& emitter = emitters_[slot];
        if (emitter.id == 0) continue;
        emitter.status.pose = emitter.next_pose;
        spawn(slot);
    }
    ++status_.tick;
}

Status System::status() const noexcept { return status_; }

EmitterStatus System::emitter_status(EmitterId id) const { return emitters_[find(id)].status; }

std::span<Instance> System::write_instances(std::span<Instance> output) const {
    if (output.size() < particles_.size()) throw std::invalid_argument("Particle instance buffer is too small.");
    auto instances = output.first(particles_.size());
    for (std::size_t index = 0; index < instances.size(); ++index) {
        const auto& particle = particles_[index];
        const auto& emitter = emitters_[particle.emitter];
        const auto& config = emitter.config;
        const auto& pose = emitter.status.pose;
        const bool local = config.simulation_space == Space::Local;
        const auto position = local ? add(pose.position_m, rotate(pose.rotation, particle.position)) : particle.position;
        const auto velocity = local ? rotate(pose.rotation, particle.velocity) : particle.velocity;
        instances[index] = {particle.id, emitter.id, position, velocity, particle.age,
                            config.lifetime_ticks, config.size_m, config.linear_rgba};
    }
    return instances;
}
}
