// SPDX-License-Identifier: Apache-2.0
#include "world_schema_asset.hpp"

#include "material_service.hpp"
#include "model_import.hpp"
#include "poima/animation.hpp"

#include <utility>

namespace poima::world_schema::detail {
void add_asset_method_schemas(Json& result, const Json& id, const Json& rev,
                              const Json& asset_id, const Json& model_node,
                              const Json& number) {
    auto& methods = result.at("methods");
    methods["asset.audio.import"]=object_schema({{"source",{{"type","string"},{"minLength",1}}}}, {"source"});
    methods["asset.audio.inspect"]=object_schema({{"asset",asset_id}}, {"asset"});
    methods.update(materials::Service::schemas());
    methods["asset.image.import"]=object_schema({{"source",{{"type","string"},{"minLength",1}}},{"color_space",{{"enum",{"srgb","linear"}}}}}, {"source","color_space"});
    methods["asset.post-effect.import"]=object_schema({{"source",{{"type","string"},{"minLength",1}}},{"expected_sha256",asset_id}},
        {"source","expected_sha256"});
    methods["asset.post-effect.import"]["description"]="Admit a local .poimapost file using its whole-file SHA-256 and publish the exact reviewed bytes in this world's asset store. Returns the asset, effect ID, artifact byte count, and SPIR-V word count. Import preserves the authored revision.";
    methods["asset.image.inspect"]=object_schema({{"asset",asset_id}}, {"asset"});
    methods["asset.image.cutout"]=object_schema({{"asset",asset_id},
        {"image",{{"type","integer"},{"minimum",0},{"maximum",255}}},
        {"alpha_threshold",{{"type","integer"},{"minimum",1},{"maximum",255}}}},{"asset","alpha_threshold"});
    methods["asset.image.cutout"]["description"]="Cook coverage-adjusted RGBA8 mips from an existing standalone image, or an embedded model image selected by image index. Mip zero stays byte-exact. Alpha threshold is an inclusive byte value; returned mip rows report covered_texels. The new standalone asset preserves the authored revision.";
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
        {"frame_transfer",frame_transfer},{"retarget",rotation_retarget},{"expected_model_sha256",asset_id}},{"source"});
    animation_source["allOf"]=Json::array({Json{{"not",{{"required",Json::array({"frame_transfer","retarget"})}}}}});
    const Json catalog_kind={{"enum",{"model","image","audio"}}};
    methods["asset.catalog"]=object_schema({{"kind",catalog_kind},
        {"after",{{"type","string"},{"pattern","^[0-9a-f]{64}\\.(pmodel|pimage|paudio)$"}}},
        {"limit",{{"type","integer"},{"minimum",1},{"maximum",64},{"default",64}}}});
    methods["asset.catalog"]["description"]="List candidate cooked models, images and audio from the current world's asset store, including unused assets. Pages are sorted by store filename. Pass next_after to continue. Package bytes are not read: use the type's inspect method before use. Labels are optional.";
    methods["asset.label"]=object_schema({{"asset",asset_id},{"kind",catalog_kind},
        {"label",{{"type","string"},{"minLength",1},{"maxLength",1024}}}},{"asset","kind","label"});
    methods["asset.label"]["description"]="Remember the first display label for an existing cooked asset. Returns the existing label on repeat. Labels accept 1..1024 UTF-8 bytes without paths or control characters. Does not change the scene, dependency set, or cooked identity.";
    methods["asset.import"]=object_schema({{"source",{{"type","string"},{"minLength",1}}},
        {"expected_model_sha256",asset_id},
        {"fbx_normal_map",{{"enum",{"opengl","directx"}}}},
        {"fbx_geometry_only",{{"type","boolean"},{"default",false},{"description","Use neutral materials for the FBX base source without resolving its textures. Geometry, material face slots, rigs and takes are retained. Animation donors keep their standard import policy."}}},
        {"fbx_profile_diagnostics",{{"type","boolean"},{"default",false}}},
        {"animations",{{"type","array"},{"minItems",1},{"maxItems",32},{"items",{{"anyOf",Json::array({Json{{"type","string"},{"minLength",1}},animation_source})}}}}}}, {"source"});
    methods["asset.import"]["description"]="Cook glTF/GLB/FBX into an immutable model. Optional expected_model_sha256 guards the original parsed base model against asset.source.inspect.model_sha256 before composition. Animation paths append all named takes; objects select a take by index and optionally rename it. A donor object's optional expected_model_sha256 guards its entire original parsed model before selection or conversion. Fingerprints are not raw-file or composed-package hashes. Exact hierarchy/rest-frame matching by default; mutually exclusive frame_transfer and retarget policies provide explicit reference-frame or quaternion-chain conversion. FBX normal maps default to OpenGL; select DirectX explicitly. Imports do not mutate the authored world.";
    auto source_changed=object_schema({{"type",{{"const","model_source_changed"}}},{"version",{{"const",1}}},
        {"source_role",{{"enum",{"base","animation_donor"}}}},
        {"donor_index",{{"anyOf",Json::array({Json{{"type","integer"},{"minimum",0},{"maximum",31}},Json{{"type","null"}}})}}},
        {"expected_model_sha256",asset_id},{"actual_model_sha256",asset_id}},
        {"type","version","source_role","donor_index","expected_model_sha256","actual_model_sha256"});
    source_changed["allOf"]=Json::array({Json{
        {"if",{{"properties",{{"source_role",{{"const","animation_donor"}}}}}}},
        {"then",{{"properties",{{"donor_index",{{"type","integer"},{"minimum",0},{"maximum",31}}}}}}},
        {"else",{{"properties",{{"donor_index",{{"type","null"}}}}}}}}});
    source_changed["description"]="The optional import guard did not match the complete original parsed base or donor model. Donor index refers to the original animations array. No package is published. Reinspect deliberately before choosing new model identities; no automatic retry or source repair occurs.";
    methods["asset.import"]["x-error-data"]["model_source_changed"]=source_changed;
    result["invariants"].push_back("Optional asset.import base and donor expected_model_sha256 guards use the complete original normalized model identity from asset.source.inspect, before filtering, renaming, frame transfer or retargeting. A mismatch returns -32050 with model_source_changed data and publishes no partial package. Existing rotation-retarget guards remain independently required. Matching or omitted new guards do not enter cooked metadata or alter package identity.");
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
        {"fbx_geometry_only",{{"type","boolean"},{"default",false},{"description","Inspect the FBX with neutral materials. Use the same setting when guarding import with the returned fingerprint."}}},
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
    const Json key_time={{"type","number"},{"minimum",0},{"maximum",3600}};
    const Json hinge_degrees={{"type","number"},{"minimum",-180},{"maximum",180}};
    const Json unit_coordinate={{"type","number"},{"minimum",-1.000001},{"maximum",1.000001}};
    const auto keys=[&](const Json& key) { return Json{{"type","array"},{"minItems",1},{"maxItems",256},{"items",key}}; };
    const Json interpolation={{"enum",{"step","linear"}},{"default","linear"}};
    const auto track_rotation=object_schema({{"node",model_node},{"interpolation",interpolation},
        {"keys",keys(object_schema({{"time",key_time},{"rotation",vector(unit_coordinate,4)}},{"time","rotation"}))}},{"node","keys"});
    const auto hinge=object_schema({{"axis",vector(unit_coordinate,3)},{"minimum_degrees",hinge_degrees},{"maximum_degrees",hinge_degrees}},
        {"axis","minimum_degrees","maximum_degrees"});
    const auto track_hinge=object_schema({{"node",model_node},{"interpolation",interpolation},{"hinge",hinge},
        {"keys",keys(object_schema({{"time",key_time},{"degrees",hinge_degrees}},{"time","degrees"}))}},{"node","hinge","keys"});
    const auto track_translation=object_schema({{"node",model_node},{"path",{{"const","translation"}}},{"interpolation",interpolation},
        {"keys",keys(object_schema({{"time",key_time},{"translation",vector(number,3)}},{"time","translation"}))}},
        {"node","path","keys"});
    const auto authored_clip=object_schema({
        {"name",{{"type","string"},{"minLength",1},{"maxLength",256}}},
        {"tracks",{{"type","array"},{"minItems",1},{"maxItems",128},{"items",{{"oneOf",Json::array({track_rotation,track_hinge,track_translation})}}}}}},
        {"name","tracks"});
    methods["asset.animation.author"]=object_schema({{"asset",asset_id},
        {"clips",{{"type","array"},{"minItems",1},{"maxItems",8},{"items",authored_clip}}}},{"asset","clips"});
    methods["asset.animation.author"]["description"]="Append 1..8 joint-local clips to an immutable cooked model and return a new model hash with clip indices. Rotation keys use unit XYZW in the joint's parent frame, or a unit hinge axis and signed degree offsets from model rest. Hinge interval width is at most 179.9 degrees. Translation tracks use path translation and absolute parent-frame XYZ keys. Times must strictly increase after float32 encoding, at most 4096 total keys. Each node supports one track per path. New clip names are unique. No world edit occurs.";
    methods["asset.mesh.extract_rigid"]=object_schema({{"asset",asset_id},{"node",model_index},{"joint",model_node},
        {"primitives",{{"type","array"},{"minItems",1},{"maxItems",32},{"uniqueItems",true},{"items",model_index}}}},
        {"asset","node","joint","primitives"});
    methods["asset.mesh.extract_rigid"]["description"]="Extract selected primitives into a separate static model in the selected joint's local space. Each vertex must have exactly one nonzero weight, one on that joint. The inverse bind transforms positions, normals and tangents. UVs, indices, material factors and used embedded textures survive. Output follows sorted source primitive order. The source package and world remain unchanged. Outside authoring-core v1.";
    methods["asset.mesh.triangles"]=object_schema({{"asset",asset_id},{"primitive",model_index},{"offset",rev},{"limit",page_limit}},
        {"asset","primitive"});
    methods["asset.mesh.triangles"]["description"]="Page zero-based triangle ordinals and the three cooked vertex indices in winding order for one immutable model primitive. Read-only, outside authoring-core v1.";
    methods["asset.animation.channel"]=object_schema({{"asset",asset_id},{"clip",model_index},{"channel",model_index},{"offset",rev},{"limit",page_limit}}, {"asset","clip","channel"});
    methods["asset.animation.skin"]=object_schema({{"asset",asset_id},{"skin",model_index},{"offset",rev},{"limit",page_limit}}, {"asset","skin"});
    methods["asset.animation.sample"]=object_schema({{"asset",asset_id},{"clip",model_index},{"time",{{"type","number"},{"minimum",0},{"maximum",1e9}}},{"loop",{{"type","boolean"},{"default",false}}},{"section",{{"enum",{"nodes","vertices"}}}},{"node",model_index},{"primitive",model_index},{"offset",rev},{"limit",page_limit}}, {"asset","time"});
    methods["asset.animation.physical_limits"]=object_schema({{"entity",id},{"revision",rev},
        {"clip",{{"type",Json::array({"integer","null"})},{"minimum",0},{"maximum",max_model_clips-1}}},
        {"time",{{"type","number"},{"minimum",0},{"maximum",1e9}}},{"loop",{{"type","boolean"},{"default",false}}}},
        {"entity","revision","clip","time"});
    auto& pose_limits=methods["asset.animation.physical_limits"];
    pose_limits["if"]={{"properties",{{"clip",{{"type","null"}}}}}};
    pose_limits["then"]={{"properties",{{"time",{{"const",0}}},{"loop",{{"const",false}}}}}};
    pose_limits["description"]="Read authored joint-limit diagnostics for one cooked source pose. Select a rig entity and its world revision, plus an explicit clip index and time in seconds or null and time zero for model rest. All authored joints report calibrated XYZW relative orientations, limits in radians and the engine's fit or an indeterminate reason. The query does not change the world, pose or physics.";
    methods["asset.animation.bounds"]=object_schema({{"asset",asset_id},
        {"clip",{{"type","integer"},{"minimum",0},{"maximum",max_model_clips-1}}},
        {"time",{{"type","number"},{"minimum",0},{"maximum",1e9}}},
        {"loop",{{"type","boolean"},{"default",false}}}}, {"asset","time"});
    result["limits"]["model_bounds_primitive_instances"]=max_model_bounds_primitive_instances;
    result["limits"]["model_bounds_vertex_instances"]=max_model_bounds_vertex_instances;
    result["invariants"].push_back("asset.animation.bounds is a read-only tight CPU-reference vertex-extrema query in model-world space at one sampled pose. Only selected scene roots and descendants contribute; every stored vertex counts per node/primitive binding, including rigid accessories and shared instances. Full work preflight permits at most 10000 primitive instances and 1000000 vertex instances before deformation. Geometryless selected scenes return empty:true and bounds:null. These are not conservative animation/collision bounds or anatomical measurements. Legacy animation inspection, authored state and cooked packages are unchanged.");
    auto preview=methods["world.capture"];
    preview["properties"]["skinning"]={{"enum",{"gpu","cpu"}},{"default","gpu"}};
    preview["properties"]["asset"]=asset_id;preview["properties"]["clip"]=model_index;
    preview["properties"]["time"]=methods["asset.animation.sample"]["properties"]["time"];
    preview["properties"]["loop"]=methods["asset.animation.sample"]["properties"]["loop"];
    preview["required"].push_back("asset");preview["required"].push_back("time");methods["asset.animation.capture"]=preview;
}
}
