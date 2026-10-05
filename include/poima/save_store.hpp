// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

namespace poima::saves {
inline constexpr std::uint64_t maximum_checkpoint_bytes=64ULL*1024*1024;
enum class ErrorKind { invalid, conflict, reused_id, busy, corrupt, io, recovery_required };
class Error : public std::runtime_error {
public:
    ErrorKind kind;
    Error(ErrorKind value,const std::string& message):std::runtime_error(message),kind(value) {}
};
struct Generation {
    std::uint64_t generation=0,bytes=0;
    std::string sha256;
    bool verified=false;
};
struct Status {
    bool exists=false;
    // Latest generation known from the selected committed manifest. When
    // manifest_recovered is true a newer lost generation may have existed.
    std::uint64_t generation=0;
    std::optional<Generation> current,previous,selected,quarantined;
    bool recovered=false,manifest_recovered=false;
    std::string recovery_reason;
    std::size_t orphan_files=0;
};
struct ReadResult { Status status;std::string bytes; };
struct WriteResult {
    Status status;
    Generation committed; // Receipt generation; may precede status.generation.
    bool replayed=false;
    bool cleanup_pending=false;
};
struct ReceiptObservation {
    std::optional<WriteResult> receipt;
    // These describe the same locked manifest as receipt. Zero/zero denotes
    // no committed manifest. Absence outside this contiguous retained window
    // cannot prove that an uncertain write never committed.
    std::uint64_t current_generation=0,oldest_retained_generation=0;
};
enum class Boundary {
    payload_flushed,payload_directory_flushed,
    backup_flushed,backup_replaced,backup_directory_flushed,
    manifest_flushed,manifest_replaced,manifest_directory_flushed,pruned
};
using FaultHook=std::function<void(Boundary)>;

// One explicitly configured slot, no global paths or game-code execution.
// Operations serialize through a nonblocking OS lock. Inspect/read never create
// a missing slot; write may create its final directory under an existing parent.
// Checkpoint bytes are opaque. The caller validates runtime/content binding.
// FaultHook is for qualification and may throw or terminate the test process.
class Store {
    std::filesystem::path root_;
    FaultHook fault_;
public:
    explicit Store(std::filesystem::path root,FaultHook fault={});
    Status inspect() const;
    ReadResult read() const;
    ReceiptObservation observe_receipt(const std::string& operation_id,const std::string& request_sha256) const;
    std::optional<WriteResult> lookup(const std::string& operation_id,const std::string& request_sha256) const;
    WriteResult write(std::uint64_t expected_generation,const std::string& operation_id,
                      const std::string& bytes,bool acknowledge_recovery=false,
                      const std::string& request_sha256={}) const;
};
// Current-payload corruption can fall back to the verified prior generation;
// a subsequent write requires explicit acknowledgement. Corrupt current.json
// may use the previous committed manifest for reads only: writes always refuse
// until a new slot/manual repair, preserving potentially lost receipt history.
// Persistence uses file flushes + POSIX directory fsync. Windows replacement
// and device/filesystem power-loss guarantees require separate qualification.
// Acknowledged payload recovery preserves one quarantined failed generation.
// A second corruption requires a new slot/manual archive, never silent deletion.
}
