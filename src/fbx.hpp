// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/assets.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
namespace poima {
enum class FbxNormalConvention { opengl,directx };
enum class FbxTextureDependencyReason { unavailable,empty,byte_budget,read_failed };
struct FbxTextureDependencyDiagnostic {
    // Original contained input with slash normalization, not a resolved OS path.
    std::string relative_path;
    FbxTextureDependencyReason reason;
    std::optional<std::uint64_t> file_bytes;
    std::uint64_t byte_limit;
    // Added only by the composition layer; the FBX importer leaves this unset.
    std::optional<std::size_t> donor_index{};
};
const char* fbx_texture_dependency_reason_name(FbxTextureDependencyReason reason) noexcept;
class FbxTextureDependencyError final : public std::runtime_error {
public:
    explicit FbxTextureDependencyError(FbxTextureDependencyDiagnostic diagnostic);
    const FbxTextureDependencyDiagnostic& diagnostic() const noexcept {return diagnostic_;}
private:
    FbxTextureDependencyDiagnostic diagnostic_;
};
// Restricted normalized FBX conversion. Geometryless animation donors are
// allowed here; ordinary cooked-model publication still requires geometry.
std::shared_ptr<const ModelAsset> import_fbx(const std::filesystem::path& source,FbxNormalConvention normals=FbxNormalConvention::opengl);
}
