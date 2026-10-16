// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <filesystem>
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
struct StorageFailure final : std::runtime_error {
    const StorageOperation operation;
    const StorageStage stage;
    const std::optional<std::uint32_t> os_error;
    const std::optional<std::size_t> requested, written;
    const std::optional<std::uint32_t> close_error;
    StorageFailure(const char* message, StorageOperation operation_value, StorageStage stage_value,
        std::optional<std::uint32_t> error, std::optional<std::size_t> requested_value = {},
        std::optional<std::size_t> written_value = {}, std::optional<std::uint32_t> close_value = {})
        : std::runtime_error(message), operation(operation_value), stage(stage_value), os_error(error),
          requested(requested_value), written(written_value), close_error(close_value) {}
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
inline void append_storage_diagnostic(std::ostream& output, const StorageFailure& failure) {
    const auto number = [&](std::uintmax_t value) {
        char digits[std::numeric_limits<std::uintmax_t>::digits10 + 1];
        const auto converted = std::to_chars(digits, digits + sizeof digits, value);
        output.write(digits, converted.ptr - digits);
    };
    output << " [storage operation=" << storage_operation_name(failure.operation);
    if (failure.stage != StorageStage::unspecified) output << " stage=" << storage_stage_name(failure.stage);
    if (failure.os_error) { output << " win32_code="; number(*failure.os_error); }
    if (failure.requested) { output << " requested="; number(*failure.requested); }
    if (failure.written) { output << " written="; number(*failure.written); }
    if (failure.close_error) { output << " close_win32_code="; number(*failure.close_error); }
    output << ']';
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
}
