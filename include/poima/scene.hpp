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

enum class LightKind : std::uint32_t { directional=0,point=1,spot=2 };
struct Light {
    LightKind kind=LightKind::point;
    std::array<float,3> color{1,1,1};
    float intensity=1,range=0,inner_angle=0,outer_angle=45;
    bool enabled=true;
};
struct LightingEnvironment { std::array<float,3> ambient{};float exposure=1; };
struct SceneLight { std::string entity_id;Light light;std::array<double,3> position{},direction{0,0,-1}; };
struct SceneLighting {
    std::vector<SceneLight> lights;
    LightingEnvironment environment;
    bool preview=true;
};
inline constexpr std::size_t max_scene_lights=64;
void validate_light(const Light& light);
void validate_environment(const LightingEnvironment& environment);
void append_light(SceneLighting& state,const std::string& id,const Light& light,const Matrix4& world);
void finalize_lighting(SceneLighting& state);
struct PbrMaterial {
    std::array<float,3> base_color{1,1,1}, emissive{0,0,0};
    float metallic=1, roughness=1;
    bool double_sided=false;
};
struct MeshAsset;
struct MaterialTextures;
struct SceneObject {
    std::string entity_id;
    Matrix4 world;
    std::array<float, 3> albedo;
    std::shared_ptr<const MeshAsset> mesh;
    std::optional<PbrMaterial> material;
    std::shared_ptr<const MaterialTextures> textures;
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
    SceneLighting lighting;
};
RenderReport run_render_scene(const RenderOptions& options, const SceneSnapshot& scene);
}
