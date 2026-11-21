// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace poima::wetness {

void validate(const Profile& profile) {
    if (profile.capacity_nm == 0 || profile.capacity_nm > max_capacity_nm)
        throw std::invalid_argument("Wet film capacity must be 1..20000000 nm.");
    if (profile.retention_q16 > units_per_nm)
        throw std::invalid_argument("Wet film retention exceeds the Q16 code for one.");
    if (profile.drying_nm_per_second > profile.capacity_nm)
        throw std::invalid_argument("Wet film drying exceeds capacity per second.");
    if (!std::ranges::all_of(profile.probe_local_m, [](double v) {
        return std::isfinite(v) && std::abs(v) <= probe_bound_m;
    })) throw std::invalid_argument("Wetness receiver probe must be within +/-16 local metres.");
    if (!(profile.darkening >= 0 && profile.darkening <= 1 &&
        profile.roughness_factor >= 0 && profile.roughness_factor <= 1))
        throw std::invalid_argument("Wetness material factors must be finite and within [0,1].");
}

std::uint64_t advance(const Profile& profile, std::uint64_t film_nm_q16,
                      std::uint32_t rain_nm, bool exposed) {
    validate(profile);
    if (rain_nm > max_rain_nm)
        throw std::invalid_argument("Rain dose exceeds 1000000 nm in one game second.");
    const auto capacity = std::uint64_t(profile.capacity_nm) * units_per_nm;
    if (film_nm_q16 > capacity)
        throw std::invalid_argument("Wet film state exceeds its capacity.");

    const auto retained = exposed ? std::uint64_t(rain_nm) * profile.retention_q16 : 0;
    const auto drying = std::uint64_t(profile.drying_nm_per_second) * units_per_nm;
    const auto wetted = film_nm_q16 + retained;
    return wetted > drying ? std::min(wetted - drying, capacity) : 0;
}

void validate(const Dose& dose) {
    if (dose.rain_nm.empty() || dose.rain_nm.size() > max_batch)
        throw std::invalid_argument("Wetness needs 1..64 complete one-second doses.");
    if (dose.first_quantum > max_quantum || dose.rain_nm.size() - 1 > max_quantum - dose.first_quantum)
        throw std::invalid_argument("Wetness dose extends beyond the last complete game second.");
    if (!std::ranges::all_of(dose.rain_nm, [](auto rain) { return rain <= max_rain_nm; }))
        throw std::invalid_argument("Rain dose exceeds 1000000 nm in one game second.");
}

} // namespace poima::wetness

namespace poima {

void validate_runtime_surface_wetness(const RuntimeDefinition& definition) {
    const auto has_rain = [](const auto& entity) {
        return entity.particle_emitter && entity.particle_emitter->rain;
    };
    for (const auto& recipe : definition.templates) {
        if (has_rain(recipe) || std::ranges::any_of(recipe.entities, has_rain))
            throw std::invalid_argument("Rain emitters require an authored world entity outside the spawn catalog.");
        for (const auto& entity : recipe.entities) {
            if (entity.surface_wetness)
                throw std::invalid_argument("SurfaceWetness is unavailable in runtime template '"+recipe.id+"'.");
        }
    }

    std::size_t receivers = 0;
    for (const auto& entity : definition.entities) {
        if (has_rain(entity)) {
            const auto& emitter = *entity.particle_emitter;
            const auto& profile = *emitter.rain;
            if (emitter.config.spawn_space != particles::Space::World ||
                emitter.config.simulation_space != particles::Space::World ||
                emitter.config.particles_per_second || emitter.burst_count ||
                profile.nm_per_particle == 0 || profile.nm_per_particle > wetness::max_rain_nm ||
                profile.max_particles_per_tick == 0 || profile.max_particles_per_tick > max_rain_particles_per_tick)
                throw std::invalid_argument("Rain emitter needs world spaces, bounded rates and weather-driven emission.");
        }
        if (!entity.surface_wetness) continue;
        if (++receivers > wetness::max_receivers)
            throw std::invalid_argument("A world supports at most 64 SurfaceWetness receivers.");
        if (!entity.mesh || !entity.mesh->material || entity.skinned_mesh || entity.camera_draw ||
            (entity.mesh->material->alpha_mode != MaterialAlphaMode::opaque &&
             entity.mesh->material->alpha_mode != MaterialAlphaMode::mask))
            throw std::invalid_argument("SurfaceWetness needs a rigid world drawable with an explicit opaque or masked PBR material.");
        wetness::validate(*entity.surface_wetness);
    }
}

} // namespace poima
