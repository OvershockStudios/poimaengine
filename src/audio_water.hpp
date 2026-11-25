// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/audio.hpp"
#include "poima/water_region.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace poima::audio_detail {

inline std::optional<AudioWaterMedium> listener_water_medium(const water::Region* region,
                                                             const Matrix4& listener) {
    if (!region || !region->listener_audio()) return std::nullopt;
    const auto& profile = *region->listener_audio();
    const auto column = region->grid().sample_world(listener[12], listener[14]);
    const double depth = column ? column->submersion_at(listener[13]) : 0;
    const double t = std::min(depth / profile.full_depth_m, 1.0);
    return AudioWaterMedium{profile.cutoff_hz, profile.gain, depth, t * t * (3 - 2 * t)};
}

// A stereo one-pole low-pass with unity DC gain and the specified digital
// -3 dB cutoff. Smooth the depth-dependent wet/dry blend across output samples.
class WaterFilter {
public:
    static constexpr std::uint32_t transition_frames = audio_rate / 20;

    explicit WaterFilter(const AudioWaterMedium& medium)
        : coefficient_(coefficient(medium.cutoff_hz)), gain_(medium.gain), blend_(medium.blend) {}

    void process(std::span<float> samples, double target) noexcept {
        constexpr double maximum_delta = 1.0 / transition_frames;
        for (std::size_t frame = 0; frame < samples.size(); frame += 2) {
            blend_ += std::clamp(target - blend_, -maximum_delta, maximum_delta);
            for (std::size_t channel = 0; channel < filtered_.size(); ++channel) {
                auto& sample = samples[frame + channel];
                auto& filtered = filtered_[channel];
                filtered += coefficient_ * (double(sample) - filtered);
                if (blend_ > 0) sample = static_cast<float>(std::lerp(double(sample), filtered * gain_, blend_));
            }
        }
    }

private:
    static double coefficient(double cutoff_hz) noexcept {
        const double offset = 1 - std::cos(2 * std::numbers::pi * cutoff_hz / audio_rate);
        return std::sqrt(offset * (offset + 2)) - offset;
    }

    double coefficient_;
    double gain_;
    double blend_;
    std::array<double, 2> filtered_{};
};

}
