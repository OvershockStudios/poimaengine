// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/gameplay_save.hpp"
#include "poima/jobs.hpp"
#include "poima/save_store.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace poima {
struct GameplaySaveWriteInput {
    std::filesystem::path path;
    GameplaySaveRequest request;
    std::string content_hash;
    std::string bytes;
    // Captured by the owner at the checkpoint boundary, not reconstructed on
    // the worker or during a later receipt-resolution submission.
    jobs::Attribution attribution{};
};
struct GameplaySaveWriteResult {
    GameplaySaveState state=GameplaySaveState::queued;
    GameplaySaveRequest request;
    GameplaySaveCompletion completion;
    std::filesystem::path path;
    std::string operation,hash;
    std::uint64_t expected_generation=0;
    bool generation_chosen=false,started=false;
};

// One immutable checkpoint on an injected owner's background lane. No World,
// Runtime, ledger or cache is borrowed. Calls and destruction belong to this
// object's creating owner; the executor must have that same owner. A zero-worker
// executor runs synchronously and is not an asynchronous capability.
class GameplaySaveWriter {
    struct Pending;
    std::shared_ptr<jobs::Executor> executor_;
    saves::FaultHook fault_;
    std::thread::id owner_;
    std::shared_ptr<Pending> pending_;
    jobs::Group group_;
    bool ready_=false;
    void require_owner() const;
public:
    explicit GameplaySaveWriter(std::shared_ptr<jobs::Executor>,saves::FaultHook={});
    ~GameplaySaveWriter() noexcept;
    GameplaySaveWriter(const GameplaySaveWriter&)=delete;
    GameplaySaveWriter& operator=(const GameplaySaveWriter&)=delete;
    void submit(GameplaySaveWriteInput);
    // Re-observe the same retained receipt. Never writes or recaptures bytes.
    // Admission failure leaves the resolving result available for another try.
    void submit_resolution();
    bool busy() const;
    // Memory-only and nonblocking. nullptr means absent or still executing.
    // The returned pointer remains valid until clear, destruction or the next
    // resolution submission; do not retain it across those boundaries.
    const GameplaySaveWriteResult* poll();
    // Only definitive terminal results may be released. Resolving identity
    // cannot be discarded to admit a different save.
    void clear();
};
}
