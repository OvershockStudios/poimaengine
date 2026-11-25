// SPDX-License-Identifier: Apache-2.0
#include "poima/particles.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
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
    check(counts.spawned == counts.live + counts.expired + counts.removed + counts.collided, message);
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

template<class T, std::size_t N> bool same_bits(const std::array<T, N>& a, const std::array<T, N>& b) {
    using Bits = std::conditional_t<std::is_same_v<T, float>, std::uint32_t, std::uint64_t>;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::bit_cast<Bits>(a[i]) != std::bit_cast<Bits>(b[i])) return false;
    return true;
}

bool same_pose_bits(const Pose& a, const Pose& b) {
    return same_bits(a.position_m, b.position_m) && same_bits(a.rotation, b.rotation);
}

void check_exact(const Snapshot& expected, const Snapshot& actual) {
    check(expected == actual, "Checkpoint changed an emitter, particle or counter.");
    for (std::size_t i = 0; i < expected.emitters.size(); ++i) {
        const auto& a = expected.emitters[i];
        const auto& b = actual.emitters[i];
        check(same_pose_bits(a.next_pose, b.next_pose) && same_pose_bits(a.status.pose, b.status.pose),
            "Checkpoint changed sampled or pending pose bits.");
        check(same_bits(a.config.linear_rgba, b.config.linear_rgba) &&
            std::bit_cast<std::uint32_t>(a.config.size_m) == std::bit_cast<std::uint32_t>(b.config.size_m) &&
            same_bits(a.config.offset_m.minimum, b.config.offset_m.minimum) &&
            same_bits(a.config.offset_m.maximum, b.config.offset_m.maximum) &&
            same_bits(a.config.velocity_m_s.minimum, b.config.velocity_m_s.minimum) &&
            same_bits(a.config.velocity_m_s.maximum, b.config.velocity_m_s.maximum) &&
            same_bits(a.config.acceleration_m_s2, b.config.acceleration_m_s2), "Checkpoint changed emitter config bits.");
    }
    for (std::size_t i = 0; i < expected.particles.size(); ++i) {
        const auto& a = expected.particles[i];
        const auto& b = actual.particles[i];
        check(same_bits(a.position, b.position) && same_bits(a.velocity, b.velocity), "Checkpoint changed particle motion bits.");
    }
}

void committed_pose() {
    System system({2, 2});
    EmitterConfig config;
    config.offset_m = {{1, 0, 0}, {1, 0, 0}};
    config.simulation_space = Space::Local;
    system.add_emitter(3, config);
    config.simulation_space = Space::World;
    system.add_emitter(4, config);
    for (const auto id : {EmitterId{3}, EmitterId{4}}) system.burst(id, 1);
    system.step();
    const auto before = instances(system);
    const auto status = system.status();
    system.set_pose(3, {{100, 0, 0}});
    const Pose attached{{20, 0, 0}};
    system.commit_pose(3, attached);
    system.commit_pose(4, attached);
    const auto after = instances(system);
    check(after[0].position_m == Vec3{21, 0, 0} && after[0].age_ticks == before[0].age_ticks && after[1] == before[1] &&
        system.status() == status, "Synchronous pose commit advanced simulation or changed world particles.");
    const auto frozen = system.snapshot();
    check(frozen.emitters[0].next_pose == attached && frozen.emitters[0].status.pose == attached,
        "Pose commit did not replace the previously queued pose.");
    rejects<std::invalid_argument>([&] { system.commit_pose(3, {{}, {0, 0, 0, 0}}); }, "Pose commit accepted a nonunit quaternion.");
    check_exact(frozen, system.snapshot());
    system.step();
    check(instances(system)[0].position_m == Vec3{21, 0, 0}, "Stale queued pose overwrote the synchronous commit.");
}

void exact_checkpoint() {
    static_assert(std::is_nothrow_move_constructible_v<System>);
    static_assert(std::is_nothrow_move_assignable_v<System>);
    System system({4, 6});
    const auto empty = system.snapshot();
    check_exact(empty, System::restore(empty).snapshot());
    check_exact(empty, system.clone().snapshot());

    auto config = random_emitter(567);
    config.simulation_space = Space::Local;
    config.lifetime_ticks = 5;
    config.live_limit = 2;
    config.particles_per_second = 30;
    config.acceleration_m_s2[2] = -0.0;
    const Pose original{{1, -0.0, 3}, {0.005, 0.01, -0.0, 0.9999376980342524}};
    system.add_emitter(11, config, original);
    config.simulation_space = Space::World;
    config.particles_per_second = 0;
    system.add_emitter(22, config, original);
    system.add_emitter(33, config, original);
    system.burst(11, 3);
    system.burst(22, 2);
    system.burst(33, 2);
    system.step();
    system.remove_emitter(22);
    system.set_emitting(33, false);
    system.set_pose(11, {{10, -0.0, 0}});
    system.burst(11, 3);
    const auto saved = system.snapshot();
    check(saved.emitters.size() == 4 && saved.emitters[0].status.pending_burst == 3 &&
        saved.emitters[0].status.rate_phase == 30 && std::signbit(saved.emitters[0].status.pose.rotation[2]) &&
        saved.emitters[1] == SnapshotEmitter{} && saved.emitters[2].id == 33 &&
        saved.status.particles == Counts{4, 6, 1, 0, 2}, "Checkpoint fixture lacks queued work, holes or historical counters.");

    auto restored = System::restore(saved);
    System moved(std::move(restored));
    restored = std::move(moved);
    auto rollback = system.clone();
    check_exact(saved, restored.snapshot());
    check_exact(saved, rollback.snapshot());
    check(instances(system) == instances(restored), "Restored projection differs at the exact checkpoint.");
    for (unsigned frame = 0; frame < 31; ++frame) {
        for (auto* branch : {&system, &restored, &rollback}) {
            if (frame == 1) branch->add_emitter(44, config);
            if (frame >= 1) branch->burst(44, frame % 4);
            if (frame == 8) branch->remove_emitter(33);
            if (frame % 2 == 1) branch->burst(11, 2);
            branch->step();
        }
        const auto state = system.snapshot();
        check_exact(state, restored.snapshot());
        check_exact(state, rollback.snapshot());
        check_exact(state, System::restore(state).snapshot());
        check(instances(system) == instances(restored), "Restored and uninterrupted instances diverged.");
    }
    check(saved.emitters[1] == SnapshotEmitter{} && saved.emitters[0].status.pending_burst == 3 &&
        saved.status.tick == 1, "Continuation changed the owned checkpoint.");
}

void malformed_checkpoints() {
    System system({3, 4});
    auto config = random_emitter(2);
    config.particles_per_second = 30;
    system.add_emitter(10, config);
    system.add_emitter(20, config);
    system.burst(10, 1);
    system.burst(20, 1);
    system.step();
    system.step();
    const auto saved = system.snapshot();
    const auto rejected = [&](auto alter, const char* message) {
        auto invalid = saved;
        alter(invalid);
        rejects<std::invalid_argument>([&] { system = System::restore(invalid); }, message);
    };
    rejected([](auto& s) { s.status.tick = std::numeric_limits<std::uint64_t>::max(); }, "Restore accepted a tick overflow.");
    rejected([](auto& s) { s.status.limits.emitters = max_emitters + 1; }, "Restore accepted excessive emitter capacity.");
    rejected([](auto& s) { s.emitters.pop_back(); }, "Restore accepted truncated slots.");
    rejected([](auto& s) { s.particles.pop_back(); }, "Restore accepted truncated live particles.");
    rejected([](auto& s) { s.emitters[2].status.pending_burst = 1; }, "Restore accepted a nonempty vacant slot.");
    rejected([](auto& s) { s.emitters[1].id = s.emitters[0].id; }, "Restore accepted duplicate emitter IDs.");
    rejected([](auto& s) { s.emitters[0].next_pose.rotation[3] = 1.0000001; }, "Restore reparsed a noncanonical queued pose.");
    rejected([](auto& s) { s.emitters[0].status.rate_phase = 1; }, "Restore accepted an impossible continuous rate phase.");
    rejected([](auto& s) { s.emitters[0].status.pending_burst = max_burst + 1; }, "Restore accepted an oversized queued burst.");
    rejected([](auto& s) { s.emitters[0].config.live_limit = 1; }, "Restore accepted live particles above an emitter's capacity.");
    rejected([](auto& s) { ++s.status.particles.removed; }, "Restore accepted unbalanced system counts.");
    rejected([](auto& s) { s.status.particles.dropped = std::numeric_limits<std::uint64_t>::max(); }, "Restore accepted overflowing spawn counts.");
    rejected([](auto& s) { s.particles[0].slot = 2; }, "Restore accepted a live particle in a vacant slot.");
    rejected([](auto& s) { s.particles[0].age = 2; }, "Restore accepted a live particle born before its first tick.");
    rejected([](auto& s) { ++s.particles[2].age; }, "Restore accepted live entries outside their birth order.");
    rejected([](auto& s) { s.particles[1].id = s.particles[0].id; }, "Restore accepted duplicate live IDs.");
    rejected([](auto& s) { s.particles[0].position[0] = 1.0e30; }, "Restore accepted impossible finite position.");
    rejected([](auto& s) { s.particles[0].velocity[0] = std::numeric_limits<double>::quiet_NaN(); }, "Restore accepted NaN motion.");
    check_exact(saved, system.snapshot());
}

void maximum_lifetime_checkpoint() {
    System system({1, 1});
    EmitterConfig config;
    config.lifetime_ticks = max_lifetime_ticks;
    config.offset_m = {{1.0e9, 1.0e9, -1.0e9}, {1.0e9, 1.0e9, -1.0e9}};
    config.velocity_m_s = config.offset_m;
    config.acceleration_m_s2 = {1.0e9, -1.0e9, 1.0e9};
    const auto turn = std::sqrt(0.5);
    system.add_emitter(1, config, {{1.0e9, -1.0e9, 0}, {0, turn, 0, turn}});
    system.burst(1, 1);
    for (std::uint32_t tick = 0; tick < max_lifetime_ticks; ++tick) system.step();
    const auto saved = system.snapshot();
    check(saved.particles.size() == 1 && saved.particles[0].age == max_lifetime_ticks - 1 &&
        saved.particles[0].position[0] > 1.0e14 && saved.particles[0].velocity[0] > 1.0e11,
        "Long-lived particle fixture did not reach the integrated coordinate range.");
    auto restored = System::restore(saved);
    check_exact(saved, restored.snapshot());
    system.step();
    restored.step();
    check_exact(system.snapshot(), restored.snapshot());
    check(restored.status().particles == Counts{0, 1, 0, 1, 0}, "Restored long-lived particle missed its expiry.");
}

void swept_contacts() {
    System system({2, 8});
    EmitterConfig config;
    config.live_limit = 8;
    config.expire_on_contact = true;
    config.velocity_m_s = {{60, 0, 0}, {60, 0, 0}};
    system.add_emitter(1, config);
    config.expire_on_contact = false;
    system.add_emitter(2, config);
    system.burst(1, 1);
    system.burst(2, 1);
    const auto pending = system.snapshot();
    rejects<std::invalid_argument>([&] { system.step(); }, "Configured collision ran without a query.");
    check_exact(pending, system.snapshot());
    ContactTest hit{nullptr, [](void*, EmitterId emitter, std::uint64_t particle, const Vec3& from, const Vec3& to) {
        check(emitter == 1 && particle == 1, "Contact query included a plain emitter or a fresh birth.");
        check_vector(from, {0, 0, 0}, "Contact origin differs from its last committed position.");
        check_vector(to, {1, 0, 0}, "Contact test did not sweep the complete segment.");
        return true;
    }};
    system.step(hit);
    const auto born = system.snapshot();
    ContactTest fail{nullptr, [](void*, EmitterId, std::uint64_t, const Vec3&, const Vec3&) -> bool {
        throw std::runtime_error("test contact query failure");
    }};
    rejects<std::runtime_error>([&] { system.step(fail); }, "Contact query unexpectedly succeeded.");
    check_exact(born, system.snapshot());
    system.step(hit);
    const auto saved = system.snapshot();
    check(saved.status.particles == Counts{1, 2, 0, 0, 0, 1}, "Contact retirement did not preserve the lifetime ledger.");
    check(instances(system).front().emitter == 2, "Contact removed a particle from the wrong emitter.");
    check_exact(saved, System::restore(saved).snapshot());
    auto invalid = saved;
    invalid.emitters[0].config.expire_on_contact = false;
    rejects<std::invalid_argument>([&] { (void)System::restore(invalid); }, "Restore accepted a collision count for a plain source.");

    auto placed = System::restore(born);
    const auto crossing = hit.hit;
    hit.take_burst = [](void*) -> std::optional<PlacedBurst> {
        const auto turn = std::sqrt(0.5);
        return PlacedBurst{2, 2, {{2, 3, -4}, {0, 0, turn, turn}}};
    };
    placed.step(hit);
    const auto presented = instances(placed);
    check(presented.size() == 3, "Impact burst did not join its contact tick.");
    for (const auto& child : std::span(presented).last(2)) {
        check(child.age_ticks == 0 && child.emitter == 2, "Impact burst advanced or used the source emitter.");
        check_vector(child.position_m, {2, 3, -4}, "Impact burst ignored the contact pose.");
        check_vector(child.velocity_m_s, {0, 60, 0}, "Impact burst ignored the contact rotation.");
    }
    check_exact(placed.snapshot(), System::restore(placed.snapshot()).snapshot());
    auto capped = System::restore(born);
    capped.burst(2, max_burst - 1);
    capped.step({nullptr, crossing, hit.take_burst});
    const auto target = capped.emitter_status(2).particles;
    check(target.spawned + target.dropped == max_burst + 1, "Impact burst exceeded the shared tick budget.");
    check_exact(capped.snapshot(), System::restore(capped.snapshot()).snapshot());
}
}

int main() {
    try {
        moving_source();
        world_aligned_spawns();
        replay_and_overflow();
        lifecycle_and_validation();
        committed_pose();
        exact_checkpoint();
        malformed_checkpoints();
        maximum_lifetime_checkpoint();
        swept_contacts();
        std::cout << "CPU particles passed moving-source, replay, capacity, lifecycle, exact checkpoint and contact checks.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
