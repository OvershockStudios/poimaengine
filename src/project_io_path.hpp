// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <filesystem>
#include <stdexcept>

namespace poima::project_detail {
namespace fs=std::filesystem;
// Only native filesystem calls receive this spelling. Logical paths, journal
// selectors and returned identities retain their ordinary absolute spelling.
inline fs::path project_io_path(const fs::path& path) {
#ifdef _WIN32
    // Filesystem iterators can already return an extended IO spelling. Do not
    // apply ordinary-path normalization to that namespace a second time.
    if(path.native().starts_with(L"\\\\?\\"))return path;
    auto native=fs::absolute(path).lexically_normal().make_preferred().native();
    if(native.starts_with(L"\\\\.\\"))throw std::runtime_error("Device namespace paths are not project files.");
    if(native.starts_with(L"\\\\"))return fs::path(L"\\\\?\\UNC\\"+native.substr(2));
    if(native.size()<3 || native[1]!=L':' || native[2]!=L'\\')throw std::runtime_error("Expected a fully qualified Windows filesystem path.");
    return fs::path(L"\\\\?\\"+native);
#else
    return path;
#endif
}

inline fs::path canonical_project_path(const fs::path& path) {
    const auto canonical=fs::canonical(project_io_path(path));
#ifdef _WIN32
    const auto& native=canonical.native();
    if(native.starts_with(L"\\\\?\\UNC\\"))return fs::path(L"\\\\"+native.substr(8));
    if(native.starts_with(L"\\\\?\\"))return fs::path(native.substr(4));
#endif
    return canonical;
}
}
