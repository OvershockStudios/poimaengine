// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/assets.hpp"
namespace poima {
enum class FbxNormalConvention { opengl,directx };
// Restricted normalized FBX conversion. Geometryless animation donors are
// allowed here; ordinary cooked-model publication still requires geometry.
std::shared_ptr<const ModelAsset> import_fbx(const std::filesystem::path& source,FbxNormalConvention normals=FbxNormalConvention::opengl);
}
