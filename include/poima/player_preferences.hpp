// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace poima {
// Single owner-thread preference state. Rendering/input/audio consume this
// immutable snapshot; preparation performs no device, runtime or storage work.
class PlayerPreferences {
public:
    struct Values {
        std::optional<double> vertical_fov;
        std::optional<float> ui_scale;
        double sensitivity_x=.1, sensitivity_y=.1, master_gain=1;
        bool invert_x=false, invert_y=false;
        std::uint32_t samples=4, frames_in_flight=2;
    };
    struct State {
        std::uint64_t revision=0;
        Values values;
        // Canonical sparse intent and bounded source/profile metadata.
        std::string requested_json, sources_json, profile_json, fields_json;
    };
    struct Error : std::runtime_error {
        int code;
        Error(int value,const std::string& message):std::runtime_error(message),code(value) {}
    };
    explicit PlayerPreferences(Values inherited,
        const std::string& initial_values_json="{}",
        const std::string& initial_sources_json="{}",
        const std::string& profile_json="{}");
    std::shared_ptr<const State> snapshot() const noexcept { return state_; }
    // Strict {expected_revision,set?,reset?}; no receipt or I/O. The candidate
    // is not visible until publish. Graphics intent changes only the next player.
    std::shared_ptr<const State> prepare(const std::string& patch_json) const;
    // Trusted owner commits a candidate prepared by this exact owner, after
    // its revision/device checks and receipt allocations have completed.
    void publish(std::shared_ptr<const State> candidate) noexcept { state_.swap(candidate); }
    std::string inspect() const;
private:
    Values inherited_;
    std::shared_ptr<const State> state_;
};
}
