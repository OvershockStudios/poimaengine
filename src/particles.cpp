// SPDX-License-Identifier: Apache-2.0
#include "poima/particles.hpp"
#include <algorithm>
#include <bitset>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>

namespace poima::particles {
namespace {
constexpr double tick_dt = 1.0 / ticks_per_second;
constexpr double component_limit = 1.0e9;
constexpr auto max_attempts_per_emitter_tick = max_rate / ticks_per_second + max_burst;
constexpr auto max_attempts_per_tick = max_emitters * max_attempts_per_emitter_tick;
constexpr auto last_tick = std::numeric_limits<std::uint64_t>::max() / max_attempts_per_tick;

void validate_limits(Limits limits) {
    if (limits.emitters == 0 || limits.emitters > max_emitters || limits.particles == 0 || limits.particles > max_particles)
        throw std::invalid_argument("Particle system capacity is outside its bounds.");
}

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

double checked_pose_norm_squared(const Pose& pose, double tolerance) {
    if (!bounded(pose.position_m)) throw std::invalid_argument("Particle emitter position is outside its bounds.");
    double norm_squared = 0;
    for (const auto component : pose.rotation) norm_squared += component * component;
    if (!std::isfinite(norm_squared) || std::abs(norm_squared - 1) > tolerance)
        throw std::invalid_argument("Particle emitter rotation must be a unit quaternion.");
    return norm_squared;
}

Pose canonical_pose(Pose pose) {
    const auto length = std::sqrt(checked_pose_norm_squared(pose, 1.0e-6));
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
    if (config.expire_on_contact && (config.simulation_space != Space::World || config.live_limit == 0 ||
        config.live_limit > max_contact_particles_per_emitter))
        throw std::invalid_argument("Particle collision needs world simulation and a live limit of 1..64.");
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

void require_snapshot(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

void validate_counts(const Counts& counts, std::uint64_t attempt_budget, std::uint64_t spawn_budget) {
    require_snapshot(counts.spawned <= spawn_budget && counts.spawned <= attempt_budget &&
        counts.dropped <= attempt_budget - counts.spawned, "Particle snapshot spawn counts exceed the completed tick budget.");
    require_snapshot(counts.live <= counts.spawned && counts.expired <= counts.spawned - counts.live &&
        counts.collided <= counts.spawned - counts.live - counts.expired &&
        counts.removed == counts.spawned - counts.live - counts.expired - counts.collided, "Particle snapshot counts do not balance.");
}

std::uint64_t possible_emitting_ticks(std::uint64_t tick, std::uint32_t rate, std::uint32_t phase) {
    const auto period = ticks_per_second / std::gcd(ticks_per_second, rate);
    for (std::uint64_t skipped = 0; skipped < period && skipped <= tick; ++skipped) {
        const auto emitting_ticks = tick - skipped;
        if (emitting_ticks * rate % ticks_per_second == phase) return emitting_ticks;
    }
    throw std::invalid_argument("Particle snapshot rate phase is impossible for the completed ticks.");
}

Vec3 range_magnitudes(const VectorRange& range, bool rotated) {
    Vec3 bounds{};
    for (std::size_t i = 0; i < bounds.size(); ++i)
        bounds[i] = std::max(std::abs(range.minimum[i]), std::abs(range.maximum[i]));
    if (rotated) bounds.fill(std::hypot(bounds[0], bounds[1], bounds[2]));
    return bounds;
}

struct RestoreSlot {
    std::size_t live = 0;
    std::uint64_t spawns_per_tick = 0;
    Vec3 initial_position_bound{}, initial_velocity_bound{};
};

RestoreSlot validate_emitter(const SnapshotEmitter& emitter, const Status& status, Counts& historical) {
    const auto& config = emitter.config;
    const auto& state = emitter.status;
    const auto& counts = state.particles;
    validate(config, status.limits.particles);
    constexpr auto unit_tolerance = 16 * std::numeric_limits<double>::epsilon();
    checked_pose_norm_squared(state.pose, unit_tolerance);
    checked_pose_norm_squared(emitter.next_pose, unit_tolerance);
    require_snapshot(state.pending_burst <= max_burst && state.rate_phase < ticks_per_second && counts.removed == 0,
        "Particle snapshot emitter queue or counters are invalid.");
    const auto emitting_ticks = possible_emitting_ticks(status.tick, config.particles_per_second, state.rate_phase);
    const auto attempt_budget = status.tick * max_burst + emitting_ticks * config.particles_per_second / ticks_per_second;
    const auto capacity = config.live_limit == 0 ? status.limits.particles : config.live_limit;
    const auto peak_rate = (config.particles_per_second + ticks_per_second - 1) / ticks_per_second;
    const auto spawns_per_tick = std::min<std::uint64_t>(capacity, max_burst + peak_rate);
    validate_counts(counts, attempt_budget, status.tick * spawns_per_tick);
    const auto expiring_ticks = status.tick - std::min<std::uint64_t>(status.tick, config.lifetime_ticks);
    require_snapshot(counts.live <= capacity && counts.expired <= expiring_ticks * spawns_per_tick,
        "Particle snapshot emitter live count or expiry exceeds its bounds.");
    const auto collision_ticks = status.tick > 0 ? status.tick - 1 : 0;
    require_snapshot(counts.collided <= (config.expire_on_contact && config.lifetime_ticks > 1 ? collision_ticks * spawns_per_tick : 0),
        "Particle snapshot collisions do not fit its emitter or available ticks.");

    require_snapshot(counts.live <= historical.live && counts.spawned <= historical.spawned &&
        counts.dropped <= historical.dropped && counts.expired <= historical.expired && counts.collided <= historical.collided,
        "Particle snapshot emitter counters exceed system counters.");
    historical.live -= counts.live;
    historical.spawned -= counts.spawned;
    historical.dropped -= counts.dropped;
    historical.expired -= counts.expired;
    historical.collided -= counts.collided;

    const auto rotated = config.spawn_space != config.simulation_space;
    auto initial_position = range_magnitudes(config.offset_m, rotated);
    if (config.simulation_space == Space::World)
        for (auto& component : initial_position) component += component_limit;
    return {0, spawns_per_tick, initial_position, range_magnitudes(config.velocity_m_s, rotated)};
}

bool within_integrated_bound(double value, double offset, double radius) noexcept {
    // Allow accumulated rounding in a full lifetime of fixed-tick updates.
    constexpr double relative_guard = 1.0e-9;
    constexpr double underflow_guard = std::numeric_limits<double>::denorm_min() *
        max_lifetime_ticks * max_lifetime_ticks;
    const auto guard = relative_guard * (std::abs(offset) + radius) + underflow_guard;
    return std::isfinite(value) && std::abs(value - offset) <= radius + guard;
}

void validate_motion(const SnapshotParticle& particle, const EmitterConfig& config, const RestoreSlot& bounds) {
    const auto seconds = particle.age * tick_dt;
    const auto acceleration_scale = seconds * (seconds + tick_dt) * 0.5;
    for (std::size_t axis = 0; axis < particle.position.size(); ++axis) {
        const auto acceleration = config.acceleration_m_s2[axis];
        const auto initial_speed = bounds.initial_velocity_bound[axis];
        const auto position_radius = bounds.initial_position_bound[axis] + seconds * initial_speed;
        require_snapshot(within_integrated_bound(particle.velocity[axis], acceleration * seconds, initial_speed) &&
            within_integrated_bound(particle.position[axis], acceleration * acceleration_scale, position_radius),
            "Particle snapshot motion exceeds the integration bounds for its age.");
    }
}
}

System::System(Limits limits) {
    validate_limits(limits);
    status_.limits = limits;
    emitters_.resize(limits.emitters);
    particles_.reserve(limits.particles);
}

System::System(const Status& status, std::span<const SnapshotEmitter> emitters, std::span<const SnapshotParticle> particles)
    : System(status.limits) {
    status_ = status;
    std::copy(emitters.begin(), emitters.end(), emitters_.begin());
    particles_.insert(particles_.end(), particles.begin(), particles.end());
}

System::~System() = default;
System::System(System&&) noexcept = default;
System& System::operator=(System&&) noexcept = default;

std::size_t System::find(EmitterId id) const {
    if (id == 0) throw std::invalid_argument("Particle emitter identity must be nonzero.");
    const auto entry = std::find_if(emitters_.begin(), emitters_.end(), [id](const SnapshotEmitter& emitter) { return emitter.id == id; });
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
    *available = SnapshotEmitter{id, config, normalized, EmitterStatus{.pose = normalized, .emitting = config.emitting}};
    ++status_.emitter_count;
}

void System::remove_emitter(EmitterId id) {
    const auto slot = find(id);
    auto& emitter = emitters_[slot];
    std::erase_if(particles_, [slot](const SnapshotParticle& particle) { return particle.slot == slot; });
    status_.particles.removed += emitter.status.particles.live;
    status_.particles.live -= emitter.status.particles.live;
    emitter = SnapshotEmitter{};
    --status_.emitter_count;
}

void System::set_pose(EmitterId id, const Pose& pose) {
    auto& emitter = emitters_[find(id)];
    emitter.next_pose = canonical_pose(pose);
}

void System::commit_pose(EmitterId id, const Pose& pose) {
    auto& emitter = emitters_[find(id)];
    const auto normalized = canonical_pose(pose);
    emitter.status.pose = emitter.next_pose = normalized;
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
    std::size_t requested = state.pending_burst;
    state.pending_burst = 0;
    if (state.emitting) {
        const auto phase = state.rate_phase + config.particles_per_second;
        requested += phase / ticks_per_second;
        state.rate_phase = phase % ticks_per_second;
    }
    spawn_at(slot, requested, state.pose);
}

void System::spawn_at(std::size_t slot, std::size_t requested, const Pose& pose) {
    if (requested == 0) return;
    auto& emitter = emitters_[slot];
    const auto& config = emitter.config;
    auto& state = emitter.status;
    auto& total = status_.particles;
    const auto available = status_.limits.particles - total.live;
    const auto emitter_available = config.live_limit == 0 ? available : config.live_limit - state.particles.live;
    const auto admitted = std::min({requested, available, emitter_available});
    const auto first = state.particles.spawned + state.particles.dropped;
    const auto dropped = requested - admitted;
    state.particles.dropped += dropped;
    total.dropped += dropped;

    const bool world = config.simulation_space == Space::World;
    const auto conversion = world ? pose.rotation : inverse_rotation(pose.rotation);
    for (std::size_t index = 0; index < admitted; ++index) {
        const auto start = (first + index) * 6;
        auto offset = sample(config.offset_m, config.seed, start);
        auto velocity = sample(config.velocity_m_s, config.seed, start + 3);
        if (config.spawn_space != config.simulation_space) {
            offset = rotate(conversion, offset);
            velocity = rotate(conversion, velocity);
        }
        const auto position = world ? add(pose.position_m, offset) : offset;
        particles_.push_back(SnapshotParticle{++total.spawned, slot, position, velocity, 0});
    }
    state.particles.spawned += admitted;
    state.particles.live += admitted;
    total.live += admitted;
}

void System::step(ContactTest contacts) {
    if (status_.tick == last_tick) throw std::overflow_error("Particle system tick counter is exhausted.");
    std::optional<std::bitset<max_particles>> impacts;
    struct Placements {
        std::array<std::vector<PlacedBurst>, max_emitters> bursts;
        std::array<std::uint32_t, max_emitters> counts{};
    };
    std::optional<Placements> placements;
    if (std::ranges::any_of(emitters_, [](const auto& emitter) { return emitter.config.expire_on_contact; })) {
        if (!contacts.hit) throw std::invalid_argument("Particle collision needs a contact test.");
        impacts.emplace();
        for (std::size_t index = 0; index < particles_.size(); ++index) {
            const auto& particle = particles_[index];
            const auto& emitter = emitters_[particle.slot];
            if (!emitter.config.expire_on_contact || particle.age + 1 == emitter.config.lifetime_ticks) continue;
            const auto velocity = add(particle.velocity, scale(emitter.config.acceleration_m_s2, tick_dt));
            const auto end = add(particle.position, scale(velocity, tick_dt));
            if (!contacts.hit(contacts.context, emitter.id, particle.id, particle.position, end)) continue;
            impacts->set(index);
            if (!contacts.take_burst) continue;
            auto burst=contacts.take_burst(contacts.context);
            if (!burst) continue;
            const auto slot = find(burst->emitter);
            if (emitters_[slot].config.simulation_space != Space::World || burst->count == 0 || burst->count > max_burst)
                throw std::invalid_argument("Placed bursts require a world emitter and a bounded positive count.");
            burst->pose = canonical_pose(burst->pose);
            if (!placements) placements.emplace();
            auto& placed = placements->counts[slot];
            burst->count = std::min(burst->count, max_burst - emitters_[slot].status.pending_burst - placed);
            if (!burst->count) continue;
            placed += burst->count;
            placements->bursts[slot].push_back(*burst);
        }
    }
    auto kept = particles_.begin();
    for (std::size_t index = 0; index < particles_.size(); ++index) {
        auto& particle = particles_[index];
        auto& emitter = emitters_[particle.slot];
        const bool expired = ++particle.age == emitter.config.lifetime_ticks;
        if (expired || (impacts && (*impacts)[index])) {
            if (expired) {
                ++emitter.status.particles.expired;
                ++status_.particles.expired;
            } else {
                ++emitter.status.particles.collided;
                ++status_.particles.collided;
            }
            --emitter.status.particles.live;
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
        if (placements) for (const auto& burst : placements->bursts[slot]) spawn_at(slot, burst.count, burst.pose);
    }
    ++status_.tick;
}

Status System::status() const noexcept { return status_; }

EmitterStatus System::emitter_status(EmitterId id) const { return emitters_[find(id)].status; }

Snapshot System::snapshot() const { return {status_, emitters_, particles_}; }

System System::clone() const { return System(status_, emitters_, particles_); }

System System::restore(const Snapshot& snapshot) {
    const auto& status = snapshot.status;
    const auto& totals = status.particles;
    validate_limits(status.limits);
    require_snapshot(status.tick <= last_tick && status.emitter_count <= status.limits.emitters &&
        snapshot.emitters.size() == status.limits.emitters && totals.live <= status.limits.particles &&
        snapshot.particles.size() == totals.live, "Particle snapshot shape or tick exceeds its bounds.");
    const auto max_attempts = status.limits.emitters * max_attempts_per_emitter_tick;
    const auto max_births = std::min<std::uint64_t>(max_attempts, status.limits.particles);
    validate_counts(totals, status.tick * max_attempts, status.tick * max_births);
    require_snapshot(status.tick > 1 || totals.expired == 0, "Particle snapshot expires particles before any lifetime elapses.");
    const auto collision_ticks = status.tick > 0 ? status.tick - 1 : 0;
    require_snapshot(totals.collided <= collision_ticks * max_births, "Particle snapshot collision count exceeds its tick budget.");

    std::array<RestoreSlot, max_emitters> slots{};
    std::array<EmitterId, max_emitters> ids{};
    std::size_t active = 0;
    auto historical = totals;
    for (std::size_t slot = 0; slot < snapshot.emitters.size(); ++slot) {
        const auto& emitter = snapshot.emitters[slot];
        if (emitter.id == 0) {
            require_snapshot(emitter == SnapshotEmitter{}, "Particle snapshot vacant slot contains emitter state.");
            continue;
        }
        ids[active++] = emitter.id;
        slots[slot] = validate_emitter(emitter, status, historical);
    }
    const auto active_ids = std::span(ids).first(active);
    std::sort(active_ids.begin(), active_ids.end());
    require_snapshot(active == status.emitter_count && historical.live == 0 &&
        std::adjacent_find(active_ids.begin(), active_ids.end()) == active_ids.end(),
        "Particle snapshot active emitter count, membership or identity is invalid.");

    std::uint64_t last_id = 0, cohort_size = 0;
    std::uint32_t last_age = max_lifetime_ticks;
    std::size_t last_slot = 0;
    for (const auto& particle : snapshot.particles) {
        require_snapshot(particle.slot < snapshot.emitters.size() && particle.id > last_id && particle.id <= totals.spawned,
            "Particle snapshot live identity, order or slot is invalid.");
        const auto& emitter = snapshot.emitters[particle.slot];
        require_snapshot(emitter.id != 0 && particle.age < emitter.config.lifetime_ticks && particle.age < status.tick &&
            (particle.age < last_age || (particle.age == last_age && particle.slot >= last_slot)),
            "Particle snapshot live membership or chronology is invalid.");
        const auto birth_tick = status.tick - particle.age;
        require_snapshot(particle.id <= max_births * birth_tick &&
            totals.spawned - particle.id < max_births * (particle.age + 1),
            "Particle snapshot live identity does not fit its birth tick.");
        if (particle.age != last_age || particle.slot != last_slot) cohort_size = 0;
        auto& slot = slots[particle.slot];
        require_snapshot(++cohort_size <= slot.spawns_per_tick && ++slot.live <= emitter.status.particles.live,
            "Particle snapshot live membership exceeds its emitter or birth budget.");
        validate_motion(particle, emitter.config, slot);
        last_id = particle.id;
        last_age = particle.age;
        last_slot = particle.slot;
    }
    // Slot counts can never be exceeded by a live particle. Their sum already
    // equals the live array size, so every slot's count must now be exact.
    return System(status, snapshot.emitters, snapshot.particles);
}

std::span<Instance> System::write_instances(std::span<Instance> output) const {
    if (output.size() < particles_.size()) throw std::invalid_argument("Particle instance buffer is too small.");
    auto instances = output.first(particles_.size());
    for (std::size_t index = 0; index < instances.size(); ++index) {
        const auto& particle = particles_[index];
        const auto& emitter = emitters_[particle.slot];
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
