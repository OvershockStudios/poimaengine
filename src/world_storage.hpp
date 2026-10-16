// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <filesystem>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <string_view>
#include <cstdint>
#include <charconv>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace poima::world_detail {
namespace fs = std::filesystem;
inline constexpr std::size_t max_document_bytes = 16 * 1024 * 1024;

// These diagnostics never replace the historical exception message or public
// JSON-RPC error. Stages are supplied only by callers that know the boundary.
enum class StorageOperation { create, write, short_write, flush, close, replace };
enum class StorageStage { unspecified, primary_staging, previous_staging, previous_publish, primary_publish };
struct StorageRetryFacts {
    std::uint32_t attempts = 1;
    std::optional<std::uint32_t> first_error, last_error;
    std::uint32_t scheduled_wait_ms = 0;
    // Measured from the first captured failure, not a hard syscall deadline.
    std::uint64_t elapsed_us = 0;
};
// Receives only real failed WinAPI observations. Production logs the first
// eligible failure; tests may release an owned blocker after an actual failure.
// Neither observer replaces the operation or manufactures its result.
struct StorageRetryObserver {
    void* context = nullptr;
    void (*failed)(void*, StorageStage, const StorageRetryFacts&) noexcept = nullptr;
};
inline constexpr std::uint32_t world_publication_max_attempts = 6;
inline constexpr std::uint32_t world_publication_retry_budget_ms = 100;
inline constexpr bool world_publication_retryable(StorageStage stage, std::uint32_t code) noexcept {
    return (stage == StorageStage::previous_publish || stage == StorageStage::primary_publish) &&
        (code == 5 || code == 32 || code == 33);
}
inline constexpr std::uint32_t world_publication_wait_ms(std::uint32_t failed_attempts,
    std::uint64_t elapsed_us = 0) noexcept {
    constexpr auto budget_us = std::uint64_t{world_publication_retry_budget_ms} * 1000;
    if (failed_attempts < 1 || failed_attempts >= world_publication_max_attempts || elapsed_us >= budget_us) return 0;
    const auto wait = std::uint32_t{1} << failed_attempts;
    return std::uint64_t{wait} * 1000 < budget_us - elapsed_us ? wait : 0;
}
struct StorageFailure final : std::runtime_error {
    const StorageOperation operation;
    const StorageStage stage;
    const std::optional<std::uint32_t> os_error;
    const std::optional<std::size_t> requested, written;
    const std::optional<std::uint32_t> close_error;
    const std::optional<StorageRetryFacts> retry;
    StorageFailure(const char* message, StorageOperation operation_value, StorageStage stage_value,
        std::optional<std::uint32_t> error, std::optional<std::size_t> requested_value = {},
        std::optional<std::size_t> written_value = {}, std::optional<std::uint32_t> close_value = {},
        std::optional<StorageRetryFacts> retry_value = {})
        : std::runtime_error(message), operation(operation_value), stage(stage_value), os_error(error),
          requested(requested_value), written(written_value), close_error(close_value), retry(retry_value) {}
};
inline const char* storage_operation_name(StorageOperation operation) noexcept {
    switch (operation) {
    case StorageOperation::create: return "CreateFileW";
    case StorageOperation::write: return "WriteFile";
    case StorageOperation::short_write: return "short_write";
    case StorageOperation::flush: return "FlushFileBuffers";
    case StorageOperation::close: return "CloseHandle";
    case StorageOperation::replace: return "MoveFileExW";
    }
    return "unknown";
}
inline const char* storage_stage_name(StorageStage stage) noexcept {
    switch (stage) {
    case StorageStage::unspecified: return "unspecified";
    case StorageStage::primary_staging: return "primary_staging";
    case StorageStage::previous_staging: return "previous_staging";
    case StorageStage::previous_publish: return "previous_publish";
    case StorageStage::primary_publish: return "primary_publish";
    }
    return "unknown";
}
inline void append_storage_number(std::ostream& output, std::uintmax_t value) {
    char digits[std::numeric_limits<std::uintmax_t>::digits10 + 1];
    const auto converted = std::to_chars(digits, digits + sizeof digits, value);
    output.write(digits, converted.ptr - digits);
}
inline void append_storage_retry(std::ostream& output, const StorageRetryFacts& facts) {
    output << " retry_attempts="; append_storage_number(output, facts.attempts);
    if (facts.first_error) { output << " first_win32_code="; append_storage_number(output, *facts.first_error); }
    if (facts.last_error) { output << " last_win32_code="; append_storage_number(output, *facts.last_error); }
    output << " scheduled_wait_ms="; append_storage_number(output, facts.scheduled_wait_ms);
    output << " retry_elapsed_us="; append_storage_number(output, facts.elapsed_us);
}
inline void append_storage_diagnostic(std::ostream& output, const StorageFailure& failure) {
    output << " [storage operation=" << storage_operation_name(failure.operation);
    if (failure.stage != StorageStage::unspecified) output << " stage=" << storage_stage_name(failure.stage);
    if (failure.os_error) { output << " win32_code="; append_storage_number(output, *failure.os_error); }
    if (failure.requested) { output << " requested="; append_storage_number(output, *failure.requested); }
    if (failure.written) { output << " written="; append_storage_number(output, *failure.written); }
    if (failure.close_error) { output << " close_win32_code="; append_storage_number(output, *failure.close_error); }
    if (failure.retry) append_storage_retry(output, *failure.retry);
    output << ']';
}
inline void report_world_publication_retry_pending(std::ostream& output, StorageStage stage,
    const StorageRetryFacts& facts) noexcept {
    if (facts.attempts != 1 || !facts.last_error || !world_publication_retryable(stage, *facts.last_error)) return;
    try {
        output << "World publication retry pending: [storage operation=MoveFileExW stage=" << storage_stage_name(stage);
        output << " win32_code="; append_storage_number(output, *facts.last_error);
        output << " retry_attempts=1]\n";
    } catch (...) {}
}
inline void report_world_publication_recovery(std::ostream& output, StorageStage stage,
    const StorageRetryFacts& facts) noexcept {
    if (facts.attempts <= 1) return;
    // Logging must never turn an already published rename into an apparent
    // failed transaction or prevent the following noexcept state swaps.
    try {
        output << "World publication recovered: [storage operation=MoveFileExW stage=" << storage_stage_name(stage);
        append_storage_retry(output, facts); output << "]\n";
    } catch (...) {}
}

// Retry-only observation: bounded streaming equality, without allocating a
// second16MiB document. Inability to prove equality is a conflict, not consent
// to overwrite. This is not a filesystem identity/TOCTOU guarantee.
inline bool world_publication_bytes_equal(const fs::path& path, std::string_view expected) noexcept {
    if (expected.size() > max_document_bytes) return false;
    try {
        std::error_code error;
        const auto size = fs::file_size(path, error);
        if (error || size != expected.size() || size > max_document_bytes) return false;
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        std::array<char, 65536> buffer{}; std::size_t offset = 0;
        while (offset < expected.size()) {
            const auto requested = std::min(buffer.size(), expected.size() - offset);
            input.read(buffer.data(), static_cast<std::streamsize>(requested));
            const auto count = static_cast<std::size_t>(input.gcount());
            if (count != requested || !std::equal(buffer.data(), buffer.data() + count, expected.data() + offset)) return false;
            offset += count;
        }
        // Recheck size rather than reading an extra chunk past the16MiB cap.
        const auto final_size = fs::file_size(path, error);
        return !input.bad() && !error && final_size == expected.size();
    } catch (...) { return false; }
}
inline bool world_publication_unchanged(const fs::path& primary, bool existed,
    std::string_view original, const fs::path& source, std::string_view prepared) noexcept {
    if (original.size() > max_document_bytes || prepared.size() > max_document_bytes) return false;
    std::error_code error;
    const bool exists = fs::exists(primary, error);
    return !error && exists == existed && (!existed || world_publication_bytes_equal(primary, original)) &&
        world_publication_bytes_equal(source, prepared);
}

inline bool same_path_name(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return left == right;
#endif
}

// An OS-held, cooperative, per-document writer lock. A process crash releases
// ownership. The empty sidecar remains; its existence is not the lock state.
class WriterLock {
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int handle_ = -1;
#endif
public:
    explicit WriterLock(const fs::path& path) {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("World is locked or lock path is unavailable.");
#else
        handle_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (handle_ < 0) throw std::runtime_error("World lock path is unavailable.");
        if (flock(handle_, LOCK_EX | LOCK_NB) != 0) {
            ::close(handle_); handle_ = -1;
            throw std::runtime_error("World is already open by another writer.");
        }
#endif
    }
    WriterLock(const WriterLock&) = delete;
    WriterLock& operator=(const WriterLock&) = delete;
    ~WriterLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (handle_ >= 0) ::close(handle_);
#endif
    }
};

inline void write_flushed(const fs::path& path, const std::string& bytes,
    StorageStage stage = StorageStage::unspecified) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        throw StorageFailure("Cannot create world staging file.", StorageOperation::create, stage, code);
    }
    DWORD written = 0;
    std::optional<StorageOperation> failed;
    std::optional<std::uint32_t> code;
    if (!WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) {
        code = GetLastError();
        failed = StorageOperation::write;
    } else if (written != bytes.size()) {
        // A successful short write has no failed-call GetLastError value.
        failed = StorageOperation::short_write;
    } else if (!FlushFileBuffers(file)) {
        code = GetLastError();
        failed = StorageOperation::flush;
    }
    const bool closed = CloseHandle(file) != 0;
    const auto close_code = closed ? std::optional<std::uint32_t>{}
        : std::optional<std::uint32_t>{GetLastError()};
    if (failed) throw StorageFailure("Cannot flush world staging file.", *failed, stage,
        code, bytes.size(), written, close_code);
    if (!closed) throw StorageFailure("Cannot flush world staging file.", StorageOperation::close, stage,
        close_code, bytes.size(), written);

#else
    (void)stage;
    int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (file < 0) throw std::runtime_error("Cannot create world staging file.");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto n = ::write(file, bytes.data() + offset, bytes.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ::close(file); throw std::runtime_error("Cannot write world staging file."); }
        offset += static_cast<std::size_t>(n);
    }
    const bool ok = fsync(file) == 0;
    const bool closed = ::close(file) == 0;
    if (!ok || !closed) throw std::runtime_error("Cannot flush world staging file.");
#endif
}

inline void replace_file(const fs::path& source, const fs::path& destination,
    StorageStage stage = StorageStage::unspecified) {
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto code = GetLastError();
        throw StorageFailure("Cannot publish world snapshot.", StorageOperation::replace, stage, code);
    }
#else
    (void)stage;
    fs::rename(source, destination);
#endif
    // Atomic name replacement is qualified; power-loss directory durability is
    // not promised by this initial implementation.
}

// Only authored-world publication opts in. Shared asset/settings/editor users
// continue to call the unchanged one-attempt replace_file above.
template<class Guard>
inline StorageRetryFacts replace_world_publication(const fs::path& source, const fs::path& destination,
    StorageStage stage, Guard&& guard, const StorageRetryObserver* observer = nullptr) {
#ifdef _WIN32
    if (stage != StorageStage::previous_publish && stage != StorageStage::primary_publish) {
        replace_file(source, destination, stage); return {};
    }
    StorageRetryFacts facts;
    if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return facts;
    const auto first = static_cast<std::uint32_t>(GetLastError());
    facts.first_error = first; facts.last_error = first;
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    const auto deadline = started + std::chrono::milliseconds(world_publication_retry_budget_ms);
    const auto elapsed = [&] {
        const auto value = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
        facts.elapsed_us = value > 0 ? static_cast<std::uint64_t>(value) : 0;
    };
    for (;;) {
        elapsed();
        if (observer && observer->failed) observer->failed(observer->context, stage, facts);
        elapsed();
        const auto wait = world_publication_wait_ms(facts.attempts, facts.elapsed_us);
        if (!world_publication_retryable(stage, *facts.last_error) || wait == 0) break;
        auto now = Clock::now();
        if (now >= deadline || std::chrono::milliseconds(wait) >= deadline - now) break;
        facts.scheduled_wait_ms += wait;
        Sleep(wait);
        if (Clock::now() >= deadline) break;
        guard(); // Caller preserves its existing -32009 conflict behavior.
        if (Clock::now() >= deadline) break;
        ++facts.attempts;
        if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            elapsed(); return facts;
        }
        facts.last_error = static_cast<std::uint32_t>(GetLastError());
    }
    elapsed();
    throw StorageFailure("Cannot publish world snapshot.", StorageOperation::replace, stage,
        facts.last_error, {}, {}, {}, facts);
#else
    (void)guard; (void)observer;
    replace_file(source, destination, stage); return {};
#endif
}

}
