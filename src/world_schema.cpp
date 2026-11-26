// SPDX-License-Identifier: Apache-2.0
#include "world_schema.hpp"
#include "world_schema_asset.hpp"
#include "poima/gameplay_compatibility.hpp"
#include "poima/runtime.hpp"
#include "poima/snow.hpp"
#include "poima/snow_releases.hpp"
#include "poima/surface_wetness.hpp"
#include "physical_rig_json.hpp"
#include "pose_frames.hpp"
#include "poima/scene.hpp"
#include "surface_mark_validation.hpp"
#include "audio_buses.hpp"
#include "poima/terrain_asset.hpp"
#include "foliage_authoring.hpp"
#include "water_asset.hpp"
#include "particle_emitter_json.hpp"
#include "input_profile_store.hpp"
#include "player_settings_store.hpp"
#include "navigation_schema.hpp"
#include "asset_provenance.hpp"
#include "authoring_reference.hpp"
#include "discovery_selection.hpp"
#include "ui_authoring_schema.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <string_view>
#include <utility>

namespace poima::world_schema {
using detail::object_schema;
using detail::vector;

using Json = nlohmann::json;
Json describe(const Json& sky_defaults, std::uint64_t max_revision, std::size_t max_document_bytes) {
    const Json id = {{"type", "string"}, {"pattern", "^[0-9a-f]{32}$"}};
    auto unsigned_range = [](std::uint32_t first,std::uint32_t last) {
        return Json{{"type","integer"},{"minimum",first},{"maximum",last}};
    };
    auto snow_tile_id=id;
    snow_tile_id["not"]={{"const","00000000000000000000000000000000"}};
    const Json rev = {{"type", "integer"}, {"minimum", 0}, {"maximum", max_revision}};
    const Json name = {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}, {"description", "Also limited to 256 UTF-8 bytes."}};
    const Json parent = {{"anyOf", {id, Json{{"type", "null"}}}}};
    const Json number = {{"type", "number"}, {"minimum", -1e9}, {"maximum", 1e9}};
    const Json transform = object_schema({{"position", vector(number, 3)}, {"rotation", vector(number, 4)},
        {"scale", vector({{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1e9}}, 3)}}, {"position", "rotation", "scale"});
    const Json component_type = {{"enum", {"Transform", "Camera", "CameraDraw", "MeshRenderer", "BoxCollider", "MeshCollider", "CharacterController", "StaticMesh", "PbrMaterial", "PbrTextures", "Light", "LightingEnvironment", "AcousticMaterial", "AudioRoom", "AudioPortal", "AudioEmitter", "AnimationRig", "PhysicalRig", "RigNode", "SkinnedMesh", "HitRegion", "ParticleEmitter", "FootprintSurface", "SurfaceWetness"}}};
    const Json lens = object_schema({{"vertical_fov", {{"type", "number"}, {"minimum", 5}, {"maximum", 150}}},
        {"near", {{"type", "number"}, {"minimum", 0.001}}}, {"far", {{"type", "number"}, {"maximum", 1e7}}}}, {"vertical_fov", "near", "far"});
    auto camera=lens;camera["properties"]["viewmodel"]=lens;
    auto focus_entity=id;focus_entity["not"]={{"const",std::string(32,'0')}};
    camera["properties"]["focus_target"]=object_schema({{"entity",focus_entity},
        {"local_offset_m",vector({{"type","number"},{"minimum",-1000},{"maximum",1000}},3)}},{"entity"});
    camera["properties"]["focus_target"]["description"]="Track another entity's local point when the scene selects depth of field. A point outside the camera clip range uses the authored environment distance.";
    const Json clearance=object_schema({
        {"position",vector({{"type","number"},{"minimum",-10},{"maximum",10}},3)},
        {"rotation",vector({{"type","number"},{"minimum",-1.00001},{"maximum",1.00001}},4)},
        {"radius",{{"type","number"},{"minimum",.001},{"maximum",10}}},
        {"half_height",{{"type","number"},{"minimum",0},{"maximum",10}}},
        {"max_retraction",{{"type","number"},{"minimum",.001},{"maximum",10}}},
        {"skin",{{"type","number"},{"minimum",.0001},{"maximum",10}}}},
        {"position","rotation","radius","half_height","max_retraction","skin"});
    camera["properties"]["viewmodel_clearance"]=clearance;
    camera["properties"]["viewmodel_match_world_fov"]={{"type","boolean"},{"default",false},
        {"description","Use the effective world field of view for the foreground pass, with its own clip planes and depth."}};
    camera["properties"]["viewmodel_world_depth"]={{"type","boolean"},{"default",false},
        {"description","Render this camera's viewmodels in the world pass, sharing projection, clip planes and depth."}};
    camera["dependentRequired"]={{"viewmodel_clearance",{"viewmodel"}},{"viewmodel_match_world_fov",{"viewmodel"}},{"viewmodel_world_depth",{"viewmodel"}}};
    auto draw_camera=id;draw_camera["not"]={{"const",std::string(32,'0')}};
    auto camera_draw=object_schema({{"camera",draw_camera},{"mode",{{"enum",{"viewmodel","hidden"}}}},
        {"scene_anchored",{{"type","boolean"},{"default",false},
            {"description","Keep the authored scene hierarchy independent of the selected camera's ancestry."}}}},{"camera","mode"});
    camera_draw["if"]={{"required",{"scene_anchored"}},{"properties",{{"scene_anchored",{{"const",true}}}}}};
    camera_draw["then"]={{"properties",{{"mode",{{"const","viewmodel"}}}}}};
    const Json mesh = object_schema({{"primitive", {{"const", "box"}}}, {"albedo", vector({{"type", "number"}, {"minimum", 0}, {"maximum", 1}}, 3)},
        {"visible", {{"type", "boolean"}}},
        {"cast_shadows",{{"type","boolean"},{"default",true}}},{"receive_shadows",{{"type","boolean"},{"default",true}}}}, {"primitive", "albedo", "visible"});
    const Json collision_surface={{"type","string"},{"pattern","^[a-z][a-z0-9_.-]{0,31}$"},
        {"description","Semantic surface name on this collision shape, independent of its render material."}};
    const auto buoyancy=object_schema({
        {"linear_drag",{{"type","number"},{"minimum",0},{"maximum",2},{"default",.6}}},
        {"angular_drag",{{"type","number"},{"minimum",0},{"maximum",2},{"default",.2}}}},{});
    Json collider = object_schema({{"half_extents", vector({{"type","number"},{"minimum",0.001},{"maximum",10000}},3)},
        {"motion",{{"enum",{"static","dynamic","kinematic"}}}}, {"mass",{{"type","number"},{"exclusiveMinimum",0},{"maximum",1e6}}},
        {"friction",{{"type","number"},{"minimum",0},{"maximum",2}}}, {"restitution",{{"type","number"},{"minimum",0},{"maximum",1}}},
        {"surface",collision_surface},{"buoyancy",buoyancy},{"navigation_obstacle",{{"type","boolean"},{"default",false},{"description","Exclude this authored box from the static navigation bake and filter runtime routes using its live physics bounds."}}}},
        {"half_extents","motion","mass","friction","restitution"});
    collider["if"]={{"required",{"navigation_obstacle"}},{"properties",{{"navigation_obstacle",{{"const",true}}}}}};
    collider["then"]={{"properties",{{"motion",{{"enum",{"static","kinematic"}}}}}}};
    collider["allOf"]={Json{{"if",{{"required",{"buoyancy"}}}},
        {"then",{{"properties",{{"motion",{{"const","dynamic"}}},{"mass",{{"minimum",.001}}}}}}}}};
    const Json stance=object_schema({{"crouched_height",{{"type","number"},{"exclusiveMinimum",0},{"maximum",4},{"default",1}}},
        {"crouched_speed",{{"type","number"},{"exclusiveMinimum",0},{"maximum",30},{"default",2}}},
        {"camera_drop",{{"type","number"},{"minimum",0},{"maximum",4},{"default",0.8}}}},{});
    const Json locomotion=object_schema({{"step_height",{{"type","number"},{"minimum",0},{"maximum",1},{"default",0.4}}},
        {"ground_snap",{{"type","number"},{"minimum",0},{"maximum",1},{"default",0.5}}},
        {"max_slope_degrees",{{"type","number"},{"minimum",0},{"maximum",80},{"default",45}}}},Json::array());
    const Json character = object_schema({{"radius",{{"type","number"},{"minimum",0.05},{"maximum",2}}},
        {"height",{{"type","number"},{"maximum",4}}}, {"speed",{{"type","number"},{"exclusiveMinimum",0},{"maximum",30}}},
        {"jump_speed",{{"type","number"},{"minimum",0},{"maximum",20}}}, {"camera",parent},{"stance",stance},{"locomotion",locomotion}}, {"radius","height","speed","jump_speed","camera"});
    const Json asset_id={{"type","string"},{"pattern","^[0-9a-f]{64}$"}};
    const Json unit={{"type","number"},{"minimum",0},{"maximum",1}};
    const Json lod_level=object_schema({{"asset",asset_id},{"primitive",{{"type","integer"},{"minimum",0},{"maximum",9999}}},
        {"min_distance_m",{{"type","number"},{"exclusiveMinimum",0},{"maximum",max_mesh_lod_distance_m}}}},
        {"asset","primitive","min_distance_m"});
    const Json foliage_tip={{"type","number"},{"minimum",0},{"maximum",2}};
    const Json foliage_height={{"type","number"},{"minimum",-1024},{"maximum",1024}};
    const Json foliage_wind=object_schema({
        {"direction",vector({{"type","number"},{"minimum",-1},{"maximum",1}},2)},
        {"tip_m",foliage_tip},{"period_ticks",unsigned_range(2,3600)},
        {"wavelength_m",{{"type","number"},{"minimum",.5},{"maximum",256}}}},
        {"direction","tip_m","period_ticks","wavelength_m"});
    const Json foliage_touch=object_schema({
        {"actor",id},{"radius_m",{{"type","number"},{"minimum",.05},{"maximum",8}}},{"tip_m",foliage_tip}},
        {"actor","radius_m","tip_m"});
    auto foliage_motion=object_schema({{"root_y",foliage_height},{"tip_y",foliage_height},
        {"wind",foliage_wind},{"touch",foliage_touch}},{"root_y","tip_y"});
    foliage_motion["anyOf"]=Json::array({Json{{"required",{"wind"}}},Json{{"required",{"touch"}}}});
    foliage_motion["description"]="Optional deformation for a rigid world drawable. Local tip_y must exceed root_y by 0.05..128 m. Wind direction uses XZ coefficients with length at least 0.0001. Touch requires a distinct authored CharacterController. CameraDraw and snow or water terrain tiles are excluded.";
    auto template_foliage_motion=foliage_motion;
    template_foliage_motion.erase("anyOf");
    template_foliage_motion["properties"].erase("touch");
    template_foliage_motion["required"].push_back("wind");
    template_foliage_motion["description"]="Wind-only deformation for a rigid spawned drawable. Local tip_y must exceed root_y by 0.05..128 m. Wind direction uses XZ coefficients with length at least 0.0001.";
    auto displacement=object_schema({{"texture",asset_id},{"height_m",{{"type","number"},{"minimum",0},{"maximum",4}}},
        {"tile_mm",unsigned_range(32,64000)},{"axis",{{"enum",{"x","y","z"}}}}},{"texture","height_m","tile_mm","axis"});
    displacement["description"]="Adaptive tessellation from a linear cooked height image. Projection follows the chosen local axis. Author collision separately.";
    auto surface_layer=object_schema({{"base_color",asset_id},{"orm",asset_id},{"normal",asset_id},{"mask",asset_id},
        {"tile_mm",unsigned_range(32,64000)},{"base_tile_mm",unsigned_range(32,64000)},{"weight",unit},
        {"slope_degrees",vector({{"type","number"},{"minimum",0},{"maximum",90}},2)},
        {"projection",{{"enum",{"xz","triplanar"}},{"default","xz"}}}},{"base_color","orm","tile_mm"});
    surface_layer["description"]="Second opaque PBR layer with world repeat in millimetres. Projection is xz or triplanar and also applies to the base when base_tile_mm is set. An optional linear red mask uses UV0. An optional increasing slope range sets the start/full blend in degrees. ORM packs occlusion, roughness and metallic in RGB.";
    auto static_mesh=object_schema({{"asset",asset_id},{"primitive",{{"type","integer"},{"minimum",0},{"maximum",9999}}},{"visible",{{"type","boolean"}}},
        {"cast_shadows",{{"type","boolean"},{"default",true}}},{"receive_shadows",{{"type","boolean"},{"default",true}}},
        {"lods",{{"type","array"},{"minItems",1},{"maxItems",max_mesh_lod_levels},{"items",lod_level},
          {"description","Optional alternate rigid geometry. Distances increase and triangle counts decrease from LOD0. Base materials and collision stay independent of visual tiers."}}},
        {"lod_anchor",vector({{"type","number"},{"minimum",-max_mesh_lod_distance_m},{"maximum",max_mesh_lod_distance_m}},3)},
        {"foliage_motion",foliage_motion},{"displacement",displacement},{"surface_layer",surface_layer}},
        {"asset","primitive","visible"});
    static_mesh["dependentRequired"]={{"lod_anchor",{"lods"}}};
    static_mesh["properties"]["lod_anchor"]["description"]="Optional point in the mesh's local coordinates from which to measure the LOD distance. Defaults to the drawable origin.";
    const Json mesh_collider=object_schema({{"asset",asset_id},{"primitive",{{"type","integer"},{"minimum",0},{"maximum",9999}}},
        {"friction",{{"type","number"},{"minimum",0},{"maximum",2}}},{"restitution",unit},{"surface",collision_surface}}, {"asset","primitive","friction","restitution"});
    const Json model_node={{"type","integer"},{"minimum",0},{"maximum",9999}};
    const Json animation_fields={{"clip",{{"anyOf",Json::array({Json{{"type","null"}},Json{{"type","integer"},{"minimum",0},{"maximum",255}}})}}},
        {"time",{{"type","number"},{"minimum",0},{"maximum",1e9}}},{"speed",{{"type","number"},{"minimum",0},{"maximum",8}}},
        {"loop",{{"type","boolean"}}},{"playing",{{"type","boolean"}}}};
    auto rig_fields=animation_fields;rig_fields["asset"]=asset_id;
    auto layer_fields=animation_fields;layer_fields["slot"]={{"type","integer"},{"minimum",1},{"maximum",4}};
    layer_fields["mode"]={{"enum",{"override","additive"}}};layer_fields["weight"]=unit;
    layer_fields["mask"]={{"type","array"},{"maxItems",10000},{"items",object_schema({{"node",model_node},{"weight",unit}},{"node","weight"})}};
    layer_fields["reference_clip"]=animation_fields.at("clip");layer_fields["reference_time"]=animation_fields.at("time");
    rig_fields["layers"]={{"type","array"},{"maxItems",4},{"items",object_schema(layer_fields,{"slot","mode","clip","time","speed","loop","playing","weight","mask"})}};
    const Json ik_slot={{"type","integer"},{"minimum",1},{"maximum",8}};
    const Json ik_fields={{"slot",ik_slot},{"enabled",{{"type","boolean"},{"default",true}}},
        {"space",{{"enum",{"world","rig"}},{"default","world"}}},{"weight",{{"type","number"},{"minimum",0},{"maximum",1},{"default",1}}},
        {"target",vector(Json{{"type","number"},{"minimum",-1e6},{"maximum",1e6}},3)},
        {"pole",vector(Json{{"type","number"},{"minimum",-1e6},{"maximum",1e6}},3)},
        {"end_orientation",object_schema({{"rotation",vector(Json{{"type","number"}},4)},
            {"weight",unit}},{"rotation","weight"})}};
    auto ik_chain=ik_fields;ik_chain["root"]=model_node;ik_chain["middle"]=model_node;ik_chain["end"]=model_node;
    ik_chain["target_node"]=model_node;
    ik_chain["target_node"]["description"]="Optional same-rig model node outside every configured solved branch. Requires space=rig, uses attachment-local target, pole and end orientation, and is mutually exclusive with grounding.";
    ik_chain["metric"]={{"enum",{"uniform","near-uniform-v1"}},{"default","uniform"},
        {"description","Frozen IK scale profile. Uniform requires exact positive uniform ancestor/root/middle scales. Near-uniform-v1 preserves source data, checks incoming and published local scale ratio, cumulative frame and Gram errors within 1e-4, and rejects an unweighted model-space endpoint residual above max(1e-9,1e-4 times combined link length)."}};
    ik_chain["grounding"]=object_schema({
        {"sole_offset",vector(Json{{"type","number"},{"minimum",-1000},{"maximum",1000}},3)},
        {"sole_up",vector(Json{{"type","number"},{"minimum",-1.000001},{"maximum",1.000001}},3)},
        {"sole_forward",vector(Json{{"type","number"},{"minimum",-1.000001},{"maximum",1.000001}},3)},
        {"reference_height",{{"type","number"},{"minimum",-1000},{"maximum",1000}}},
        {"plant_height",{{"type","number"},{"minimum",0},{"maximum",10},{"default",.03}}},
        {"plant_release_height",{{"type","number"},{"exclusiveMinimum",0},{"exclusiveMaximum",10},
            {"description","Optional fade start for a held plant, in model units above reference_height. Requires plant_lock=true and plant_height < plant_release_height < lift_height. Acquisition and unheld conformance still use plant_height."}}},
        {"lift_height",{{"type","number"},{"exclusiveMinimum",0},{"maximum",10},{"default",.15}}},
        {"probe_above",{{"type","number"},{"minimum",.001},{"maximum",10},{"default",.3}}},
        {"probe_below",{{"type","number"},{"minimum",.001},{"maximum",10},{"default",.6}}},
        {"max_correction",{{"type","number"},{"exclusiveMinimum",0},{"maximum",10},{"default",.25}}},
        {"max_slope_degrees",{{"type","number"},{"minimum",0},{"maximum",80},{"default",55}}},
        {"plant_lock",{{"type","boolean"},{"default",false},
            {"description","Optional support-relative planted-foot anchor. Capture at plant_height; release/rearm on lift or disabled control. Support/constraint failures disarm until release, preventing repeated stance reacquisition. Uses one fresh validation ray and the rigid static/kinematic body frame; no pelvis or dynamic-body balance."}}}},
        {"sole_offset","sole_up","sole_forward","reference_height"});
    ik_chain["grounding"]["if"]={{"required",{"plant_release_height"}}};
    ik_chain["grounding"]["then"]={{"required",{"plant_lock"}},{"properties",{{"plant_lock",{{"const",true}}}}}};
    ik_chain["grounding"]["description"]="Optional native foot contact owner. Unit orthogonal sole axes and plant_height < lift_height required. At most two feet per rig, 64 per world. Only final post-physics sampling probes static/kinematic supports; misses and raised feet preserve animation. Requires upright positive uniform wrapper. Manual controls and explicit end_orientation are rejected for owned slots.";
    rig_fields["ik"]={{"type","array"},{"maxItems",8},{"items",object_schema(ik_chain,{"slot","root","middle","end","target","pole"})}};
    Json root_fields={{"node",model_node},{"mode",{{"enum",{"extract","character"}},{"default","extract"}}},
        {"actor",parent},{"enabled",{{"type","boolean"},{"default",true}}},{"yaw",{{"type","boolean"},{"default",false}}}};
    auto root_profile=object_schema(root_fields,{"node"});
    root_profile["if"]={{"properties",{{"mode",{{"const","character"}}}}},{"required",{"mode"}}};
    root_profile["then"]={{"required",{"actor"}},{"properties",{{"actor",id}}}};
    root_profile["else"]={{"properties",{{"actor",{{"type","null"}}}}}};
    rig_fields["root_motion"]=std::move(root_profile);
    const auto animation_rig=object_schema(rig_fields,{"asset","clip","time","speed","loop","playing"});
    const auto rig_node=object_schema({{"rig",id},{"node",model_node}},{"rig","node"});
    auto skin_fields=static_mesh.at("properties");skin_fields.erase("lods");skin_fields.erase("lod_anchor");skin_fields.erase("foliage_motion");skin_fields.erase("surface_layer");skin_fields["rig"]=id;skin_fields["node"]=model_node;
    const auto skinned_mesh=object_schema(skin_fields,{"asset","primitive","visible","rig","node"});
    Json pbr=object_schema({{"base_color",vector(unit,3)},{"emissive",vector(unit,3)},{"metallic",unit},{"roughness",unit},{"double_sided",{{"type","boolean"}}},
        {"alpha_mode",{{"enum",{"opaque","mask"}},{"default","opaque"}}},
        {"opacity",{{"type","number"},{"minimum",0},{"maximum",1},{"default",1}}},
        {"alpha_cutoff",{{"type","number"},{"minimum",0},{"maximum",1e6},{"default",.5}}},
        {"diffuse_transmission",{{"type","number"},{"minimum",0},{"maximum",1},{"default",0},
          {"description","Diffuse light fraction transmitted by a thin surface. Positive values require double_sided, metallic zero, and no surface layer."}}}},
        {"base_color","emissive","metallic","roughness","double_sided"});
    pbr["description"]="Optional alpha_mode defaults opaque, which ignores alpha. Mask discards sampled base-color alpha times opacity strictly below alpha_cutoff; equality survives. Texture alpha is linear. Defaults preserve legacy materials. Coverage affects raster color/depth/products/shadows, not triangle physics or ray queries; blended transparency is unsupported.";
    const Json sampler_wrap={{"enum",{10497,33071,33648}}};
    const Json map_ref=object_schema({{"asset",asset_id},{"image",{{"type","integer"},{"minimum",0},{"maximum",255}}},
        {"wrap_s",sampler_wrap},{"wrap_t",sampler_wrap},{"min_filter",{{"enum",{9728,9729,9984,9985,9986,9987}}}},{"mag_filter",{{"enum",{9728,9729}}}}}, {"asset"});
    const Json optional_map={{"anyOf",{map_ref,Json{{"type","null"}}}}};
    const Json textures=object_schema({{"base_color",optional_map},{"metallic_roughness",optional_map},{"emissive",optional_map},{"occlusion",optional_map},{"normal",optional_map},
        {"occlusion_strength",unit},{"normal_scale",{{"type","number"},{"minimum",0},{"maximum",16}}}});
    const Json shadow=object_schema({{"enabled",{{"type","boolean"}}},{"near",{{"type","number"},{"minimum",.001},{"maximum",100},{"default",.05}}},
        {"distance",{{"type","number"},{"exclusiveMinimum",.001},{"maximum",100000},{"default",80}}},
        {"bias",{{"type","number"},{"minimum",0},{"maximum",.1},{"default",.0005}}},{"normal_bias",{{"type","number"},{"minimum",0},{"maximum",1},{"default",.01}}}}, {"enabled"});
    Json light_variants=Json::array();
    for(const auto* kind:{"directional","point","spot"}) {
        Json props={{"kind",{{"const",kind}}},{"color",vector(unit,3)},{"intensity",{{"type","number"},{"minimum",0},{"maximum",1e9}}},{"enabled",{{"type","boolean"}}}};
        props["shadow"]=shadow;
        if(std::string_view(kind)=="directional")props["shadow"]["properties"]["angular_size_degrees"]={{"type","number"},{"minimum",0},{"maximum",20},
            {"description","Directional emitter angular diameter. Omission uses 0.53 degrees in pcss_directional only; omitted fields are not inserted into stored or resolved lighting. Separate from sky.sun_size_degrees."}};
        if(std::string_view(kind)!="directional")props["range"]={{"type","number"},{"minimum",0},{"maximum",1e9},{"default",0}};
        if(std::string_view(kind)=="spot") {
            props["inner_angle"]={{"type","number"},{"minimum",0},{"exclusiveMaximum",90},{"default",0}};
            props["outer_angle"]={{"type","number"},{"exclusiveMinimum",0},{"maximum",90},{"default",45}};
        }
        light_variants.push_back(object_schema(props,{"kind","color","intensity","enabled"}));
    }
    Json sky_properties={{"enabled",{{"type","boolean"}}},{"zenith",vector(unit,3)},{"horizon",vector(unit,3)},{"ground",vector(unit,3)},
        {"horizon_falloff",{{"type","number"},{"minimum",.1},{"maximum",16}}},{"sun",{{"type",{"string","null"}},{"pattern","^[0-9a-f]{32}$"}}},
        {"sun_size_degrees",{{"type","number"},{"minimum",.1},{"maximum",20}}},{"sun_intensity",{{"type","number"},{"minimum",0},{"maximum",1e6}}}};
    for(const auto& [key,value]:sky_defaults.items())sky_properties[key]["default"]=value;
    auto sky=object_schema(sky_properties,{"enabled","zenith","horizon","ground","horizon_falloff","sun","sun_size_degrees","sun_intensity"});sky["default"]=sky_defaults;
    const Json ibl=object_schema({{"enabled",{{"type","boolean"}}},
        {"diffuse_intensity",{{"type","number"},{"minimum",0},{"maximum",64}}},
        {"specular_intensity",{{"type","number"},{"minimum",0},{"maximum",64}}}},
        {"enabled","diffuse_intensity","specular_intensity"});
    const Json bloom=object_schema({
        {"intensity",{{"type","number"},{"minimum",0},{"maximum",1},{"default",0},
            {"description","Blend strength. Zero disables bloom."}}},
        {"threshold",{{"type","number"},{"minimum",0},{"maximum",65504},{"default",1},
            {"description","Scene-linear peak RGB threshold before exposure."}}},
        {"radius",{{"type","number"},{"minimum",2},{"maximum",32},{"default",8},
            {"description","Approximate blur support in source pixels."}}}});
    const Json output_grade=object_schema({
        {"input_max",{{"type","number"},{"minimum",.125},{"maximum",64}}},
        {"strength",unit},
        {"knots",{{"type","array"},{"minItems",output_grade_knot_count},{"maxItems",output_grade_knot_count},
            {"prefixItems",Json::array({Json{{"const",Json::array({0,0,0})}}})},
            {"items",vector({{"type","number"},{"minimum",0},{"maximum",16}},3)}}}},
        {"input_max","strength","knots"});
    Json lighting_environment=object_schema({{"ambient",vector({{"type","number"},{"minimum",0},{"maximum",1e6}},3)},
        {"exposure",{{"type","number"},{"minimum",0},{"maximum",1e6}}},{"shadow_resolution",{{"enum",{256,512,1024,2048}},{"default",1024}}},{"shadow_filter",{{"enum",{"pcf3_point","pcf3_fractional","pcss_directional"}},{"default","pcf3_point"}}},{"sky",sky},{"ibl",ibl},{"bloom",bloom}}, {"ambient","exposure"});
    const AutoExposureSettings exposure_defaults;
    const auto exposure_number=[](float minimum,float maximum,float fallback) {
        return Json{{"type","number"},{"minimum",minimum},{"maximum",maximum},{"default",fallback}};
    };
    auto auto_exposure=object_schema({
        {"min_stops",exposure_number(-8,8,exposure_defaults.min_stops)},
        {"max_stops",exposure_number(-8,8,exposure_defaults.max_stops)},
        {"brighten_stops_per_second",exposure_number(.125f,32,exposure_defaults.brighten_stops_per_second)},
        {"darken_stops_per_second",exposure_number(.125f,32,exposure_defaults.darken_stops_per_second)}});
    auto_exposure["description"]="Optional GPU auto exposure in stops. An empty object enables the defaults. min_stops must not exceed max_stops. The exposure field remains a separate manual multiplier.";
    lighting_environment["properties"]["auto_exposure"]=std::move(auto_exposure);
    lighting_environment["properties"]["geometric_specular_aa"]={{"type","number"},{"minimum",0},{"maximum",1},{"default",0},
        {"description","Reduce very small PBR highlights by widening the specular lobe where mesh normals vary within a pixel footprint."}};
    lighting_environment["properties"]["tone_mapper"]={{"enum",{"reinhard","agx","pbr_neutral","aces2_sdr"}},
        {"default","reinhard"},{"description","Choose the SDR display transform after exposed HDR grading. Reinhard is the legacy default."}};
    lighting_environment["properties"]["output_grade"]=output_grade;
    lighting_environment["properties"]["output_grade"]["description"]="Optional per-channel curves over exposed scene HDR, after bloom and exposure and before tone mapping. All 33 RGB knots are required and knot zero is [0,0,0].";
    auto screen_reflections=object_schema({
        {"strength",unit},
        {"max_distance_m",{{"type","number"},{"minimum",.25},{"maximum",32}}},
        {"thickness_m",{{"type","number"},{"minimum",.01},{"maximum",.5}}}},
        {"strength","max_distance_m","thickness_m"});
    screen_reflections["description"]="Optional screen reflections for single-sample deferred color, including FSR when available. Set strength to zero to retain a disabled selection.";
    lighting_environment["properties"]["screen_reflections"]=std::move(screen_reflections);
    const auto bounded_number=[](double minimum,double maximum) {
        return Json{{"type","number"},{"minimum",minimum},{"maximum",maximum}};
    };
    auto screen_contact_shadows=object_schema({
        {"light",id},{"length_m",bounded_number(.02,2)},{"thickness_m",bounded_number(.001,.15)},{"strength",unit}},
        {"light","length_m","thickness_m","strength"});
    screen_contact_shadows["description"]="Trace short shadows for one authored directional Light in single-sample deferred color. Thickness in camera depth must not exceed length. Strength zero disables drawing and retains the authored selection.";
    lighting_environment["properties"]["screen_contact_shadows"]=std::move(screen_contact_shadows);
    auto screen_diffuse_bounce=object_schema({
        {"radius_m",bounded_number(.2,4)},{"thickness_m",bounded_number(.005,.25)},{"strength",unit},
        {"resolution",{{"enum",{"full","half"}},{"default","full"},
            {"description","Gather at full or half internal render resolution. Omission uses full."}}}},
        {"radius_m","thickness_m","strength"});
    screen_diffuse_bounce["description"]="Gather local diffuse light from visible opaque geometry in single-sample deferred color. Thickness in camera depth must not exceed radius. Strength zero disables drawing and retains the authored selection.";
    lighting_environment["properties"]["screen_diffuse_bounce"]=std::move(screen_diffuse_bounce);
    auto focus=object_schema({
        {"focus_distance_m",bounded_number(.01,1000000)},{"focus_band_m",bounded_number(0,1000000)},
        {"radius_at_infinity_px",bounded_number(0,24)},{"max_radius_px",bounded_number(0,24)}},
        {"focus_distance_m","focus_band_m","radius_at_infinity_px","max_radius_px"});
    focus["description"]="Single-sample world depth of field in color views. Radius is in internal render pixels. Either zero radius disables the effect.";
    lighting_environment["properties"]["depth_of_field"]=std::move(focus);
    auto cloud_wind=vector(bounded_number(-100,100),2);
    cloud_wind["description"]="Horizontal wind along world +X and +Z in metres per second.";
    auto clouds=object_schema({{"base_height_m",bounded_number(250,12000)},
        {"thickness_m",bounded_number(100,6000)},{"coverage",bounded_number(0,1)},
        {"extinction_per_km",bounded_number(0,32)},{"wind_m_per_s",std::move(cloud_wind)},
        {"shadow_strength",bounded_number(0,1)}},
        {"base_height_m","thickness_m","coverage","extinction_per_km","wind_m_per_s"});
    clouds["description"]="Optional volumetric cloud layer. Base height is relative to atmospheric sea level. Wind advances the layer with committed solar world time.";
    clouds["properties"]["shadow_strength"]["description"]="Optional strength of cloud shadows on direct light from the linked sun. Zero disables cloud shadows.";
    auto atmosphere=object_schema({{"sea_level_m",bounded_number(-1e6,1e6)},
        {"planet_center_m",vector(bounded_number(-1e9,1e9),3)},
        {"aerosol_strength",bounded_number(0,4)},{"radiance_scale",bounded_number(0,64)},
        {"aerial_perspective_m",bounded_number(128,65536)},{"clouds",std::move(clouds)}},
        {"aerosol_strength","radiance_scale"});
    atmosphere["if"]={{"required",{"planet_center_m"}}};
    atmosphere["then"]["not"]["anyOf"]=Json::array({
        Json{{"required",{"sea_level_m"}}},Json{{"required",{"aerial_perspective_m"}}},Json{{"required",{"clouds"}}}});
    atmosphere["else"]={{"required",{"sea_level_m"}}};
    atmosphere["description"]="Optional atmospheric sky scattering. The profile uses an enabled procedural sky linked to a directional Light. A disabled linked light retains the selection.";
    atmosphere["properties"]["planet_center_m"]["description"]="World-space XYZ center in metres for an Earth-sized atmosphere. The sky uses the radial direction from the center to the camera. This replaces sea_level_m and cannot be combined with aerial perspective or volumetric clouds.";
    atmosphere["properties"]["aerial_perspective_m"]["description"]="Maximum camera-ray distance in metres for aerial perspective on opaque world geometry. Omitting this leaves atmospheric scattering on the sky.";
    lighting_environment["properties"]["atmosphere"]=std::move(atmosphere);
    const auto fog_light=object_schema({{"entity",id},{"scattering",unit}},{"entity","scattering"});
    auto fog_lights=vector(fog_light,2);
    fog_lights["minItems"]=0;
    fog_lights["uniqueItems"]=true;
    fog_lights["description"]="Up to two distinct authored spot Light entities. Disabled spotlights remain valid selections.";
    auto height_fog=object_schema({{"density_per_m",bounded_number(0,.75)},{"base_height_m",bounded_number(-8192,8192)},
        {"height_falloff_per_m",bounded_number(0,4)},{"start_distance_m",bounded_number(0,300)},{"max_distance_m",bounded_number(.1,512)},
        {"ambient_radiance",vector(bounded_number(0,32),3)},{"anisotropy",bounded_number(-.8,.8)},{"spot_lights",fog_lights}},
        {"density_per_m","base_height_m","height_falloff_per_m","start_distance_m","max_distance_m","ambient_radiance","anisotropy"});
    height_fog["description"]="Optional scene-linear height fog for single-sample color, including FSR when available. Max distance must exceed start distance. A density of zero disables the effect and retains the settings.";
    lighting_environment["properties"]["height_fog"]=std::move(height_fog);
    lighting_environment["properties"]["post_effect"]=asset_id;
    lighting_environment["properties"]["post_effect"]["description"]="Optional content-addressed .poimapost artifact SHA-256. The runtime freezes the selected effect with the authored scene.";
    const Json enabled_ibl={{"required",Json::array({"ibl"})},{"properties",Json{{"ibl",Json{
        {"required",Json::array({"enabled"})},{"properties",Json{{"enabled",Json{{"const",true}}}}}}}}}};
    const Json enabled_sky={{"required",Json::array({"sky"})},{"properties",Json{{"sky",Json{
        {"required",Json::array({"enabled"})},{"properties",Json{{"enabled",Json{{"const",true}}}}}}}}}};
    auto linked_sky=enabled_sky;
    linked_sky["properties"]["sky"]["required"].push_back("sun");
    linked_sky["properties"]["sky"]["properties"]["sun"]=id;
    lighting_environment["allOf"]=Json::array({Json{{"if",enabled_ibl},{"then",enabled_sky}},
        Json{{"if",Json{{"required",Json::array({"atmosphere"})}}},{"then",std::move(linked_sky)}}});
    const Json acoustic=object_schema({{"absorption",vector(unit,3)},{"transmission",vector(unit,3)},{"scattering",unit},{"enabled",{{"type","boolean"}}}}, {"absorption","transmission","scattering","enabled"});
    auto audio_room=object_schema({
        {"half_extents",vector(bounded_number(min_room_extent_m,max_room_extent_m),3)},
        {"decay_seconds",bounded_number(min_room_decay_s,max_room_decay_s)},
        {"high_frequency_decay_seconds",bounded_number(min_room_decay_s,max_room_decay_s)},
        {"wet_gain",unit},{"priority",{{"type","integer"},{"minimum",0},{"maximum",255},{"default",0}}},
        {"portal_send",{{"type","number"},{"minimum",0},{"maximum",1},{"default",0}}}},
        {"half_extents","decay_seconds","high_frequency_decay_seconds","wet_gain"});
    audio_room["description"]="Diffuse late reverb in a transformed box. Highest priority wins; ties use the lowest entity ID. High-frequency decay must not exceed decay_seconds. A positive portal_send also routes the source room's late tail through visible apertures.";
    auto audio_portal=object_schema({
        {"half_extents",vector(bounded_number(min_audio_portal_extent_m,max_audio_portal_extent_m),2)},
        {"gain",{{"type","number"},{"minimum",0},{"maximum",1},{"default",1}}}},{"half_extents"});
    audio_portal["description"]="A finite opening in local XY. The occupied acoustic geometry on both sides gates an occluded source's path.";
    const Json audio_bus_name={{"type","string"},{"pattern",audio_buses::name_pattern}};
    const Json audio_bus_catalog={{"type","object"},{"propertyNames",audio_bus_name},{"minProperties",1},
        {"maxProperties",audio_buses::max_buses},{"additionalProperties",{{"type","number"},{"minimum",0},{"maximum",1}}}};
    auto water_flow=object_schema({{"port",{{"type","string"},{"pattern","^[a-z][a-z0-9_.-]{0,63}$"}}},
        {"full_discharge_m3_s",bounded_number(min_audio_full_discharge_m3_s,max_audio_full_discharge_m3_s)},
        {"direction",{{"enum",{"forward","reverse","both"}},{"default","forward"}}}},{"port","full_discharge_m3_s"});
    water_flow["description"]="Optional gain multiplier driven by the committed signed storage-port discharge. Selected positive discharge divided by the full-gain rate, clamped to one.";
    const Json emitter=object_schema({{"asset",asset_id},{"gain",{{"type","number"},{"minimum",0},{"maximum",4}}},{"loop",{{"type","boolean"}}},{"enabled",{{"type","boolean"}}},{"bus",audio_bus_name},{"water_flow",std::move(water_flow)}}, {"asset","gain","loop","enabled"});
    Json hit_variants=Json::array();
    for(const auto* shape:{"sphere","box","capsule"}) {
        Json props={{"owner",id},{"name",{{"type","string"},{"pattern","^[a-z][a-z0-9_.-]{0,63}$"}}},
            {"shape",{{"const",shape}}},{"center",vector(Json{{"type","number"},{"minimum",-10000},{"maximum",10000}},3)},
            {"enabled",{{"type","boolean"},{"default",true}}}};
        Json required=Json::array({"owner","name","shape"});
        if(std::string_view(shape)=="box") {props["half_extents"]=vector(Json{{"type","number"},{"minimum",.001},{"maximum",10000}},3);required.push_back("half_extents");}
        else {props["radius"]={{"type","number"},{"minimum",.001},{"maximum",10000}};required.push_back("radius");}
        if(std::string_view(shape)=="capsule") {props["half_height"]={{"type","number"},{"minimum",0},{"maximum",10000}};required.push_back("half_height");}
        hit_variants.push_back(object_schema(props,required));
    }
    const Json hit_region={{"oneOf",hit_variants},{"description","Immutable query-only region; owner is self or an ancestor. Unique owner/name pairs and at most 4096 regions include disabled members. Local spheres, boxes and Y capsules use exact affine poses; no physics body or collision response."}};
    auto physical_rig_schema=physical_rig::configuration_schema();
    physical_rig_schema["description"]="Frozen physical profile on an AnimationRig owner. Part nodes refer to the same model's local indices and require matching RigNode entities; joint parents must be model ancestors. Optional pose_frame selects rigid_uniform (default) or skeletal_fixed_scale with immutable initialized local scales and logical quaternion-chain axes. No separate asset or editor-only rig state.";
    const auto mark_range=[](double minimum,double maximum) {
        return Json{{"type","number"},{"minimum",minimum},{"maximum",maximum}};
    };
    const Json mark_profile_id={{"type","string"},{"pattern","^(?!0{32}$)[0-9a-f]{32}$"}};
    const Json mark_lifetime={{"type","integer"},{"minimum",1},{"maximum",max_surface_mark_lifetime_ticks}};
    auto mark_relief=mark_range(0,surface_mark_validation::max_authored_relief_depth_m);
    mark_relief["default"]=0;
    mark_relief["description"]="Shading depression depth in metres, independent of color opacity.";
    const Json mark_style_props={{"half_extent_m",vector(mark_range(.005,4),2)},{"half_depth_m",mark_range(.001,.25)},
        {"minimum_normal_dot",unit},{"edge_fraction",unit},{"linear_rgb",vector(unit,3)},
        {"opacity",unit},{"roughness",mark_range(.045,1)},{"relief_depth_m",mark_relief}};
    const Json mark_style_required={"half_extent_m","half_depth_m","minimum_normal_dot","edge_fraction","linear_rgb","opacity","roughness"};
    auto footprint=object_schema(mark_style_props,mark_style_required);
    footprint["properties"]["profile"]=mark_profile_id;
    footprint["properties"]["lifetime_ticks"]=mark_lifetime;
    footprint["properties"]["sole_tolerance_m"]=mark_range(.001,.1);
    footprint["properties"]["sole_tolerance_m"]["default"]=.025;
    footprint["required"].push_back("lifetime_ticks");
    footprint["description"]="Author on a static or kinematic rigid surface with matching visible opaque render and collision geometry. A newly acquired planted foot can leave a timed mark here when the solved sole is within sole_tolerance_m of the contact. The first half extent follows the foot heading. Optional profile refers to the frozen surface mark catalog. Available for authored world entities.";
    auto surface_wetness=object_schema({{"capacity_nm",unsigned_range(1,wetness::max_capacity_nm)},
        {"retention_q16",unsigned_range(0,wetness::units_per_nm)},
        {"drying_nm_per_second",unsigned_range(0,wetness::max_capacity_nm)},
        {"probe_local_m",vector(mark_range(-wetness::probe_bound_m,wetness::probe_bound_m),3)},
        {"darkening",unit},{"roughness_factor",unit}},
        {"capacity_nm","retention_q16","drying_nm_per_second","probe_local_m","darkening","roughness_factor"});
    surface_wetness["description"]="Attach to an authored rigid world drawable with explicit PbrMaterial and no CameraDraw. Drying per game second must be at most capacity_nm. Darkening sets the full-film linear base color reduction; roughness_factor sets the full-film roughness multiplier.";
    const Json components = {{"Transform", transform}, {"Camera", camera}, {"CameraDraw",camera_draw}, {"MeshRenderer", mesh}, {"BoxCollider",collider}, {"MeshCollider",mesh_collider}, {"CharacterController",character},{"StaticMesh",static_mesh},{"SkinnedMesh",skinned_mesh},{"AnimationRig",animation_rig},{"PhysicalRig",physical_rig_schema},{"RigNode",rig_node},{"HitRegion",hit_region},{"PbrMaterial",pbr},{"PbrTextures",textures},{"Light",{{"oneOf",light_variants}}},{"LightingEnvironment",lighting_environment},{"AcousticMaterial",acoustic},{"AudioRoom",std::move(audio_room)},{"AudioPortal",std::move(audio_portal)},{"AudioEmitter",emitter},{"ParticleEmitter",particle_emitter_schema()},{"FootprintSurface",footprint},
        {"SurfaceWetness",std::move(surface_wetness)}};
    Json ops = Json::array();
    auto op = [&](const char* kind, Json properties, Json required) {
        properties["op"] = {{"const", kind}}; properties["id"] = id;
        required.push_back("op"); required.push_back("id"); ops.push_back(object_schema(properties, required));
    };
    op("entity.create", {{"name", name}, {"parent", parent}}, {"name"});
    op("asset.instantiate", {{"asset",asset_id},{"name",name},{"parent",parent}}, {"asset","name"});
    op("entity.rename", {{"name", name}}, {"name"});
    op("entity.reparent", {{"parent", parent}, {"mode", {{"const", "keep_local"}}}}, {"parent", "mode"});
    op("entity.delete", {{"recursive", {{"type", "boolean"}}}}, {"recursive"});
    for (const auto& [type, value] : components.items())
        op("component.set", {{"type", {{"const", type}}}, {"value", value}}, {"type", "value"});
    auto removable_component=component_type;
    auto& removable_types=removable_component.at("enum");
    removable_types.erase(std::find(removable_types.begin(),removable_types.end(),Json("Transform")));
    op("component.remove", {{"type", removable_component}}, {"type"});
    ops.push_back(object_schema({{"op",{{"const","navigation.set"}}},{"asset",{{"anyOf",Json::array({asset_id,Json{{"type","null"}}})}}}},{"op","asset"}));
    ops.push_back(object_schema({{"op",{{"const","water.set"}}},{"asset",{{"anyOf",Json::array({asset_id,Json{{"type","null"}}})}}}},{"op","asset"}));
    auto snow_tiles=vector(snow_tile_id,2);
    snow_tiles["uniqueItems"]=true;
    auto sole_shape=object_schema({
        {"half_length_units",unsigned_range(snow::min_sole_half_length_units,snow::max_sole_half_length_units)},
        {"half_width_units",unsigned_range(snow::min_sole_half_width_units,snow::max_sole_half_width_units)}},
        {"half_length_units","half_width_units"});
    sole_shape["description"]="Projected half extents. Their sum plus 2 must fit within 1536 units.";
    auto foot_release=object_schema({{"radius_units",unsigned_range(snow::min_radius_units,snow::max_radius_units)},
        {"sole",std::move(sole_shape)},
        {"depth_units",unsigned_range(1,snow::max_depth_units)},
        {"sole_tolerance_m",mark_range(snow::min_sole_tolerance_m,snow::max_sole_tolerance_m)}},
        {"depth_units","sole_tolerance_m"});
    foot_release["oneOf"]=Json::array({Json{{"required",{"radius_units"}}},Json{{"required",{"sole"}}}});
    foot_release["description"]="Choose a radius or shaped sole. A qualifying lift queues the footprint for the next eligible tick.";
    auto snow_definition=object_schema({{"tiles",snow_tiles},{"thickness_units",unsigned_range(1,snow::max_depth_units)},
        {"foot_release",foot_release}},{"tiles","thickness_units"});
    snow_definition["description"]="Bind two existing cooked terrain entities in west-east order. Each must use the same visible static primitive for rendering and collision. A water region may use separate terrain and an XZ grid with no positive-area overlap with the snow. Null clears the binding.";
    ops.push_back(object_schema({{"op",{{"const","snow.set"}}},
        {"definition",{{"anyOf",Json::array({snow_definition,Json{{"type","null"}}})}}}},{"op","definition"}));
    const auto checkpoint_profile=object_schema({{"version",{{"type","integer"},{"const",1}}}},{"version"});
    ops.push_back(object_schema({{"op",{{"const","physics_checkpoint.set"}}},
        {"definition",{{"anyOf",Json::array({checkpoint_profile,Json{{"type","null"}}})}}}},{"op","definition"}));
    ops.push_back(object_schema({{"op",{{"const","audio.buses.set"}}},
        {"buses",{{"anyOf",Json::array({audio_bus_catalog,Json{{"type","null"}}})}}}},{"op","buses"}));
    ops.push_back(object_schema({{"op",{{"const","surface_mark.profile.set"}}},{"id",mark_profile_id},
        {"mask",{{"anyOf",Json::array({asset_id,Json{{"type","null"}}})}}}},{"op","id","mask"}));
    ops.push_back(object_schema({{"op",{{"const","asset.provenance.set"}}},{"asset",asset_id},{"records",{{"type","array"},{"maxItems",8},{"uniqueItems",true},{"items",asset_id}}}},{"op","asset","records"}));
    const auto reference_owner=object_schema({{"kind",{{"enum",{"entity","template","world"}}}},{"id",id}},{"kind","id"});
    auto reference_cursor_variant=[&](const char* kind,Json types,Json paths) {
        auto owner=reference_owner;owner["properties"]["kind"]={{"const",kind}};
        return object_schema({{"owner",owner},{"component",std::move(types)},{"path",std::move(paths)}},{"owner","component","path"});
    };
    const Json texture_paths={{"enum",{"/base_color/asset","/emissive/asset","/metallic_roughness/asset","/normal/asset","/occlusion/asset"}}};
    const Json texture_type={{"const","PbrTextures"}},direct_path={{"const","/asset"}};
    const Json sprite_type={{"const","ParticleEmitter"}},sprite_path={{"const","/sprite/texture"}};
    const Json lod_type={{"const","StaticMesh"}};
    const Json lod_path={{"type","string"},{"pattern","^/lods/[0-3]/asset$"}};
    const Json layer_path={{"type","string"},{"pattern","^/surface_layer/(base_color|mask|normal|orm)$"}};
    Json reference_cursor={{"oneOf",Json::array({
        reference_cursor_variant("entity",{{"enum",{"AnimationRig","AudioEmitter","MeshCollider","SkinnedMesh","StaticMesh"}}},direct_path),
        reference_cursor_variant("entity",texture_type,texture_paths),
        reference_cursor_variant("entity",sprite_type,sprite_path),
        reference_cursor_variant("entity",lod_type,lod_path),
        reference_cursor_variant("entity",lod_type,layer_path),
        reference_cursor_variant("entity",lod_type,{{"const","/displacement/texture"}}),
        reference_cursor_variant("template",{{"const","StaticMesh"}},direct_path),
        reference_cursor_variant("template",lod_type,lod_path),
        reference_cursor_variant("template",lod_type,layer_path),
        reference_cursor_variant("template",lod_type,{{"type","string"},{"pattern","^/entities/(?!0{32}/)[0-9a-f]{32}/components/StaticMesh/surface_layer/(base_color|mask|normal|orm)$"}}),
        reference_cursor_variant("template",lod_type,{{"const","/displacement/texture"}}),
        reference_cursor_variant("template",lod_type,{{"type","string"},{"pattern","^/entities/(?!0{32}/)[0-9a-f]{32}/components/StaticMesh/displacement/texture$"}}),
        reference_cursor_variant("template",lod_type,{{"type","string"},{"pattern","^/entities/(?!0{32}/)[0-9a-f]{32}/components/StaticMesh/lods/[0-3]/asset$"}}),
        reference_cursor_variant("template",texture_type,texture_paths),
        reference_cursor_variant("template",sprite_type,sprite_path),
        reference_cursor_variant("template",sprite_type,{{"type","string"},{"pattern","^/entities/(?!0{32}/)[0-9a-f]{32}/components/ParticleEmitter/sprite/texture$"}}),
        reference_cursor_variant("world",{{"const","navigation"}},direct_path),
        reference_cursor_variant("world",{{"const","surface_mark_profiles"}},{{"type","string"},{"pattern","^/(?!0{32}/)[0-9a-f]{32}/mask$"}}),
        reference_cursor_variant("world",{{"const","water"}},direct_path),
        reference_cursor_variant("world",{{"const","ui"}},{{"type","string"},{"pattern","^/ui/(?!0{32}/)[0-9a-f]{32}/style/image/asset$"}})})}};
    for(const auto* type:{"AnimationRig","AudioEmitter","MeshCollider","PbrTextures","SkinnedMesh","StaticMesh"}) {
        const std::string suffix=std::string(type)=="PbrTextures" ? "/(base_color|emissive|metallic_roughness|normal|occlusion)/asset" : "/asset";
        reference_cursor["oneOf"].push_back(reference_cursor_variant("template",{{"const",type}},
            {{"type","string"},{"pattern","^/entities/(?!0{32}/)[0-9a-f]{32}/components/"+std::string(type)+suffix+"$"}}));
    }
    auto references_schema=object_schema({{"revision",rev},{"asset",asset_id},{"owner",reference_owner},{"after",reference_cursor},
        {"limit",{{"type","integer"},{"minimum",1},{"maximum",256},{"default",64}}}});
    references_schema["allOf"]=Json::array();
    references_schema["allOf"].push_back(Json{{"not",Json{{"required",Json::array({"asset","owner"})}}}});
    references_schema["allOf"].push_back(Json{
        {"if",Json{{"required",Json::array({"after"})}}},
        {"then",Json{{"required",Json::array({"revision"})}}}
    });
    references_schema["description"]="Read current authored typed asset references without package I/O. Asset and owner filters are exclusive; continuation requires the returned revision. This evolving API is outside authoring-core v1.";
    Json result = {{"protocol_version", 1}, {"schema_revision", 262}, {"transport", "JSON-RPC 2.0; one request per line; no batches"},
        {"methods", {
            {"world.describe", {{"type","object"},{"description","Full discovery by default; catalog lists names, while method/component/section retrieves one entry and mutation selects transaction operation schemas. Read the invariants section before mutations."},{"oneOf",Json::array({
                object_schema({{"view",{{"enum",{"full","catalog"}},{"default","full"}}}}),
                object_schema({{"view",{{"enum",{"method","component","section"}}}},{"name",{{"type","string"},{"minLength",1}}}},{"view","name"}),
                object_schema({{"view",{{"const","mutation"}}},{"operation",{{"type","string"},{"minLength",1},{"maxLength",128}}},{"type",{{"type","string"},{"minLength",1},{"maxLength",128}}}},{"view","operation"})
            })}}}, {"world.inspect", object_schema(Json::object())},
            {"world.describe.select",discovery_detail::selection_parameters_schema()},
            {"world.dependencies",object_schema(Json::object())},
            {"jobs.status",object_schema(Json::object())},
            {"world.asset.references",references_schema},
            {"world.reference",authoring_reference::schema()},
            {"session.close", object_schema(Json::object())},
            {"entity.get", object_schema({{"id", id}, {"revision", rev}, {"component", component_type}}, {"id"})},
            {"entity.world_transform", object_schema({{"id", id}, {"revision", rev}}, {"id"})},
            {"world.capture", object_schema({{"revision", rev}, {"camera", id},
                {"path", {{"type", "string"}, {"minLength", 1}, {"description", "New BMP path; parent must exist. Opens a bounded native window."}}},
                {"width", {{"type", "integer"}, {"minimum", 128}, {"maximum", 4096}, {"default", 960}}},
                {"height", {{"type", "integer"}, {"minimum", 128}, {"maximum", 4096}, {"default", 540}}},
                {"gpu", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4095}}},
                {"lighting_path",{{"enum",{"forward","deferred"}},{"default","forward"}}},{"ambient_occlusion",object_schema({{"mode",{{"enum",{"none","gtao"}},{"default","none"}}},{"quality",{{"enum",{"low","medium","high"}},{"default","medium"}}},{"radius",{{"type","number"},{"minimum",0.01},{"maximum",100},{"default",1}}}})}, {"reconstruction",{{"enum",{"none","fsr3_native","fsr3_quality","fsr3_balanced","fsr3_performance"}},{"default","none"}}},{"capture_frames",{{"type","integer"},{"minimum",1},{"maximum",128},{"default",2}}},{"scene_product_probes",{{"type","array"},{"maxItems",64},{"items",object_schema({{"x",{{"type","integer"},{"minimum",0},{"maximum",4095}}},{"y",{{"type","integer"},{"minimum",0},{"maximum",4095}}}},{"x","y"})}}},{"scene_debug_view",{{"enum",{"color","depth","shading_normal","motion","motion_validity","ambient_occlusion"}},{"default","color"},{"description","Non-color views require samples=1; sky has no surface product."}}},{"samples", {{"enum", {1, 4}}, {"default", 4}}},{"viewmodel_samples",{{"type","integer"},{"enum",{1,2,4,8}},{"default",1}}},{"culling",{{"type","boolean"},{"default",true}}},{"draw_instancing",{{"type","boolean"},{"default",true},{"description","Combine immediately adjacent compatible visible objects into native instanced commands; false keeps direct per-instance submission. Does not reorder objects or change visibility."}}},{"clustered_lighting",{{"type","boolean"},{"default",true}}},{"frames_in_flight",{{"type","integer"},{"minimum",1},{"maximum",2},{"default",2}}},{"profile",{{"type","boolean"},{"default",false}}}}, {"revision", "camera", "path"})},
            {"entity.query", object_schema({{"revision", rev}, {"parent", parent}, {"after", id}, {"component", component_type},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}, {"default", 64}}}})},
            {"world.transact", object_schema({{"request_id", id}, {"base_revision", rev},
                {"ops", {{"type", "array"}, {"minItems", 1}, {"maxItems", 256}, {"items", {{"oneOf", ops}}}}},
                {"preview", {{"type", "boolean"}, {"default", false}}}}, {"request_id", "base_revision", "ops"})}}},
        {"components", components},
        {"limits", {{"entities", 10000}, {"request_bytes", 1048576}, {"document_bytes", max_document_bytes},
            {"camera_draw_bindings_per_mode_per_camera",32},{"receipt_window", 128}, {"json_depth", 64},{"enabled_lights",max_scene_lights},{"lighting_environments",1},{"shadow_views",max_shadow_views},{"shadow_bytes",max_shadow_bytes},
            {"mesh_collider_triangles_per_body",100000},{"mesh_collider_vertices_per_body",300000},
            {"mesh_collider_triangles_world",250000},{"mesh_collider_vertices_world",750000}}},
        {"invariants", {"Normalized XYZW quaternion; meters; local transforms; positive scale.",
            "Stable IDs are caller-supplied and cannot be reused after deletion.",
            "Pagination with after requires the returned revision.",
            "Single cooperative writer per document; manual file changes require reopening.",
            "Camera and optional complete viewmodel lens require 5..150 degree FOV and 0.001 <= near < far <= 10000000; camera world transforms must be unscaled.",
            "Optional Camera.viewmodel_clearance requires a foreground lens and a conservative local-Y capsule envelope: normalized XYZW rotation, position within10m, radius.001..10m, half_height0..10m, max_retraction.001..10m, and skin.0001..min(radius,max_retraction). Runtime presentation uses current physics after pose interpolation, ignores physical camera ancestors, retracts all foreground draws together and reports unresolved anchor/final checks with foreground suppression. Authored Scene previews do not evaluate physics clearance. Envelope containment of arbitrary meshes/animation is author-owned.",
            "CameraDraw requires one drawable and an existing nonzero Camera reference. Viewmodels require that Camera's viewmodel lens and a strict descendant hierarchy, unless scene_anchored is true. Scene-anchored viewmodels use their own hierarchy and cannot select themselves as the camera. Hidden references need no lens or descendant relationship.",
            "CameraDraw limits count at most 32 bindings per mode per camera, including invisible meshes. Templates require local camera references. Named Game views separate viewmodels globally and hide bodies only for the target camera while retaining body shadows; external editor Scene views keep ordinary geometry. Viewmodels never cast world shadows and retain receive_shadows.",
            "Box primitive is centered at the origin with unit side lengths; albedo is linear RGB.",
            "Capture is a bounded forward preview, not a playable runtime or advanced renderer.",
            "Custom components use registered stable scalar/collection schemas and game:<type-id> keys; collection capacities count toward the 512-byte component budget. Prefabs and keep_world reparenting remain unsupported.",
            "Simulation is optional; runtime.start freezes authored state at a revision.",
            "Dynamic/kinematic bodies and controllers must be roots; colliders reject shear; a supplied controller camera must be a direct child; camera:null creates a camera-free actor.",
            "Character height must exceed twice radius; runtime is single-threaded fixed 60 Hz."}}};
    auto& methods=result["methods"];
    methods["world.capture"]["properties"]["gpu_visibility"]={{"type","boolean"},{"default",false},
        {"description","Use Vulkan GPU-generated indirect draws for eligible rigid indexed world meshes when supported."}};
    methods["world.capture"]["properties"]["shadow_filter"]={{"enum",{"pcf3_point","pcf3_fractional","pcss_directional"}},{"description","Explicit override; omission inherits LightingEnvironment.shadow_filter, falling back to pcf3_point for legacy scenes."}};
    methods["world.capture"]["properties"]["post_aa"]={{"type","string"},{"enum",{"none","fxaa"}},{"default","none"},
        {"description","FXAA requires effective samples=1, scene_debug_view=color and reconstruction=none."}};
    methods.update(navigation::schemas());
    methods.update(provenance::schemas());
    result["navigation"]=navigation::capability();
    methods["world.history"]=object_schema(Json::object());
    for(const auto* method:{"world.undo","world.redo"})methods[method]=object_schema({{"request_id",id},{"base_revision",rev}},{"request_id","base_revision"});
    result["limits"]["history_entries"]=32;result["limits"]["history_bytes"]=max_document_bytes;
    result["gameplay_particle_bursts"]={{"feature",gameplay_abi::particle_burst_feature},
        {"services_abi",gameplay_abi::services_version},{"services_bytes",gameplay_abi::particle_services_bytes},
        {"command_version",1},{"command_bytes",sizeof(PoimaGameParticleBurstV1)},
        {"attempts_per_tick",max_gameplay_particle_burst_attempts},{"particles_per_request",{{"minimum",1},{"maximum",particles::max_burst}}},
        {"combined_particles_per_emitter_per_tick",particles::max_burst},
        {"target","An active ParticleEmitter entity or a ParticleEmitter node spawned in this Tick."},
        {"publication","Tick-only. Apply at the final emitter pose after animation and physics. The named-action burst and gameplay requests share the per-emitter limit. Exceeding it rolls back the step batch."},
        {"observation","Read runtime.particles.inspect at the completed tick."}};
    result["gameplay_item_attachments"]={{"feature","item_attachment_v1"},{"services_abi",7},{"services_bytes",528},
        {"command_bytes",128},{"state_bytes",64},{"contact_feature","item_attachment_contact_v1"},{"contact_command_bytes",256},{"surface_feature","item_attachment_surface_v1"},
        {"surface_command_bytes",272},{"surface_probe_bytes",88},{"recovery_feature","item_attachment_recovery_v1"},
        {"recovery_command_version",4},{"outcome_bytes",120},{"release_feature","item_attachment_release_v1"},
        {"release_command_version",5},{"release_command_bytes",152},{"release_outcome_version",3},{"release_outcome_bytes",80},
        {"pickup_feature","item_attachment_pickup_v1"},{"pickup_command_version",6},{"pickup_command_bytes",272},
        {"pickup_clearance_feature","item_attachment_pickup_clearance_v1"},{"pickup_clearance_command_version",7},
        {"pickup_pose_feature","item_attachment_pickup_pose_v1"},{"pickup_pose_command_version",8},
        {"pickup_blend_feature","item_attachment_pickup_blend_v1"},{"pickup_blend_command_version",9},
        {"pickup_steering_feature","item_attachment_pickup_steering_v1"},{"pickup_steering_command_version",10},
        {"pickup_hand_fallback_feature","item_attachment_pickup_hand_fallback_v1"},
        {"pickup_hand_fallback_command_version",11},{"pickup_hand_radius_m",.001},
        {"pickup_clearance_command_bytes",312},{"pickup_clearance_outcome_version",5},{"pickup_clearance_outcome_bytes",88},
        {"aim_feature","item_attachment_aim_v1"},{"aim_query_version",4},{"aim_query_bytes",208},
        {"max_surface_probes",max_attachment_surface_probes},{"reads_per_tick",1024},{"writes_per_tick",max_runtime_attachments},
        {"reads","Committed attachment before the current tick's movement."},
        {"writes","Tick-only. Apply after final animation and physics, before committing the tick and servicing gameplay saves."}};
    result["gameplay_camera_poses"]={{"feature","camera_pose_v1"},{"services_abi",7},{"services_bytes",392},
        {"command_bytes",144},{"state_bytes",272},{"reads_per_tick",256},{"combined_writes_per_tick",64},
        {"translation_metres_per_axis",10},{"quaternion_norm_tolerance",1e-5},{"viewmodel_presence","frozen"},
        {"save_extension","Optional camera_poses version1; only nonidentity states. Legacy snapshots are unchanged at identity."}};
    result["gameplay_camera_lenses"]={{"feature","camera_lens_v1"},{"services_abi",7},{"services_bytes",320},
        {"command_bytes",80},{"state_bytes",104},{"reads_per_tick",256},{"combined_writes_per_tick",64},
        {"snapshot_version",7},{"viewmodel_presence","frozen"},{"player_fov","Perspective ratio against authored baseline, saturated to5..150 degrees."}};
    result["gameplay_player_preferences"]={{"feature","player_preferences_v1"},{"services_abi",7},{"services_bytes",256},
        {"wire_version",1},{"snapshot_bytes",216},{"patch_bytes",104},{"enqueue_bytes",40},{"result_bytes",56},
        {"authority","The live native PlayerPreferences owner, outside authored worlds and gameplay saves."},
        {"availability","Named host support is separate from an attached player. Tick/Control reads report unavailable without a current player; Initialize has no services."},
        {"publication","At most one staged preference patch per whole Control or step batch. Reads see committed state. Host publication follows complete native success and precedes queued save/load; failed batches consume no ticket or revision."},
        {"observation","Accepted configuration and cached native application/presentation are distinct. Graphics samples/frame slots remain next-player intent; replay is observation-only."}};
    result["invariants"].push_back("Undo/redo is session-local core history, bounded to 32 edits and 16 MiB of entity and component schema snapshots. Restoration advances revision and preserves inactive ID retirement. Oversized edits commit but clear history; new edits invalidate redo.");
    result["invariants"].push_back("project.inspect and project.gameplay.adopt select immutable host development profiles: project paths are portable relative names under project_root, artifact descriptors under output_root. Adoption is a synchronous native copy and guarded manifest update, requires exact project/artifact SHA-256 and a request ID, and returns a durable per-project replay receipt instead of a development job. It launches no compiler, imports no authored component schemas, and does not activate or replace runtime gameplay. Both methods are unavailable in read-only packaged sessions and remain outside authoring-core v1.");
    const Json input_path={{"type","string"},{"minLength",1},{"maxLength",4096},{"description","Profile file ending .poima-input.json; relative paths resolve beside the world."}};
    methods["input.describe"]=object_schema(Json::object());
    methods["input.devices"]=object_schema(Json::object());
    methods["input.inspect"]=object_schema({{"path",input_path},{"read_only",{{"type","boolean"},{"default",false},
        {"description","Inspect immutable or quiescent input without creating/opening a writer lock. No cooperative writer synchronization or new atomic/concurrent-read guarantee. Read-only packaged hosts always use this mode, including explicit false."}}}},{"path"});
    Json input_event={{"oneOf",Json::array({
        object_schema({{"control",{{"type","string"},{"minLength",1},{"maxLength",64}}},{"down",{{"type","boolean"}}}},{"control","down"}),
        object_schema({{"motion",vector({{"type","number"},{"minimum",-1e6},{"maximum",1e6}},2)}},{"motion"}),
        object_schema({{"consume",{{"const",true}}}},{"consume"}),object_schema({{"clear",{{"const",true}}}},{"clear"})})}};
    input_event["oneOf"].push_back(object_schema({{"gamepad_connect",object_schema({{"axes",vector({{"type","integer"},{"minimum",-32768},{"maximum",32767}},6)},{"buttons",{{"type","integer"},{"minimum",0},{"maximum",4294967295ULL}}}})}},{"gamepad_connect"}));
    input_event["oneOf"].push_back(object_schema({{"gamepad_axis",object_schema({{"axis",{{"enum",{"left_x","left_y","right_x","right_y","left_trigger","right_trigger"}}}},{"value",{{"type","integer"},{"minimum",-32768},{"maximum",32767}}}},{"axis","value"})}},{"gamepad_axis"}));
    input_event["oneOf"].push_back(object_schema({{"gamepad_disconnect",{{"const",true}}}},{"gamepad_disconnect"}));
    input_event["oneOf"].push_back(object_schema({{"gamepad_button",object_schema({{"control",{{"type","string"},{"maxLength",32}}},{"down",{{"type","boolean"}}}},{"control","down"})}},{"gamepad_button"}));
    methods["input.evaluate"]=object_schema({{"path",input_path},{"gamepad_defaults",{{"type","boolean"},{"default",false}}},{"events",{{"type","array"},{"maxItems",256},{"items",input_event}}}},{"events"});
    methods["input.transact"]=object_schema({{"path",input_path},{"request_id",id},{"expected_revision",rev},
        {"profile",input_profiles::profile_schema()},{"preview",{{"type","boolean"},{"default",false}}}},
        {"path","request_id","expected_revision","profile"});
    methods["runtime.start"]=object_schema({{"session_id",id},{"revision",rev}},{"session_id","revision"});
    methods["runtime.status"]=object_schema(Json::object());
    methods["runtime.save.status"]=object_schema({{"session_id",id}},{"session_id"});
    methods["runtime.save.result"]=object_schema({{"epoch",id},{"sequence",{{"type","integer"},{"minimum",1},{"maximum",max_revision}}}},{"epoch","sequence"});
    for(const auto* method:{"runtime.inspect","runtime.stop"}) methods[method]=object_schema({{"session_id",id}},{"session_id"});
    methods["runtime.entity"]=object_schema({{"session_id",id},{"id",id},{"tick",rev}},{"session_id","id"});
    if(Runtime::available()) {
        const Json entity_fields={{"type","array"},{"maxItems",13},{"uniqueItems",true},{"items",{{"enum",{
            "world_matrix","layout","local_transform","animation","motion","kinematic_target","motion_remaining_ticks",
            "velocity","has_body","is_character","ground","yaw","pitch"}}}},{"default",{
            "world_matrix","layout","velocity","has_body","is_character","ground","yaw","pitch"}}};
        const Json component_selector=object_schema({{"type",id},{"fields",{{"type","array"},{"maxItems",32},{"uniqueItems",true},{"items",id}}}},{"type"});
        const Json query=object_schema({{"type",id},{"after",id},{"limit",{{"type","integer"},{"minimum",1},{"maximum",64},{"default",32}}}},{"type"});
        auto observation=object_schema({{"session_id",id},{"tick",rev},{"ids",{{"type","array"},{"maxItems",32},{"uniqueItems",true},{"items",id}}},
            {"query",query},{"entity_fields",entity_fields},{"components",{{"type","array"},{"maxItems",4},{"items",component_selector}}},
            {"include_schemas",{{"type","boolean"},{"default",false}}}},{"session_id","tick"});
        for(const auto* key:{"structure_revision","component_revision","gameplay_revision","ui_revision","control_sequence"})observation["properties"][key]=rev;
        observation["anyOf"]=Json::array({{{"required",{"ids"}},{"properties",{{"ids",{{"minItems",1}}}}}},{{"required",{"query"}}}});
        observation["allOf"]=Json::array({{{"if",{{"required",{"query"}},{"properties",{{"query",{{"required",{"after"}}}}}}}},
            {"then",{{"required",{"structure_revision","component_revision"}}}}}});
        methods["runtime.observe"]=std::move(observation);
    }
    methods["runtime.gameplay.inspect"]=object_schema({{"session_id",id},{"tick",rev},{"include_schema",{{"type","boolean"},{"default",false}}},{"fields",{{"type","array"},{"maxItems",128},{"uniqueItems",true},{"items",{{"type","string"}}}}}},{"session_id"});
    methods["runtime.gameplay.metrics"]=object_schema({{"session_id",id},{"tick",rev}},{"session_id"});
    methods["runtime.gameplay.metrics"]["description"]="Read optional per-module managed callback current-thread allocated bytes and runtime-wide GC collection deltas, without simulation or forced collection. Allocation churn is not heap/live bytes or per-frame allocation. Outside frozen authoring-core/runtime-game-session contracts; null module when no gameplay is loaded.";
    methods["runtime.gameplay.memory"]=object_schema({{"session_id",id},{"tick",rev}},{"session_id"});
    methods["runtime.gameplay.memory"]["description"]="Read a passive process-global CLR memory and module-lifetime snapshot through the current gameplay module, without simulation, forced collection or retirement pruning. Observations are non-atomic and can allocate; last-GC history is not current retained heap, process RAM or VRAM. Unnegotiated CoreCLR bridges and NativeAOT report unavailable data; null module when no gameplay is loaded. Outside frozen authoring-core/runtime-game-session contracts.";
    methods["runtime.gameplay.collect"]=object_schema({{"session_id",id}},{"session_id"});
    auto game_edit=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"expected_revision",rev},
        {"values",{{"type","object"},{"maxProperties",128}}}}, {"session_id","request_id","expected_tick","expected_revision","values"});
    methods["runtime.gameplay.edit"]=game_edit;
    auto game_load=game_edit;
    for(const auto* key:{"hostfxr","bridge","assembly","type"})game_load["properties"][key]={{"type","string"},{"minLength",1},{"maxLength",4096}};
    game_load["properties"]["profile_callbacks"]={{"type","boolean"},{"default",false},{"description","Opt-in CoreCLR callback allocation/GC observations; requires matched bridge negotiation, not saved gameplay state."}};
    game_load["required"]={"session_id","request_id","expected_tick","expected_revision","hostfxr","bridge","assembly","type"};
    methods["runtime.gameplay.load"]=game_load;
    auto native_load=game_edit;
    native_load["properties"]["descriptor"]={{"type","string"},{"minLength",1},{"maxLength",4096}};
    native_load["properties"]["expected_descriptor_sha256"]={{"type","string"},{"pattern","^[0-9a-f]{64}$"}};
    native_load["required"]={"session_id","request_id","expected_tick","expected_revision","descriptor"};
    methods["runtime.gameplay.load_native"]=native_load;
    auto input=object_schema({{"entity",id},{"move",vector({{"type","number"},{"minimum",-1},{"maximum",1}},2)},
        {"look",vector({{"type","number"},{"minimum",-180},{"maximum",180}},2)},{"jump",{{"type","boolean"}}},{"use",{{"type","boolean"}}},
        {"actions",{{"type","array"},{"maxItems",32},{"description","Names must be sorted and unique. Held state repeats through a tick batch; pressed/released apply on its first tick only."},
            {"items",object_schema({{"name",{{"type","string"},{"pattern","^[a-z][a-z0-9_.-]{0,63}$"}}},
                {"held",{{"type","boolean"}}},{"pressed",{{"type","boolean"}}},{"released",{{"type","boolean"}}}}, {"name","held","pressed","released"})}}}}, {"entity"});
    const auto motion=object_schema({{"entity",id},{"position",vector({{"type","number"},{"minimum",-1e6},{"maximum",1e6}},3)},
        {"rotation",vector({{"type","number"},{"minimum",-1},{"maximum",1}},4)},
        {"duration_ticks",{{"type","integer"},{"minimum",1},{"maximum",36000}}}}, {"entity","position","rotation","duration_ticks"});
    const Json motions={{"type","array"},{"maxItems",128},{"items",motion}};
    methods["runtime.raycast"]=object_schema({{"session_id",id},{"tick",rev},
        {"origin",vector({{"type","number"},{"minimum",-1e6},{"maximum",1e6}},3)},
        {"direction",vector({{"type","number"},{"minimum",-1e6},{"maximum",1e6}},3)},
        {"distance",{{"type","number"},{"minimum",.001},{"maximum",10000}}},
        {"ignore",{{"type","array"},{"maxItems",128},{"items",id},{"uniqueItems",true}}}}, {"session_id","tick","origin","direction","distance"});
    if(Runtime::available()) {
        methods["runtime.surface.raycast"]=methods.at("runtime.raycast");
        methods["runtime.surface.raycast"]["description"]="Read the closest committed physics ray hit and its authored collider surface name or null. Outside frozen runtime-game-session v1.";
    }
    methods["runtime.capsule_cast"]=object_schema({{"session_id",id},{"tick",rev},
        {"position",vector({{"type","number"},{"minimum",-1e6},{"maximum",1e6}},3)},
        {"rotation",clearance.at("properties").at("rotation")},
        {"radius",clearance.at("properties").at("radius")},{"half_height",clearance.at("properties").at("half_height")},
        {"direction",vector({{"type","number"},{"minimum",-1e6},{"maximum",1e6}},3)},
        {"distance",{{"type","number"},{"minimum",0},{"maximum",10000}}},
        {"ignore",{{"type","array"},{"maxItems",128},{"items",id},{"uniqueItems",true}}}},
        {"session_id","tick","position","rotation","radius","half_height","direction","distance"});
    methods["runtime.capsule_cast"]["description"]="Read-only fixed-orientation local-Y capsule sweep against current physics. half_height0 selects a sphere; distance0 checks initial overlap. Direction is nonzero and normalized for distance; quaternion norm-squared tolerance1e-5. Both mesh sides are queried; mesh shells remain shells. Initial overlaps select deepest contact; sweeps select earliest fraction; exact ties use entity/triangle then contact-point order. Hit includes both contact points, optional normal/triangle, penetration_depth and initial_overlap. Exact session/tick required; no simulation or save mutation. Outside frozen runtime-game-session v1.";
    result["capsule_queries"]={{"feature","capsule_cast_v1"},{"services_bytes",400},
        {"gameplay_attempts_per_tick",capsule_query_tick_budget},{"candidate_body_visits",capsule_query_body_budget},
        {"contact_callbacks",capsule_query_contact_budget},{"collision_tolerance_metres",capsule_query_collision_tolerance},
        {"budget_exhaustion","Error: clearance unknown; never a miss or clear result."},
        {"shape","Fixed-orientation local-Y capsule; half_height excludes spherical caps. Zero height is a sphere."},
        {"coordinates","Input center/endpoint within +/-1000km; contact points within +/-1e12m. Numeric bounds are not precision guarantees."},
        {"presentation","Authored Camera.viewmodel_clearance constrains its conservative proxy against current physics after interpolation; unresolved anchor/final contacts suppress foreground with an explicit receipt. No mutable presentation state is saved."}};
    methods["runtime.hit_test"]=methods.at("runtime.raycast");
    methods["runtime.hit_test"]["description"]="Read-only closest committed-pose collider/HitRegion query. Enabled region owners suppress only their own physics proxy; ignored owners suppress their regions, ignored attachments suppress that region. Exact ties prefer world geometry then region owner/name/attachment. Hit includes nullable region_entity and region; entity is damage owner for regions. Exact session/tick required, no tick advance.";
    methods["runtime.character_stance"]=object_schema({{"session_id",id},{"tick",rev},{"entity",id}},{"session_id","tick","entity"});
    methods["runtime.character_stance"]["description"]="Read committed controller stance with exact session/tick. Noncontrollers return state=null; legacy controllers report configured=false; missing entities reject. Reads do not change shape, retry standing, advance ticks or render.";
    methods["runtime.camera_pose"]=object_schema({{"session_id",id},{"tick",rev},{"entity",id}},{"session_id","tick","entity"});
    methods["runtime.camera_pose"]["description"]="Read committed additive local world and optional foreground-only poses and base_world_matrix with exact session/tick. Base matrix excludes all ancestor and local camera pose effects while retaining native look/stance and physical motion. Existing noncamera returns state=null; missing entity rejects. Does not advance, render or acknowledge.";
    methods["runtime.camera_lens"]=object_schema({{"session_id",id},{"tick",rev},{"entity",id}},{"session_id","tick","entity"});
    methods["runtime.camera_lens"]["description"]="Read committed world and optional first-person lenses plus immutable authored_world with exact session/tick. Existing noncamera returns state=null; missing entity rejects. No tick advancement, rendering or acknowledgement.";
    methods["runtime.camera_focus"]=methods.at("runtime.camera_lens");
    methods["runtime.camera_focus"]["description"]="Read the focus distance for the committed camera pose at the exact session and tick. Return the optional tracked entity's world point and range status. A camera with no depth-of-field profile or a noncamera returns state=null. Read only, independent of presentation eligibility.";
    methods["runtime.physical_rig"]=object_schema({{"session_id",id},{"tick",rev},{"entity",id},
        {"include_parts",{{"type","boolean"},{"default",false}}},{"include_joints",{{"type","boolean"},{"default",false}}}},
        {"session_id","tick","entity"});
    methods["runtime.physical_rig"]["description"]="Inspect committed native PhysicalRig summary with exact session/tick; absent component returns state=null, missing entity rejects. Optional parts expose actual allocated Jolt COM poses/velocities, including Animated off-broadphase allocations: those are not current animated targets. Joints expose instantaneous evaluated passive torques and optional drive controls/torques, not historical applied forces. Modes are animated, passive or driven. Reads never sample, synchronize, wake or tick. Outside frozen runtime-game-session-v1.";
    methods["runtime.physical_animation"]=object_schema({{"session_id",id},{"tick",rev},{"entity",id}},{"session_id","tick","entity"});
    methods["runtime.physical_animation"]["description"]="Inspect committed opt-in physical animation targets with exact session/tick; absent animation_drive configuration returns state=null, missing entity rejects. State includes entity, sampled_tick, strict/fit limit_policy, playback using the existing base/layer animation observation shape, and configured drive joints in authored order. Each joint has name, manual/animation source and sample: animation samples contain requested_rotation, target_rotation, correction_radians, chart_condition, hinge_projected, twist_clamped and swing_clamped; manual samples are null. Requested rotations are canonical child-joint rotations relative to parent joint, before the declared limit policy. Effective target and strength remain in runtime.physical_rig include_joints. With velocity_tracking enabled, state also contains velocity_tracking=true and each joint contains motion=null (manual) or {start_rotation,angular_velocity}: the fitted post-command interval start and parent-joint angular velocity in rad/s. These values drive damping toward target motion and are preserved in snapshots. Inspection never samples, queries contacts, wakes, advances or changes playback. Outside frozen runtime-game-session-v1.";
    methods["runtime.procedural_pose"]=object_schema({{"session_id",id},{"tick",rev},{"entity",id}},{"session_id","tick","entity"});
    methods["runtime.procedural_pose"]["description"]="Inspect opt-in PhysicalRig procedural ownership at an exact session/tick. animated_only owns manual IK; grounded_animated_only also owns configured grounding and planting. Animated diagnostics are current; physical diagnostics retain the last committed animated pose and its original sampled_tick, including historical grounding decisions and support frames for the grounded policy. Reports configured slots, never re-solves or re-probes a corpse. Absent policy returns state=null; missing entity rejects. No sampling, synchronization or tick advance. Additive boundary outside runtime-game-session-v1.";
    methods["runtime.controller_handoff"]=object_schema({{"session_id",id},{"tick",rev},{"entity",id}},{"session_id","tick","entity"});
    methods["runtime.physical_recovery"]=methods.at("runtime.controller_handoff");
    methods["runtime.physical_recovery"]["description"]="Read the committed opted-in recovery assessment with exact session/tick. Absent recovery profile returns state=null; missing entities reject. Reports physical/returning/locomotion phase, refusal/ready status, complete-window count, last request outcome, generation, bounded posture/speed measurements and current support body IDs. No query, pose sample, contact refresh or tick advance occurs on inspection. Submit physical_rigs:[{entity,recover:true}] on runtime.step; it is exclusive of other physical/animation/procedural writes to that rig. The request revalidates current static supports and whole standing capsule clearance; unsafe requests remain physical. Eligible return preserves the current pose before a bounded transition, changes one collision owner and clears animation-target delivery. This is near-upright handback, not balance or prone get-up. Outside frozen runtime-game-session-v1.";
    methods["runtime.controller_handoff"]["description"]="Inspect committed opt-in PhysicalRig controller ownership with exact session/tick. Reports actor, locomotion/physical phase, allocated capsule ID/enrollment and takeover tick. Passive support is historical cached_at_takeover, never current corpse grounding. Absent link returns state=null; missing entity rejects. No sampling, synchronization, waking or tick advance. Additive boundary outside runtime-game-session-v1.";
    methods["runtime.root_motion"]=object_schema({{"session_id",id},{"tick",rev},{"entity",id}},{"session_id","tick","entity"});
    methods["runtime.root_motion"]["description"]="Read cached committed planar root-motion controls and requested/actual interval diagnostics with exact session/tick. Existing nonrig or unconfigured rigs return state=null; missing entities reject. Inspection never samples, consumes or advances.";
    methods["runtime.grounding"]=object_schema({{"session_id",id},{"tick",rev},{"entity",id},{"slot",ik_slot}},{"session_id","tick","entity","slot"});
    methods["runtime.grounding"]["description"]="Read requested automatic-contact enable/weight multipliers with exact session/tick. Existing manual or absent slots return state=null; missing entity rejects. Frozen calibration and ownership remain unchanged. Requested controls are distinct from runtime.ik include_grounding effective decisions; reads never sample or probe.";
    methods["runtime.ik"]=object_schema({{"session_id",id},{"tick",rev},{"entity",id},{"slot",ik_slot},
        {"include_end_orientation",{{"type","boolean"},{"default",false}}},
        {"include_metric",{{"type","boolean"},{"default",false}}},
        {"include_grounding",{{"type","boolean"},{"default",false}}},
        {"include_planting",{{"type","boolean"},{"default",false}}},
        {"include_target_node",{{"type","boolean"},{"default",false}}}}, {"session_id","tick","entity","slot"});
    methods["runtime.ik"]["description"]="Read cached committed two-bone IK controls and diagnostics with exact session/tick. Returns source=runtime and state=null for an existing nonrig or absent configured slot; missing entity rejects. State includes frozen model indices, target/pole/space/weight/enabled, actual world end_position/target_error and clamped/pole_fallback. Inspection never samples or advances.";
    methods["runtime.ik"]["description"]=methods["runtime.ik"]["description"].get<std::string>()+" Opt-in include_end_orientation adds state.end_orientation=null or requested goal rotation/weight, actual rig-space rotation and angular_error radians. The default response shape is unchanged.";
    methods["runtime.ik"]["description"]=methods["runtime.ik"]["description"].get<std::string>()+" Opt-in include_metric adds state.metric=null for the uniform profile, or near-uniform-v1 cached evaluated/frame/Gram/local-scale and unweighted endpoint diagnostics. Frame/Gram/local-scale errors are maxima over the incoming and published poses. Inactive metric errors are null. The authored metric profile is frozen; runtime.step cannot replace it.";
    methods["runtime.ik"]["description"]=methods["runtime.ik"]["description"].get<std::string>()+" Opt-in include_grounding adds cached grounding decisions, probe counts, support and full-affine sole diagnostics, or null for manual slots. It never queries physics.";
    methods["runtime.ik"]["description"]=methods["runtime.ik"]["description"].get<std::string>()+" Independent include_planting adds state.planting=null or cached armed/status/support-local anchor, world sole/normal/heading targets and achieved residuals for plant_lock slots. It changes neither the previous grounding shape nor state; inspection never resolves support frames or probes.";
    methods["runtime.ik"]["description"]=methods["runtime.ik"]["description"].get<std::string>()+" Opt-in include_target_node adds the frozen attachment node index or null. For anchored slots the reported target, pole and orientation goal are the requested attachment-local values. Position and orientation errors include the committed attachment transform.";
    const Json sounds={{"type","array"},{"maxItems",64},{"items",{{"oneOf",Json::array({
        object_schema({{"op",{{"const","play"}}},{"emitter",id},{"gain",{{"type","number"},{"minimum",0},{"maximum",4},{"default",1}}}}, {"op","emitter"}),
        object_schema({{"op",{{"const","stop"}}},{"voice",{{"type","integer"},{"minimum",1},{"maximum",max_revision}}}}, {"op","voice"})
    })}}}};
    methods["runtime.step"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},
        {"ticks",{{"type","integer"},{"minimum",1},{"maximum",600}}},
        {"inputs",{{"type","array"},{"maxItems",32},{"items",input}}},{"motions",motions},{"sounds",sounds}}, {"session_id","request_id","expected_tick","ticks"});
    auto animation_command=animation_fields;animation_command["entity"]=id;
    animation_command["blend_ticks"]={{"type","integer"},{"minimum",0},{"maximum",3600},{"default",0}};
    animation_command["transition_mode"]={{"enum",{"crossfade","inertial"}},{"default","crossfade"}};
    const Json phase_markers={{"type","array"},{"minItems",2},{"maxItems",max_animation_phase_markers},{"items",{{"type","number"},{"minimum",0}}}};
    auto phase_sync=object_schema({{"source",phase_markers},{"destination",phase_markers}},{"source","destination"});
    phase_sync["description"]="Pairs clip landmarks in seconds, such as left and right foot contacts. Requires equal-length arrays in cyclic order, positive-duration looping clips, and destination time zero. Resolves the destination start at the committed command tick.";
    auto layer_command=animation_command;layer_command["layer"]={{"type","integer"},{"minimum",1},{"maximum",4}};
    layer_command["weight"]=unit;layer_command["weight_blend_ticks"]={{"type","integer"},{"minimum",0},{"maximum",3600},{"default",0}};
    // Keep the existing common properties/required projection discoverable.
    // Closed branches add layer-specific requirements without weakening the
    // base command's rejection of weight-only or unknown fields.
    auto animation_item=object_schema(layer_command,{"entity","clip","time","speed","loop","playing"});
    animation_item["oneOf"]=Json::array({
        object_schema(animation_command,{"entity","clip","time","speed","loop","playing"}),
        object_schema(layer_command,{"entity","layer","clip","time","speed","loop","playing","weight"})});
    methods["runtime.step"]["properties"]["animations"]={{"type","array"},{"maxItems",64},{"items",animation_item}};
    // Keep the released ordinary-animation union byte-identical. This is a
    // separately discoverable optional request field within the same step.
    auto phase_base=animation_command;phase_base["phase_sync"]=phase_sync;
    auto phase_layer=layer_command;phase_layer["phase_sync"]=phase_sync;
    auto phase_item=object_schema(phase_layer,{"entity","clip","time","speed","loop","playing","phase_sync"});
    phase_item["oneOf"]=Json::array({
        object_schema(phase_base,{"entity","clip","time","speed","loop","playing","phase_sync"}),
        object_schema(phase_layer,{"entity","layer","clip","time","speed","loop","playing","weight","phase_sync"})});
    methods["runtime.step"]["properties"]["animation_phase_matches"]={{"type","array"},{"maxItems",64},{"items",phase_item}};
    auto ik_command=ik_fields;ik_command["entity"]=id;
    methods["runtime.ik.clearance"]=object_schema({
        {"session_id",id},{"tick",rev},{"structure_revision",rev},
        {"goal",object_schema(ik_command,{"entity","slot","target","pole"})},
        {"radii",{{"type","array"},{"minItems",2},{"maxItems",2},{"items",{{"type","number"},{"minimum",.001},{"maximum",1}}}}},
        {"ignore",{{"type","array"},{"maxItems",128},{"uniqueItems",true},{"items",id}}}},
        {"session_id","tick","structure_revision","goal","radii"});
    methods["runtime.ik.clearance"]["description"]="Predict a manual two-bone slot over the current committed clip and layer sample. World-space radii describe the upper and lower segments. Four intervals conservatively sweep two capsules along linear joint paths. Reports current and candidate joint points and the first segment/interval blocker. Caller owns exclusions. Requires exact session, tick and structure revision. No state or clock changes. Outside frozen runtime-game-session-v1.";
    methods["runtime.step"]["properties"]["ik"]={{"type","array"},{"maxItems",64},{"items",object_schema(ik_command,{"entity","slot","target","pole"})}};
    auto lens_command=object_schema({{"entity",id},{"world",lens},{"viewmodel",lens}},{"entity"});
    lens_command["anyOf"]=Json::array({Json{{"required",{"world"}}},Json{{"required",{"viewmodel"}}}});
    const auto pose_vector=[](unsigned count,double bound) {return Json{{"type","array"},{"minItems",count},{"maxItems",count},{"items",{{"type","number"},{"minimum",-bound},{"maximum",bound}}}};};
    const auto frame_offset=object_schema({{"position",pose_vector(3,1e9)},{"rotation",pose_vector(4,1.000001)}},{"position","rotation"});
    const auto observed_frame=object_schema({{"entity",id},{"offset",frame_offset}},{"entity"});
    const auto frame_pair=object_schema({{"source",observed_frame},{"target",observed_frame},
        {"position_limit_m",{{"type","number"},{"minimum",0},{"maximum",1e9}}},
        {"rotation_limit_deg",{{"type","number"},{"minimum",0},{"maximum",180}}}},{"source","target"});
    methods["runtime.pose.compare"]=object_schema({{"session_id",id},{"tick",rev},{"structure_revision",rev},
        {"pairs",{{"type","array"},{"minItems",1},{"maxItems",pose_frames::max_pairs},{"items",frame_pair}}}},
        {"session_id","tick","pairs"});
    methods["runtime.pose.compare"]["description"]="Compare committed entity frames with optional rigid local offsets at an exact runtime tick. Reports origin displacement in metres and shortest angular error in degrees for proper orthogonal bases. Shear, reflection or degeneracy suppress angular results. Optional limits are caller-selected. Read-only, outside runtime-game-session v1.";
    const auto joint_check=object_schema({{"node",{{"type","integer"},{"minimum",0},{"maximum",physical_rig::max_node}}},
        {"limits",physical_rig::joint_limits_schema()},{"limit_frame",pose_vector(4,1.000001)},
        {"reference_rotation",pose_vector(4,1.000001)}},{"node","limits"});
    methods["runtime.pose.joints"]=object_schema({{"session_id",id},{"tick",rev},{"structure_revision",rev},{"rig",id},
        {"joints",{{"type","array"},{"minItems",1},{"maxItems",max_joint_pose_checks},{"items",joint_check}}}},
        {"session_id","tick","rig","joints"});
    methods["runtime.pose.joints"]["description"]="Check final committed rig-node local rotations against explicit hinge or swing/twist limits in radians. Defaults to the frozen authored local reference and identity limit frame. Returns signed twist, swing, per-joint status and a read-only fit when the chart is well defined. Model node indices must be distinct. Outside runtime-game-session v1.";
    const auto pose_point=pose_vector(3,1e6);
    const auto two=[](const Json& items){return Json{{"type","array"},{"minItems",2},{"maxItems",2},{"items",items}};};
    const auto pose_endpoint=object_schema({{"entity",id},{"point",pose_point}},{"entity"});
    const auto pose_segment=object_schema({{"endpoints",two(pose_endpoint)},
        {"radius",{{"type","number"},{"minimum",.001},{"maximum",1}}},
        {"previous",two(pose_point)}},{"endpoints","radius"});
    methods["runtime.pose.clearance"]=object_schema({{"session_id",id},{"tick",rev},{"structure_revision",rev},
        {"segments",{{"type","array"},{"minItems",1},{"maxItems",max_pose_clearance_segments},{"items",pose_segment}}},
        {"ignore",{{"type","array"},{"maxItems",128},{"uniqueItems",true},{"items",id}}}},
        {"session_id","tick","structure_revision","segments"});
    methods["runtime.pose.clearance"]["description"]="Check committed joint or offset-point capsules against current colliders. With two caller-supplied previous world points per segment, test both endpoints, then their conservative four-interval linear sweep if both are clear. Radii are world metres. Does not predict articulated paths or moving obstacles. Read-only, outside runtime-game-session v1.";
    methods["runtime.attachment.inspect"]=object_schema({{"session_id",id},{"tick",rev},{"structure_revision",rev}},
        {"session_id","tick"});
    methods["runtime.attachment.inspect"]["description"]="Read active rigid item attachment anchors and affine matrices at an exact committed tick. Outside runtime-game-session v1.";
    auto aim=methods.at("runtime.raycast");
    aim["properties"]["structure_revision"]=rev;
    aim["required"].push_back("structure_revision");
    aim["description"]="Trace the nearest physical collider at a committed tick and structure revision. Read the collider and its released item if the hit is that item's active proxy. Outside runtime-game-session v1.";
    methods["runtime.attachment.aim"]=std::move(aim);
    methods["runtime.attachment.outcomes"]=methods["runtime.attachment.inspect"];
    methods["runtime.attachment.outcomes"]["description"]="Read committed grip and release outcomes from the exact tick. A release reports the generated body as anchor. A blocked release reports the unchanged holder as anchor, an obstacle entity and penetration in metres. A reach_blocked pickup reports the attempted hand, the obstacle, segment, interval, fraction and penetration in metres. The item stays released. Outside runtime-game-session v1.";
    const auto source_vertex=object_schema({{"entity",id},{"vertex",rev}},{"entity","vertex"});
    const auto source_face=object_schema({{"entity",id},{"triangle",rev},
        {"barycentric",vector(unit,3)}},{"entity","triangle","barycentric"});
    const auto target_faces=object_schema({{"entity",id},
        {"triangles",{{"type","array"},{"minItems",1},{"maxItems",max_surface_selected_triangles},{"uniqueItems",true},{"items",rev}}}},{"entity"});
    const auto surface_pair=object_schema({{"source",{{"oneOf",Json::array({source_vertex,source_face})}}},
        {"target",target_faces},{"distance_limit_m",{{"type","number"},{"minimum",0},{"maximum",1e9}}}},
        {"source","target"});
    auto guarded_pair=surface_pair;
    guarded_pair["required"].push_back("distance_limit_m");
    guarded_pair["description"]="Source stays outside the item's effective subtree and target stays inside it. Samples candidate transforms before publication.";
    const auto attachment_contact=object_schema({{"item_offset",frame_offset},{"anchor_offset",frame_offset},
        {"position_limit_m",{{"type","number"},{"minimum",0},{"maximum",1e9}}},
        {"rotation_limit_deg",{{"type","number"},{"minimum",0},{"maximum",180}}},
        {"surface",{{"type","array"},{"minItems",1},{"maxItems",max_attachment_surface_probes},{"items",guarded_pair},
            {"description","At most 32 probes and 32768 target triangle checks across the batch."}}}},
        {"position_limit_m","rotation_limit_deg"});
    auto pickup_contact=attachment_contact;
    pickup_contact["required"].push_back("surface");
    const auto attachment_command=Json{{"oneOf",Json::array({
        object_schema({{"entity",id},{"mode",{{"const","keep_world"}}},{"anchor",parent}},{"entity","mode","anchor"}),
        object_schema({{"entity",id},{"mode",{{"const","keep_world"}}},{"anchor",id},{"contact",attachment_contact}},
            {"entity","mode","anchor","contact"}),
        object_schema({{"entity",id},{"mode",{{"const","snap"}}},{"anchor",id},{"offset",transform}},{"entity","mode","anchor","offset"}),
        object_schema({{"entity",id},{"mode",{{"const","reset"}}}},{"entity","mode"}),
        object_schema({{"entity",id},{"mode",{{"const","pickup"}}},{"anchor",id},{"contact",pickup_contact}},
            {"entity","mode","anchor","contact"})})}};
    methods["runtime.attachment.apply"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},
        {"expected_structure_revision",rev},{"commands",{{"type","array"},{"minItems",1},
            {"maxItems",max_runtime_attachments},{"items",attachment_command}}}},
        {"session_id","request_id","expected_tick","expected_structure_revision","commands"});
    methods["runtime.attachment.apply"]["description"]="Atomically transfer ordinary visual items to live anchors, hold their current world matrices, or restore authored hierarchy. Keep-world preserves the item at handoff and may use null to hold it in world space. Optional frame and surface contact limits with a live anchor reject a missed grip before publication. Snap uses the supplied local transform. Pickup requires an active released item and sampled contact; on success it rebinds the item and retires its release body together. Increments the structure revision without advancing time. Outside runtime-game-session v1.";
    methods["runtime.attachment.release"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},
        {"expected_structure_revision",rev},{"item",id},{"body_template",id},{"body_in_item",frame_offset},
        {"linear_velocity_m_s",pose_vector(3,100)},{"angular_velocity_rad_s",pose_vector(3,20)}},
        {"session_id","request_id","expected_tick","expected_structure_revision","item","body_template","linear_velocity_m_s","angular_velocity_rad_s"});
    methods["runtime.attachment.release"]["description"]="At the exact committed tick, release a held visual item onto a newly spawned world physics proxy. Supply a collider-only dynamic BoxCollider template, optional box frame in item space, and explicit world-space body velocities. Magnitudes are limited to 100 m/s and 20 rad/s. Reject initial penetration greater than 1 mm. The item keeps its pose and moves to world drawing. One structure revision. Outside runtime-game-session v1.";
    methods["runtime.surface.measure"]=object_schema({{"session_id",id},{"tick",rev},{"structure_revision",rev},
        {"pairs",{{"type","array"},{"minItems",1},{"maxItems",max_surface_pairs},{"items",surface_pair}}}},
        {"session_id","tick","pairs"});
    methods["runtime.surface.measure"]["description"]="Measure selected points against the closest rigid or skinned indexed triangles in committed world space. Supply a cooked vertex index or a triangle ordinal and three weights that sum to one. Omit target triangles to scan the whole target. Maximum 32768 target triangle checks per request. Returns closest point, unsigned distance, winding normals, signed normal offset and an optional distance verdict. Geometry precedes camera-specific effects. Read-only, outside runtime-game-session v1.";
    const auto mark_ray_vector=vector(mark_range(-1e6,1e6),3);
    const auto mark_ray=object_schema({{"origin",mark_ray_vector},{"direction",mark_ray_vector},
        {"distance",mark_range(.001,10000)}},{"origin","direction","distance"});
    auto snow_stamp=object_schema({{"expected_generation",rev},{"receiver",snow_tile_id},
        {"triangle",unsigned_range(0,2*snow::tile_cells*snow::tile_cells-1)},{"ray",mark_ray},
        {"radius_units",unsigned_range(snow::min_radius_units,snow::max_radius_units)},
        {"depth_units",unsigned_range(1,snow::max_depth_units)}},
        {"expected_generation","receiver","triangle","ray","radius_units","depth_units"});
    snow_stamp["description"]="On the first tick, compact the committed snow surface hit by the ray. Its first hit must match the receiver and cooked triangle at the expected generation. A failed stamp rejects the batch. Outside runtime-game-session v1.";
    result["snow"]={{"available",Runtime::available()},{"binding_operation","snow.set"},{"water_coexistence",true},
        {"material_displacement","Set the same y-axis StaticMesh.displacement on both tiles. Height texture relief affects rendering and shadows on the compacted mesh. Collisions use the compacted lattice."},
        {"samples_x",snow::samples_x},{"samples_z",snow::samples_z},{"spacing_m",snow::spacing_m},
        {"depth_unit_m",snow::unit_m},{"volume_unit_m3",snow::volume_unit_m3},{"stamps_per_step",1},
        {"foot_release",std::move(foot_release)},
        {"foot_release_limits",{{"holding",snow::max_held_releases},{"ready",snow::max_ready_releases},
            {"attempts_per_tick",1},{"snapshot_version",2},{"sole_snapshot_version",3}}}};
    result["physics_checkpoint"]={{"available",Runtime::available()},{"binding_operation","physics_checkpoint.set"},
        {"profile_version",1},{"profile",checkpoint_profile},
        {"scope","Preserve named rigid-body contact, sleep and solver state in a portable save. Virtual controllers and PhysicalRig content use their existing profiles."}};
    if(Runtime::available()) {
        const Json sample={{"type","array"},{"minItems",2},{"maxItems",2},
            {"prefixItems",Json::array({unsigned_range(0,snow::samples_x-1),unsigned_range(0,snow::samples_z-1)})}};
        methods["runtime.snow.inspect"]=object_schema({{"session_id",id},{"tick",rev},{"structure_revision",rev},
            {"samples",{{"type","array"},{"maxItems",64},{"items",sample}}}},{"session_id","tick","structure_revision"});
        methods["runtime.snow.inspect"]["description"]="Read committed generation, tile binding, loss and up to 64 lattice samples. Opt-in foot_release includes held and ready intents and outcome counts. Outside runtime-game-session v1.";
    }
    auto mark_command=object_schema({{"receiver",id},{"source",id},{"ray",mark_ray},{"travel_direction",mark_ray_vector},{"profile",mark_profile_id},
        {"lifetime_ticks",mark_lifetime},{"overflow",{{"enum",{"reject","recycle_oldest_timed"}},{"default","reject"}}}},{"receiver","ray"});
    mark_command["properties"].update(mark_style_props);
    for(const auto& field:mark_style_required)mark_command["required"].push_back(field);
    mark_command["allOf"]=Json::array({Json{
        {"if",{{"properties",{{"overflow",{{"const","recycle_oldest_timed"}}}}},{"required",{"overflow"}}}},
        {"then",{{"required",{"lifetime_ticks"}}}}}});
    methods["runtime.step"]["properties"]["surface_marks"]={{"type","array"},{"maxItems",max_surface_mark_commands},
        {"items",mark_command},{"description","Deposit marks in list order after final motion on the first simulated tick. Optional source makes the ray and travel direction relative to its final unscaled rigid frame. Optional profile selects a mask in the frozen world catalog. Each ray must hit its named rigid opaque receiver first. Caller and gameplay placements take priority over automatic footprints in the shared budget. Outside runtime-game-session v1."}};
    methods["runtime.surface_marks.inspect"]=object_schema({{"session_id",id},{"tick",rev},{"structure_revision",rev},{"entity",id}},
        {"session_id","tick","structure_revision"});
    methods["runtime.surface_marks.inspect"]["description"]="Read receiver-local mark frames, material parameters and optional expires_tick and relief_depth_m in placement order. Zero relief is omitted. The optional entity filter selects one receiver. Lifecycle counts cover the whole scene. Outside runtime-game-session v1.";
    result["surface_marks"]={{"available",Runtime::available()},{"commands_per_step",max_surface_mark_commands},
        {"marks_per_receiver",max_surface_marks_per_receiver},{"marks_per_scene",max_scene_surface_marks},
        {"receiver","Unscaled opaque world drawable with a matching built-in box or cooked triangle collider."},
        {"orientation","Optional travel_direction aligns the first half extent along the contact plane. Minimum length 1e-8. A normalized projection of length at most 1e-6 uses the default receiver-local axes."},
        {"profiles",{{"max_profiles",max_surface_mark_profiles},{"max_masks",max_surface_mark_masks},
            {"max_mask_side",max_surface_mark_mask_side},{"format","Linear cooked image, red channel coverage, mip 0."}}},
        {"relief",{{"field","relief_depth_m"},{"max_depth_m",surface_mark_validation::max_authored_relief_depth_m},
            {"default_depth_m",0},{"composition","Affects the forward and deferred shading normal independently of color opacity."}}},
        {"lifecycle",{{"max_lifetime_ticks",max_surface_mark_lifetime_ticks},
            {"expiration","Marks expire at placement tick plus lifetime, before new placements on that tick."},
            {"recycling","On capacity, use the oldest timed mark on a full receiver, or the oldest scene-wide timed mark when only the scene is full. A request without an eligible replacement is dropped after validation. Permanent marks are protected."}}},
        {"footprints",{{"component","FootprintSurface"},
            {"trigger","A newly acquired native foot plant within the surface's solved sole tolerance."},
            {"order","After caller and gameplay commands, in ascending rig ID and IK slot. Skip acquisitions above the shared 16-attempt limit."},
            {"placement","Use the committed grounding hit and planted heading. Timed marks recycle oldest eligible timed marks and protect permanent marks."}}},
        {"persistence","Optional surface_marks snapshot extension. Version 4 stores positive relief with lifecycle counts when needed. Version 3 stores timed rows and lifecycle counts. Version 2 stores permanent textured marks. Removing a receiver retires its marks."}};
    result["gameplay_surface_marks"]={{"feature",gameplay_abi::surface_mark_feature},
        {"services_abi",gameplay_abi::services_version},{"services_bytes",gameplay_abi::surface_mark_services_bytes},
        {"command_bytes",sizeof(PoimaGameSurfaceMarkCommandV1)},{"command_version",1},
        {"directional",{{"feature",gameplay_abi::surface_mark_direction_feature},
            {"services_bytes",gameplay_abi::surface_mark_direction_services_bytes},
            {"command_bytes",sizeof(PoimaGameSurfaceMarkCommandV2)},{"command_version",2}}},
        {"textured",{{"feature",gameplay_abi::surface_mark_texture_feature},
            {"services_bytes",gameplay_abi::surface_mark_texture_services_bytes},
            {"command_bytes",sizeof(PoimaGameSurfaceMarkCommandV3)},{"command_version",3}}},
        {"timed",{{"feature",gameplay_abi::surface_mark_timed_feature},
            {"services_bytes",gameplay_abi::surface_mark_timed_services_bytes},
            {"command_bytes",sizeof(PoimaGameSurfaceMarkCommandV4)},{"command_version",4}}},
        {"attempts_per_tick",max_surface_mark_commands},{"combined_commands_per_tick",max_surface_mark_commands},
        {"order","Caller commands, then gameplay commands in callback order, then automatic footprints."},
        {"placement","Tick only. Ray starts in world coordinates or in the final unscaled rigid frame of a source entity."}};
    const auto camera_pose=object_schema({{"position",pose_vector(3,10)},{"rotation",pose_vector(4,1.00001)}},{"position","rotation"});
    auto pose_command=object_schema({{"entity",id},{"world",camera_pose},{"viewmodel",camera_pose}},{"entity"});
    pose_command["anyOf"]=Json::array({Json{{"required",{"world"}}},Json{{"required",{"viewmodel"}}}});
    methods["runtime.step"]["properties"]["camera_poses"]={{"type","array"},{"maxItems",64},{"items",pose_command}};
    result["invariants"].push_back("camera_pose_v1 replaces selected additive local rigid offsets; explicit identity resets. World effect composes after native look/stance and is visible in committed runtime entity world transforms. Foreground effect changes selected foreground draw matrices only, leaving physical/hit-query state unchanged. Cameras owned by ModelNode animation or physics reject; world effects reject physical descendants. Authored Camera membership and foreground presence remain frozen. Compiled setters stage, reads stay committed; caller/gameplay duplicate targets, more than64 writes, removal conflicts and invalid poses reject the full batch. Nonidentity states use optional camera_poses version1 save extension, with complete world/viewmodel rows; identity defaults retain old save shape.");
    methods["runtime.step"]["properties"]["camera_lenses"]={{"type","array"},{"maxItems",64},{"items",lens_command}};
    methods["runtime.step"]["properties"]["animation_rates"]={{"type","array"},{"maxItems",64},{"items",object_schema({{"entity",id},{"speed",{{"type","number"},{"minimum",0},{"maximum",8}}},{"layer",{{"type","integer"},{"minimum",1},{"maximum",4}}}},{"entity","speed"})}};
    result["invariants"].push_back("Animation rate commands change only destination speed, preserving committed phase and an active fade's original source, correction and deadline. Omitted layer selects the base; layer1..4 must be configured. Speeds0..8; zero holds phase while fade ticks continue, without resuming stopped/ended clips. Full replacements and rates share64 unique-clock writes per Tick across caller/gameplay; conflicts reject the whole batch. Same-speed writes consume admission but do not reanchor. Retimed clocks select nested animation-statev9 with explicit legacy_version; unretimed saves retain their original version. Existing animation inspection reports the effective rate/phase; no getter samples or advances.");
    const Json physical_point={{"type","array"},{"minItems",3},{"maxItems",3},{"items",{{"type","number"},{"minimum",-1e6},{"maximum",1e6}}}};
    const Json physical_impulse_vector={{"type","array"},{"minItems",3},{"maxItems",3},{"items",{{"type","number"},{"minimum",-1e5},{"maximum",1e5}}}};
    const auto physical_impulse=object_schema({{"part",{{"type","string"},{"minLength",1},{"maxLength",64},{"pattern","^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$"}}},
        {"world_point",physical_point},{"impulse",physical_impulse_vector}},{"part","world_point","impulse"});
    const auto physical_drive=object_schema({{"joint",{{"type","string"},{"pattern","^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$"}}},
        {"target_rotation",pose_vector(4,1.000001)},{"strength",unit}},{"joint","target_rotation","strength"});
    const auto physical_source=object_schema({{"joint",{{"type","string"},{"minLength",1},{"maxLength",64},{"pattern","^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$"}}},
        {"source",{{"enum",{"manual","animation"}}}},{"strength",unit}},{"joint","source","strength"});
    auto target_base_fields=phase_base;target_base_fields.erase("entity");
    auto target_layer_fields=phase_layer;target_layer_fields.erase("entity");
    auto physical_animation=object_schema(target_layer_fields,{"clip","time","speed","loop","playing"});
    physical_animation["oneOf"]=Json::array({
        object_schema(target_base_fields,{"clip","time","speed","loop","playing"}),
        object_schema(target_layer_fields,{"layer","clip","time","speed","loop","playing","weight"})});
    auto target_rate_fields=methods["runtime.step"]["properties"]["animation_rates"]["items"]["properties"];target_rate_fields.erase("entity");
    const auto physical_animation_rate=object_schema(target_rate_fields,{"speed"});
    auto physical_command=object_schema({{"entity",id},{"activate",{{"type","boolean"},{"default",false}}},{"impulse",physical_impulse},{"drive",physical_drive},
        {"drive_source",physical_source},{"animation",physical_animation},{"animation_rate",physical_animation_rate},{"recover",{{"type","boolean"}}}},{"entity"});
    physical_command["anyOf"]=Json::array({{{"required",{"activate"}},{"properties",{{"activate",{{"const",true}}}}}},{{"required",{"impulse"}}},{{"required",{"drive"}}},
        {{"required",{"drive_source"}}},{{"required",{"animation"}}},{{"required",{"animation_rate"}}},{{"required",{"recover"}},{"properties",{{"recover",{{"const",true}}}}}}});
    Json recover_only=Json::object();recover_only["if"]["required"]=Json::array({"recover"});
    recover_only["if"]["properties"]["recover"]["const"]=true;
    recover_only["then"]["not"]["anyOf"]=Json::array();
    for(const auto* other:{"activate","impulse","drive","drive_source","animation","animation_rate"})
        recover_only["then"]["not"]["anyOf"].push_back({{"required",Json::array({other})}});
    physical_command["allOf"]=Json::array({std::move(recover_only)});
    methods["runtime.step"]["properties"]["physical_rigs"]={{"type","array"},{"maxItems",64},{"items",physical_command}};
    result["invariants"].push_back("Opt-in physical joint drives hold a bounded relative orientation with explicit strength0..1. They require an authored drive profile and physical ownership; activation then drive follows literal command order. Duplicate rig/joint writes across caller/gameplay reject atomically. Driven mode means at least one positive strength, while zero returns passive physical ownership without controller recovery. Applied internal torques retain the passive response and native friction; no joint/body teleport. Complete targets/strengths select physical save extension5, preserving earlier encodings for worlds without authored drives. Joint inspection reports instantaneous evaluated response, not historical applied torque. Compiled control requires physical_rig_drive_v1 and448-byte services; legacy physical_rig_v1 summaries explicitly reject Driven mode.");
    result["invariants"].push_back("PhysicalRig.animation_drive opts into a detached clip/layer target player sharing the frozen source model. physical_rigs.animation and animation_rate use ordinary animation command fields and validation, except entity is selected by the outer physical command. Target clocks advance independently of displayed/source animation, with no IK, grounding, root-motion stripping or skin publication. Per-joint drive_source selects manual or animation and an explicit strength0..1. Manual selection keeps the effective backend orientation while applying the requested strength and clears current animation-sample attribution; an ordinary drive writes a strict manual target. Animation uses the authored strict/fit limit policy and exposes its canonical request, admitted orientation and fitting diagnostics. These fields share the64 ordered physical-command limit and whole-batch rollback/retry; duplicate target-clock or joint-source writes reject. Target playback does not activate physics implicitly or recover animation ownership. Opted-in targets use complete target-history persistence, separately from unchanged legacy animation-state formats and earlier physical-save encodings. No active balance, contact-aware gait or recovery is implied.");
    result["invariants"].push_back("PhysicalRig commands join full fixed-tick batch rollback. At most64 ordered caller/gameplay commands apply on their first Tick; at mostone activation per rig. Multiple per-part impulses are preserved in order. Activation copies final committed animation COM poses/momentum; impulses require Passive or Driven mode and use world-metre impact points and Newton-seconds, with a100m native lever bound. No implicit mode flip, recovery or active balance is implied. Physical rig part allocations belong to their exact compound rig owner for existing physical queries/ignores; arbitrary hierarchy descendants remain separate.");
    methods["runtime.step"]["properties"]["grounding"]={{"type","array"},{"maxItems",64},{"items",object_schema({{"entity",id},{"slot",ik_slot},{"enabled",{{"type","boolean"}}},{"weight",unit}},{"entity","slot","enabled","weight"})}};
    result["invariants"].push_back("Grounding commands control only frozen automatic slots. Complete unique entity/slot/enabled/weight rows apply on the first Tick; gameplay stages writes with a combined64-per-Tick budget and same-slot conflicts reject. Enabled is ANDed with authored enable and weight multiplies authored weight; defaults true/1 preserve prior behavior. Disabling never releases automatic-slot ownership. No control read/write issues a probe: only the existing final postphysics phase computes effective contacts. Requested policy, decisions, counters and game/physics state share whole-batch rollback. Nonidentity policy selects nested animationv8; default identity retains exactv7 and v7 loads reset multipliers.");
    methods["runtime.step"]["properties"]["character_stances"]={{"type","array"},{"maxItems",32},{"items",object_schema({{"entity",id},{"crouch",{{"type","boolean"}}}},{"entity","crouch"})}};
    result["invariants"].push_back("CharacterController stance is optional frozen content; omission preserves the old controller profile. Empty stance selects crouched_height=1, crouched_speed=2, camera_drop=0.8. Crouched height must exceed twice radius and be below standing height; camera drop must fit their difference (1e-6 numeric allowance). Complete unique character_stances commands persist desired posture and apply on the first Tick under full batch rollback. Obstructed standing remains crouched and reports stand_blocked; inspection never retries it. Body feet and collision-tested capsule clearance remain authoritative. No stair, platform, prone, camera-pose interpolation or general locomotion guarantee is implied.");
    result["invariants"].push_back("CharacterController locomotion is an optional frozen virtual-v1 profile. Omission retains the legacy physical controller and omits locomotion from runtime.entity. Empty locomotion selects step_height=0.4, ground_snap=0.5 and max_slope_degrees=45; finite bounds are 0..1, 0..1 and 0..80. Runtime entity diagnostics expose only the configured profile and cached ground_normal/ground_velocity; inspection does not advance or refresh contacts. Body queries retain the character owner identity. Live authored edits do not change the frozen controller backend or profile.");
    result["invariants"].push_back("Camera lens commands replace complete world and/or configured viewmodel lenses, at most64 unique camera targets combined with gameplay per tick. Caller commands apply on the first tick; gameplay writes stage until callback completion. Viewmodel presence and CameraDraw membership stay frozen. All native camera changes roll back with a failed batch and persist in snapshotv7; old snapshots use authored lenses. Player FOV preferences form the neutral baseline: effective FOV composes the runtime/authored tan-half-angle ratio and saturates to5..150; viewmodel lens is independent.");
    methods["runtime.step"]["properties"]["root_motion"]={{"type","array"},{"maxItems",64},{"items",object_schema({{"entity",id},{"enabled",{{"type","boolean"},{"default",true}}}},{"entity"})}};
    result["invariants"].push_back("AnimationRig optionally freezes an explicit model-node root_motion profile. Extract observes planar intervals without moving physics; Character drives one self/ancestor capsule through the normal physics update, preserving gravity/jump and collision-resolved displacement. Configured pose stripping remains active when disabled. Positive-uniform upright bindings, exact source ancestry and layer-source exclusion are required. Enabled Character forbids move input, and yaw-enabled drivers forbid horizontal look. Complete first-Tick enable commands share rollback/retry; no blocked-motion debt, vertical root locomotion or motion warping is implied. Root-configured animation saves use nested statev5; absent profiles retain previous serialization.");
    result["invariants"].push_back("AnimationRig optionally freezes up to eight IK chains with unique slots1..8 and distinct direct-parent model indices root/middle/end, exact matching RigNode graph and no overlapping solved branches. IK wrappers cannot be below RigNode. The default uniform profile requires positive uniform evaluated model-ancestor/root/middle scales; wrapper affine transforms must be invertible. Point-only post-layer corrections preserve local translation/scale/end rotation and pre-IK animation histories. World point goals stay fixed as the wrapper moves; rig point goals follow it. Zero positional weight without an orientation goal, or disabled chains, preserve incoming locals exactly; reach clamping uses rig metric and pole fallback is deterministic.");
    result["invariants"].push_back("runtime.step ik complete controls persist, apply on the first Tick and share full batch rollback. Up to64 independent caller/gameplay IK writes are permitted per Tick with unique rig/slot targets; same-slot conflicts reject, while playback and IK can address the same rig. runtime.ik is cached committed state. Optional end_orientation requires a finite nonzero XYZW quaternion and independent0..1 weight in rig space; omitted/zero clears the goal. Without an authored grounding profile, retained nonzero goals select nested animation-statev6, preserving legacy point-only serialization. Manual IK does not imply automatic contacts, full-body solving or joint limits. Authored grounding with identity requested controls selects nestedv7; nonidentity requested controls selectv8.");
    result["invariants"].push_back("AnimationRig IK metric defaults to uniform. Explicit near-uniform-v1 keeps original scales and uses bounded approximation: incoming and published local scale spread, normalized cumulative frame deviation and Gram error must each be at most1e-4; full unweighted endpoint error must be at most max(1e-9,1e-4 times combined link length) in model coordinates before positional blending. Rest admission and every active evaluated solve validate independently; failure rolls back the whole batch. include_metric reads cached diagnostics only. This metric alone is not general affine IK or an automatic contact controller.");
    result["invariants"].push_back("MeshRenderer, StaticMesh and SkinnedMesh optionally define strict boolean cast_shadows and receive_shadows, each defaulting to true when omitted. visible=false suppresses the drawable entirely. These controls do not change colliders, acoustics, material ambient/AO or AnimationRig playback; AnimationRig and RigNode are not drawables and do not inherit controls to their children.");
    result["invariants"].push_back("Animated asset instances expose a wrapper AnimationRig, ordinary RigNode entities for every model node, and SkinnedMesh primitive children. Authored transforms are the baseline; authored capture does not play the initial clip. runtime.step animations replace complete clip/time/speed/loop/playing state atomically with other tick commands.");
    result["invariants"].push_back("Animation commands optionally accept blend_ticks (0..3600, default 0). Zero switches immediately; positive values blend the outgoing and destination local poses over fixed ticks, independently of playback speed. Both clip clocks advance during an ordinary fade. Interrupting a fade freezes its evaluated local pose as the new source; no nested blend tree is retained. runtime.entity animation.transition reports active weights/clocks and is null on completion. Transition state and frozen source poses join batch rollback.");
    result["invariants"].push_back("Optional animation transition_mode defaults to crossfade. Inertial mode applies finite-duration translation, shortest-arc rotation and positive log-scale corrections to the destination, initialized from the last two distinct output ticks and analytic incoming clip derivatives. Missing output history uses zero outgoing velocity; STEP keys, loop seams and endpoints may still be discontinuous. Same-tick samples do not advance velocity history. Native checkpoints and version-2 animation saves retain history and corrections. Active inertial transitions report mode=inertial; weight reports elapsed progress, not a two-clip pose weight. The legacy C# setter selects crossfades; opt-in animation_inertial_v1 supports both transition modes.");
    result["invariants"].push_back("AnimationRig optionally authors up to four frozen layers in slots1..4. Unique model-node mask weights multiply the current layer weight; unlisted nodes are unaffected locally. Ascending slots compose override or additive local TRS. Additive deltas use a frozen authored or explicit reference clip pose, with node-local rotation postmultiplication and positive scale ratios. Descendant world poses can change when a masked ancestor moves.");
    result["invariants"].push_back("Animation commands with layer target a configured slot and require weight0..1. Clip transitions and optional linear weight_blend_ticks run on independent fixed clocks; interrupted weight fades retain the current value without guaranteeing continuous weight velocity. Base and distinct layer targets may share a batch, but duplicate(entity,slot) commands and more than64 combined caller/gameplay commands reject. Each clock retains its own unmasked motion history. animation.layers exposes configured summaries; nested animation-statev3 persists all clocks and weights. Existing C#176/192 callbacks remain base-only; opt-in animation_layers_v1 adds compiled layer queries and commands through a separate208-byte services7 table. Desktop layer widgets are not implemented.");
    auto capture=methods["world.capture"];
    capture["properties"].erase("revision"); capture["properties"]["session_id"]=id; capture["properties"]["tick"]=rev;capture["properties"]["ui_revision"]=rev;
    capture["required"]={"session_id","tick","camera","path"}; methods["runtime.capture"]=capture;
    methods["runtime.capture"]["properties"]["reuse_renderer"]={{"type","boolean"},{"default",false},
        {"description","Retain one renderer for subsequent matching live captures. Temporal history may carry between samples."}};
    methods["runtime.capture.release"]=object_schema({{"session_id",id}},{"session_id"});
    auto play=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"controller",id},{"camera",id},
        {"mode",{{"enum",{"interactive","replay","background"}}}}, {"max_frames",{{"type","integer"},{"minimum",0},{"maximum",36000}}}},
        {"session_id","request_id","expected_tick","camera","mode"});
    play["allOf"]=Json::array({{{"if",{{"properties",{{"mode",{{"const","replay"}}}}}}},{"then",{{"required",{"controller","sequence"}}}}}});
    auto segment=input; segment["properties"].erase("entity"); segment["properties"]["ticks"]={{"type","integer"},{"minimum",1},{"maximum",600}}; segment["required"]={"ticks"};
    segment["properties"]["motions"]=motions;segment["properties"]["sounds"]=sounds;
    play["properties"]["sequence"]={{"type","array"},{"minItems",1},{"maxItems",256},{"items",segment}};
    for(const auto* key:{"path","width","height","gpu","samples","viewmodel_samples","culling","draw_instancing","gpu_visibility","clustered_lighting","frames_in_flight","scene_debug_view","scene_product_probes","lighting_path","ambient_occlusion","reconstruction","shadow_filter","post_aa","profile"}) play["properties"][key]=capture["properties"][key];
    play["properties"]["audio"]={{"type","boolean"},{"default",false}};
    play["properties"]["early_reflections"]={{"type","boolean"},{"default",false},
        {"description","Enable moving first-order reflections during player audio output. Requires audio:true. Outside authoring-core v1."}};
    play["allOf"].push_back({
        {"if",{{"required",{"early_reflections"}},{"properties",{{"early_reflections",{{"const",true}}}}}}},
        {"then",{{"required",{"audio"}},{"properties",{{"audio",{{"const",true}}}}}}}});
    play["properties"]["pose_interpolation"]={{"type","boolean"},{"default",false},
        {"description","Opt-in saved camera-effect presentation interpolation; native look/movement/base aim stay committed. Interactive/background only; replay rejects true. Outside authoring-core v1."}};
    play["allOf"].push_back({{"if",{{"properties",{{"mode",{{"const","replay"}}}}}}},
        {"then",{{"properties",{{"pose_interpolation",{{"const",false}}}}}}}});
    play["properties"]["lens_interpolation"]={{"type","boolean"},{"default",false},
        {"description","Opt-in lens-only presentation interpolation between the final two consecutive committed Ticks. Interactive/background elapsed clocks only; replay rejects true. Does not change native camera state or authored cameras. Outside authoring-core v1."}};
    play["allOf"].push_back({{"if",{{"properties",{{"mode",{{"const","replay"}}}}}}},
        {"then",{{"properties",{{"lens_interpolation",{{"const",false}}}}}}}});
    play["allOf"].push_back({{"if",{{"properties",{{"mode",{{"const","background"}}}}}}},
        {"then",{{"not",{{"required",{"sequence"}}}}}}});
    play["properties"]["input_profile"]=input_path;
    play["properties"]["input_revision"]=rev;
    play["properties"]["settings_profile"]=input_path;
    play["properties"]["settings_revision"]=rev;
    play["properties"]["settings_overrides"]=player_settings::values_schema();
    play["properties"]["gamepad"]=object_schema({{"mode",{{"enum",{"disabled","only_connected","explicit"}}}},{"id",{{"type","integer"},{"minimum",1},{"maximum",4294967295ULL}}}},{"mode"});
    methods["runtime.play"]=play;
    auto player_start=play;
    player_start["properties"]["expected_generation"]=rev;
    player_start["properties"]["initially_paused"]={{"type","boolean"},{"default",false}};
    player_start["required"].push_back("expected_generation");
    player_start["description"]="Start an owner-pumped native player in a shared headless host. Acknowledgement precedes graphics initialization; inspect readiness/completion separately. Outside authoring-core v1.";
    methods["player.start"]=std::move(player_start);
    methods["player.inspect"]=object_schema({{"player_id",id}});
    methods["player.inspect"]["description"]="Read lifecycle and cached readiness conditions without polling or OS/device queries. readiness is null without a retained owner; focus/drawable are null until existing platform paths observe them. Terminal observations are historical. Predicate conditions explain current ready/paused booleans; they are not inferred OS causes. Outside authoring-core v1.";
    methods["player.ui.inspect"]=object_schema({{"player_id",id},{"generation",rev},{"session_id",id},{"tick",rev},{"ui_revision",rev}},
        {"player_id","generation","session_id"});
    methods["player.ui.inspect"]["description"]="Read the last successful UI presentation for an exact live player identity/generation/session, without poll, DOM work, input or device access. Tick and ui_revision are optional strict optimistic guards; omit them for running observation without a read/retry race. The response always reports the atomically observed current Tick/UI revision separately from displayed tags. Displayed tags remain historical after edits/replacement until another frame is presented; matches_current_owner exposes staleness. Null displayed means no UI frame was presented. Physical viewport-local untransformed border bounds, ancestor scissor, computed font pixels and sampled hittability are not glyph ink, transformed coverage or an occlusion-area measurement. Labels/buttons only; use runtime.ui.inspect for the complete logical hierarchy. No terminal retention. Outside authoring-core v1.";
    methods["player.diagnostics.inspect"]=object_schema({{"player_id",id},{"generation",rev}},{"player_id","generation"});
    methods["player.diagnostics.inspect"]["description"]="Read up to 128 chronological retained slow CPU polls for an exact player identity/generation. No simulation, presentation, device access, drain or receipt. A terminal snapshot survives until another player replaces its owner. Outside authoring-core v1.";
    methods["player.control"]=object_schema({{"player_id",id},{"request_id",id},{"expected_control_revision",rev},
        {"action",{{"enum",{"pause","resume","stop"}}}}},{"player_id","request_id","expected_control_revision","action"});
    methods["player.capture"]=object_schema({{"player_id",id},{"request_id",id},{"expected_control_revision",rev},
        {"session_id",id},{"tick",rev},{"expected_structure_revision",rev},{"expected_ui_revision",rev},
        {"path",{{"type","string"},{"minLength",1},{"description","New BMP output in an existing directory; captures the live player's graphics context without advancing simulation."}}}},
        {"player_id","request_id","expected_control_revision","session_id","tick","path"});
    methods["player.capture"]["properties"]["include_ui_layout"]={{"type","boolean"},{"default",false},
        {"description","Include capture.ui_layout from the frame copied to the BMP and presented, or null when it has no logical HUD. The layout contains viewport-local control rectangles, flags and the exact submission identity."}};
    methods["player.capture_current"]=methods["player.capture"];
    methods["player.capture_current"]["properties"].erase("tick");
    methods["player.capture_current"]["required"]={"player_id","request_id","expected_control_revision","session_id","path"};
    methods["player.capture_current"]["description"]="Atomically choose the current committed Tick and freeze one owned live-window presentation before capturing it. Tick is forbidden; player/control/session and applicable material/structure/UI guards remain exact. Returns captured_tick and full raw committed_camera_lens {entity,world,viewmodel,authored_world}, separate from rendered lens/presentation metadata. Retried request IDs return the original captured sample even after later Ticks. Does not step simulation, advance the interpolation clock or pause playback. Outside authoring-core v1.";
    result["invariants"].push_back("player.start/control/capture require the shared headless owner pump. Start acknowledges deferred initialization; player.inspect reports current readiness and the latest owned partial/final report. Generation, player identity and control revisions guard lifecycle commands; 32 process-local receipts replay acknowledged commands without reapplying them. Stop closes presentation and retains runtime state. Running players reject external simulation mutations; paused interactive players accept guarded edits. Replay rejects intervening runtime edits. Replacement invalidates input and pauses interactive playback. Captures use the same live graphics context and current committed tick with exclusive new output; they do not simulate.");
    result["invariants"].push_back("Optional strict Boolean pose_interpolation defaults false and rejects replay before graphics initialization. Only selected world/foreground camera effect offsets interpolate between consecutive committed Ticks, using linear local position and shortest-arc quaternion SLERP. Native look, movement, queries, saved state and BaseWorld remain committed. Source/session/camera/material/presentation cuts, pause/inactivity, resize, preference edits and discontinuous Ticks reset the interval. Opt-in player receipts/captures expose pose_presentation and actual renderer camera/foreground matrices. Default receipts remain unchanged. Outside authoring-core v1.");
    result["invariants"].push_back("Optional strict Boolean lens_interpolation defaults false on runtime.play/player.start and rejects true for replay before device initialization. Opt-in interpolation changes only the presented world/foreground lens; committed native camera values and saved state remain exact. Player observed/submitted/presented receipts and captures add lens_presentation with previous_tick/target_tick, alpha, reset_reason, committed world/foreground endpoints, rendered lenses and authored/preference FOV baselines. Captures freeze one owned presentation sample without advancing a Tick or clock, and require an exact rendered scene match to that sample. Omitted/default-false receipts retain their existing fields. This evolving player option is outside authoring-core v1.");
    result["invariants"].push_back("player.capture retains its required exact Tick guard. The separate player.capture_current operation forbids tick and chooses captured_tick atomically inside the shared headless owner dispatch, preserving player/control/session and material/structure/UI guards. Its raw committed_camera_lens is the complete native lens state at that Tick, independent of whether interpolation is enabled. Both methods render the exact frozen owned sample, preserve their original retry outcome and never advance simulation or the interpolation clock.");
    methods["settings.describe"]=object_schema({});
    methods["settings.inspect"]=object_schema({{"path",input_path}},{"path"});
    const auto settings_properties=player_settings::values_schema().at("properties");
    Json settings_ids=Json::array();for(auto it=settings_properties.begin();it!=settings_properties.end();++it)settings_ids.push_back(it.key());
    methods["settings.transact"]=object_schema({{"path",input_path},{"request_id",id},{"expected_revision",rev},
        {"set",player_settings::values_schema()},{"reset",{{"type","array"},{"uniqueItems",true},{"maxItems",settings_ids.size()},{"items",{{"enum",settings_ids}}}}},
        {"preview",{{"type","boolean"},{"default",false}}}},{"path","request_id","expected_revision"});
    methods["player.settings.inspect"]=object_schema({{"player_id",id}});
    methods["player.settings.transact"]=object_schema({{"player_id",id},{"request_id",id},{"expected_control_revision",rev},
        {"expected_settings_revision",rev},{"set",player_settings::values_schema()},
        {"reset",{{"type","array"},{"uniqueItems",true},{"maxItems",settings_ids.size()},{"items",{{"enum",settings_ids}}}}},
        {"preview",{{"type","boolean"},{"default",false}}}},
        {"player_id","request_id","expected_control_revision","expected_settings_revision"});
    result["live_player_settings"]={{"schema",player_settings::values_schema()},
        {"lifetime","Native presentation owner, separate from authored worlds, runtime checkpoints and stored profiles."},
        {"live",{"camera.vertical_fov","input.sensitivity_x","input.sensitivity_y","input.invert_x","input.invert_y","ui.scale","audio.master_gain"}},
        {"next_player",{"graphics.samples","graphics.frames_in_flight"}},
        {"revisions","Configuration revisions guard acceptance; native applied/presented revisions report the live subset separately. Control revisions also invalidate stale captures."},
        {"persistence","Session changes do not write profiles. Explicit settings.transact remains independently guarded; changing storage does not change an active player."},
        {"audio","Verified output-sink gain when enabled; no emitter/mixer/save or offline PCM changes, and no audible-output guarantee."},
        {"limitations","No C# settings service/menu yet. No live GPU sample/frame-slot transition. Physical-device input qualification remains separate."}};
    result["invariants"].push_back("Sparse stored player settings have independent revisions and next_player application. Native player.settings.transact changes the current interactive owner's live subset; graphics samples/frame slots retain explicit next-player intent. Shared-headless owner, player/control/configuration guards, validation and receipts protect accepted changes. Preview prepares without publication/receipt. Applying does not step simulation or persist a profile; actual device application/presentation can follow acceptance and is reported separately. Exact retries recover retained outcomes even after replacement or stop. Replay rejects intervening changes. C# menu access remains separate work.");
    result["invariants"].push_back("runtime.play blocks this session until exit; replay requires controller and sequence (at most 36000 total ticks); interactive accepts max_frames (0 means until exit) and may omit controller for menu-only scenes. Play results retain partial progress on window/device failure.");
    result["invariants"].push_back("Player polling observes background save completion without advancing simulation, including paused polls. runtime.play pending_gameplay_save is the queued/resolving operation at its final report boundary, or null; null is not proof of durable save success. Query runtime.save.result for the original ticket.");
    result["invariants"].push_back("At most 1024 enabled Light components and one LightingEnvironment. Any authored lighting, including a disabled light, suppresses the preview fallback.");
    result["invariants"].push_back("LightingEnvironment.sky is optional and disabled when absent; when present all sky fields are required. Non-null sun must reference an existing directional Light, including when sky is disabled. Remove or change that light only while clearing/changing the reference in the same transaction. Disabled sun lights hide the disk. Runtime retains frozen sky settings/reference and resolves the sun direction from its live pose.");
    result["invariants"].push_back("LightingEnvironment.ibl is optional. If present it requires enabled Boolean and finite numeric diffuse_intensity/specular_intensity gains in [0,64], including while disabled. Enabled IBL requires an enabled procedural sky. Omission preserves the old stored and resolved lighting shapes and does not insert IBL defaults; explicit disabled settings and zero gains retain their presence. Runtime freezes the authored presence and values, including through complete saves and restoration. IBL samples the artistic sky gradient without the visible Sun disk/halo, ambient fill or exposure; it adds diffuse irradiance and roughness-filtered specular environment response, not local scene reflections, geometry occlusion or dynamic GI.");
    result["invariants"].push_back("LightingEnvironment.output_grade optionally selects a per-channel curve over exposed HDR before tone mapping. input_max is in [0.125,64], strength in [0,1] and 33 uniform RGB knots in [0,16] start at [0,0,0]. Above input_max the curve extends from the last knot with unit slope. The renderer blends graded and original values by strength. Omission preserves prior lighting responses. A selected zero-strength grade has identity output and remains observable, frozen with the scene and included in export feature requirements.");
    result["invariants"].push_back("LightingEnvironment.screen_reflections selects optional screen reflections for single-sample deferred color. The three required settings are finite: strength in [0,1], max_distance_m in [0.25,32], and thickness_m in [0.01,0.5]. Forward, diagnostic views and world-depth foreground retain the fallback. When FSR is enabled, reconstruction consumes the composed world image after reflections and optional height fog. Omission preserves lighting and dependency shapes. A selected zero strength disables the effect while retaining the authored selection and its export feature requirement.");
    result["invariants"].push_back("LightingEnvironment.screen_contact_shadows targets an existing directional Light, including when disabled or strength is zero. Removing the Light or changing its kind requires clearing or repointing the reference in the same transaction. length_m is finite in [0.02,2], thickness_m is finite in [0.001,0.15] and at most length_m, and strength is finite in [0,1]. The selected profile freezes with the runtime scene and requires an export runtime with screen_contact_shadows support. Omission keeps the prior lighting and dependency shapes.");
    result["invariants"].push_back("LightingEnvironment.screen_diffuse_bounce selects a bounded local diffuse bounce in single-sample deferred color. radius_m is finite in [0.2,4], thickness_m is finite in [0.005,0.25] and at most radius_m, and strength is finite in [0,1]. Selection freezes with the runtime scene and requires an export runtime with screen_diffuse_bounce support even at zero strength. Omission keeps the prior lighting and dependency shapes.");
    result["invariants"].push_back("LightingEnvironment.screen_diffuse_bounce.resolution is optional and defaults to full. An explicit full or half selection remains observable. Half requires an export runtime with screen_diffuse_bounce_half_resolution support even at zero strength.");
    result["invariants"].push_back("LightingEnvironment.height_fog optionally selects scene-linear fog. It requires finite density_per_m in [0,0.75], base_height_m in [-8192,8192], height_falloff_per_m in [0,4], start_distance_m in [0,300], max_distance_m in [0.1,512] greater than start_distance_m, ambient_radiance as three RGB values in [0,32], and anisotropy in [-0.8,0.8]. Up to two optional distinct spot_lights name existing authored spot Light entities, including disabled lights, with scattering in [0,1]. References remain validated and observable at zero density.");
    result["invariants"].push_back("LightingEnvironment.post_effect selects one optional compiled scene-linear HDR effect by its whole-file SHA-256. Import the reviewed .poimapost bytes before selecting them. Dependency resolution and frozen scene preparation revalidate the file and reject missing or changed bytes. An active runtime owns its admitted effect until replaced. Omission leaves lighting observations and dependencies unchanged.");
    result["invariants"].push_back("Shadow maps are opt-in per light. Directional=4 views, point=6, spot=1; at most 16 views and 128 MiB of D32 depth storage. Shadowed spot outer_angle <= 89.5; local range must exceed shadow near.");
    result["invariants"].push_back("Capture/play clustered_lighting defaults true. False evaluates the full light table as a reference. Cluster overflow falls back to all lights; directional and range-zero lights are never distance-culled. Assignment diagnostics distinguish requested mode, active mode and measured GPU statistics.");
    result["invariants"].push_back("Capture/play shadow_filter explicitly overrides the authored LightingEnvironment.shadow_filter. Omission inherits it; legacy scenes without the authored field retain pcf3_point. Choices are pcf3_point, pcf3_fractional or pcss_directional. Existing point/fractional modes retain their arithmetic and view matrices. Fractional mode continuously weights four neighboring three-by-three comparison boxes. pcss_directional adds bounded deterministic spatial directional contact hardening: 16 raw-depth blocker reads and 16 bilinear-comparison filter locations (at most 64 filter depth comparisons), capped at 16 shadow texels with an 18-texel directional guard. Actual orthographic light depth span and guarded world units per texel scale separation times tan(half emitter diameter); every comparison retains geometric receiver-plane correction and bias at its fetched texel. Local point/spot lights, zero angular size and no blocker use pcf3_fractional; no-blocker search does not establish global blocker absence. The fractional minimum footprint is antialiasing, not uncapped physical area-light integration. No temporal randomness/history, cross-face point filtering, caching or ray tracing is added. Only mode2 diagnostics add contact_shadows counts/caps/fallbacks; counts are configured bounds, not measured per-pixel work.");
    result["invariants"].push_back("Light.shadow.angular_size_degrees is an optional finite number in [0,20] for directional lights only, even when the light or shadow is disabled. Omission uses 0.53 degrees only for pcss_directional and preserves omitted stored/resolved shapes; explicit zero selects fractional hard visibility. It is independent of LightingEnvironment.sky.sun_size_degrees. Runtime freezes the authored presence/value with its lighting; frozen content, export and save identities retain explicit fields without inserting omitted defaults.");
    result["invariants"].push_back("Capture/play ambient_occlusion defaults to {mode:none,quality:medium,radius:1}; gtao requires deferred and samples=1. Radius is finite in [0.01,100] world units. The ambient_occlusion debug view requires enabled AO. Probes expose raw_ambient_visibility and ambient_visibility; 1 means unoccluded. This contract does not imply renderer qualification.");
    result["invariants"].push_back("Capture/play viewmodel_samples is 1 (default), 2, 4 or 8 independently of world samples, lighting path and reconstruction. Configured foreground diagnostics report the selected count even when no foreground draw is rendered; settings profiles do not persist this option.");
    result["invariants"].push_back("Capture/play lighting_path is forward (default) or deferred. Deferred requires samples=1; this option does not change authored content or imply renderer qualification.");
    result["invariants"].push_back("Capture/play reconstruction is none, fsr3_native, fsr3_quality, fsr3_balanced or fsr3_performance. Enabled reconstruction requires samples=1 and scene_debug_view=color and must be available in this build. Capture-only capture_frames is 1..128 (default 2), repeatedly rendering one frozen snapshot; runtime.play does not accept it. Probe x/y are render-input pixels; resolved_x/y identify mapped reconstruction-output pixels. Raw and resolved HDR values are pre-tone-map radiance. Textured world sprites blend into the FSR color input at render resolution; current alpha coverage increases reactivity while depth and geometric motion remain opaque scene products. Sprite reactivity and the fog/reflection composition mask are separate.");
    result["invariants"].push_back("Capture/play post_aa is none (default) or fxaa. FXAA requires effective samples=1, scene_debug_view=color and reconstruction=none.");
    result["invariants"].push_back("A numeric geometry probe includes surface_mark_reactive:true on the accepted submission where a corresponding receiver's local surface marks changed. Geometric motion remains valid. FSR uses this signal alongside its invalid-correspondence fallback.");
    result["invariants"].push_back("Capture/play scene_debug_view is color, depth, shading_normal, motion, motion_validity or ambient_occlusion. Non-color views require samples=1 and bypass exposure/output tone mapping. Single-sample scene products contain device depth, final world-space shading normals and previous-minus-current UV motion with explicit validity. Up to 64 scene_product_probes return raw captured values; probes require samples=1 and a capture path. History advances on accepted per-view submission, independently of simulation tick; missing continuity, cuts and replaced objects invalidate correspondence.");
    result["invariants"].push_back("Capture/play frames_in_flight is 1 or 2, default 2. One selects serialized completion; two bounds outstanding graphics submissions. Frame execution diagnostics distinguish submission, completion and explicit waits; neither completion nor presentation-fence retirement proves monitor scanout.");
    result["invariants"].push_back("Capture/play draw_instancing defaults true. Adjacent visible objects share a command only with consecutive GPU object slots and identical geometry, previous-position stream, bindings and pipeline. camera_draws/shadow_draws retain object/view-instance counts; camera_batches/shadow_batches count geometry commands, excluding sky/composition/UI. The CPU path supports per-instance submission with draw_instancing=false. Opt-in gpu_visibility allows multiple indexed indirect commands in a compatible camera batch.");
    result["invariants"].push_back("Capture/play culling defaults true; camera and each shadow view cull independently. Profile defaults false. Render diagnostics report submitted draws and optional CPU/GPU intervals, not a qualified game frame time.");
    result["invariants"].push_back("Kinematic targets begin on the first tick of step/replay segments and persist across batches. Targets must be unique roots, normalized, at most 100 m/s and 20 rad/s. Raycasts query physics, including hidden colliders; ties use stable IDs. Primitive origin-inside hits have no surface normal; mesh hits retain triangle winding normals.");
    result["invariants"].push_back("StaticMesh.foliage_motion optionally adds local-height wind and touch deformation. The profile can coexist with rigid LOD and collision proxies, which continue to use the authored collision shape. CameraDraw, snow tiles and terrain tiles bound to water cannot use it. A touch actor is a distinct authored CharacterController. Spawn templates support wind profiles only.");
    result["invariants"].push_back("StaticMesh.lods optionally declares one to four indexed rigid primitives with increasing min_distance_m thresholds and decreasing triangle counts. Camera distance from the object's world origin or transformed lod_anchor selects the same tier for camera and shadows. LOD0 retains material ownership and physics/navigation use their authored colliders. Completed draw diagnostics report selected tiers for LOD-enabled objects.");
    result["invariants"].push_back("MeshCollider is an explicit static indexed model primitive, independent of rendering visibility. It cannot combine with BoxCollider/CharacterController or inherit moving/animation-owned transforms. Physics contacts use triangle front faces; rays query both sides and return the source triangle ordinal (null for non-mesh hits), retaining winding normals. No convexification, texture-cutout collision or deforming meshes. Limits: 100000 triangles/300000 vertices per body and 250000 triangles/750000 vertices per world. Geometry/material/transform validation runs at runtime preparation and export; malformed or degenerate geometry rejects.");
    result["invariants"].push_back("Managed gameplay is optional trusted project code. One module per runtime; typed native state is inspected/edited separately from authoring. Load/edit use tick/revision guards and shared retry receipts. Use input is a first-tick edge. Gameplay updates and queued motion join physics batch rollback.");
    result["input_actions"]={{"maximum_controllers",32},{"maximum_actions_per_controller",32},{"name_pattern","^[a-z][a-z0-9_.-]{0,63}$"},
        {"ordering","Names in runtime input records must be sorted and unique."},
        {"state","Held repeats throughout a tick batch or replay segment. Pressed/released apply only on its first tick; a quick tap may set both with held=false. Omitted records are absent and neutral."},
        {"profiles","An actions object selects poima.input.v3. Adding keyboard chords for named actions selects poima.input.v4. Keyboard, mouse and optional gamepad bindings share the same control catalog and recovery policy."},
        {"gameplay",{{"feature","input_actions_v1"},{"services_epoch",7},{"required_prefix_bytes",264},{"result_bytes",16},{"maximum_reads_per_tick",1024},{"phase","Tick only"}}},
        {"persistence","Input is transient. Game state derived from input participates in rollback and durable saves."}};
    methods["world.lighting"]=object_schema({{"revision",rev}});
    methods["runtime.lighting"]=object_schema({{"session_id",id},{"tick",rev}}, {"session_id"});
    methods["entity.material"]=object_schema({{"id",id},{"revision",rev}}, {"id"});
    for(const auto* method:{"world.audio.inspect","world.audio.capture","runtime.audio.inspect","runtime.audio.capture"}) {
        const bool live=std::string_view(method).starts_with("runtime"),capture_audio=std::string_view(method).ends_with("capture");
        Json props={{"listener",id},{"early_reflections",{{"type","boolean"},{"default",false}}}};Json required={"listener"};
        if(live) { props["session_id"]=id;props["tick"]=rev;required.push_back("session_id");required.push_back("tick"); }
        else { props["revision"]=rev;required.push_back("revision"); }
        if(capture_audio) { props["path"]={{"type","string"},{"minLength",1}};props["frames"]={{"type","integer"},{"minimum",1},{"maximum",480000},{"default",48000}};required.push_back("path"); }
        methods[method]=object_schema(props,required);
    }
    methods["runtime.audio.voices"]=object_schema({{"session_id",id},{"tick",rev},{"after",rev},{"limit",{{"type","integer"},{"minimum",1},{"maximum",256},{"default",64}}}}, {"session_id","tick"});
    result["audio_portals"]={{"maximum_portals",max_audio_portals},{"feature","audio_portals"},
        {"maximum_chain_length",max_audio_portal_chain},
        {"selection","An occluded source can reach the listener through one or more visible openings. Each crossing uses committed acoustic geometry, including moving door panels."}};
    result["audio_rooms"]={{"maximum_rooms",max_audio_rooms},{"transition_frames",audio_rate/20},
        {"maximum_portal_sends",max_audio_room_sends},
        {"feature","audio_room_reverb"},{"selection","Contain the committed listener in a transformed local box; prefer higher priority, then lower entity ID."}};
    result["audio_buses"]={{"operation","audio.buses.set"},{"catalog",audio_bus_catalog},
        {"routing","Optional AudioEmitter.bus refers to one authored name. Unrouted sources keep unity gain."},
        {"runtime","The mix and routes freeze at session start and persist in the saved authored definition."},
        {"observation","Routed sources and voices report bus, bus_gain and effective_gain before acoustic path attenuation and output-sink gain."}};
    const auto audio_segment=object_schema({{"ticks",{{"type","integer"},{"minimum",1},{"maximum",600}}},
        {"inputs",{{"type","array"},{"maxItems",32},{"items",input}}},{"motions",motions},{"sounds",sounds}}, {"ticks"});
    methods["runtime.audio.replay"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"listener",id},
        {"path",{{"type","string"},{"minLength",1}}},{"early_reflections",{{"type","boolean"},{"default",false}}},
        {"sequence",{{"type","array"},{"minItems",1},{"maxItems",256},{"items",audio_segment}}}},
        {"session_id","request_id","expected_tick","listener","path","sequence"});
    result["invariants"].push_back("Sound play/stop commands join runtime.step batch rollback and apply on the first tick; C# sound calls share that transaction. Voices use session-local monotonic handles, at most 64 emitting and 256 retained records. runtime.audio.replay advances and records committed ticks (max 3600 total), retaining partial progress and retry receipts on failure. DSP uses persistent filters and does not alter logical voice state.");
    result["invariants"].push_back("Audio observation is synchronous and frozen: fresh geometry and poses on every query, no simulation advance, source cursor or device playback. AcousticMaterial requires box/mesh geometry. At most 64 enabled emitters, 131072 acoustic triangles, 64 MiB clip packages; mono 48 kHz PCM16/float32 WAV import, up to 60 seconds per clip. Captures are 1..480000 stereo float frames with direct paths and HRTF. Enable early_reflections for a bounded first-order path off a finite acoustic box face.");
    result["invariants"].push_back("Native animation tasks and material baking share one lazy executor per creating owner thread. POIMA_JOB_WORKERS selects 0..8 workers (default 2) before first use; zero is the serial reference. Frame/background admission is separate, and reserved frame workers do not execute background work. Registry, physics, C# callbacks and publication remain owner-only. jobs.status inspects this shared owner pool without creating it; its counts may include other sessions on the same owner.");
    result["invariants"].push_back("Procedural brick/plaster recipes bake compiled CPU work without world changes or worker file I/O. One active material bake and eight retained records per service; explicit polling publishes immutable image packages and a recipe descriptor. Cancellation before publication vetoes even completed CPU results. Jobs are authoring-only; runtime closure contains referenced baked images. These evolving methods are outside authoring-core v1.");
    methods.update(terrain::authoring_schemas());
    methods.update(foliage::authoring::authoring_schemas());
    methods.update(water::region_authoring_schemas());
    if(Runtime::available()) {
        methods["runtime.water.inspect"]=water::region_runtime_schema();
        methods["runtime.foliage.inspect"]=object_schema({{"session_id",id},{"tick",rev},{"entity",id},
            {"structure_revision",rev}},{"session_id","tick","entity"});
        methods["runtime.foliage.inspect"]["description"]="Read the named drawable at a committed tick. Reports session_id, tick, structure_revision, entity, tip_local_xz, tip_world_offset_m and touch_weight. Include structure_revision when runtime membership has changed. Outside runtime-game-session v1.";
    }
    if(Runtime::available())methods["runtime.particles.inspect"]=particle_observation_schema();
    result["particle_collisions"]={{"available",Runtime::available()},{"component","ParticleEmitter.surface_collision"},
        {"max_live_per_source",particles::max_contact_particles_per_emitter},
        {"max_sweeps_per_tick",runtime_particle_limits.emitters*particles::max_contact_particles_per_emitter},
        {"runtime_feature","particle_collisions"},{"observation","runtime.particles.inspect"}};
    result["particle_impact_bursts"]={{"available",Runtime::available()},{"component","ParticleEmitter.impact_burst"},
        {"max_per_contact",max_particle_impact_burst},{"runtime_feature","particle_impact_bursts"},
        {"observation","runtime.particles.inspect"}};
    result["particle_water_contact"]={{"available",Runtime::available()},{"component","ParticleEmitter.water_contact"},
        {"surface","water"},{"runtime_feature","particle_water_contact"},{"observation","runtime.water.inspect"}};
    result["particle_water_flow"]={{"available",Runtime::available()},{"component","ParticleEmitter.water_flow"},
        {"max_particles_per_tick",max_water_flow_particles_per_tick},{"runtime_feature","particle_water_flow"},
        {"observation","runtime.particles.inspect"}};
    result["audio_water_flow"]={{"available",Runtime::available() && audio_available()},
        {"component","AudioEmitter.water_flow"},{"runtime_feature","audio_water_flow"},
        {"observation","runtime.audio.inspect"},{"envelope_frames",audio_tick_frames}};
    result["water_buoyancy"]={{"available",Runtime::available()},{"component","BoxCollider.buoyancy"},
        {"max_bodies",water::max_buoyant_bodies},{"runtime_feature","water_buoyancy"},{"observation","runtime.water.inspect"}};
    result["water_currents"]={{"available",Runtime::available()},{"profile","world.water.plan.currents"},
        {"runtime_feature","water_currents"},{"observation","runtime.water.inspect"}};
    result["water_surface_wakes"]={{"available",Runtime::available()},{"profile","world.water.plan.surface_wakes"},
        {"runtime_feature","water_surface_wakes"},{"observation","runtime.water.inspect"}};
    result["water_body_displacement"]={{"available",Runtime::available()},{"profile","world.water.plan.body_displacement"},
        {"runtime_feature","water_body_displacement"},{"observation","runtime.water.inspect"}};
    result["water_storage"]={{"available",Runtime::available()},{"profile","world.water.plan.storage"},
        {"max_reservoirs",water::max_water_reservoirs},{"max_ports",water::max_water_ports},
        {"runtime_feature","water_storage"},{"observation","runtime.water.inspect"}};
    result["water_reservoirs"]={{"available",Runtime::available()},{"profile","world.water.plan.storage.reservoirs[].footprint"},
        {"runtime_feature","water_reservoirs"},{"observation","runtime.water.inspect"}};
    result["water_overflow"]={{"available",Runtime::available()},{"profile","world.water.plan.storage.ports[].overflow"},
        {"runtime_feature","water_overflow"},{"observation","runtime.water.inspect"}};
    result["water_sheets"]={{"available",Runtime::available()},{"profile","world.water.plan.storage.ports[].sheet"},
        {"max_sheets",water::max_water_sheets},{"runtime_feature","water_sheets"},{"observation","runtime.water.inspect"}};
    result["water_sheet_aeration"]={{"available",Runtime::available()},
        {"profile","world.water.plan.storage.ports[].sheet.aeration"},{"runtime_feature","water_sheet_aeration"},
        {"observation","runtime.water.inspect"}};
    result["water_impact_foam"]={{"available",Runtime::available()},
        {"profile","world.water.plan.storage.ports[].sheet.impact_foam"},{"runtime_feature","water_impact_foam"},
        {"observation","runtime.water.inspect"}};
    result["collision_surfaces"]={{"available",Runtime::available()},{"ray_method","runtime.surface.raycast"},
        {"max_name_bytes",max_collision_surface_length},{"max_impact_routes",max_particle_impact_surfaces},
        {"runtime_feature","collision_surfaces"}};
    detail::add_asset_method_schemas(result, id, rev, asset_id, model_node, number);
    const Json save_slot_schema={{"type","string"},{"pattern","^[a-z0-9_-]{1,64}$"}};
    const Json save_path_schema={{"type","string"},{"minLength",1},{"maxLength",4096}};
    const Json nullable_revision={{"anyOf",{rev,Json{{"type","null"}}}}};
    methods["save.status"]=object_schema(Json::object());
    methods["save.configure"]=object_schema({{"request_id",id},{"expected_generation",rev},{"root",{{"anyOf",{save_path_schema,Json{{"type","null"}}}}}},{"write_mode",{{"type","string"},{"enum",{"synchronous","background"}},{"default","synchronous"}}}},{"request_id","expected_generation","root"});
    methods["save.inspect"]=object_schema({{"slot",save_slot_schema}},{"slot"});
    methods["save.write"]=object_schema({{"request_id",id},{"configuration_generation",rev},{"slot",save_slot_schema},{"expected_generation",rev},
        {"session_id",id},{"expected_tick",rev},{"expected_gameplay_revision",rev},{"acknowledge_recovery",{{"type","boolean"},{"default",false}}}},
        {"request_id","configuration_generation","slot","expected_generation","session_id","expected_tick","expected_gameplay_revision"});
    const auto saved_managed=object_schema({{"hostfxr",save_path_schema},{"bridge",save_path_schema},{"assembly",save_path_schema},
        {"type",{{"type","string"},{"minLength",1},{"maxLength",512}}}},{"hostfxr","bridge","assembly","type"});
    const auto saved_native=object_schema({{"descriptor",save_path_schema},{"expected_descriptor_sha256",{{"type","string"},{"pattern","^[0-9a-f]{64}$"}}}},{"descriptor"});
    methods["save.load"]=object_schema({{"request_id",id},{"configuration_generation",rev},{"slot",save_slot_schema},{"expected_generation",rev},
        {"revision",rev},{"expected_session_id",parent},{"expected_tick",nullable_revision},{"expected_gameplay_revision",nullable_revision},{"new_session_id",id},
        {"gameplay",{{"anyOf",{saved_managed,saved_native,Json{{"type","null"}}}}}},
        {"upgrade",object_schema({{"path",save_path_schema},{"expected_sha256",{{"type","string"},{"pattern","^[0-9a-f]{64}$"}}}},{"path","expected_sha256"})},
        {"allow_recovery",{{"type","boolean"},{"default",false}}}},
        {"request_id","configuration_generation","slot","expected_generation","revision","expected_session_id","expected_tick","expected_gameplay_revision","new_session_id"});
    result["saves"]={{"storage","Explicit existing root, session-local configuration. Each lowercase slot uses a separate locked subdirectory. Saves never modify the authored document; packaged hosts protect the entire bundle root."},
        {"content","Frozen authored definition plus logical snapshot; all referenced assets are content-verified at start and again with fresh caches on load. No executable paths are selected from save bytes."},
        {"guards","Configuration generation, slot generation, runtime session/tick/gameplay revision. Restore always uses a fresh session ID and preserves authoring. Pause desktop playback before configure/write/load."},
        {"retry","Write receipts persist per slot; configure/load retain the latest32 session-local receipts. Exact retries do not repeat mutations. Forgotten IDs beyond retention are new requests subject to guards."},
        {"recovery","Inspect reports verified previous-generation fallback. Load requires allow_recovery:true; writes after payload fallback require acknowledge_recovery:true. Manifest recovery permits reads only."},
        {"upgrades",{{"plan_versions",{1,2,3}},{"snapshot_versions",{1,2,3,4,5,6,7}},
            {"globals","Explicit stable scalar IDs or complete legacy-name mapping."},
            {"components","Stable type/field IDs; scalar and bounded array values. Retained kinds, element kinds and units stay exact."},
            {"array_capacity","Plan v2 explicitly authorizes changed capacities with overflow=reject. Never truncate; all authored, recipe and saved instances must fit."},
            {"add_components",{{"plan_version",3},{"maximum_types",64},{"maximum_instances",10000},
                {"policy","Exact target-only schemas and typed defaults on explicitly listed original authored entities present in the saved live graph; no template/spawned instance additions or resurrection."}}},
            {"hierarchy","Preserve local/live maps, native membership, allocation history and topology; map every recipe member and existing component instance."}}},
        {"limitations","Synchronous bounded64MiB save; exact restore by default. Explicit upgrades require a host-selected plan and target gameplay, unchanged world membership and approved component edits. No automatic/general migrations, autosave scheduler, platform/cloud adapters or power-loss qualification."}};
    result["gameplay_saves"]={
        {"services_abi",7},{"kinds",{{"none",0},{"save",1},{"load",2}}},
        {"states",{{"expired",0},{"queued",1},{"resolving",2},{"succeeded",3},{"failed",4}}},
        {"request_rejections",{{"none",0},{"disabled",1},{"busy",2},{"invalid",3},{"exhausted",4}}},
        {"boundary","One runtime request, serviced only after the complete atomic batch commits. Requested tick and committed tick may differ. Failed batches perform no save I/O."},
        {"observation","runtime.save.status/result are memory-only. 64 owner terminal results; unknown and evicted tickets expire. Resolving retains uncertain write identity and blocks another batch until receipt verification succeeds."},
        {"step_result","session_id/tick/stepped and sound_events describe the source batch; current_session_id/current_tick/runtime_replaced describe the active world. Exact advance retries survive replacement, stop and restart within the32 receipt owner history."},
        {"load","Fresh epoch/session; saved pending tickets are data, not commands. LastRestore precedes the replacement first Tick. Desktop and interactive player pause/clear old input; recorded player/audio replay stop at replacement."}};
    const Json custom_key={{"type","string"},{"pattern","^game:[0-9a-f]{32}$"}};
    const Json stable_type={{"type","string"},{"pattern","^(?!0{32}$)[0-9a-f]{32}$"}};
    const Json scalar={{"anyOf",Json::array({Json{{"type","integer"}},Json{{"type","number"}},Json{{"type","string"}}})}};
    const Json value={{"anyOf",Json::array({scalar,Json{{"type","array"},{"maxItems",31},{"items",scalar}}})}};
    const Json custom_values={{"type","object"},{"minProperties",1},{"maxProperties",32},{"propertyNames",id},{"additionalProperties",value}};
    const auto field=object_schema({{"id",stable_type},{"name",{{"type","string"},{"minLength",1},{"maxLength",64}}},{"kind",{{"enum",{"int32","int64","float32","float64","entity"}}}},
        {"default",scalar},{"unit",{{"type","string"},{"maxLength",24}}}},{"id","name","kind","default"});
    const auto scalar_schema=object_schema({{"id",stable_type},{"name",{{"type","string"},{"minLength",1},{"maxLength",64}}},{"version",{{"const",1}}},
        {"fingerprint",{{"type","string"},{"pattern","^[0-9a-f]{64}$"}}},{"fields",{{"type","array"},{"minItems",1},{"maxItems",32},{"items",field}}}},{"id","name","version","fields"});
    const auto array_field=object_schema({{"id",stable_type},{"name",{{"type","string"},{"minLength",1},{"maxLength",64}}},
        {"kind",{{"const","array"}}},{"element_kind",{{"enum",{"int32","int64","float32","float64","entity"}}}},
        {"capacity",{{"type","integer"},{"minimum",1},{"maximum",31}}},{"default",{{"type","array"},{"maxItems",0}}},
        {"unit",{{"type","string"},{"maxLength",24}}}},{"id","name","kind","element_kind","capacity","default"});
    auto collection_schema=scalar_schema;collection_schema["properties"]["version"]={{"const",2}};
    collection_schema["properties"]["fields"]["items"]={{"oneOf",Json::array({field,array_field})}};
    collection_schema["properties"]["fields"]["contains"]=array_field;
    const Json schema={{"oneOf",Json::array({scalar_schema,collection_schema})}};
    const auto manifest=object_schema({{"format",{{"const","poima.components"}}},{"version",{{"const",1}}},{"schemas",{{"type","array"},{"maxItems",64},{"items",schema}}}},{"format","version","schemas"});
    methods["component.schemas"]=object_schema({{"id",stable_type}});
    methods["component.schema.import"]=object_schema({{"request_id",id},{"base_revision",rev},{"manifest",manifest}},{"request_id","base_revision","manifest"});
    auto& mutations=methods["world.transact"]["properties"]["ops"]["items"]["oneOf"];
    mutations.push_back(object_schema({{"op",{{"const","component.schema.set"}}},{"schema",schema}},{"op","schema"}));
    mutations.push_back(object_schema({{"op",{{"const","component.schema.remove"}}},{"id",stable_type}},{"op","id"}));
    mutations.push_back(object_schema({{"op",{{"const","component.set"}}},{"id",id},{"type",custom_key},{"value",custom_values}},{"op","id","type","value"}));
    mutations.push_back(object_schema({{"op",{{"const","component.remove"}}},{"id",id},{"type",custom_key}},{"op","id","type"}));
    for(const auto* method:{"entity.get","entity.query"})methods[method]["properties"]["component"]={{"anyOf",Json::array({component_type,custom_key})}};
    methods["runtime.components"]=object_schema({{"session_id",id}},{"session_id"});
    methods["runtime.component.get"]=object_schema({{"session_id",id},{"tick",rev},{"id",id},{"type",stable_type}},{"session_id","tick","id","type"});
    methods["runtime.component.query"]=object_schema({{"session_id",id},{"tick",rev},{"type",stable_type},{"after",id},{"limit",{{"type","integer"},{"minimum",1},{"maximum",256},{"default",64}}}},{"session_id","tick","type"});
    methods["runtime.component.edit"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"expected_revision",rev},{"id",id},{"type",stable_type},{"values",custom_values}},
        {"session_id","request_id","expected_tick","expected_revision","id","type","values"});
    const Json material_ids={{"type","array"},{"minItems",1},{"maxItems",32},{"uniqueItems",true},{"items",id},{"description","IDs must be in ascending order."}};
    methods["runtime.material.inspect"]=object_schema({{"session_id",id},{"tick",rev},{"material_revision",rev},{"ids",material_ids}},
        {"session_id","tick","ids"});
    methods["runtime.material.apply"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"expected_structure_revision",rev},
        {"expected_material_revision",rev},{"revision",rev},{"expected_authored_revision",rev},{"expected_content_sha256",asset_id},{"ids",material_ids}},
        {"session_id","request_id","expected_tick","expected_structure_revision","expected_material_revision","revision","expected_authored_revision","expected_content_sha256","ids"});
    methods["runtime.material.apply"]["description"]="Explicitly adopt authored base_color/emissive/metallic/roughness/alpha_mode/opacity/alpha_cutoff/diffuse_transmission changes on existing explicit PbrMaterial drawables without restarting gameplay. Full content and asset closure guards reject unrelated edits. Accepted state is distinct from renderer submission/presentation. Outside authoring-core v1.";
    methods["runtime.texture.inspect"]=methods.at("runtime.material.inspect");
    methods["runtime.texture.inspect"]["description"]="Read the frozen authored PbrTextures component and effective immutable five-slot map identities, samplers and strengths without resolving storage or advancing gameplay. image_sha256 identifies canonical cooked image bytes, including images inherited from model containers.";
    methods["runtime.texture.apply"]=methods.at("runtime.material.apply");
    methods["runtime.texture.apply"]["description"]="Explicitly adopt selected authored PbrTextures insertion, removal or edits on existing authored PBR drawables, including imported inherited PBR and explicit-PBR primitives. Omitted maps/strengths inherit the original mesh; null maps clear. Five slots, samplers, normal_scale and occlusion_strength are supported. Complete guarded content must differ only in these components and provenance bindings precisely tied to newly introduced or fully retired selected texture dependencies. Original mesh/rig owners and all gameplay state stay owned by the same runtime. Shared material_revision advances once; acceptance is distinct from submission/presentation. Outside authoring-core v1.";
    result["live_textures"]={{"maximum_entities",32},{"slots",{"base_color","metallic_roughness","emissive","occlusion","normal"}},
        {"fields",{"occlusion_strength","normal_scale"}},{"epoch","Shares material_revision, capture/save guards and exact renderer snapshot acknowledgements with scalar material adoption."},
        {"inheritance","Absent PbrTextures restores the original mesh maps and strengths; omitted members inherit original mesh values; null map members clear that slot."},
        {"dependencies","The original frozen asset/provenance closure must still validate unchanged. Only dependencies directly selected by changed PbrTextures may be introduced or fully retired. New provenance bindings require newly introduced selected dependencies; existing bindings cannot be rewritten."},
        {"limitations","No geometry, rig, PbrMaterial scalar, template, double_sided or general scene changes. Every selected component must change. Synchronous asset validation and texture upload are not a latency guarantee."}};
    methods["runtime.capture"]["properties"]["material_revision"]=rev;
    methods["runtime.capture"]["properties"]["structure_revision"]=rev;
    methods["player.capture"]["properties"]["expected_material_revision"]=rev;
    methods["player.capture_current"]["properties"]["expected_material_revision"]=rev;
    methods["save.write"]["properties"]["expected_material_revision"]=rev;
    methods["save.load"]["properties"]["expected_material_revision"]={{"anyOf",Json::array({rev,Json{{"type","null"}}})}};
    result["material_alpha"]={{"modes",{"opaque","mask"}},{"default_mode","opaque"},{"default_opacity",1},{"default_cutoff",.5},
        {"cutoff_maximum",1e6},{"discard","alpha_mode == mask AND sampled base-color alpha * opacity < alpha_cutoff"},
        {"coverage","Hard alpha test before color, depth and surface-product writes, including foreground and shadow depth. Surviving fragments remain opaque."},
        {"limitations","No blended transparency, alpha-to-coverage, alpha-aware collision/rays or coverage-preserving mipmaps. Ordinary alpha mip filtering can lose distant coverage."}};
    result["live_materials"]={{"maximum_entities",32},{"fields",{"base_color","emissive","metallic","roughness","alpha_mode","opacity","alpha_cutoff","diffuse_transmission"}},
        {"baseline","Explicit apply adopts the exact guarded authored material/save baseline; immutable native assets and gameplay state remain owned by the same runtime."},
        {"guards","After material_revision becomes nonzero, save.write/load and exact runtime/player captures require its guard. Retained retries resolve first. Session replacement resets this observation epoch; values persist in the frozen saved document."},
        {"limitations","Scalar apply does not adopt textures, geometry, double_sided or general live scene changes; texture adoption is a separate explicit operation. Every selected ID must have an actual scalar change. Preparation validates complete assets synchronously; no hitch-free latency guarantee."}};
    methods["save.write"]["properties"]["expected_component_revision"]=rev;
    methods["save.load"]["properties"]["expected_component_revision"]={{"anyOf",Json::array({rev,Json{{"type","null"}}})}};
    result["custom_components"]={{"manifest",manifest},{"type_prefix","game:"},{"services_abi",7},{"max_types",64},{"max_fields",32},{"max_instances",32768},{"max_payload_bytes",16777216},
        {"wire","16-byte canonical little-endian cells in ascending stable field ID order"},{"int64_json","Canonical signed decimal strings; exact full Int64 range"},
        {"schema_changes","Labels/units may change; shape/default changes require a future explicit migration. Removed type IDs cannot be reused except known undo/redo history."},
        {"runtime","Native-owned membership; queries sorted by entity ID; writes publish after Tick and before physics. Whole batch rollback includes payloads and component revision."},
        {"save_guard","save.write/load require expected_component_revision when replacing or saving an active runtime with declared custom schemas. Stopped restore accepts absent or null."}};
    Json recipe_properties=Json::object();for(const auto* type:{"Transform","BoxCollider","MeshRenderer","StaticMesh","PbrMaterial","PbrTextures","ParticleEmitter"})recipe_properties[type]=components.at(type);
    recipe_properties["ParticleEmitter"]["properties"].erase("rain");
    recipe_properties["StaticMesh"]["properties"]["foliage_motion"]=template_foliage_motion;
    auto recipe_components=object_schema(recipe_properties,{"Transform"});recipe_components["patternProperties"]={{"^game:[0-9a-f]{32}$",custom_values}};
    mutations.push_back(object_schema({{"op",{{"const","template.set"}}},{"id",stable_type},{"name",name},{"components",recipe_components}},{"op","id","name","components"}));
    auto member_components=object_schema(components,{"Transform"});
    member_components["properties"].erase("FootprintSurface");
    member_components["properties"].erase("SurfaceWetness");
    member_components["properties"]["ParticleEmitter"]["properties"].erase("rain");
    member_components["properties"]["StaticMesh"]["properties"]["foliage_motion"]=template_foliage_motion;
    member_components["patternProperties"]={{"^game:[0-9a-f]{32}$",custom_values}};
    const auto template_member=object_schema({{"name",name},{"parent",{{"anyOf",Json::array({stable_type,Json{{"type","null"}}})}}},
        {"components",member_components}},{"name","parent","components"});
    const Json template_entities={{"type","object"},{"minProperties",1},{"maxProperties",max_runtime_template_entities},
        {"propertyNames",stable_type},{"additionalProperties",template_member}};
    mutations.push_back(object_schema({{"op",{{"const","template.set"}}},{"id",stable_type},{"name",name},
        {"root",stable_type},{"entities",template_entities}},{"op","id","name","root","entities"}));
    mutations.push_back(object_schema({{"op",{{"const","template.remove"}}},{"id",stable_type}},{"op","id"}));
    const Json template_page_limit={{"type","integer"},{"minimum",1},{"maximum",256},{"default",64}};
    const auto ui_kind_schema=[&](const char* kind,const Json& text,const Json& action) {
        auto element_schema=object_schema({{"parent",{{"anyOf",Json::array({stable_type,Json{{"type","null"}}})}}},
            {"name",{{"type","string"},{"minLength",1},{"maxLength",128}}},{"kind",{{"const",kind}}},
            {"text",text},{"action",action},{"visible",{{"type","boolean"}}},{"enabled",{{"type","boolean"}}}},
            {"parent","name","kind","text","action","visible","enabled"});
        element_schema["properties"]["layout"]=ui_authoring_schema::layout(std::string_view(kind)=="panel");
        element_schema["properties"]["style"]=ui_authoring_schema::style();
        element_schema.update(ui_authoring_schema::rounded_clip_exclusion());
        return element_schema;
    };
    const Json ui_text={{"type","string"},{"maxLength",16384}},ui_action={{"type","string"},{"pattern","^[A-Za-z0-9_.-]{1,128}$"}};
    const Json ui_element={{"description","Choose exactly one kind: panels require empty text and null action; labels require null action; buttons require a nonempty action token."},
        {"oneOf",Json::array({ui_kind_schema("panel",Json{{"type","string"},{"const",""}},Json{{"type","null"}}),
            ui_kind_schema("label",ui_text,Json{{"type","null"}}),ui_kind_schema("button",ui_text,ui_action)})}};
    mutations.push_back(object_schema({{"op",{{"const","ui.element.set"}}},{"id",stable_type},{"element",ui_element}},{"op","id","element"}));
    mutations.push_back(object_schema({{"op",{{"const","ui.element.remove"}}},{"id",stable_type}},{"op","id"}));
    methods["world.ui.get"]=object_schema({{"id",stable_type},{"revision",rev}},{"id"});
    methods["world.ui.list"]=object_schema({{"revision",rev},{"after",stable_type},{"limit",template_page_limit}});
    methods["runtime.capture"]["properties"]["ui_scale"]={{"type","number"},{"minimum",.25},{"maximum",8},
        {"description","Optional density for a frozen native UI capture; omission retains window display density. Match world.ui.layout scale for geometry comparisons."}};
    methods["world.ui.layout"]=object_schema({{"revision",rev},{"width",{{"type","integer"},{"minimum",1},{"maximum",8192}}},
        {"height",{{"type","integer"},{"minimum",1},{"maximum",8192}}},{"scale",{{"type","number"},{"minimum",.25},{"maximum",8},{"default",1}}}},
        {"revision","width","height"});
    methods["runtime.ui.inspect"]=object_schema({{"session_id",id},{"tick",rev},{"ui_revision",rev},{"after",stable_type},{"limit",template_page_limit}},
        {"session_id","tick"});
    const auto ui_edit=object_schema({{"id",stable_type},{"text",{{"type","string"},{"maxLength",16384}}},
        {"visible",{{"type","boolean"}}},{"enabled",{{"type","boolean"}}}},{"id"});
    methods["runtime.ui.edit"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"expected_ui_revision",rev},
        {"edits",{{"type","array"},{"maxItems",256},{"items",ui_edit}}},{"modal",{{"anyOf",Json::array({stable_type,Json{{"type","null"}}})}}}},
        {"session_id","request_id","expected_tick","expected_ui_revision","edits"});
    methods["save.write"]["properties"]["expected_ui_revision"]=rev;
    methods["save.load"]["properties"]["expected_ui_revision"]={{"anyOf",Json::array({rev,Json{{"type","null"}}})}};
    methods["runtime.ui.activate"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"expected_ui_revision",rev},
        {"expected_control_sequence",rev},{"expected_gameplay_revision",rev},{"expected_structure_revision",rev},{"id",stable_type}},
        {"session_id","request_id","expected_tick","expected_ui_revision","expected_control_sequence","expected_gameplay_revision","id"});
    methods["save.write"]["properties"]["expected_control_sequence"]=rev;
    methods["save.load"]["properties"]["expected_control_sequence"]={{"anyOf",Json::array({rev,Json{{"type","null"}}})}};
    result["ui"]={{"presentation_available",static_cast<bool>(POIMA_GAME_UI)},{"authored_version",4},{"max_elements",256},{"max_depth",32},{"element",ui_element},
        {"max_text_bytes",16384},{"max_total_text_bytes",1048576},
        {"images",{{"max_unique",ui::max_images},{"max_base_level_bytes",ui::max_image_bytes},
            {"scope","Immutable standalone sRGB image packages loaded with runtime content. The image paints behind the element contents without changing layout or hit-testing."}}},
        {"state","Same-tick native text/visibility/enabled/modal edits with independent ui_revision and retry receipts. Runtime inspection pagination requires ui_revision after the first page."},
        {"save_guard","save.write/load require expected_ui_revision and expected_control_sequence for an active UI-bearing world; stopped restore accepts absent or null. Snapshot v5 preserves logical UI state and control sequence; v4 restores sequence zero."},
        {"limits","Button actions invoke compiled Control callbacks without advancing simulation. C# UI writes are staged atomically; Optional typed layout/style metadata is frozen at runtime start; absent metadata retains the default layout. world.ui.layout observes an ephemeral virtual viewport, not live focus/scroll or physical input."},
        {"authority","Stable native panel/label/button definitions; frozen at runtime start. Parent must be a panel; panel text is empty; only buttons have non-null action tokens. RmlUi is presentation, not authored authority."}};
    methods["template.get"]=object_schema({{"id",stable_type},{"revision",rev}},{"id"});
    methods["template.query"]=object_schema({{"revision",rev},{"after",stable_type},{"limit",template_page_limit}});
    methods["runtime.template.get"]=object_schema({{"session_id",id},{"tick",rev},{"revision",rev},{"id",stable_type}},{"session_id","tick","id"});
    methods["runtime.template.query"]=object_schema({{"session_id",id},{"tick",rev},{"revision",rev},{"after",stable_type},{"limit",template_page_limit}},{"session_id","tick"});
    methods["runtime.instance"]=object_schema({{"session_id",id},{"id",stable_type},{"tick",rev},
        {"expected_structure_revision",rev}},{"session_id","id","tick"});
    methods["runtime.structure.transact"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"expected_structure_revision",rev},
        {"spawns",{{"type","array"},{"maxItems",4096},{"items",object_schema({{"template_id",stable_type},{"transform",components.at("Transform")}},{"template_id"})}}},
        {"despawns",{{"type","array"},{"maxItems",4096},{"items",stable_type}}}},
        {"session_id","request_id","expected_tick","expected_structure_revision"});
    methods["save.write"]["properties"]["expected_structure_revision"]=rev;
    methods["save.load"]["properties"]["expected_structure_revision"]={{"anyOf",Json::array({rev,Json{{"type","null"}}})}};
    for(const auto* method:{"runtime.entity","runtime.component.get","runtime.component.query"})
        methods[method]["properties"]["structure_revision"]=rev;
    result["spawn_templates"]={{"authored_version",3},{"max_templates",max_runtime_spawn_templates},{"max_custom_payload_bytes",max_runtime_template_payload_bytes},{"max_entities_per_template",max_runtime_template_entities},{"max_total_template_entities",max_runtime_template_total_entities},
        {"components",recipe_components},{"hierarchy",template_entities},{"references","Legacy root-prop references remain literal world IDs. Hierarchical native references are local; custom handles matching local IDs remap within the instance, other handles remain external and are validated against final live membership. Template/local IDs are separate namespaces, not live EntityIds."},
        {"save_guard","save.write/load require expected_structure_revision after any structural transaction; stopped restore accepts absent or null."},
        {"gameplay_services_abi",7},{"gameplay_services_bytes",176},{"hierarchical_services_bytes",232},{"gameplay_reads","Committed membership; reserved births support custom initializers and local-member resolution. Template defaults are read explicitly."},{"runtime","Frozen root-prop or connected hierarchical recipes; atomic spawn and whole-instance despawn. runtime.instance returns canonical local-to-live member handles. Existing transform/physics/rig ownership and aggregate budgets apply to expanded membership; RPC tick scheduling remains unavailable."}};
    for(const auto* method:{"runtime.step","runtime.component.edit","runtime.gameplay.edit","runtime.gameplay.load","runtime.gameplay.load_native","runtime.audio.replay","runtime.play","player.start"})
        methods[method]["properties"]["expected_structure_revision"]=rev;
    const bool solar_available=Runtime::available() && (Gameplay::available() || Gameplay::native_available());
    result["solar_lighting"]={{"available",solar_available},{"gameplay_feature",gameplay_abi::solar_lighting_feature},
        {"gameplay_services_bytes",gameplay_abi::solar_lighting_services_bytes},{"observation","runtime.lighting"},
        {"max_world_milliseconds",max_solar_world_milliseconds},{"snapshot_version",1},
        {"target","Enabled authored LightingEnvironment.sky with an enabled linked directional Light."},
        {"ownership","Gameplay stages one sample during Update. It publishes on the committed tick."}};
    result["atmosphere_sky"]={{"available",POIMA_RENDER_SMOKE != 0 && POIMA_SIMULATION != 0},
        {"profile","LightingEnvironment.atmosphere"},{"runtime_feature","atmosphere_sky"},
        {"lighting_observation","atmosphere"},{"render_diagnostics","atmosphere_sky"},
        {"orbital_atmosphere",{{"available",POIMA_RENDER_SMOKE != 0 && POIMA_SIMULATION != 0},
            {"profile","LightingEnvironment.atmosphere.planet_center_m"},
            {"runtime_feature","orbital_atmosphere"},{"render_diagnostics","atmosphere_sky"}}},
        {"aerial_perspective",{{"available",POIMA_RENDER_SMOKE != 0 && POIMA_SIMULATION != 0},
            {"profile","LightingEnvironment.atmosphere.aerial_perspective_m"},
            {"runtime_feature","aerial_perspective"},{"render_diagnostics","aerial_perspective"}}},
        {"volumetric_clouds",{{"available",POIMA_RENDER_SMOKE != 0 && POIMA_SIMULATION != 0},
            {"profile","LightingEnvironment.atmosphere.clouds"},
            {"runtime_feature","volumetric_clouds"},{"render_diagnostics","volumetric_clouds"}}},
        {"cloud_shadows",{{"available",POIMA_RENDER_SMOKE != 0 && POIMA_SIMULATION != 0},
            {"profile","LightingEnvironment.atmosphere.clouds.shadow_strength"},
            {"runtime_feature","cloud_shadows"},{"render_diagnostics","cloud_shadows"}}}};
    result["wetness"]={{"available",Runtime::available()},{"component","SurfaceWetness"},
        {"seconds_per_quantum",1},{"max_receivers",wetness::max_receivers},{"max_batch",wetness::max_batch},
        {"max_rain_nm",wetness::max_rain_nm},{"film_units_per_nm",wetness::units_per_nm},
        {"shelter_distance_m",wetness::shelter_distance_m},{"snapshot_version",1},
        {"precipitation",{{"component","ParticleEmitter.rain"},{"window_ticks",rain_particle_window_ticks},
            {"max_particles_per_tick",max_rain_particles_per_tick},{"observation","runtime.particles.inspect"}}}};
    if(Runtime::available()) {
        auto snow_step=methods.at("runtime.step");
        snow_step["properties"]["snow_stamp"]=std::move(snow_stamp);
        snow_step["required"].push_back("snow_stamp");
        methods["runtime.snow.step"]=std::move(snow_step);
        result["snow"]["stamp_method"]="runtime.snow.step";

        auto wetness_step=methods.at("runtime.step");
        wetness_step["properties"]["wetness_dose"]=object_schema({
            {"first_quantum",{{"type","integer"},{"minimum",0},{"maximum",wetness::max_quantum}}},
            {"rain_nm",{{"type","array"},{"minItems",1},{"maxItems",wetness::max_batch},
                {"items",unsigned_range(0,wetness::max_rain_nm)}}}},
            {"first_quantum","rain_nm"});
        wetness_step["required"].push_back("wetness_dose");
        wetness_step["description"]="Advance a guarded runtime step with contiguous rain depths in nanometres per game second. The first simulated tick adds the batch depth to authored water rain_cells and supplies the committed poses for film receivers. The latest depth sets the authored precipitation rate. Outside runtime-game-session v1.";
        methods["runtime.wetness.step"]=std::move(wetness_step);
        methods["runtime.wetness.inspect"]=object_schema({{"session_id",id},{"tick",rev},
            {"structure_revision",rev},{"entity",id}},{"session_id","tick","structure_revision"});
        methods["runtime.wetness.inspect"]["description"]="Read committed films and last sampled shelter, optionally for one receiver. Outside runtime-game-session v1.";
        result["wetness"]["step_method"]="runtime.wetness.step";
        result["wetness"]["inspect_method"]="runtime.wetness.inspect";
    }
    result["invariants"].push_back("Runtime mutations guarded by expected_tick also require expected_structure_revision after any structural transaction. Retained retries use the original guard and return their committed result. Before structural edits the field is optional, but a supplied guard is always checked.");
    result["runtime_available"]=Runtime::available();
    result["invariants"].push_back("Explicit IK grounding owns its slot, uses one bounded post-physics probe per eligible foot, and preserves animation for misses/raised feet. Grounded sessions use nested animation-state v7 with cached decisions; initialization, inspection, preliminary samples and restored poses do not refresh foot probes. Maximum two automatic feet per rig and64 per world; upright uniform wrappers only.");
    return result;
}
}
