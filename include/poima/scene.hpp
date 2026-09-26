// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/core.hpp"
#include <array>
#include <memory>
#include <optional>

namespace poima {
// Column-major matrices multiplying column vectors. Right-handed, Y-up,
// camera looks along local -Z; projection depth is [0, 1].
using Matrix4 = std::array<double, 16>;
Matrix4 identity_matrix();
Matrix4 multiply(const Matrix4& left, const Matrix4& right);
Matrix4 local_matrix(const std::array<double, 3>& position,
                     const std::array<double, 4>& rotation, const std::array<double, 3>& scale);
Matrix4 inverse_affine(const Matrix4& matrix);
Matrix4 perspective(double vertical_fov_degrees, double aspect, double near_plane, double far_plane);
bool rigid_transform(const Matrix4& matrix);

struct PbrMaterial {
    std::array<float,3> base_color{1,1,1}, emissive{0,0,0};
    float metallic=1, roughness=1;
    bool double_sided=false;
};
struct MeshAsset;
struct SceneObject {
    std::string entity_id;
    Matrix4 world;
    std::array<float, 3> albedo;
    std::shared_ptr<const MeshAsset> mesh;
    std::optional<PbrMaterial> material;
};
// An immutable presentation copy; no pointers into authored or simulation state.
struct SceneSnapshot {
    std::string world_id;
    std::uint64_t revision = 0;
    std::string camera_id;
    Matrix4 camera_world;
    double vertical_fov = 60;
    double near_plane = 0.1;
    double far_plane = 1000;
    std::vector<SceneObject> objects;
};
RenderReport run_render_scene(const RenderOptions& options, const SceneSnapshot& scene);
}
