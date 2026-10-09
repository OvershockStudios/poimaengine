// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/gameplay_abi.h"
#include "poima/player_preferences.hpp"
#include <array>
#include <cstddef>
#include <memory>

namespace poima {
struct PlayerPreferenceGraphicsConstraints {
    bool deferred=false,ambient_occlusion=false,color_debug_view=true,reconstruction=false;
};
// Shared by external and compiled preparation; no device/runtime work.
void validate_player_preference_graphics(const PlayerPreferences::State&,
    PlayerPreferenceGraphicsConstraints);

// Owner-thread attachment to the SAME native preference authority. Pure
// preparation is rollback-safe. Only the successful whole Runtime boundary
// publishes; observations are assigned by its host outside game callbacks.
class GameplayPlayerPreferences {
public:
    static constexpr std::uint32_t known_mask=511;
    struct Observation {
        // Bits 0..5: observed/applied/presented revisions, FOV, UI, sink gain.
        std::uint32_t mask=0,audio_outcome=PoimaPreferenceAudioDisabled;
        std::uint64_t observed_revision=0,applied_revision=0,presented_revision=0;
        double effective_fov=0,effective_ui_scale=0,requested_gain=1,sink_gain=0;
    };
    class Prepared {
    public:
        const PoimaGamePreferenceTicket& ticket() const noexcept {return ticket_;}
        const std::shared_ptr<const PlayerPreferences::State>& candidate() const noexcept {return candidate_;}
        const std::shared_ptr<const PlayerPreferences::State>& baseline() const noexcept {return baseline_;}
    private:
        friend class GameplayPlayerPreferences;
        std::shared_ptr<const std::uint8_t> identity_;
        PoimaGamePreferenceTicket ticket_{};
        std::shared_ptr<const PlayerPreferences::State> baseline_,candidate_;
    };
    struct Preparation {
        std::shared_ptr<const Prepared> prepared;
        PoimaGamePreferenceEnqueueV1 enqueue{};
    };
    GameplayPlayerPreferences(std::shared_ptr<PlayerPreferences>,PoimaEntityId owner,
        bool replay=false,PlayerPreferenceGraphicsConstraints={});
    GameplayPlayerPreferences(const GameplayPlayerPreferences&)=delete;
    GameplayPlayerPreferences& operator=(const GameplayPlayerPreferences&)=delete;
    bool active() const noexcept {return active_;}
    void deactivate() noexcept {active_=false;}
    // Host-only admission guard for exhausted owner mutation revisions. Reads
    // remain available, and denied preparation consumes no ticket/revision.
    void mutation_capacity(bool available) noexcept {mutation_capacity_=available;}
    void observe(const Observation& value) noexcept {observation_=value;}
    // Validate full wire encoding/ranges even when no owner is attached.
    static void validate_patch_encoding(const PoimaGamePreferencePatchV1&);
    static PoimaGamePreferenceSnapshotV1 unavailable_snapshot() noexcept;
    static PoimaGamePreferenceEnqueueV1 unavailable_enqueue() noexcept;
    static PoimaGamePreferenceResultV1 unavailable_result(const PoimaGamePreferenceTicket&) noexcept;
    PoimaGamePreferenceSnapshotV1 snapshot() const;
    Preparation prepare(const PoimaGamePreferencePatchV1&,bool busy=false) const;
    // Check before the irreversible native boundary; no intervening publication
    // is permitted before commit on this serialized owner. Repeated/stale
    // commits return false and mutate neither configuration nor receipt ledger.
    bool can_commit(const Prepared&) const noexcept;
    bool commit(const Prepared&) noexcept;
    PoimaGamePreferenceResultV1 query(const PoimaGamePreferenceTicket&,
        const Prepared* staged=nullptr) const noexcept;
private:
    std::shared_ptr<PlayerPreferences> preferences_;
    std::shared_ptr<const std::uint8_t> identity_;
    PoimaEntityId owner_{};
    bool replay_=false,active_=false,mutation_capacity_=true;
    PlayerPreferenceGraphicsConstraints constraints_;
    Observation observation_;
    std::uint64_t sequence_=0;
    std::array<PoimaGamePreferenceResultV1,32> receipts_{};
    std::size_t receipt_cursor_=0;
};
}
