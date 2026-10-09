// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/assets.hpp"
#include "fbx.hpp"
#include <optional>
#include <stdexcept>
#include <utility>

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
enum class AnimationCompositionIssueKind {
    missing_joint_or_ancestor, missing_animation_target, rest_frame
};
inline constexpr double animation_composition_translation_tolerance=1e-5;
inline constexpr double animation_composition_scale_tolerance=1e-6;
inline constexpr double animation_composition_quaternion_dot_min=1-1e-10;
inline constexpr std::size_t animation_composition_issue_limit=64;
struct AnimationCompositionIssue {
    AnimationCompositionIssueKind kind=AnimationCompositionIssueKind::rest_frame;
    std::optional<std::uint32_t> base_node,donor_node;
    std::string base_name,donor_name;
    bool required_skeleton=false,animated_ancestry=false;
    double max_translation_meters=0,max_scale_absolute=0,absolute_quaternion_dot=1,rotation_degrees=0;
    bool translation_mismatch=false,scale_mismatch=false,rotation_mismatch=false;
};
struct AnimationCompositionReport {
    std::size_t donor_index=0,required_base_nodes=0,checked_source_nodes=0,matched_nodes=0,issue_count=0;
    // At most 64 issues, with validated node names of at most 256 bytes each.
    // Missing nodes have no identity/name on the absent side. No source paths
    // or full hierarchy paths are retained in this diagnostic report.
    std::vector<AnimationCompositionIssue> issues;
    bool truncated=false;
};
class AnimationCompositionError final : public std::runtime_error {
public:
    AnimationCompositionError(const char* message,AnimationCompositionReport report)
        :std::runtime_error(message),report_(std::move(report)) {}
    const AnimationCompositionReport& report() const noexcept { return report_; }
private:
    AnimationCompositionReport report_;
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
