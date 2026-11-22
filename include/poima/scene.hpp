// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/ui.hpp"
#include "poima/ui_model.hpp"
#include "poima/collision_queries.hpp"
#include "poima/foliage_motion.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace poima {
struct RenderOptions;
struct RenderReport;
struct PostEffect;
struct TextureImage;
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

enum class ShadowFilter : std::uint32_t { pcf3_point=0, pcf3_fractional=1, pcss_directional=2 };
constexpr std::string_view shadow_filter_name(ShadowFilter filter) {
    switch(filter) {
    case ShadowFilter::pcf3_point: return "pcf3_point";
    case ShadowFilter::pcf3_fractional: return "pcf3_fractional";
    case ShadowFilter::pcss_directional: return "pcss_directional";
    }
    return "invalid";
}

enum class LightKind : std::uint32_t { directional=0,point=1,spot=2 };
struct ShadowSettings {
    bool enabled=false;float near_plane=.05f,distance=80,bias=.0005f,normal_bias=.01f;
    // Directional emitter diameter; omission uses .53 degrees in PCSS only.
    std::optional<float> angular_size_degrees;
};
inline constexpr std::uint32_t contact_shadow_blocker_samples=16;
inline constexpr std::uint32_t contact_shadow_filter_samples=16;
// Each filter location evaluates four comparison texels before interpolation.
inline constexpr std::uint32_t contact_shadow_max_comparisons=64;
inline constexpr double contact_shadow_max_radius_texels=16;
inline constexpr double contact_shadow_guard_texels=18;
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
struct IblSettings {
    bool enabled=false;
    float diffuse_intensity=1,specular_intensity=1;
};
struct BloomSettings {
    float intensity=0;
    float threshold=1;
    float radius=8;
};
inline constexpr std::size_t output_grade_knot_count=33;
struct OutputGrade {
    float input_max=4,strength=1;
    std::array<std::array<float,3>,output_grade_knot_count> knots{};
};
struct ScreenReflections {
    float strength=0,max_distance_m=8,thickness_m=.1f;
};
struct FogSpotLight {
    std::string entity;
    float scattering=0;
};
struct HeightFog {
    float density_per_m=0,base_height_m=0,height_falloff_per_m=0;
    float start_distance_m=0,max_distance_m=100;
    std::array<float,3> ambient_radiance{};
    float anisotropy=0;
    std::vector<FogSpotLight> spot_lights;
};
struct AutoExposureSettings {
    float min_stops=-4,max_stops=4;
    float brighten_stops_per_second=2,darken_stops_per_second=4;
};
struct LightingEnvironment {
    std::array<float,3> ambient{};
    float exposure=1;
    std::uint32_t shadow_resolution=1024;
    SkySettings sky;
    ShadowFilter shadow_filter=ShadowFilter::pcf3_point;
    std::optional<IblSettings> ibl;
    BloomSettings bloom;
    std::string post_effect_asset;
    // Runtime lighting retains the artifact admitted when the scene was frozen.
    std::shared_ptr<const PostEffect> post_effect;
    std::optional<OutputGrade> output_grade;
    std::optional<ScreenReflections> screen_reflections;
    std::optional<HeightFog> height_fog;
    std::optional<AutoExposureSettings> auto_exposure;
};
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
enum class MaterialAlphaMode : std::uint32_t { opaque=0,mask=1 };
constexpr std::string_view material_alpha_mode_name(MaterialAlphaMode mode) {
    switch(mode) {
    case MaterialAlphaMode::opaque:return "opaque";
    case MaterialAlphaMode::mask:return "mask";
    }
    return "invalid";
}
inline constexpr float max_material_alpha_cutoff=1e6f;
struct PbrMaterial {
    std::array<float,3> base_color{1,1,1}, emissive{0,0,0};
    float metallic=1, roughness=1;
    bool double_sided=false;
    MaterialAlphaMode alpha_mode=MaterialAlphaMode::opaque;
    float opacity=1,alpha_cutoff=.5f;
};
void validate_pbr_material(const PbrMaterial& material);
void validate_material_alpha(const PbrMaterial& material);
struct MeshAsset;
struct Bounds { std::array<double,3> minimum{},maximum{}; };
using Frustum=std::array<std::array<double,4>,6>;
Bounds mesh_bounds(const MeshAsset* mesh); // null selects the built-in unit box
Bounds transform_bounds(const Bounds& local,const Matrix4& world);
Bounds foliage_displaced_bounds(Bounds world_bounds, const Matrix4& world, const FoliageMotionSample& pose);
Frustum make_frustum(const Matrix4& view_projection);
bool intersects(const Bounds& world,const Frustum& frustum);
struct MaterialTextures;
struct SkinPose { std::vector<Matrix4> palette; };
struct SceneLens {
    double vertical_fov=60,near_plane=.1,far_plane=1000;
    friend bool operator==(const SceneLens&,const SceneLens&)=default;
};
enum class CameraDrawMode { Viewmodel, Hidden };
enum class ViewmodelProjection { Independent, MatchWorldFov, WorldDepth };
struct CameraDrawBinding {
    std::string camera;
    CameraDrawMode mode=CameraDrawMode::Viewmodel;
    bool scene_anchored=false;
};
inline constexpr std::size_t max_surface_marks_per_receiver=4;
inline constexpr std::size_t max_scene_surface_marks=256;
inline constexpr std::size_t max_surface_mark_profiles=32;
inline constexpr std::size_t max_surface_mark_masks=16;
inline constexpr std::uint32_t max_surface_mark_mask_side=256;
inline constexpr float max_surface_mark_relief_depth_m=.025f;
struct SceneSurfaceMarkMask {
    std::string sha256;
    std::shared_ptr<const TextureImage> image;
};
struct SceneSurfaceMark {
    // Position and orthonormal projection axes in rigid mesh-local space.
    std::array<float,3> center_m{},normal{0,0,1},tangent{1,0,0};
    std::array<float,2> half_extent_m{.1f,.1f};
    float half_depth_m=.01f,minimum_normal_dot=.8f,edge_fraction=.25f;
    std::array<float,3> linear_rgb{.2f,.2f,.2f};
    float opacity=1,roughness=1;
    std::string mask_sha256; // Empty is the analytic ellipse.
    float relief_depth_m=0;

    bool operator==(const SceneSurfaceMark&) const=default;
};
struct SceneWetness {
    float base_scale=1, roughness_scale=1;

    bool operator==(const SceneWetness&) const=default;
};
inline constexpr std::size_t max_mesh_lod_levels=4;
inline constexpr double max_mesh_lod_distance_m=1000000;
struct MeshLodLevel {
    double min_distance_m=0;
    std::shared_ptr<const MeshAsset> mesh;
};
using MeshLodChain=std::vector<MeshLodLevel>;

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
    // Indexed opaque world geometry replaced by the next presentation.
    bool stream_geometry=false;
    std::vector<SceneSurfaceMark> surface_marks{};
    // Fixed-topology terrain whose current and preceding positions belong to frame slots.
    bool mutable_geometry=false;
    // Linear factors applied to the drawable's base tint and roughness.
    std::optional<SceneWetness> wetness{};
    // Additional fixed rigid geometry. The base mesh always owns material inputs.
    std::shared_ptr<const MeshLodChain> lods{};
    bool water_ripple_surface=false;
    std::optional<FoliageMotionSample> foliage_motion{};
};
struct SceneViewmodel {
    SceneLens lens;
    std::vector<SceneObject> objects;
    ViewmodelProjection projection=ViewmodelProjection::Independent;

    SceneLens effective_lens(SceneLens world) const {
        if(projection==ViewmodelProjection::WorldDepth)return world;
        return {projection==ViewmodelProjection::MatchWorldFov ? world.vertical_fov : lens.vertical_fov,
            lens.near_plane,lens.far_plane};
    }
};
inline constexpr std::size_t max_scene_particle_sprite_textures=16;
inline constexpr std::uint32_t max_scene_particle_sprite_texture_side=1024;
inline constexpr std::size_t max_scene_particle_sprite_texture_bytes=16*1024*1024;
struct SceneParticleSpriteStyle {
    std::string texture_sha256;
    std::shared_ptr<const TextureImage> texture;
    float depth_fade_m=0;
};
struct SceneParticleSprite {
    std::uint64_t id=0;
    std::uint32_t style=0;
    std::array<double,3> position_m{};
    float size_m=0;
    std::array<float,4> linear_rgba{};
    std::array<std::uint32_t,4> texel_rect{}; // Base mip x, y, width, height.
};
struct SceneParticleSprites {
    // Includes frozen root and template styles even with no live instances.
    std::vector<SceneParticleSpriteStyle> styles;
    std::vector<SceneParticleSprite> instances;
};
struct SceneWaterRipples {
    std::array<float,2> direction{1,0};
    float slope=0,radians_per_metre=0,phase_radians=0;
};
inline constexpr std::size_t max_scene_water_wave_layers=4;
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
    std::shared_ptr<const PostEffect> post_effect;
    SceneParticleSprites particle_sprites;
    std::vector<SceneSurfaceMarkMask> surface_mark_masks; // Frozen, sorted by unique cooked image digest.
    std::optional<SceneWaterRipples> water_ripples{};
    std::array<SceneWaterRipples,max_scene_water_wave_layers> water_wave_layers{};
    std::size_t water_wave_layer_count=0;

    SceneLens world_lens() const { return {vertical_fov,near_plane,far_plane}; }
    std::span<const SceneObject> world_viewmodel_draws() const {
        if(viewmodel && viewmodel->projection==ViewmodelProjection::WorldDepth)return viewmodel->objects;
        return {};
    }
    std::size_t world_object_count() const { return objects.size()+world_viewmodel_draws().size(); }
};
struct ShadowView {
    Matrix4 view_projection;std::size_t light_index=0;double split_near=0,split_far=0;
    // Actual orthographic light-depth span and expanded cascade texel scale.
    // Local-light views retain zero; camera split depths cannot replace these.
    double depth_span=0,world_units_per_texel=0;
};
std::vector<ShadowView> shadow_views(const SceneSnapshot& scene,double aspect,
                                  std::optional<ShadowFilter> filter_override={});
RenderReport run_render_scene(const RenderOptions& options, const SceneSnapshot& scene);
}
