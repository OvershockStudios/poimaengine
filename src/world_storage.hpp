// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <filesystem>
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

inline void write_flushed(const fs::path& path, const std::string& bytes) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create world staging file.");
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        && written == bytes.size() && FlushFileBuffers(file);
    const bool closed = CloseHandle(file) != 0;
    if (!ok || !closed) throw std::runtime_error("Cannot flush world staging file.");
#else
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

inline void replace_file(const fs::path& source, const fs::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish world snapshot.");
#else
    fs::rename(source, destination);
#endif
    // Atomic name replacement is qualified; power-loss directory durability is
    // not promised by this initial implementation.
}
}
