// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace poima::particles {
inline constexpr std::uint32_t ticks_per_second = 60;
inline constexpr std::size_t max_emitters = 128;
inline constexpr std::size_t max_particles = 65'536;
inline constexpr std::uint32_t max_burst = 1'024;
inline constexpr std::uint32_t max_rate = 1'024 * ticks_per_second;
inline constexpr std::uint32_t max_lifetime_ticks = 36'000;
inline constexpr std::size_t max_contact_particles_per_emitter = 64;

using EmitterId = std::uint64_t;
using Vec3 = std::array<double, 3>;
using Quaternion = std::array<double, 4>;
using Color = std::array<float, 4>;

enum class Space { Local, World };

struct Pose {
    Vec3 position_m{};
    Quaternion rotation{0, 0, 0, 1}; // x, y, z, w
    bool operator==(const Pose&) const = default;
};

struct VectorRange {
    Vec3 minimum{}, maximum{};
    bool operator==(const VectorRange&) const = default;
};

struct EmitterConfig {
    std::uint64_t seed = 0;
    Space spawn_space = Space::Local;
    Space simulation_space = Space::World;
    std::uint32_t particles_per_second = 0;
    std::uint32_t lifetime_ticks = ticks_per_second;
    std::size_t live_limit = 0; // Zero uses the shared particle capacity.
    VectorRange offset_m, velocity_m_s;
    Vec3 acceleration_m_s2{}; // In simulation coordinates.
    float size_m = 0.1f;
    Color linear_rgba{1, 1, 1, 1};
    bool emitting = true; // Applies to the continuous rate. Bursts are explicit.
    bool expire_on_contact = false;
    bool operator==(const EmitterConfig&) const = default;
};

struct Limits {
    std::size_t emitters = 64, particles = 16'384;
    bool operator==(const Limits&) const = default;
};

struct Counts {
    std::size_t live = 0;
    std::uint64_t spawned = 0, dropped = 0, expired = 0, removed = 0;
    std::uint64_t collided = 0;
    bool operator==(const Counts&) const = default;
};

struct Status {
    Limits limits{};
    std::uint64_t tick = 0;
    std::size_t emitter_count = 0;
    Counts particles{};
    bool operator==(const Status&) const = default;
};

struct EmitterStatus {
    Counts particles{};
    Pose pose{};
    std::uint32_t pending_burst = 0, rate_phase = 0;
    bool emitting = true;
    bool operator==(const EmitterStatus&) const = default;
};

struct Instance {
    std::uint64_t id = 0;
    EmitterId emitter = 0;
    Vec3 position_m{}, velocity_m_s{}; // World-space. Velocity excludes attachment motion.
    std::uint32_t age_ticks = 0, lifetime_ticks = 0;
    float size_m = 0;
    Color linear_rgba{};
    bool operator==(const Instance&) const = default;
};

struct SnapshotEmitter {
    EmitterId id = 0;
    EmitterConfig config{};
    Pose next_pose{};
    EmitterStatus status{};
    bool operator==(const SnapshotEmitter&) const = default;
};

struct SnapshotParticle {
    std::uint64_t id = 0;
    std::size_t slot = 0;
    Vec3 position{}, velocity{}; // Simulation coordinates.
    std::uint32_t age = 0;
    bool operator==(const SnapshotParticle&) const = default;
};

struct Snapshot {
    Status status{};
    std::vector<SnapshotEmitter> emitters; // Slot order, including vacant slots.
    std::vector<SnapshotParticle> particles; // Live particles in spawn order.
    bool operator==(const Snapshot&) const = default;
};

struct PlacedBurst {
    EmitterId emitter;
    std::uint32_t count;
    Pose pose;
};

struct ContactTest {
    void* context = nullptr;
    bool (*hit)(void*, EmitterId, std::uint64_t particle, const Vec3& from, const Vec3& to) = nullptr;
    // Called after each hit. Return a detached burst into a world emitter.
    std::optional<PlacedBurst> (*take_burst)(void*) = nullptr;
};

// One owner calls or inspects the system at a time. Accepted operations use
// the storage reserved at construction. Particle overflow drops new spawns.
class System {
public:
    explicit System(Limits limits = {});
    ~System();
    System(const System&) = delete;
    System& operator=(const System&) = delete;
    // A moved-from system can be destroyed or assigned a new system.
    System(System&&) noexcept;
    System& operator=(System&&) noexcept;

    void add_emitter(EmitterId id, const EmitterConfig& config, const Pose& pose = {});
    void remove_emitter(EmitterId id); // Immediately retires this emitter's particles.
    void set_pose(EmitterId id, const Pose& pose); // Applied on the next step.
    void commit_pose(EmitterId id, const Pose& pose); // Updates both poses without advancing time.
    void set_emitting(EmitterId id, bool emitting);
    void burst(EmitterId id, std::uint32_t count); // Queues up to max_burst for the next step.
    // Test world-space centre segments before mutating particle state. A hit
    // retires the particle at this tick and increments its contact counter.
    void step(ContactTest contacts = {});

    Status status() const noexcept;
    EmitterStatus emitter_status(EmitterId id) const;
    // Writes all live instances in spawn order. Rejects short buffers before writing.
    std::span<Instance> write_instances(std::span<Instance> output) const;

    Snapshot snapshot() const;
    // Validates the complete state before constructing an independent simulation.
    static System restore(const Snapshot& snapshot);
    // Copies trusted state into storage reserved to the current limits.
    System clone() const;

private:
    System(const Status& status, std::span<const SnapshotEmitter> emitters, std::span<const SnapshotParticle> particles);
    std::size_t find(EmitterId id) const;
    void spawn(std::size_t slot);
    void spawn_at(std::size_t slot, std::size_t requested, const Pose& pose);

    Status status_;
    std::vector<SnapshotEmitter> emitters_;
    std::vector<SnapshotParticle> particles_;
};
}
