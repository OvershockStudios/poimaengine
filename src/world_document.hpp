// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/runtime.hpp"
#include "audio_buses.hpp"
#include "world_json.hpp"

#include <map>
#include <string>
#include <vector>

namespace poima::world_detail {

// Authored data decoding shared by document validation and scene construction.
void validate_name(const Json& value);
void validate_transform(const Json& value);
Light light_value(const Json& value);
Json sky_json(const SkySettings& sky);
LightingEnvironment environment_value(const Json& value);
AcousticMaterial acoustic_value(const Json& value);
AudioEmitter emitter_value(const Json& value);
audio_buses::Catalog authored_audio_buses(const Json& document);

AnimationCommand animation_value(const Json& value);
std::vector<RuntimeAnimationLayer> animation_layers_value(const Json& values);
RuntimeIkCommand ik_control_value(const Json& value);
std::vector<RuntimeTwoBoneIk> animation_ik_value(const Json& values);
RuntimeCharacterLocomotionProfile character_locomotion_value(const Json& value);
RuntimeCharacterStanceProfile character_stance_value(const Json& value, double radius, double height);
RuntimeRootMotionDefinition root_motion_value(const Json& value);
RuntimeHitRegion hit_region_value(const Json& value);
SceneLens scene_lens_value(const Json& value);
ViewmodelProjection viewmodel_projection_value(const Json& camera);
CameraDrawBinding camera_draw_value(const Json& value);
ViewmodelClearanceProfile viewmodel_clearance_value(const Json& value);
physical_rig::Configuration physical_rig_value(const Json& value);
double surface_mark_scalar(const Json& value, double low, double high);
SceneSurfaceMark surface_mark_style(const Json& row);
std::string surface_mark_profile_id(const Json& value);
std::uint32_t surface_mark_lifetime(const Json& value);
RuntimeFootprintSurface footprint_surface_value(const Json& value);
wetness::Profile surface_wetness_value(const Json& value);

void validate_component(const std::string& type, const Json& value);
std::map<std::string, Matrix4> world_matrices(const Json& entities);

// Authored snapshot membership and version handling. The world owner still
// assembles candidate state, records history, and publishes validated bytes.
bool custom_component(const std::string& type);
std::string custom_type(const std::string& type);
std::vector<components::Schema> authored_component_schemas(const Json& doc);
const components::Schema& authored_schema(const std::vector<components::Schema>& schemas, const std::string& id);
Json history_state(const Json& doc);
bool optional_authored_value_differs(const Json& a, const Json& b, const char* key);
bool same_authored_state(const Json& a, const Json& b);
void upgrade_components(Json& doc);
void upgrade_templates(Json& doc);
void upgrade_ui(Json& doc);
Json template_document(const Json& document, const Json& entities);
void validate(const Json& doc);
void validate_physics_checkpoint_profile(const Json& profile);

} // namespace poima::world_detail
