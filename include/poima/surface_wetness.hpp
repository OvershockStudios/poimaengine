// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace poima::wetness {

inline constexpr std::uint32_t units_per_nm = 65'536;
inline constexpr std::uint32_t max_capacity_nm = 20'000'000;
inline constexpr std::uint32_t max_rain_nm = 1'000'000;
inline constexpr std::uint64_t max_quantum = 315'359'999'998;
inline constexpr std::size_t max_batch = 64, max_receivers = 64;
inline constexpr double probe_bound_m = 16, shelter_distance_m = 100;

struct Profile {
    std::uint32_t capacity_nm = 0;
    std::uint32_t retention_q16 = 0;
    std::uint32_t drying_nm_per_second = 0;
    std::array<double, 3> probe_local_m{};
    float darkening = 0;
    float roughness_factor = 1;
};

struct Dose {
    std::uint64_t first_quantum = 0;
    std::vector<std::uint32_t> rain_nm;
};

struct Receiver {
    std::uint64_t film_nm_q16 = 0;
    // The nearest shelter on the last accepted climate quantum.
    std::optional<std::string> blocker;
};

struct State {
    std::optional<std::uint64_t> next_quantum;
    std::map<std::string, Receiver> receivers;
};

void validate(const Profile& profile);
std::uint64_t advance(const Profile& profile, std::uint64_t film_nm_q16,
                      std::uint32_t rain_nm, bool exposed);
void validate(const Dose& dose);

} // namespace poima::wetness
