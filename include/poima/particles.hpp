// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace poima::particles {
inline constexpr std::uint32_t ticks_per_second = 60;
inline constexpr std::size_t max_emitters = 128;
inline constexpr std::size_t max_particles = 65'536;
inline constexpr std::uint32_t max_burst = 1'024;
inline constexpr std::uint32_t max_rate = 1'024 * ticks_per_second;
inline constexpr std::uint32_t max_lifetime_ticks = 36'000;

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
};

struct Limits {
    std::size_t emitters = 64, particles = 16'384;
    bool operator==(const Limits&) const = default;
};

struct Counts {
    std::size_t live = 0;
    std::uint64_t spawned = 0, dropped = 0, expired = 0, removed = 0;
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

// One owner calls or inspects the system at a time. Accepted operations use
// the storage reserved at construction. Particle overflow drops new spawns.
class System {
public:
    explicit System(Limits limits = {});
    ~System();
    System(const System&) = delete;
    System& operator=(const System&) = delete;

    void add_emitter(EmitterId id, const EmitterConfig& config, const Pose& pose = {});
    void remove_emitter(EmitterId id); // Immediately retires this emitter's particles.
    void set_pose(EmitterId id, const Pose& pose); // Applied on the next step.
    void set_emitting(EmitterId id, bool emitting);
    void burst(EmitterId id, std::uint32_t count); // Queues up to max_burst for the next step.
    void step();

    Status status() const noexcept;
    EmitterStatus emitter_status(EmitterId id) const;
    // Writes all live instances in spawn order. Rejects short buffers before writing.
    std::span<Instance> write_instances(std::span<Instance> output) const;

private:
    struct Emitter;
    struct Particle;
    std::size_t find(EmitterId id) const;
    void spawn(std::size_t slot);

    Status status_;
    std::vector<Emitter> emitters_;
    std::vector<Particle> particles_;
};
}
