// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/ui.hpp"
#include "poima/ui_model.hpp"
#include "poima/collision_queries.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poima {
struct RenderOptions;
struct RenderReport;
// Column-major matrices multiplying column vectors. Right-handed, Y-up,
// camera looks along local -Z; projection depth is [0, 1].
using Matrix4 = std::array<double, 16>;
// Opaque, nonserialized identity for one presentation source lifetime.
std::string new_presentation_source_id();
Matrix4 identity_matrix();
Matrix4 multiply(const Matrix4& left, const Matrix4& right);
Matrix4 local_matrix(const std::array<double, 3>& position,
                     const std::array<double, 4>& rotation, const std::array<double, 3>& scale);
Matrix4 inverse_affine(const Matrix4& matrix);
Matrix4 perspective(double vertical_fov_degrees, double aspect, double near_plane, double far_plane);
bool rigid_transform(const Matrix4& matrix);

enum class ShadowFilter : std::uint32_t { pcf3_point=0, pcf3_fractional=1 };
constexpr std::string_view shadow_filter_name(ShadowFilter filter) {
    switch(filter) {
    case ShadowFilter::pcf3_point: return "pcf3_point";
    case ShadowFilter::pcf3_fractional: return "pcf3_fractional";
    }
    return "invalid";
}

enum class LightKind : std::uint32_t { directional=0,point=1,spot=2 };
struct ShadowSettings { bool enabled=false;float near_plane=.05f,distance=80,bias=.0005f,normal_bias=.01f; };
struct Light {
    LightKind kind=LightKind::point;
    std::array<float,3> color{1,1,1};
    float intensity=1,range=0,inner_angle=0,outer_angle=45;
    bool enabled=true;
    ShadowSettings shadow;
};
struct SkySettings {
    bool enabled=false;
    std::array<float,3> zenith{.06f,.22f,.55f},horizon{.55f,.70f,.85f},ground{.12f,.10f,.08f};
    float horizon_falloff=.35f;
    std::string sun; // Empty means no sun disk; otherwise an authored directional Light ID.
    float sun_size_degrees=.53f,sun_intensity=20; // Angular diameter; background radiance multiplier.
};
struct LightingEnvironment { std::array<float,3> ambient{};float exposure=1;std::uint32_t shadow_resolution=1024;SkySettings sky;ShadowFilter shadow_filter=ShadowFilter::pcf3_point; };
struct SceneLight { std::string entity_id;Light light;std::array<double,3> position{},direction{0,0,-1}; };
struct SceneLighting {
    std::vector<SceneLight> lights;
    LightingEnvironment environment;
    bool preview=true;
};
inline constexpr std::size_t max_scene_lights=1024;
inline constexpr std::size_t max_shadow_views=16;
inline constexpr std::size_t max_shadow_bytes=128*1024*1024;
std::size_t shadow_view_count(const Light& light);
void validate_shadow_budget(std::size_t views,std::uint32_t resolution);
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
struct Bounds { std::array<double,3> minimum{},maximum{}; };
using Frustum=std::array<std::array<double,4>,6>;
Bounds mesh_bounds(const MeshAsset* mesh); // null selects the built-in unit box
Bounds transform_bounds(const Bounds& local,const Matrix4& world);
Frustum make_frustum(const Matrix4& view_projection);
bool intersects(const Bounds& world,const Frustum& frustum);
struct MaterialTextures;
struct SkinPose { std::vector<Matrix4> palette; };
struct SceneLens {
    double vertical_fov=60,near_plane=.1,far_plane=1000;
    friend bool operator==(const SceneLens&,const SceneLens&)=default;
};
enum class CameraDrawMode { Viewmodel, Hidden };
struct CameraDrawBinding {
    std::string camera;
    CameraDrawMode mode=CameraDrawMode::Viewmodel;
};
struct SceneObject {
    std::string entity_id;
    Matrix4 world;
    std::array<float, 3> albedo;
    std::shared_ptr<const MeshAsset> mesh;
    std::optional<PbrMaterial> material;
    std::shared_ptr<const MaterialTextures> textures;
    std::shared_ptr<const SkinPose> skin{};
    // Zero opts out of temporal correspondence; authority changes this on replacement.
    std::uint64_t incarnation=0;
    // Per drawable; independent of visibility, materials and rig playback.
    bool cast_shadows=true,receive_shadows=true;
    // Camera exclusion is independent of frustum culling and shadow eligibility.
    bool camera_visible=true;
    std::optional<CameraDrawBinding> camera_draw;
};
struct SceneViewmodel { SceneLens lens;std::vector<SceneObject> objects; };
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
    std::shared_ptr<const UiFrame> ui;
    std::shared_ptr<const poima::ui::Presentation> logical_ui;
    // Independent of authored revision and simulation tick. Empty source opts out.
    std::string presentation_source_id;
    std::uint64_t presentation_generation=0;
    std::uint64_t view_cut_generation=0;
    // Session-local accepted scalar-material epoch, separate from authored/tick.
    std::uint64_t material_revision=0;
    std::optional<SceneViewmodel> viewmodel;
    // Optional native post-pose safety outcome, owned by this presentation.
    // Committed transforms, physics and saved game state remain untouched.
    std::optional<ViewmodelClearanceResult> viewmodel_clearance;
    // Frozen named-runtime baseline for composing gameplay zoom with a player's
    // preferred neutral FOV. External camera snapshots retain legacy omission.
    std::optional<double> authored_vertical_fov;
    // Opt-in receipt of actual prepared camera and foreground matrices.
    bool camera_pose_receipt=false;
};
struct ShadowView { Matrix4 view_projection;std::size_t light_index=0;double split_near=0,split_far=0; };
std::vector<ShadowView> shadow_views(const SceneSnapshot& scene,double aspect);
RenderReport run_render_scene(const RenderOptions& options, const SceneSnapshot& scene);
}
