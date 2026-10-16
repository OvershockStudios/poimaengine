// SPDX-License-Identifier: Apache-2.0
#include "world_schema.hpp"
#include "poima/runtime.hpp"
#include "poima/scene.hpp"
#include "input_profile_store.hpp"
#include "player_settings_store.hpp"
#include "material_service.hpp"
#include "navigation_schema.hpp"
#include "asset_provenance.hpp"
#include "model_import.hpp"
#include "ui_authoring_schema.hpp"
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>

namespace poima::world_schema {
namespace {
using Json = nlohmann::json;
Json object_schema(Json properties, Json required = Json::array()) {
    return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}
}
using Json = nlohmann::json;
Json describe(const Json& sky_defaults, std::uint64_t max_revision, std::size_t max_document_bytes) {
    const Json id = {{"type", "string"}, {"pattern", "^[0-9a-f]{32}$"}};
    const Json rev = {{"type", "integer"}, {"minimum", 0}, {"maximum", max_revision}};
    const Json name = {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}, {"description", "Also limited to 256 UTF-8 bytes."}};
    const Json parent = {{"anyOf", {id, Json{{"type", "null"}}}}};
    const Json number = {{"type", "number"}, {"minimum", -1e9}, {"maximum", 1e9}};
    auto vector = [](Json item, int size) { return Json{{"type", "array"}, {"items", item}, {"minItems", size}, {"maxItems", size}}; };
    const Json transform = object_schema({{"position", vector(number, 3)}, {"rotation", vector(number, 4)},
        {"scale", vector({{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1e9}}, 3)}}, {"position", "rotation", "scale"});
    const Json component_type = {{"enum", {"Transform", "Camera", "CameraDraw", "MeshRenderer", "BoxCollider", "MeshCollider", "CharacterController", "StaticMesh", "PbrMaterial", "PbrTextures", "Light", "LightingEnvironment", "AcousticMaterial", "AudioEmitter", "AnimationRig", "RigNode", "SkinnedMesh", "HitRegion"}}};
    const Json lens = object_schema({{"vertical_fov", {{"type", "number"}, {"minimum", 5}, {"maximum", 150}}},
        {"near", {{"type", "number"}, {"minimum", 0.001}}}, {"far", {{"type", "number"}, {"maximum", 1e7}}}}, {"vertical_fov", "near", "far"});
    auto camera=lens;camera["properties"]["viewmodel"]=lens;
    auto draw_camera=id;draw_camera["not"]={{"const",std::string(32,'0')}};
    const Json camera_draw=object_schema({{"camera",draw_camera},{"mode",{{"enum",{"viewmodel","hidden"}}}}},{"camera","mode"});
    const Json mesh = object_schema({{"primitive", {{"const", "box"}}}, {"albedo", vector({{"type", "number"}, {"minimum", 0}, {"maximum", 1}}, 3)},
        {"visible", {{"type", "boolean"}}},
        {"cast_shadows",{{"type","boolean"},{"default",true}}},{"receive_shadows",{{"type","boolean"},{"default",true}}}}, {"primitive", "albedo", "visible"});
    const Json collider = object_schema({{"half_extents", vector({{"type","number"},{"minimum",0.001},{"maximum",10000}},3)},
        {"motion",{{"enum",{"static","dynamic","kinematic"}}}}, {"mass",{{"type","number"},{"exclusiveMinimum",0},{"maximum",1e6}}},
        {"friction",{{"type","number"},{"minimum",0},{"maximum",2}}}, {"restitution",{{"type","number"},{"minimum",0},{"maximum",1}}}}, {"half_extents","motion","mass","friction","restitution"});
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
    const Json static_mesh=object_schema({{"asset",asset_id},{"primitive",{{"type","integer"},{"minimum",0},{"maximum",9999}}},{"visible",{{"type","boolean"}}},
        {"cast_shadows",{{"type","boolean"},{"default",true}}},{"receive_shadows",{{"type","boolean"},{"default",true}}}}, {"asset","primitive","visible"});
    const Json mesh_collider=object_schema({{"asset",asset_id},{"primitive",{{"type","integer"},{"minimum",0},{"maximum",9999}}},
        {"friction",{{"type","number"},{"minimum",0},{"maximum",2}}},{"restitution",unit}}, {"asset","primitive","friction","restitution"});
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
        {"pole",vector(Json{{"type","number"},{"minimum",-1e6},{"maximum",1e6}},3)}};
    auto ik_chain=ik_fields;ik_chain["root"]=model_node;ik_chain["middle"]=model_node;ik_chain["end"]=model_node;
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
    auto skin_fields=static_mesh.at("properties");skin_fields["rig"]=id;skin_fields["node"]=model_node;
    const auto skinned_mesh=object_schema(skin_fields,{"asset","primitive","visible","rig","node"});
    const Json pbr=object_schema({{"base_color",vector(unit,3)},{"emissive",vector(unit,3)},{"metallic",unit},{"roughness",unit},{"double_sided",{{"type","boolean"}}}}, {"base_color","emissive","metallic","roughness","double_sided"});
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
    const Json lighting_environment=object_schema({{"ambient",vector({{"type","number"},{"minimum",0},{"maximum",1e6}},3)},
        {"exposure",{{"type","number"},{"minimum",0},{"maximum",1e6}}},{"shadow_resolution",{{"enum",{256,512,1024,2048}},{"default",1024}}},{"shadow_filter",{{"enum",{"pcf3_point","pcf3_fractional"}},{"default","pcf3_point"}}},{"sky",sky}}, {"ambient","exposure"});
    const Json acoustic=object_schema({{"absorption",vector(unit,3)},{"transmission",vector(unit,3)},{"scattering",unit},{"enabled",{{"type","boolean"}}}}, {"absorption","transmission","scattering","enabled"});
    const Json emitter=object_schema({{"asset",asset_id},{"gain",{{"type","number"},{"minimum",0},{"maximum",4}}},{"loop",{{"type","boolean"}}},{"enabled",{{"type","boolean"}}}}, {"asset","gain","loop","enabled"});
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
    const Json components = {{"Transform", transform}, {"Camera", camera}, {"CameraDraw",camera_draw}, {"MeshRenderer", mesh}, {"BoxCollider",collider}, {"MeshCollider",mesh_collider}, {"CharacterController",character},{"StaticMesh",static_mesh},{"SkinnedMesh",skinned_mesh},{"AnimationRig",animation_rig},{"RigNode",rig_node},{"HitRegion",hit_region},{"PbrMaterial",pbr},{"PbrTextures",textures},{"Light",{{"oneOf",light_variants}}},{"LightingEnvironment",lighting_environment},{"AcousticMaterial",acoustic},{"AudioEmitter",emitter}};
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
    op("component.remove", {{"type", {{"enum", {"Camera", "CameraDraw", "MeshRenderer", "BoxCollider", "MeshCollider", "CharacterController", "StaticMesh", "PbrMaterial", "PbrTextures", "Light", "LightingEnvironment", "AcousticMaterial", "AudioEmitter", "AnimationRig", "RigNode", "SkinnedMesh", "HitRegion"}}}}}, {"type"});
    ops.push_back(object_schema({{"op",{{"const","navigation.set"}}},{"asset",{{"anyOf",Json::array({asset_id,Json{{"type","null"}}})}}}},{"op","asset"}));
    ops.push_back(object_schema({{"op",{{"const","asset.provenance.set"}}},{"asset",asset_id},{"records",{{"type","array"},{"maxItems",8},{"uniqueItems",true},{"items",asset_id}}}},{"op","asset","records"}));
    const auto reference_owner=object_schema({{"kind",{{"enum",{"entity","template","world"}}}},{"id",id}},{"kind","id"});
    auto reference_cursor_variant=[&](const char* kind,Json types,Json paths) {
        auto owner=reference_owner;owner["properties"]["kind"]={{"const",kind}};
        return object_schema({{"owner",owner},{"component",std::move(types)},{"path",std::move(paths)}},{"owner","component","path"});
    };
    const Json texture_paths={{"enum",{"/base_color/asset","/emissive/asset","/metallic_roughness/asset","/normal/asset","/occlusion/asset"}}};
    const Json texture_type={{"const","PbrTextures"}},direct_path={{"const","/asset"}};
    Json reference_cursor={{"oneOf",Json::array({
        reference_cursor_variant("entity",{{"enum",{"AnimationRig","AudioEmitter","MeshCollider","SkinnedMesh","StaticMesh"}}},direct_path),
        reference_cursor_variant("entity",texture_type,texture_paths),
        reference_cursor_variant("template",{{"const","StaticMesh"}},direct_path),
        reference_cursor_variant("template",texture_type,texture_paths),
        reference_cursor_variant("world",{{"const","navigation"}},direct_path)})}};
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
    Json result = {{"protocol_version", 1}, {"schema_revision", 103}, {"transport", "JSON-RPC 2.0; one request per line; no batches"},
        {"methods", {
            {"world.describe", {{"type","object"},{"description","Full discovery by default; catalog lists names, while method/component/section retrieves one entry and mutation selects transaction operation schemas. Read the invariants section before mutations."},{"oneOf",Json::array({
                object_schema({{"view",{{"enum",{"full","catalog"}},{"default","full"}}}}),
                object_schema({{"view",{{"enum",{"method","component","section"}}}},{"name",{{"type","string"},{"minLength",1}}}},{"view","name"}),
                object_schema({{"view",{{"const","mutation"}}},{"operation",{{"type","string"},{"minLength",1},{"maxLength",128}}},{"type",{{"type","string"},{"minLength",1},{"maxLength",128}}}},{"view","operation"})
            })}}}, {"world.inspect", object_schema(Json::object())},
            {"world.dependencies",object_schema(Json::object())},
            {"jobs.status",object_schema(Json::object())},
            {"world.asset.references",references_schema},
            {"session.close", object_schema(Json::object())},
            {"entity.get", object_schema({{"id", id}, {"revision", rev}, {"component", component_type}}, {"id"})},
            {"entity.world_transform", object_schema({{"id", id}, {"revision", rev}}, {"id"})},
            {"world.capture", object_schema({{"revision", rev}, {"camera", id},
                {"path", {{"type", "string"}, {"minLength", 1}, {"description", "New BMP path; parent must exist. Opens a bounded native window."}}},
                {"width", {{"type", "integer"}, {"minimum", 128}, {"maximum", 4096}, {"default", 960}}},
                {"height", {{"type", "integer"}, {"minimum", 128}, {"maximum", 4096}, {"default", 540}}},
                {"gpu", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4095}}},
                {"lighting_path",{{"enum",{"forward","deferred"}},{"default","forward"}}},{"ambient_occlusion",object_schema({{"mode",{{"enum",{"none","gtao"}},{"default","none"}}},{"quality",{{"enum",{"low","medium","high"}},{"default","medium"}}},{"radius",{{"type","number"},{"minimum",0.01},{"maximum",100},{"default",1}}}})}, {"reconstruction",{{"enum",{"none","fsr3_native","fsr3_quality","fsr3_balanced","fsr3_performance"}},{"default","none"}}},{"capture_frames",{{"type","integer"},{"minimum",1},{"maximum",128},{"default",2}}},{"scene_product_probes",{{"type","array"},{"maxItems",64},{"items",object_schema({{"x",{{"type","integer"},{"minimum",0},{"maximum",4095}}},{"y",{{"type","integer"},{"minimum",0},{"maximum",4095}}}},{"x","y"})}}},{"scene_debug_view",{{"enum",{"color","depth","shading_normal","motion","motion_validity","ambient_occlusion"}},{"default","color"},{"description","Non-color views require samples=1; sky has no surface product."}}},{"samples", {{"enum", {1, 4}}, {"default", 4}}},{"viewmodel_samples",{{"type","integer"},{"enum",{1,2,4,8}},{"default",1}}},{"culling",{{"type","boolean"},{"default",true}}},{"clustered_lighting",{{"type","boolean"},{"default",true}}},{"frames_in_flight",{{"type","integer"},{"minimum",1},{"maximum",2},{"default",2}}},{"profile",{{"type","boolean"},{"default",false}}}}, {"revision", "camera", "path"})},
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
            "CameraDraw requires one drawable and an existing nonzero Camera reference. Viewmodels require that Camera's viewmodel lens and a strict descendant hierarchy. Hidden references need no lens or descendant relationship.",
            "CameraDraw limits count at most 32 bindings per mode per camera, including invisible meshes. Templates require local camera references. Named Game views separate viewmodels globally and hide bodies only for the target camera while retaining body shadows; external editor Scene views keep ordinary geometry. Viewmodels never cast world shadows and retain receive_shadows.",
            "Box primitive is centered at the origin with unit side lengths; albedo is linear RGB.",
            "Capture is a bounded forward preview, not a playable runtime or advanced renderer.",
            "Custom components use registered stable scalar/collection schemas and game:<type-id> keys; collection capacities count toward the 512-byte component budget. Prefabs and keep_world reparenting remain unsupported.",
            "Simulation is optional; runtime.start freezes authored state at a revision.",
            "Dynamic/kinematic bodies and controllers must be roots; colliders reject shear; a supplied controller camera must be a direct child; camera:null creates a camera-free actor.",
            "Character height must exceed twice radius; runtime is single-threaded fixed 60 Hz."}}};
    auto& methods=result["methods"];
    methods["world.capture"]["properties"]["shadow_filter"]={{"enum",{"pcf3_point","pcf3_fractional"}},{"description","Explicit override; omission inherits LightingEnvironment.shadow_filter, falling back to pcf3_point for legacy scenes."}};
    methods.update(navigation::schemas());
    methods.update(provenance::schemas());
    result["navigation"]=navigation::capability();
    methods["world.history"]=object_schema(Json::object());
    for(const auto* method:{"world.undo","world.redo"})methods[method]=object_schema({{"request_id",id},{"base_revision",rev}},{"request_id","base_revision"});
    result["limits"]["history_entries"]=32;result["limits"]["history_bytes"]=max_document_bytes;
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
    methods["input.inspect"]=object_schema({{"path",input_path}},{"path"});
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
    methods["runtime.hit_test"]=methods.at("runtime.raycast");
    methods["runtime.hit_test"]["description"]="Read-only closest committed-pose collider/HitRegion query. Enabled region owners suppress only their own physics proxy; ignored owners suppress their regions, ignored attachments suppress that region. Exact ties prefer world geometry then region owner/name/attachment. Hit includes nullable region_entity and region; entity is damage owner for regions. Exact session/tick required, no tick advance.";
    methods["runtime.character_stance"]=object_schema({{"session_id",id},{"tick",rev},{"entity",id}},{"session_id","tick","entity"});
    methods["runtime.character_stance"]["description"]="Read committed controller stance with exact session/tick. Noncontrollers return state=null; legacy controllers report configured=false; missing entities reject. Reads do not change shape, retry standing, advance ticks or render.";
    methods["runtime.camera_lens"]=object_schema({{"session_id",id},{"tick",rev},{"entity",id}},{"session_id","tick","entity"});
    methods["runtime.camera_lens"]["description"]="Read committed world and optional first-person lenses plus immutable authored_world with exact session/tick. Existing noncamera returns state=null; missing entity rejects. No tick advancement, rendering or acknowledgement.";
    methods["runtime.root_motion"]=object_schema({{"session_id",id},{"tick",rev},{"entity",id}},{"session_id","tick","entity"});
    methods["runtime.root_motion"]["description"]="Read cached committed planar root-motion controls and requested/actual interval diagnostics with exact session/tick. Existing nonrig or unconfigured rigs return state=null; missing entities reject. Inspection never samples, consumes or advances.";
    methods["runtime.ik"]=object_schema({{"session_id",id},{"tick",rev},{"entity",id},{"slot",ik_slot}}, {"session_id","tick","entity","slot"});
    methods["runtime.ik"]["description"]="Read cached committed two-bone IK controls and diagnostics with exact session/tick. Returns source=runtime and state=null for an existing nonrig or absent configured slot; missing entity rejects. State includes frozen model indices, target/pole/space/weight/enabled, actual world end_position/target_error and clamped/pole_fallback. Inspection never samples or advances.";
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
    auto ik_command=ik_fields;ik_command["entity"]=id;
    methods["runtime.step"]["properties"]["ik"]={{"type","array"},{"maxItems",64},{"items",object_schema(ik_command,{"entity","slot","target","pole"})}};
    auto lens_command=object_schema({{"entity",id},{"world",lens},{"viewmodel",lens}},{"entity"});
    lens_command["anyOf"]=Json::array({Json{{"required",{"world"}}},Json{{"required",{"viewmodel"}}}});
    methods["runtime.step"]["properties"]["camera_lenses"]={{"type","array"},{"maxItems",64},{"items",lens_command}};
    methods["runtime.step"]["properties"]["character_stances"]={{"type","array"},{"maxItems",32},{"items",object_schema({{"entity",id},{"crouch",{{"type","boolean"}}}},{"entity","crouch"})}};
    result["invariants"].push_back("CharacterController stance is optional frozen content; omission preserves the old controller profile. Empty stance selects crouched_height=1, crouched_speed=2, camera_drop=0.8. Crouched height must exceed twice radius and be below standing height; camera drop must fit their difference (1e-6 numeric allowance). Complete unique character_stances commands persist desired posture and apply on the first Tick under full batch rollback. Obstructed standing remains crouched and reports stand_blocked; inspection never retries it. Body feet and collision-tested capsule clearance remain authoritative. No stair, platform, prone, camera-pose interpolation or general locomotion guarantee is implied.");
    result["invariants"].push_back("CharacterController locomotion is an optional frozen virtual-v1 profile. Omission retains the legacy physical controller and omits locomotion from runtime.entity. Empty locomotion selects step_height=0.4, ground_snap=0.5 and max_slope_degrees=45; finite bounds are 0..1, 0..1 and 0..80. Runtime entity diagnostics expose only the configured profile and cached ground_normal/ground_velocity; inspection does not advance or refresh contacts. Body queries retain the character owner identity. Live authored edits do not change the frozen controller backend or profile.");
    result["invariants"].push_back("Camera lens commands replace complete world and/or configured viewmodel lenses, at most64 unique camera targets combined with gameplay per tick. Caller commands apply on the first tick; gameplay writes stage until callback completion. Viewmodel presence and CameraDraw membership stay frozen. All native camera changes roll back with a failed batch and persist in snapshotv7; old snapshots use authored lenses. Player FOV preferences form the neutral baseline: effective FOV composes the runtime/authored tan-half-angle ratio and saturates to5..150; viewmodel lens is independent.");
    methods["runtime.step"]["properties"]["root_motion"]={{"type","array"},{"maxItems",64},{"items",object_schema({{"entity",id},{"enabled",{{"type","boolean"},{"default",true}}}},{"entity"})}};
    result["invariants"].push_back("AnimationRig optionally freezes an explicit model-node root_motion profile. Extract observes planar intervals without moving physics; Character drives one self/ancestor capsule through the normal physics update, preserving gravity/jump and collision-resolved displacement. Configured pose stripping remains active when disabled. Positive-uniform upright bindings, exact source ancestry and layer-source exclusion are required. Enabled Character forbids move input, and yaw-enabled drivers forbid horizontal look. Complete first-Tick enable commands share rollback/retry; no blocked-motion debt, vertical root locomotion or motion warping is implied. Root-configured animation saves use nested statev5; absent profiles retain previous serialization.");
    result["invariants"].push_back("AnimationRig optionally freezes up to eight IK chains with unique slots1..8 and distinct direct-parent model indices root/middle/end, exact matching RigNode graph and no overlapping solved branches. IK wrappers cannot be below RigNode. Active chains require positive uniform evaluated model-ancestor/root/middle scales; wrapper affine transforms must be invertible. Rotation-only post-layer corrections preserve local translation/scale/end rotation and pre-IK animation histories. World goals stay fixed as the wrapper moves; rig goals follow it. Weight0/disabled preserve incoming locals exactly; reach clamping uses rig metric and pole fallback is deterministic.");
    result["invariants"].push_back("runtime.step ik complete controls persist, apply on the first Tick and share full batch rollback. Up to64 independent caller/gameplay IK writes are permitted per Tick with unique rig/slot targets; same-slot conflicts reject, while playback and IK can address the same rig. runtime.ik is cached committed state. IK-configured worlds use nested animation-statev4, preserving old absent-IK serialization and save-upgrade permissions. No automatic grounding, full-body solving or joint-limit system is implied.");
    result["invariants"].push_back("MeshRenderer, StaticMesh and SkinnedMesh optionally define strict boolean cast_shadows and receive_shadows, each defaulting to true when omitted. visible=false suppresses the drawable entirely. These controls do not change colliders, acoustics, material ambient/AO or AnimationRig playback; AnimationRig and RigNode are not drawables and do not inherit controls to their children.");
    result["invariants"].push_back("Animated asset instances expose a wrapper AnimationRig, ordinary RigNode entities for every model node, and SkinnedMesh primitive children. Authored transforms are the baseline; authored capture does not play the initial clip. runtime.step animations replace complete clip/time/speed/loop/playing state atomically with other tick commands.");
    result["invariants"].push_back("Animation commands optionally accept blend_ticks (0..3600, default 0). Zero switches immediately; positive values blend the outgoing and destination local poses over fixed ticks, independently of playback speed. Both clip clocks advance during an ordinary fade. Interrupting a fade freezes its evaluated local pose as the new source; no nested blend tree is retained. runtime.entity animation.transition reports active weights/clocks and is null on completion. Transition state and frozen source poses join batch rollback.");
    result["invariants"].push_back("Optional animation transition_mode defaults to crossfade. Inertial mode applies finite-duration translation, shortest-arc rotation and positive log-scale corrections to the destination, initialized from the last two distinct output ticks and analytic incoming clip derivatives. Missing output history uses zero outgoing velocity; STEP keys, loop seams and endpoints may still be discontinuous. Same-tick samples do not advance velocity history. Native checkpoints and version-2 animation saves retain history and corrections. Active inertial transitions report mode=inertial; weight reports elapsed progress, not a two-clip pose weight. The legacy C# setter selects crossfades; opt-in animation_inertial_v1 supports both transition modes.");
    result["invariants"].push_back("AnimationRig optionally authors up to four frozen layers in slots1..4. Unique model-node mask weights multiply the current layer weight; unlisted nodes are unaffected locally. Ascending slots compose override or additive local TRS. Additive deltas use a frozen authored or explicit reference clip pose, with node-local rotation postmultiplication and positive scale ratios. Descendant world poses can change when a masked ancestor moves.");
    result["invariants"].push_back("Animation commands with layer target a configured slot and require weight0..1. Clip transitions and optional linear weight_blend_ticks run on independent fixed clocks; interrupted weight fades retain the current value without guaranteeing continuous weight velocity. Base and distinct layer targets may share a batch, but duplicate(entity,slot) commands and more than64 combined caller/gameplay commands reject. Each clock retains its own unmasked motion history. animation.layers exposes configured summaries; nested animation-statev3 persists all clocks and weights. Existing C#176/192 callbacks remain base-only; opt-in animation_layers_v1 adds compiled layer queries and commands through a separate208-byte services7 table. Desktop layer widgets are not implemented.");
    auto capture=methods["world.capture"];
    capture["properties"].erase("revision"); capture["properties"]["session_id"]=id; capture["properties"]["tick"]=rev;capture["properties"]["ui_revision"]=rev;
    capture["required"]={"session_id","tick","camera","path"}; methods["runtime.capture"]=capture;
    auto play=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"controller",id},{"camera",id},
        {"mode",{{"enum",{"interactive","replay","background"}}}}, {"max_frames",{{"type","integer"},{"minimum",0},{"maximum",36000}}}},
        {"session_id","request_id","expected_tick","camera","mode"});
    play["allOf"]=Json::array({{{"if",{{"properties",{{"mode",{{"const","replay"}}}}}}},{"then",{{"required",{"controller","sequence"}}}}}});
    auto segment=input; segment["properties"].erase("entity"); segment["properties"]["ticks"]={{"type","integer"},{"minimum",1},{"maximum",600}}; segment["required"]={"ticks"};
    segment["properties"]["motions"]=motions;segment["properties"]["sounds"]=sounds;
    play["properties"]["sequence"]={{"type","array"},{"minItems",1},{"maxItems",256},{"items",segment}};
    for(const auto* key:{"path","width","height","gpu","samples","viewmodel_samples","culling","clustered_lighting","frames_in_flight","scene_debug_view","scene_product_probes","lighting_path","ambient_occlusion","reconstruction","shadow_filter","profile"}) play["properties"][key]=capture["properties"][key];
    play["properties"]["audio"]={{"type","boolean"},{"default",false}};
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
    methods["player.capture_current"]=methods["player.capture"];
    methods["player.capture_current"]["properties"].erase("tick");
    methods["player.capture_current"]["required"]={"player_id","request_id","expected_control_revision","session_id","path"};
    methods["player.capture_current"]["description"]="Atomically choose the current committed Tick and freeze one owned live-window presentation before capturing it. Tick is forbidden; player/control/session and applicable material/structure/UI guards remain exact. Returns captured_tick and full raw committed_camera_lens {entity,world,viewmodel,authored_world}, separate from rendered lens/presentation metadata. Retried request IDs return the original captured sample even after later Ticks. Does not step simulation, advance the interpolation clock or pause playback. Outside authoring-core v1.";
    result["invariants"].push_back("player.start/control/capture require the shared headless owner pump. Start acknowledges deferred initialization; player.inspect reports current readiness and the latest owned partial/final report. Generation, player identity and control revisions guard lifecycle commands; 32 process-local receipts replay acknowledged commands without reapplying them. Stop closes presentation and retains runtime state. Running players reject external simulation mutations; paused interactive players accept guarded edits. Replay rejects intervening runtime edits. Replacement invalidates input and pauses interactive playback. Captures use the same live graphics context and current committed tick with exclusive new output; they do not simulate.");
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
    result["invariants"].push_back("Shadow maps are opt-in per light. Directional=4 views, point=6, spot=1; at most 16 views and 128 MiB of D32 depth storage. Shadowed spot outer_angle <= 89.5; local range must exceed shadow near.");
    result["invariants"].push_back("Capture/play clustered_lighting defaults true. False evaluates the full light table as a reference. Cluster overflow falls back to all lights; directional and range-zero lights are never distance-culled. Assignment diagnostics distinguish requested mode, active mode and measured GPU statistics.");
    result["invariants"].push_back("Capture/play shadow_filter explicitly overrides the authored LightingEnvironment.shadow_filter. Omission inherits it; legacy scenes without the authored field retain pcf3_point. Both choices are pcf3_point or pcf3_fractional. Both retain the same shadow maps, geometric receiver-plane correction and bias. Fractional mode continuously weights four neighboring three-by-three comparison boxes; it does not add contact hardening, cross-face point filtering or shadow caching. Render diagnostics identify the selected filter.");
    result["invariants"].push_back("Capture/play ambient_occlusion defaults to {mode:none,quality:medium,radius:1}; gtao requires deferred and samples=1. Radius is finite in [0.01,100] world units. The ambient_occlusion debug view requires enabled AO. Probes expose raw_ambient_visibility and ambient_visibility; 1 means unoccluded. This contract does not imply renderer qualification.");
    result["invariants"].push_back("Capture/play viewmodel_samples is 1 (default), 2, 4 or 8 independently of world samples, lighting path and reconstruction. Configured foreground diagnostics report the selected count even when no foreground draw is rendered; settings profiles do not persist this option.");
    result["invariants"].push_back("Capture/play lighting_path is forward (default) or deferred. Deferred requires samples=1; this option does not change authored content or imply renderer qualification.");
    result["invariants"].push_back("Capture/play reconstruction is none, fsr3_native, fsr3_quality, fsr3_balanced or fsr3_performance. Enabled reconstruction requires samples=1 and scene_debug_view=color and must be available in this build. Capture-only capture_frames is 1..128 (default 2), repeatedly rendering one frozen snapshot; runtime.play does not accept it. Probe x/y are render-input pixels; resolved_x/y identify mapped reconstruction-output pixels. Raw and resolved HDR values are pre-tone-map radiance.");
    result["invariants"].push_back("Capture/play scene_debug_view is color, depth, shading_normal, motion, motion_validity or ambient_occlusion. Non-color views require samples=1 and bypass exposure/output tone mapping. Single-sample scene products contain device depth, final world-space shading normals and previous-minus-current UV motion with explicit validity. Up to 64 scene_product_probes return raw captured values; probes require samples=1 and a capture path. History advances on accepted per-view submission, independently of simulation tick; missing continuity, cuts and replaced objects invalidate correspondence.");
    result["invariants"].push_back("Capture/play frames_in_flight is 1 or 2, default 2. One selects serialized completion; two bounds outstanding graphics submissions. Frame execution diagnostics distinguish submission, completion and explicit waits; neither completion nor presentation-fence retirement proves monitor scanout.");
    result["invariants"].push_back("Capture/play culling defaults true; camera and each shadow view cull independently. Profile defaults false. Render diagnostics report submitted draws and optional CPU/GPU intervals, not a qualified game frame time.");
    result["invariants"].push_back("Kinematic targets begin on the first tick of step/replay segments and persist across batches. Targets must be unique roots, normalized, at most 100 m/s and 20 rad/s. Raycasts query physics, including hidden colliders; ties use stable IDs. Primitive origin-inside hits have no surface normal; mesh hits retain triangle winding normals.");
    result["invariants"].push_back("MeshCollider is an explicit static indexed model primitive, independent of rendering visibility. It cannot combine with BoxCollider/CharacterController or inherit moving/animation-owned transforms. Physics contacts use triangle front faces; rays query both sides and return the source triangle ordinal (null for non-mesh hits), retaining winding normals. No convexification, texture-cutout collision or deforming meshes. Limits: 100000 triangles/300000 vertices per body and 250000 triangles/750000 vertices per world. Geometry/material/transform validation runs at runtime preparation and export; malformed or degenerate geometry rejects.");
    result["invariants"].push_back("Managed gameplay is optional trusted project code. One module per runtime; typed native state is inspected/edited separately from authoring. Load/edit use tick/revision guards and shared retry receipts. Use input is a first-tick edge. Gameplay updates and queued motion join physics batch rollback.");
    result["input_actions"]={{"maximum_controllers",32},{"maximum_actions_per_controller",32},{"name_pattern","^[a-z][a-z0-9_.-]{0,63}$"},
        {"ordering","Names in runtime input records must be sorted and unique."},
        {"state","Held repeats throughout a tick batch or replay segment. Pressed/released apply only on its first tick; a quick tap may set both with held=false. Omitted records are absent and neutral."},
        {"profiles","An actions object selects poima.input.v3; omission preserves v1/v2. Keyboard, mouse and optional gamepad bindings share the existing control catalog and recovery policy."},
        {"gameplay",{{"feature","input_actions_v1"},{"services_epoch",7},{"required_prefix_bytes",264},{"result_bytes",16},{"maximum_reads_per_tick",1024},{"phase","Tick only"}}},
        {"persistence","Input is transient. Game state derived from input participates in rollback and durable saves."}};
    methods["world.lighting"]=object_schema({{"revision",rev}});
    methods["runtime.lighting"]=object_schema({{"session_id",id},{"tick",rev}}, {"session_id"});
    methods["entity.material"]=object_schema({{"id",id},{"revision",rev}}, {"id"});
    methods["asset.audio.import"]=object_schema({{"source",{{"type","string"},{"minLength",1}}}}, {"source"});
    methods["asset.audio.inspect"]=object_schema({{"asset",asset_id}}, {"asset"});
    for(const auto* method:{"world.audio.inspect","world.audio.capture","runtime.audio.inspect","runtime.audio.capture"}) {
        const bool live=std::string_view(method).starts_with("runtime"),capture_audio=std::string_view(method).ends_with("capture");
        Json props={{"listener",id}};Json required={"listener"};
        if(live) { props["session_id"]=id;props["tick"]=rev;required.push_back("session_id");required.push_back("tick"); }
        else { props["revision"]=rev;required.push_back("revision"); }
        if(capture_audio) { props["path"]={{"type","string"},{"minLength",1}};props["frames"]={{"type","integer"},{"minimum",1},{"maximum",480000},{"default",48000}};required.push_back("path"); }
        methods[method]=object_schema(props,required);
    }
    methods["runtime.audio.voices"]=object_schema({{"session_id",id},{"tick",rev},{"after",rev},{"limit",{{"type","integer"},{"minimum",1},{"maximum",256},{"default",64}}}}, {"session_id","tick"});
    const auto audio_segment=object_schema({{"ticks",{{"type","integer"},{"minimum",1},{"maximum",600}}},
        {"inputs",{{"type","array"},{"maxItems",32},{"items",input}}},{"motions",motions},{"sounds",sounds}}, {"ticks"});
    methods["runtime.audio.replay"]=object_schema({{"session_id",id},{"request_id",id},{"expected_tick",rev},{"listener",id},
        {"path",{{"type","string"},{"minLength",1}}},{"sequence",{{"type","array"},{"minItems",1},{"maxItems",256},{"items",audio_segment}}}},
        {"session_id","request_id","expected_tick","listener","path","sequence"});
    result["invariants"].push_back("Sound play/stop commands join runtime.step batch rollback and apply on the first tick; C# sound calls share that transaction. Voices use session-local monotonic handles, at most 64 emitting and 256 retained records. runtime.audio.replay advances and records committed ticks (max 3600 total), retaining partial progress and retry receipts on failure. DSP uses persistent filters and does not alter logical voice state.");
    result["invariants"].push_back("Audio observation is synchronous and frozen: fresh geometry and poses on every query, no simulation advance, source cursor or device playback. AcousticMaterial requires box/mesh geometry. At most 64 enabled emitters, 131072 acoustic triangles, 64 MiB clip packages; mono 48 kHz PCM16/float32 WAV import, up to 60 seconds per clip. Captures are 1..480000 stereo float frames with direct paths and HRTF only.");
    methods.update(materials::Service::schemas());
    result["invariants"].push_back("Native animation tasks and material baking share one lazy executor per creating owner thread. POIMA_JOB_WORKERS selects 0..8 workers (default 2) before first use; zero is the serial reference. Frame/background admission is separate, and reserved frame workers do not execute background work. Registry, physics, C# callbacks and publication remain owner-only. jobs.status inspects this shared owner pool without creating it; its counts may include other sessions on the same owner.");
    result["invariants"].push_back("Procedural brick/plaster recipes bake compiled CPU work without world changes or worker file I/O. One active material bake and eight retained records per service; explicit polling publishes immutable image packages and a recipe descriptor. Cancellation before publication vetoes even completed CPU results. Jobs are authoring-only; runtime closure contains referenced baked images. These evolving methods are outside authoring-core v1.");
    methods["asset.image.import"]=object_schema({{"source",{{"type","string"},{"minLength",1}}},{"color_space",{{"enum",{"srgb","linear"}}}}}, {"source","color_space"});
    methods["asset.image.inspect"]=object_schema({{"asset",asset_id}}, {"asset"});
    const Json reference_pose={{"oneOf",Json::array({
        object_schema({{"kind",{{"const","rest"}}}},{"kind"}),
        object_schema({{"kind",{{"const","sample"}}},{"clip",{{"type","integer"},{"minimum",0},{"maximum",255}}},
            {"time",{{"type","number"},{"minimum",0},{"maximum",3600}}}},{"kind","clip","time"})})}};
    const auto frame_alignment=object_schema({
        {"position",vector(Json{{"type","number"},{"minimum",-1e9},{"maximum",1e9}},3)},
        {"rotation",vector(Json{{"type","number"},{"minimum",-1},{"maximum",1}},4)}},{"position","rotation"});
    auto frame_transfer=object_schema({{"policy",{{"const","reference-frame-v1"}}},
        {"source_pose",reference_pose},{"target_pose",reference_pose},{"alignment",frame_alignment}},
        {"policy","source_pose","target_pose"});
    frame_transfer["description"]="Explicit same-origin rigid bone-frame conversion. References sample original unfiltered source/base takes; rest uses imported defaults. Alignment defaults to identity and requires normalized XYZW rotation. Different origins/proportions and nonuniform scale reject. Target rest/geometry/inverse binds and runtime actor ownership remain unchanged.";
    const Json retarget_positions={{"oneOf",Json::array({
        object_schema({{"kind",{{"const","target_reference"}}}},{"kind"}),
        object_schema({{"kind",{{"const","reference_delta"}}},
            {"nodes",{{"type","array"},{"minItems",1},{"maxItems",64},{"uniqueItems",true},{"items",{{"type","integer"},{"minimum",0},{"maximum",9999}}}}},
            {"scale",{{"type","number"},{"exclusiveMinimum",0},{"maximum",100}}}},{"kind","nodes","scale"})})}};
    auto rotation_retarget=object_schema({{"policy",{{"const","reference-rotation-v1"}}},
        {"source_pose",reference_pose},{"target_pose",reference_pose},
        {"alignment_rotation",vector(Json{{"type","number"},{"minimum",-1},{"maximum",1}},4)},
        {"positions",retarget_positions},{"scales",{{"const","target_reference"}}},
        {"expected_source_model_sha256",asset_id},{"expected_target_model_sha256",asset_id},
        {"node_map",{{"type","array"},{"minItems",1},{"maxItems",1024},{"items",object_schema({
            {"source_node",{{"type","integer"},{"minimum",0},{"maximum",9999}}},
            {"target_node",{{"type","integer"},{"minimum",0},{"maximum",9999}}}}, {"source_node","target_node"})}}}},
        {"policy","source_pose","target_pose","positions","scales","expected_source_model_sha256","expected_target_model_sha256"});
    rotation_retarget["description"]="Explicit quaternion-chain retargeting with target-reference positions/scales and optional selected source-local position deltas. Original source/base fingerprints from asset.source.inspect are required. Preserves target geometry/defaults/binds, not source affine matrices, contacts or planted feet. Alignment is normalized XYZW; no automatic proportions, root-motion extraction or controller movement. Optional node_map uses 1..1024 original source/target index pairs, one-to-one direct-parent/root correspondence, complete target skin and selected source channel ancestry, and no unrelated pairs; omission retains exact named hierarchy matching.";
    auto animation_source=object_schema({{"source",{{"type","string"},{"minLength",1}}},
        {"clip",{{"type","integer"},{"minimum",0},{"maximum",255}}},{"name",{{"type","string"},{"minLength",1},{"maxLength",256}}},
        {"frame_transfer",frame_transfer},{"retarget",rotation_retarget}},{"source"});
    animation_source["allOf"]=Json::array({Json{{"not",{{"required",Json::array({"frame_transfer","retarget"})}}}}});
    methods["asset.import"]=object_schema({{"source",{{"type","string"},{"minLength",1}}},
        {"fbx_normal_map",{{"enum",{"opengl","directx"}}}},
        {"fbx_profile_diagnostics",{{"type","boolean"},{"default",false}}},
        {"animations",{{"type","array"},{"minItems",1},{"maxItems",32},{"items",{{"anyOf",Json::array({Json{{"type","string"},{"minLength",1}},animation_source})}}}}}}, {"source"});
    methods["asset.import"]["description"]="Cook glTF/GLB/FBX into an immutable model. Animation paths append all named takes; objects select a take by index and optionally rename it. Exact hierarchy/rest-frame matching by default; mutually exclusive frame_transfer and retarget policies provide explicit reference-frame or quaternion-chain conversion. FBX normal maps default to OpenGL; select DirectX explicitly. Imports do not mutate the authored world.";
    result["invariants"].push_back("Animation frame_transfer is opt-in reference-frame-v1: explicit imported-rest or sampled source/target references, source sampling before selected-take filtering, and optional rigid alignment. Same named topology and coincident global origins are required; mapped scales and scale curves must be uniform. Conversion preserves curve interpolation and includes transformed sparse defaults; target geometry/rest/inverse binds are unchanged. It does not infer bind poses, remove root motion or move runtime controllers. Failures publish no partial model.");
    result["invariants"].push_back("Animation retarget is opt-in reference-rotation-v1 and mutually exclusive with frame_transfer. Quaternion-chain orientation transfer ignores affine scale; explicit target-reference scale/position policies preserve target geometry and optionally transfer selected local position deltas. Original source/base model fingerprints guard source-local indices and reference takes before filtering. Source scaling and unselected translation tracks are discarded with counts; constant target-reference channels preserve original duration. Shape similarity, contacts, ground placement and loop continuity require separate observation/authoring, not an inferred guarantee.");
    const Json composition_identity={{"anyOf",Json::array({Json{{"type","integer"},{"minimum",0},{"maximum",9999}},Json{{"type","null"}}})}};
    const Json composition_name={{"anyOf",Json::array({Json{{"type","string"},{"minLength",1},{"maxLength",256}},Json{{"type","null"}}})}};
    const Json nonnegative_metric={{"type","number"},{"minimum",0}};
    auto composition_issue=object_schema({
        {"kind",{{"enum",{"missing_joint_or_ancestor","missing_animation_target","rest_frame"}}}},
        {"base_node",composition_identity},{"donor_node",composition_identity},
        {"base_name",composition_name},{"donor_name",composition_name},
        {"required_skeleton",{{"type","boolean"}}},{"animated_ancestry",{{"type","boolean"}}},
        {"metrics",object_schema({{"max_translation_meters",nonnegative_metric},{"max_scale_absolute",nonnegative_metric},
            {"absolute_quaternion_dot",nonnegative_metric},{"rotation_degrees",{{"type","number"},{"minimum",0},{"maximum",180}}}},
            {"max_translation_meters","max_scale_absolute","absolute_quaternion_dot","rotation_degrees"})},
        {"mismatches",object_schema({{"translation",{{"type","boolean"}}},{"rotation",{{"type","boolean"}}},{"scale",{{"type","boolean"}}}},
            {"translation","rotation","scale"})}},
        {"kind","base_node","donor_node","base_name","donor_name","required_skeleton","animated_ancestry"});
    Json composition_fields;
    composition_fields["if"]["properties"]["kind"]["const"]="rest_frame";
    composition_fields["then"]["required"]={"metrics","mismatches"};
    composition_fields["else"]["not"]["anyOf"]=Json::array({
        Json{{"required",Json::array({"metrics"})}},Json{{"required",Json::array({"mismatches"})}}});
    composition_issue["allOf"]=Json::array({composition_fields});
    methods["asset.import"]["x-error-data"]["animation_composition"]=object_schema({
        {"type",{{"const","animation_composition"}}},{"version",{{"const",1}}},
        {"policy",{{"const","exact-skeleton-v1"}}},{"comparison",{{"const","normalized_local_rest"}}},
        {"donor_index",{{"type","integer"},{"minimum",0},{"maximum",31}}},
        {"required_base_nodes",rev},{"checked_source_nodes",rev},{"matched_nodes",rev},{"issue_count",rev},
        {"limit",{{"const",animation_composition_issue_limit}}},{"truncated",{{"type","boolean"}}},
        {"thresholds",object_schema({{"translation_meters",{{"const",animation_composition_translation_tolerance}}},
            {"scale_absolute",{{"const",animation_composition_scale_tolerance}}},
            {"absolute_quaternion_dot_min",{{"const",animation_composition_quaternion_dot_min}}}},
            {"translation_meters","scale_absolute","absolute_quaternion_dot_min"})},
        {"issues",{{"type","array"},{"minItems",1},{"maxItems",animation_composition_issue_limit},{"items",composition_issue}}}},
        {"type","version","policy","comparison","donor_index","required_base_nodes","checked_source_nodes","matched_nodes",
         "issue_count","limit","truncated","thresholds","issues"});
    result["invariants"].push_back("An exact-skeleton composition failure returns asset.import error -32050 with versioned animation_composition data: first incompatible donor, complete issue counts and at most 64 node records. It compares normalized local rest transforms, not recovered bind poses. No partial model is published. Other import failures may have no data.");
    methods["asset.inspect"]=object_schema({{"asset",asset_id},{"section",{{"enum",{"summary","nodes","primitives","images","skins","animations"}}}},{"offset",rev},{"limit",{{"type","integer"},{"minimum",1},{"maximum",64}}}}, {"asset"});
    auto source_inspect=object_schema({{"source",{{"type","string"},{"minLength",1},{"maxLength",4096}}},
        {"fbx_normal_map",{{"enum",{"opengl","directx"}}}},
        {"fbx_profile_diagnostics",{{"type","boolean"},{"default",false}}},
        {"section",{{"enum",{"summary","nodes","skins","animations","skin_joints","animation_channels"}},{"default","summary"}}},
        {"offset",rev},{"limit",{{"type","integer"},{"minimum",1},{"maximum",64},{"default",64}}},
        {"pose",reference_pose},{"expected_model_sha256",asset_id},{"skin",rev},{"clip",rev}},{"source"});
    source_inspect["allOf"]=Json::array({Json{{"if",{{"required",Json::array({"pose"})}}},
        {"then",{{"required",Json::array({"section"})},{"properties",{{"section",{{"const","nodes"}}}}}}}}});
    for(const auto& [section_name,selector_name]:{std::pair{"skin_joints","skin"},std::pair{"animation_channels","clip"}}) {
        source_inspect["allOf"].push_back({{"if",{{"required",Json::array({selector_name})}}},
            {"then",{{"required",Json::array({"section"})},{"properties",{{"section",{{"const",section_name}}}}}}}});
        source_inspect["allOf"].push_back({{"if",{{"required",Json::array({"section"})},{"properties",{{"section",{{"const",section_name}}}}}}},
            {"then",{{"required",Json::array({selector_name})}}}});
    }
    source_inspect["description"]="Inspect normalized original glTF/GLB/FBX data without publishing a cooked asset or changing the world. Geometryless animation donors are supported. Page node/default/clip/skin metadata and optionally sample an original node pose without looping or out-of-duration clamping. skin_joints requires an original skin selector and returns ordered palette indices, original node IDs and inverse binds. animation_channels requires an original clip selector and returns ordered channel indices, original target nodes, path, interpolation and key counts. Selectors apply only to their corresponding sections; pose applies only to nodes. model_sha256 identifies the original parsed model; use expected_model_sha256 across pages and the retarget source/base guards. Diagnostics retain complete counts and at most 64 bounded records. This evolving read-only method is outside authoring-core v1.";
    methods["asset.source.inspect"]=source_inspect;
    auto texture_dependency_error=object_schema({
        {"type",{{"const","fbx_texture_dependency"}}},{"version",{{"const",1}}},
        {"reason",{{"enum",{"unavailable","empty","byte_budget","read_failed"}}}},
        {"relative_path",{{"type","string"},{"minLength",1},{"maxLength",4096}}},
        {"file_bytes",{{"anyOf",Json::array({Json{{"type","integer"},{"minimum",0}},Json{{"type","null"}}})}}},
        {"byte_limit",{{"type","integer"},{"minimum",0},{"maximum",32*1024*1024}}},
        {"source_role",{{"enum",{"base","animation_donor","inspection"}}}},
        {"donor_index",{{"anyOf",Json::array({Json{{"type","integer"},{"minimum",0},{"maximum",31}},Json{{"type","null"}}})}}}},
        {"type","version","reason","relative_path","file_bytes","byte_limit","source_role","donor_index"});
    texture_dependency_error["description"]="A contained external FBX texture file could not be read. relative_path is the slash-normalized source reference, never a resolved OS path. Limits and file sizes are bytes; unavailable metadata is null. Validation, decoding, embedded images and unsafe paths retain their existing errors.";
    texture_dependency_error["allOf"]=Json::array({Json{
        {"if",{{"properties",{{"source_role",{{"const","animation_donor"}}}}}}},
        {"then",{{"properties",{{"donor_index",{{"type","integer"},{"minimum",0},{"maximum",31}}}}}}},
        {"else",{{"properties",{{"donor_index",{{"type","null"}}}}}}}}});
    methods["asset.import"]["x-error-data"]["fbx_texture_dependency"]=texture_dependency_error;
    methods["asset.source.inspect"]["x-error-data"]["fbx_texture_dependency"]=texture_dependency_error;
    const Json nullable_count={{"anyOf",Json::array({rev,Json{{"type","null"}}})}};
    auto material_profile_error=object_schema({
        {"type",{{"const","fbx_material_profile"}}},{"version",{{"const",1}}},
        {"reason",{{"enum",{"transparency","advanced_feature","unlit","shader_model","texture_slot","uv_binding","normal_response"}}}},
        {"feature",{{"type","string"},{"minLength",1},{"maxLength",64}}},
        {"node",{{"type","integer"},{"minimum",0},{"maximum",9999}}},
        {"node_name",{{"type","string"},{"maxLength",256}}},
        {"material_slot",{{"type","integer"},{"minimum",0},{"maximum",4294967295ull}}},
        {"material_name",{{"type","string"},{"maxLength",256}}},
        {"observed_count",nullable_count},{"limit",nullable_count},
        {"source_role",{{"enum",{"base","animation_donor","inspection"}}}},
        {"donor_index",{{"anyOf",Json::array({Json{{"type","integer"},{"minimum",0},{"maximum",31}},Json{{"type","null"}}})}}}},
        {"type","version","reason","feature","node","node_name","material_slot","material_name","observed_count","limit","source_role","donor_index"});
    material_profile_error["description"]="Opt-in witness for selected material-profile rejection sites. node is the original normalized source-node index; material_slot is local to it. Display names are repaired and bounded, not matching identities. observed_count/limit describe UV-set or connected normal-response component counts, never an inferred source total or scalar magnitude; normal_response additionally requires scalar value one. No source path or raw material graph is returned.";
    material_profile_error["allOf"]=Json::array({Json{
        {"if",{{"properties",{{"source_role",{{"const","animation_donor"}}}}}}},
        {"then",{{"properties",{{"donor_index",{{"type","integer"},{"minimum",0},{"maximum",31}}}}}}},
        {"else",{{"properties",{{"donor_index",{{"type","null"}}}}}}}}});
    methods["asset.import"]["x-error-data"]["fbx_material_profile"]=material_profile_error;
    methods["asset.source.inspect"]["x-error-data"]["fbx_material_profile"]=material_profile_error;
    result["invariants"].push_back("fbx_profile_diagnostics defaults false. Only true adds versioned fbx_material_profile data to selected existing material rejections; legacy omitted/false error envelopes and all accepted cooked content are preserved. Source node indices refer to normalized original FBX nodes and material_slot is local to that node. Counts/limits are present only when observed; normal_response requires a single component with scalar value one. Donor attribution follows the original animations array. No source repair, skipped materials, partial publication or absolute path disclosure is performed.");
    result["invariants"].push_back("Contained external FBX texture read failures return error -32050 with versioned fbx_texture_dependency data, relative dependency identity, reason and effective byte limit. Animation donor failures identify the zero-based donor index. Source inspection and rejected imports do not publish partial models or mutate authored state; no texture search or admission relaxation is performed.");
    const Json page_limit={{"type","integer"},{"minimum",1},{"maximum",64}};
    const Json model_index={{"type","integer"},{"minimum",0},{"maximum",9999}};
    methods["asset.animation.channel"]=object_schema({{"asset",asset_id},{"clip",model_index},{"channel",model_index},{"offset",rev},{"limit",page_limit}}, {"asset","clip","channel"});
    methods["asset.animation.skin"]=object_schema({{"asset",asset_id},{"skin",model_index},{"offset",rev},{"limit",page_limit}}, {"asset","skin"});
    methods["asset.animation.sample"]=object_schema({{"asset",asset_id},{"clip",model_index},{"time",{{"type","number"},{"minimum",0},{"maximum",1e9}}},{"loop",{{"type","boolean"},{"default",false}}},{"section",{{"enum",{"nodes","vertices"}}}},{"node",model_index},{"primitive",model_index},{"offset",rev},{"limit",page_limit}}, {"asset","time"});
    auto preview=methods["world.capture"];
    preview["properties"]["skinning"]={{"enum",{"gpu","cpu"}},{"default","gpu"}};
    preview["properties"]["asset"]=asset_id;preview["properties"]["clip"]=model_index;
    preview["properties"]["time"]=methods["asset.animation.sample"]["properties"]["time"];
    preview["properties"]["loop"]=methods["asset.animation.sample"]["properties"]["loop"];
    preview["required"].push_back("asset");preview["required"].push_back("time");methods["asset.animation.capture"]=preview;
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
    methods["runtime.material.apply"]["description"]="Explicitly adopt authored base_color/emissive/metallic/roughness changes on existing explicit PbrMaterial drawables without restarting gameplay. Full content and asset closure guards reject unrelated edits. Accepted state is distinct from renderer submission/presentation. Outside authoring-core v1.";
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
    methods["player.capture"]["properties"]["expected_material_revision"]=rev;
    methods["player.capture_current"]["properties"]["expected_material_revision"]=rev;
    methods["save.write"]["properties"]["expected_material_revision"]=rev;
    methods["save.load"]["properties"]["expected_material_revision"]={{"anyOf",Json::array({rev,Json{{"type","null"}}})}};
    result["live_materials"]={{"maximum_entities",32},{"fields",{"base_color","emissive","metallic","roughness"}},
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
    Json recipe_properties=Json::object();for(const auto* type:{"Transform","BoxCollider","MeshRenderer","StaticMesh","PbrMaterial","PbrTextures"})recipe_properties[type]=components.at(type);
    auto recipe_components=object_schema(recipe_properties,{"Transform"});recipe_components["patternProperties"]={{"^game:[0-9a-f]{32}$",custom_values}};
    mutations.push_back(object_schema({{"op",{{"const","template.set"}}},{"id",stable_type},{"name",name},{"components",recipe_components}},{"op","id","name","components"}));
    auto member_components=object_schema(components,{"Transform"});
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
    result["invariants"].push_back("Runtime mutations guarded by expected_tick also require expected_structure_revision after any structural transaction. Retained retries use the original guard and return their committed result. Before structural edits the field is optional, but a supplied guard is always checked.");
    result["runtime_available"]=Runtime::available();
    return result;
}
}
