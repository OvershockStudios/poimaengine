// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/assets.hpp"
namespace poima {
inline constexpr std::size_t max_model_skins=64,max_skin_joints=256,max_model_clips=256,max_animation_channels=8192,max_animation_keys=1000000;
struct NodePose { std::array<double,3> position{0,0,0},scale{1,1,1};std::array<double,4> rotation{0,0,0,1}; };
struct SkinBounds { std::vector<std::optional<Bounds>> joints;double weight_sum_error=0; };
SkinBounds skin_bounds(const MeshAsset& mesh);
Bounds posed_bounds(const SkinBounds& source,std::span<const Matrix4> palette);
struct ModelPose { double time=0;std::vector<NodePose> local;std::vector<Matrix4> world; };
// Shared import/package validation; sampling never trusts external key arrays.
void validate_animation_data(const ModelAsset& model);
ModelPose sample_model(const ModelAsset& model,std::optional<std::uint32_t> clip,double time,bool loop);
std::vector<Matrix4> skin_palette(const ModelAsset& model,const ModelPose& pose,std::uint32_t node);
// CPU reference for agent inspection/offline preview and future GPU comparison.
// This does not select CPU skinning as the production character renderer.
std::shared_ptr<const MeshAsset> deform_mesh(const MeshAsset& mesh,std::span<const Matrix4> palette);
}
