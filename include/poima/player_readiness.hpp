// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace poima {
// CPU values only. Focus and drawable facts remain absent until the existing
// platform path observes them. No read queries an OS, device, clock or window.
struct PlayerReadiness {
    enum class FocusObservation { none,initial_flags,focus_gained_event,focus_lost_event,minimized_event };
    enum ReadyCondition : std::uint32_t { backend_unavailable=1,not_initialized=2,no_current_presentation=4,closing_condition=8,finished_condition=16 };
    enum PauseCondition : std::uint32_t { owner_pause=1,interactive_inactive=2 };
    bool backend_available=false,initialized=false,closing=false,finished=false,replay=false;
    bool owner_paused=false,interactive_active=false,ui_ready=false,ui_presented=false;
    bool ui_observation_retained=false,ui_scale_pending=false;
    std::optional<bool> focused,drawable;
    FocusObservation focus_observation=FocusObservation::none;
    std::optional<std::array<std::int32_t,2>> drawable_size;
    constexpr std::uint32_t ready_conditions() const noexcept {
        return (!backend_available ? std::uint32_t{backend_unavailable}:0u) | (!initialized ? std::uint32_t{not_initialized}:0u) |
            (!ui_presented ? std::uint32_t{no_current_presentation}:0u) | (closing ? std::uint32_t{closing_condition}:0u) |
            (finished ? std::uint32_t{finished_condition}:0u);
    }
    constexpr std::uint32_t pause_conditions() const noexcept {
        return (owner_paused ? std::uint32_t{owner_pause}:0u) | (!replay && initialized && !interactive_active ? std::uint32_t{interactive_inactive}:0u);
    }
    constexpr bool ready() const noexcept {return ready_conditions()==0;}
    constexpr bool paused() const noexcept {return pause_conditions()!=0;}
};
namespace player_readiness {
inline constexpr const char* initial_detail="Native player awaits its first owner poll.";
inline constexpr const char* initialized_detail="Native player initialized; awaiting its first successful presentation.";
inline constexpr const char* presented_detail="Native player has presented frames; inspect readiness for the current owner boundary.";
inline const char* phase_detail(const PlayerReadiness& state,std::uint64_t frames) noexcept {
    return !state.initialized ? initial_detail : frames==0 ? initialized_detail : presented_detail;
}
}
}
