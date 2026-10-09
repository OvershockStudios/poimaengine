// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/assets.hpp"
#include "fbx.hpp"
#include <optional>

namespace poima {
struct ImportedModel {
    std::shared_ptr<const ModelAsset> model;
    // Empty selects the unchanged legacy glTF encoder identity.
    std::string importer;
};
struct ModelAnimationSource {
    std::filesystem::path source;
    std::optional<std::uint32_t> clip;
    std::string name;
};
ImportedModel import_model(const std::filesystem::path& source,FbxNormalConvention convention=FbxNormalConvention::opengl);
ImportedModel import_animation_source(const ModelAnimationSource& source,FbxNormalConvention convention);
std::size_t retained_model_import_bytes(const ModelAsset& model);
std::string encode_model_with_importer(const ModelAsset& model,const std::string& importer);
// Exact hierarchy/rest-frame composition, not humanoid retargeting. Returns a
// new model; primitive/image storage is shared and the inputs remain immutable.
std::shared_ptr<const ModelAsset> compose_model_animations(const ModelAsset& base,
    const std::vector<std::shared_ptr<const ModelAsset>>& donors);
}
