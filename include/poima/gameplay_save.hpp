// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace poima {
inline constexpr std::uint64_t gameplay_save_max_revision=9007199254740991ULL;
struct GameplaySaveEpoch {
    std::uint64_t high=0,low=0;
    bool valid() const noexcept { return high!=0 || low!=0; }
    bool operator==(const GameplaySaveEpoch&) const=default;
};
struct GameplaySaveTicket {
    GameplaySaveEpoch epoch;
    std::uint64_t sequence=0;
    bool valid() const noexcept { return epoch.valid() && sequence>0 && sequence<=gameplay_save_max_revision; }
    bool operator==(const GameplaySaveTicket&) const=default;
};
enum class GameplaySaveKind : std::uint32_t { none=0,save=1,load=2 };
enum class GameplaySaveState : std::uint32_t { expired=0,queued=1,resolving=2,succeeded=3,failed=4 };
enum class GameplaySaveRejection : std::uint32_t { none=0,disabled,busy,invalid,exhausted };
struct GameplaySaveRequest {
    GameplaySaveTicket ticket;
    GameplaySaveKind kind=GameplaySaveKind::none;
    std::array<char,65> slot{};
    std::uint64_t configuration_generation=0,requested_tick=0,committed_tick=0;
    bool committed=false,has_expected_generation=false;
    std::uint64_t expected_generation=0;
    bool allow_recovery=false;
    bool operator==(const GameplaySaveRequest&) const=default;
};
struct GameplaySaveCompletion {
    bool succeeded=false;
    std::uint64_t generation=0;
    bool recovered=false;
    std::int32_t error_code=0;
    std::array<char,256> diagnostic{};
    GameplaySaveEpoch restored_epoch;
    std::uint64_t restored_tick=0;
    bool operator==(const GameplaySaveCompletion&) const=default;
};
struct GameplaySaveResult {
    GameplaySaveState state=GameplaySaveState::expired;
    GameplaySaveRequest request;
    GameplaySaveCompletion completion;
    bool operator==(const GameplaySaveResult&) const=default;
};
struct GameplaySaveRestore {
    // Empty ticket denotes an external host load rather than a game request.
    GameplaySaveTicket initiating_ticket;
    GameplaySaveEpoch destination_epoch;
    std::uint64_t committed_source_tick=0,restored_tick=0,generation=0;
    bool recovered=false;
    bool operator==(const GameplaySaveRestore&) const=default;
};
struct GameplaySaveEnqueue {
    GameplaySaveRejection rejection=GameplaySaveRejection::none;
    GameplaySaveTicket ticket;
};
enum class GameplaySavePublication : std::uint32_t { recorded,replayed,invalid,conflict };

// Copies bounded diagnostic bytes, always NUL terminates, and avoids cutting a
// UTF-8 sequence at the buffer end. False reports truncation (not an exception).
bool gameplay_save_diagnostic(std::array<char,256>& destination,std::string_view text) noexcept;

// Owner control plane: never mutated by gameplay callbacks and never rolled
// back with simulation. It must outlive runtimes borrowing it. No heap or I/O.
class GameplaySaveLedger {
    std::array<GameplaySaveResult,64> results_{};
    std::size_t next_=0,size_=0;
    std::optional<GameplaySaveRestore> restore_;
public:
    static constexpr std::size_t capacity=64;
    std::size_t size() const noexcept { return size_; }
    std::optional<GameplaySaveResult> find(GameplaySaveTicket ticket) const noexcept;
    // Validate before any publication. Exact repeat is read-only; changed
    // content for a retained ticket conflicts. Neither allocates nor throws.
    GameplaySavePublication complete(const GameplaySaveRequest&,const GameplaySaveCompletion&) noexcept;
    bool set_restore(const GameplaySaveRestore&) noexcept;
    std::optional<GameplaySaveRestore> last_restore(GameplaySaveEpoch destination) const noexcept;
};

// Runtime checkpoint: copying/restoring this small value checkpoints ticket
// allocation and the staged intent. Epoch comes from the owning host; disabled
// default instances provide no capability to unhosted native runtimes.
class GameplaySaveQueue {
    GameplaySaveEpoch epoch_;
    std::uint64_t next_sequence_=1,configuration_generation_=0;
    bool enabled_=false;
    std::optional<GameplaySaveRequest> pending_;
    bool resolving_=false;
    std::int32_t resolving_code_=0;
    std::array<char,256> resolving_diagnostic_{};
public:
    GameplaySaveQueue() noexcept=default;
    explicit GameplaySaveQueue(GameplaySaveEpoch epoch) noexcept:epoch_(epoch) {}
    GameplaySaveEpoch epoch() const noexcept { return epoch_; }
    bool enabled() const noexcept { return enabled_; }
    std::uint64_t configuration_generation() const noexcept { return configuration_generation_; }
    // Pending work cannot be redirected, disabled or rebased to another root.
    bool configure(bool enabled,std::uint64_t configuration_generation) noexcept;
    GameplaySaveEnqueue enqueue(GameplaySaveKind,std::string_view slot,
        std::optional<std::uint64_t> expected_generation,bool allow_recovery,
        std::uint64_t requested_tick) noexcept;
    const GameplaySaveRequest* pending() const noexcept { return pending_ ? &*pending_ : nullptr; }
    // Called only after the complete atomic batch commits. Same tick is an
    // idempotent repeat; another tick is rejected once committed.
    bool commit(std::uint64_t committed_tick) noexcept;
    // Unknown durable outcome stays pending and blocks another enqueue/batch.
    // The owner must resolve using the same operation identity, never publish
    // a false terminal failure. Memory queries report resolving plus diagnosis.
    bool mark_resolving(std::int32_t error_code,std::string_view diagnostic) noexcept;
    bool clear(GameplaySaveTicket) noexcept;
    GameplaySaveResult query(GameplaySaveTicket,const GameplaySaveLedger* ledger=nullptr) const noexcept;
};
}
