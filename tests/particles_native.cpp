// SPDX-License-Identifier: Apache-2.0
#include "poima/particles.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace poima::particles;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Error, class Function> void rejects(Function function, const char* message) {
    try {
        function();
    } catch (const Error&) {
        return;
    }
    throw std::runtime_error(message);
}

void check_vector(const Vec3& actual, const Vec3& expected, const char* message) {
    for (std::size_t axis = 0; axis < expected.size(); ++axis)
        check(std::abs(actual[axis] - expected[axis]) < 1.0e-11, message);
}

std::vector<Instance> instances(const System& system) {
    std::vector<Instance> output(system.status().particles.live);
    check(system.write_instances(output).size() == output.size(), "Reported live count differs from output.");
    return output;
}

void check_counts(const Counts& counts, const char* message) {
    check(counts.spawned == counts.live + counts.expired + counts.removed, message);
}

void moving_source() {
    System system({2, 8});
    EmitterConfig config;
    config.simulation_space = Space::Local;
    config.offset_m = {{1, 0, 0}, {1, 0, 0}};
    config.velocity_m_s = {{60, 0, 0}, {60, 0, 0}};
    config.acceleration_m_s2 = {0, -60, 0};
    config.lifetime_ticks = 3;
    config.size_m = 0.25f;
    config.linear_rgba = {2, 0.5f, 0.25f, 0.8f};
    const auto quarter_turn = std::sqrt(0.5);
    const Pose initial{{10, 0, 0}, {0, 0, quarter_turn, quarter_turn}};
    system.add_emitter(11, config, initial);
    config.simulation_space = Space::World;
    system.add_emitter(22, config, initial);
    system.burst(11, 1);
    system.burst(22, 1);
    check(instances(system).empty(), "A queued burst spawned before stepping.");
    system.step();
    const auto born = instances(system);
    check(born.size() == 2 && born[0].emitter == 11 && born[1].emitter == 22, "Unexpected initial spawn order.");
    for (const auto& particle : born) {
        check_vector(particle.position_m, {10, 1, 0}, "Rotated local spawn offset differs.");
        check_vector(particle.velocity_m_s, {0, 60, 0}, "Rotated local spawn velocity differs.");
        check(particle.age_ticks == 0 && particle.lifetime_ticks == 3, "New particle had an invalid lifetime or age.");
        check(particle.size_m == config.size_m && particle.linear_rgba == config.linear_rgba, "Presentation fields differ.");
    }

    const Pose moved{{20, 5, 0}};
    system.set_pose(11, moved);
    system.set_pose(22, moved);
    check(instances(system) == born, "Queued motion changed the previous observation.");
    system.burst(11, 1);
    system.burst(22, 1);
    system.step();
    const auto after = instances(system);
    check(after.size() == 4 && after[0].id == born[0].id && after[1].id == born[1].id,
        "Moving emitter changed stable particle identities.");
    check_vector(after[0].position_m, {22, 5 - 1.0 / 60, 0}, "Attached particle did not follow the new pose.");
    check_vector(after[0].velocity_m_s, {60, -1, 0}, "Attached acceleration did not stay in local axes.");
    check_vector(after[1].position_m, {10, 1 + 59.0 / 60, 0}, "World particle moved with its source.");
    check_vector(after[1].velocity_m_s, {0, 59, 0}, "World acceleration or rotation changed.");
    check(after[0].age_ticks == 1 && after[1].age_ticks == 1 && after[2].age_ticks == 0 && after[3].age_ticks == 0,
        "New and existing particles advanced out of order.");
    check_vector(after[2].position_m, {21, 5, 0}, "New local spawn did not use the sampled pose.");
    check_vector(after[3].position_m, {21, 5, 0}, "New world spawn did not use the sampled pose.");
    system.step();
    check(instances(system)[1].age_ticks == 2, "Particle expired too early.");
    system.step();
    const auto survivors = instances(system);
    check(survivors.size() == 2 && survivors[0].id == after[2].id && survivors[1].id == after[3].id,
        "Lifetime did not retire the oldest particles in stable order.");
    system.step();
    check(system.status().tick == 5 && system.status().particles == Counts{0, 4, 0, 4, 0}, "Expiry counts differ.");
    check(system.emitter_status(11).particles == Counts{0, 2, 0, 2, 0}, "Emitter expiry counts differ.");
}

void world_aligned_spawns() {
    System system({2, 2});
    const auto quarter_turn = std::sqrt(0.5);
    const Pose initial{{5, 10, 15}, {0, 0, quarter_turn, quarter_turn}};
    EmitterConfig config;
    config.spawn_space = Space::World;
    config.offset_m = {{1, 0, 0}, {1, 0, 0}};
    config.velocity_m_s = {{60, 0, 0}, {60, 0, 0}};
    system.add_emitter(1, config, initial);
    config.simulation_space = Space::Local;
    system.add_emitter(2, config, initial);
    system.burst(1, 1);
    system.burst(2, 1);
    system.step();
    for (const auto& particle : instances(system)) {
        check_vector(particle.position_m, {6, 10, 15}, "World spawn axes rotated with the emitter.");
        check_vector(particle.velocity_m_s, {60, 0, 0}, "World spawn velocity rotated with the emitter.");
    }
    system.set_pose(1, {{8, 1, 2}});
    system.set_pose(2, {{8, 1, 2}});
    system.step();
    const auto after = instances(system);
    check_vector(after[0].position_m, {7, 10, 15}, "World-simulated particle did not remain detached.");
    check_vector(after[1].position_m, {8, -1, 2}, "World-aligned spawn did not bind to local simulation correctly.");
}

EmitterConfig random_emitter(std::uint64_t seed) {
    EmitterConfig config;
    config.seed = seed;
    config.offset_m.maximum = {1, 1, 1};
    config.velocity_m_s.maximum = {1, 1, 1};
    return config;
}

void replay_and_overflow() {
    System first({2, 10}), replay({2, 10}), other_seed({2, 10});
    first.add_emitter(1, random_emitter(7));
    replay.add_emitter(1, random_emitter(7));
    other_seed.add_emitter(1, random_emitter(8));
    for (auto* system : {&first, &replay, &other_seed}) {
        system->burst(1, 1);
        system->step();
    }
    const auto initial = instances(first)[0];
    check_vector(initial.position_m, {0.3898297483912715, 0.01678829452815611, 0.9007606806068834}, "Seeded offset sequence changed.");
    check_vector(initial.velocity_m_s, {0.5829302930280781, 0.45244189501146836, 0.24943152228274335}, "Seeded velocity sequence changed.");
    check(instances(other_seed)[0].position_m != initial.position_m, "Different seeds produced identical output.");
    for (unsigned tick = 0; tick < 40; ++tick) {
        for (auto* system : {&first, &replay}) {
            system->set_pose(1, {{static_cast<double>(tick), 1, -2}});
            system->burst(1, tick % 4);
            system->step();
        }
        check(first.status() == replay.status() && instances(first) == instances(replay), "Same-input replay diverged.");
    }

    auto config = random_emitter(7);
    config.lifetime_ticks = 1;
    System constrained({1, 1}), reference({1, 4});
    for (auto* system : {&constrained, &reference}) {
        system->add_emitter(1, config);
        system->burst(1, 3);
        system->step();
        system->burst(1, 1);
        system->step();
    }
    check(constrained.status().particles == Counts{1, 2, 2, 1, 0}, "Global overflow or release at expiry was miscounted.");
    check(instances(constrained)[0].position_m == instances(reference)[0].position_m,
        "Dropping spawns shifted later random samples.");
    check_counts(reference.status().particles, "Reference particle conservation failed.");

    System shared({3, 2});
    config.live_limit = 1;
    shared.add_emitter(90, config);
    config.live_limit = 0;
    shared.add_emitter(20, config);
    shared.add_emitter(30, config);
    for (const auto id : {90U, 20U, 30U}) shared.burst(id, 2);
    shared.step();
    const auto selected = instances(shared);
    check(selected.size() == 2 && selected[0].emitter == 90 && selected[1].emitter == 20,
        "Slot order or per-emitter capacity did not bound admission.");
    check(shared.emitter_status(90).particles.dropped == 1 && shared.emitter_status(20).particles.dropped == 1 &&
        shared.emitter_status(30).particles.dropped == 2, "Drop diagnostics were attributed incorrectly.");
}

void lifecycle_and_validation() {
    rejects<std::invalid_argument>([] { System invalid({0, 1}); }, "Zero emitter capacity was accepted.");
    rejects<std::invalid_argument>([] { System invalid({1, max_particles + 1}); }, "Oversize particle capacity was accepted.");
    System system({1, 4});
    EmitterConfig config;
    config.particles_per_second = 30;
    system.add_emitter(77, config);
    rejects<std::invalid_argument>([&] { system.add_emitter(77, config); }, "Duplicate emitter identity was accepted.");
    rejects<std::length_error>([&] { system.add_emitter(78, config); }, "Emitter cap was exceeded.");
    system.step();
    check(system.emitter_status(77).rate_phase == 30 && instances(system).empty(), "Fractional tick rate was rounded up.");
    system.set_emitting(77, false);
    system.burst(77, 1);
    const auto before = system.emitter_status(77);
    rejects<std::length_error>([&] { system.burst(77, max_burst); }, "Burst budget was exceeded.");
    rejects<std::invalid_argument>([&] { system.set_pose(77, {{}, {0, 0, 0, 0}}); }, "Zero quaternion was accepted.");
    check(system.emitter_status(77) == before, "Rejected action changed emitter state.");
    for (int index = 0; index < 4; ++index) system.step();
    check(system.status().particles.live == 1 && system.emitter_status(77).rate_phase == 30,
        "Suspended continuous emission did not preserve phase or explicit burst.");
    system.set_emitting(77, true);
    system.step();
    check(system.status().particles.live == 2 && system.emitter_status(77).rate_phase == 0, "Emission phase did not resume.");

    std::array<Instance, 3> buffer{};
    buffer[0].id = 100;
    buffer[2].id = 101;
    rejects<std::invalid_argument>([&] { system.write_instances(std::span(buffer).first(1)); }, "Short output buffer was accepted.");
    check(buffer[0].id == 100, "Short instance buffer was partially written.");
    check(system.write_instances(buffer).size() == 2 && buffer[2].id == 101, "Output overwrote unused span storage.");

    system.remove_emitter(77);
    check(system.status().particles == Counts{0, 2, 0, 0, 2} && system.status().emitter_count == 0, "Removing an emitter leaked live particles.");
    rejects<std::out_of_range>([&] { system.burst(77, 1); }, "Removed emitter still accepted bursts.");
    const auto empty = system.status();
    auto bad = config;
    bad.offset_m.minimum[0] = 1;
    rejects<std::invalid_argument>([&] { system.add_emitter(78, bad); }, "Reversed range was accepted.");
    bad = config;
    bad.acceleration_m_s2[0] = std::numeric_limits<double>::infinity();
    rejects<std::invalid_argument>([&] { system.add_emitter(78, bad); }, "Nonfinite acceleration was accepted.");
    bad = config;
    bad.lifetime_ticks = 0;
    rejects<std::invalid_argument>([&] { system.add_emitter(78, bad); }, "Zero lifetime was accepted.");
    check(system.status() == empty, "Rejected emitter consumed a slot or changed the system.");
    system.add_emitter(78, config);
    system.burst(78, 1);
    system.step();
    check(instances(system)[0].id > buffer[1].id, "A recycled emitter slot reused particle identities.");
    check_counts(system.status().particles, "Particle conservation failed after removal.");
}
}

int main() {
    try {
        moving_source();
        world_aligned_spawns();
        replay_and_overflow();
        lifecycle_and_validation();
        std::cout << "CPU particles passed moving-source, replay, capacity and lifecycle checks.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
