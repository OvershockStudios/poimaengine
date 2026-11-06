// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/gameplay_abi.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace poima { bool runtime_navigation_available() noexcept; }

namespace poima::gameplay_abi {
inline constexpr std::uint32_t call_version=1,call_bytes=80;
inline constexpr std::uint32_t services_version=7,services_bytes=176;
inline constexpr const char* baseline_feature="baseline_v7";
inline constexpr const char* persistence_feature="gameplay_persistence_v1";
inline constexpr const char* collections_feature="component_collections_v1";
inline constexpr const char* animation_feature="animation_inertial_v1";
inline constexpr std::uint32_t animation_services_bytes=192;
inline constexpr const char* animation_layers_feature="animation_layers_v1";
inline constexpr std::uint32_t animation_layers_services_bytes=208;
inline constexpr const char* character_input_feature="character_input_v1";
inline constexpr std::uint32_t character_services_bytes=216;
inline constexpr const char* navigation_feature="navigation_query_v1";
inline constexpr std::uint32_t navigation_services_bytes=224;
inline constexpr const char* instances_feature="hierarchical_instances_v1";
inline constexpr std::uint32_t instances_services_bytes=232;
inline constexpr const char* player_preferences_feature="player_preferences_v1";
inline constexpr std::uint32_t player_preferences_services_bytes=256;
inline constexpr const char* input_actions_feature="input_actions_v1";
inline constexpr std::uint32_t input_actions_services_bytes=264;
inline constexpr const char* hit_regions_feature="hit_regions_v1";
inline constexpr std::uint32_t hit_regions_services_bytes=272;
inline constexpr const char* character_stance_feature="character_stance_v1";
inline constexpr std::uint32_t character_stance_services_bytes=336;
inline constexpr const char* camera_lens_feature="camera_lens_v1";
inline constexpr std::uint32_t camera_lens_services_bytes=320;
inline constexpr const char* root_motion_feature="animation_root_motion_v1";
inline constexpr std::uint32_t root_motion_services_bytes=304;
inline constexpr const char* two_bone_ik_feature="animation_ik_v1";
inline constexpr std::uint32_t two_bone_ik_services_bytes=288;
inline constexpr const char* two_bone_ik_orientation_feature="animation_ik_end_orientation_v1";
inline constexpr std::uint32_t two_bone_ik_orientation_services_bytes=352;
inline constexpr const char* grounding_control_feature="animation_grounding_control_v1";
inline constexpr std::uint32_t grounding_control_services_bytes=368;
inline constexpr const char* animation_rate_feature="animation_rate_v1";
inline constexpr std::uint32_t animation_rate_services_bytes=376;

inline constexpr const char* camera_pose_feature="camera_pose_v1";
inline constexpr std::uint32_t camera_pose_services_bytes=392;
inline constexpr const char* capsule_cast_feature="capsule_cast_v1";
inline constexpr std::uint32_t capsule_cast_services_bytes=400;
inline constexpr const char* physical_rig_feature="physical_rig_v1";
inline constexpr std::uint32_t physical_rig_services_bytes=424;
inline constexpr const char* physical_rig_drive_feature="physical_rig_drive_v1";
inline constexpr std::uint32_t physical_rig_drive_services_bytes=448;
inline constexpr const char* physical_rig_animation_feature="physical_rig_animation_v1";
inline constexpr std::uint32_t physical_rig_animation_services_bytes=496;

// In a requirement, services_bytes is the minimum readable prefix. In an
// availability declaration it is the provided extent. Epochs describe callback
// semantics as well as layout; appending a tail does not change the epoch.
struct Contract {
    std::uint32_t call_version=gameplay_abi::call_version;
    std::uint32_t call_bytes=gameplay_abi::call_bytes;
    std::uint32_t services_version=gameplay_abi::services_version;
    std::uint32_t services_bytes=gameplay_abi::services_bytes;
    std::vector<std::string> features{baseline_feature};
};

inline Contract available_contract(bool navigation_bound=true) {
    Contract result;result.services_bytes=character_services_bytes;
    result.features.push_back(persistence_feature);result.features.push_back(collections_feature);result.features.push_back(animation_feature);result.features.push_back(animation_layers_feature);result.features.push_back(character_input_feature);
    if(navigation_bound && runtime_navigation_available())result.features.push_back(navigation_feature);
    result.services_bytes=instances_services_bytes;result.features.push_back(instances_feature);
    result.services_bytes=player_preferences_services_bytes;result.features.push_back(player_preferences_feature);
    result.services_bytes=input_actions_services_bytes;result.features.push_back(input_actions_feature);
    result.services_bytes=hit_regions_services_bytes;result.features.push_back(hit_regions_feature);
    result.services_bytes=two_bone_ik_services_bytes;result.features.push_back(two_bone_ik_feature);
    result.services_bytes=root_motion_services_bytes;result.features.push_back(root_motion_feature);
    result.services_bytes=camera_lens_services_bytes;result.features.push_back(camera_lens_feature);
    result.services_bytes=character_stance_services_bytes;result.features.push_back(character_stance_feature);
    result.services_bytes=two_bone_ik_orientation_services_bytes;result.features.push_back(two_bone_ik_orientation_feature);
    result.services_bytes=grounding_control_services_bytes;result.features.push_back(grounding_control_feature);
    result.services_bytes=animation_rate_services_bytes;result.features.push_back(animation_rate_feature);
    result.services_bytes=camera_pose_services_bytes;result.features.push_back(camera_pose_feature);
    result.services_bytes=capsule_cast_services_bytes;result.features.push_back(capsule_cast_feature);
    result.services_bytes=physical_rig_services_bytes;result.features.push_back(physical_rig_feature);
    result.services_bytes=physical_rig_drive_services_bytes;result.features.push_back(physical_rig_drive_feature);
    result.services_bytes=physical_rig_animation_services_bytes;result.features.push_back(physical_rig_animation_feature);
    return result;
}

// Existing compiled bridges/modules require exactly 176 bytes. Baseline
// consumers receive a bounded view even when a host
// appends opaque services. Never copy an unadvertised prefix or reinterpret a
// different semantic epoch. A module that requires a tail needs an
// explicitly negotiated view rather than silently changing this legacy view.
inline PoimaGameServices baseline_view(const PoimaGameServices& provided) {
    static_assert(sizeof(PoimaGameServices)>=services_bytes);
    if(provided.version!=services_version || provided.bytes<services_bytes)
        throw std::runtime_error("Gameplay requires services epoch 7 and at least 176 bytes.");
    PoimaGameServices view{};
    std::memcpy(&view,&provided,services_bytes);
    view.bytes=services_bytes;
    return view;
}

// One policy for artifact inspection, runtime selection and package validation.
// This does not validate executable integrity, target OS, saved state or build
// cohort identity; those remain separate checks at their existing boundaries.
inline std::string compatibility_error(const Contract& required,const Contract& available) {
    if(required.call_version!=call_version || available.call_version!=call_version ||
       required.call_bytes!=call_bytes || available.call_bytes!=call_bytes)
        return "Gameplay requires call ABI 1 with 80 bytes.";
    if(required.services_version!=services_version || available.services_version!=services_version)
        return "Gameplay service compatibility epoch must be 7.";
    if(required.services_bytes<services_bytes || available.services_bytes<services_bytes)
        return "Gameplay service table is shorter than the 176-byte baseline.";
    if(required.services_bytes>available.services_bytes)
        return "Runtime does not provide the required gameplay service prefix.";
    if(std::find(required.features.begin(),required.features.end(),baseline_feature)==required.features.end())return "Gameplay requirements must declare baseline_v7.";
    // A required prefix grants known operations only with their named feature.
    // Availability may grow, but an opaque extent is not an opt-in capability.
    const bool animation_required=std::find(required.features.begin(),required.features.end(),animation_feature)!=required.features.end();
    const bool layers_required=std::find(required.features.begin(),required.features.end(),animation_layers_feature)!=required.features.end();
    const bool character_required=std::find(required.features.begin(),required.features.end(),character_input_feature)!=required.features.end();
    const bool navigation_required=std::find(required.features.begin(),required.features.end(),navigation_feature)!=required.features.end();
    const bool instances_required=std::find(required.features.begin(),required.features.end(),instances_feature)!=required.features.end();
    const bool preferences_required=std::find(required.features.begin(),required.features.end(),player_preferences_feature)!=required.features.end();
    const bool actions_required=std::find(required.features.begin(),required.features.end(),input_actions_feature)!=required.features.end();
    const bool regions_required=std::find(required.features.begin(),required.features.end(),hit_regions_feature)!=required.features.end();
    const bool stance_required=std::find(required.features.begin(),required.features.end(),character_stance_feature)!=required.features.end();
    const bool lens_required=std::find(required.features.begin(),required.features.end(),camera_lens_feature)!=required.features.end();
    const bool root_required=std::find(required.features.begin(),required.features.end(),root_motion_feature)!=required.features.end();
    const bool ik_required=std::find(required.features.begin(),required.features.end(),two_bone_ik_feature)!=required.features.end();
    const bool ik_orientation_required=std::find(required.features.begin(),required.features.end(),two_bone_ik_orientation_feature)!=required.features.end();
    const bool grounding_required=std::find(required.features.begin(),required.features.end(),grounding_control_feature)!=required.features.end();
    const bool target_required=std::find(required.features.begin(),required.features.end(),physical_rig_animation_feature)!=required.features.end();
    const bool drive_required=std::find(required.features.begin(),required.features.end(),physical_rig_drive_feature)!=required.features.end();
    const bool physical_required=std::find(required.features.begin(),required.features.end(),physical_rig_feature)!=required.features.end();
    const bool capsule_required=std::find(required.features.begin(),required.features.end(),capsule_cast_feature)!=required.features.end();
    const bool pose_required=std::find(required.features.begin(),required.features.end(),camera_pose_feature)!=required.features.end();
    const bool rate_required=std::find(required.features.begin(),required.features.end(),animation_rate_feature)!=required.features.end();
    if(target_required && (!drive_required || !physical_required))return "Gameplay physical_rig_animation_v1 also requires physical_rig_drive_v1 and physical_rig_v1.";
    if(drive_required && !physical_required)return "Gameplay physical_rig_drive_v1 also requires physical_rig_v1.";
    if(rate_required && !animation_required)return "Gameplay animation_rate_v1 also requires animation_inertial_v1.";
    if(grounding_required && !ik_orientation_required)return "Gameplay animation_grounding_control_v1 also requires animation_ik_end_orientation_v1.";
    if(ik_orientation_required && !ik_required)return "Gameplay animation_ik_end_orientation_v1 also requires animation_ik_v1.";
    if(layers_required && !animation_required)return "Gameplay animation_layers_v1 also requires animation_inertial_v1.";
    if(required.services_bytes!=(target_required ? physical_rig_animation_services_bytes : drive_required ? physical_rig_drive_services_bytes : physical_required ? physical_rig_services_bytes : capsule_required ? capsule_cast_services_bytes : pose_required ? camera_pose_services_bytes : rate_required ? animation_rate_services_bytes : grounding_required ? grounding_control_services_bytes : ik_orientation_required ? two_bone_ik_orientation_services_bytes : stance_required ? character_stance_services_bytes : lens_required ? camera_lens_services_bytes : root_required ? root_motion_services_bytes : ik_required ? two_bone_ik_services_bytes : regions_required ? hit_regions_services_bytes : actions_required ? input_actions_services_bytes : preferences_required ? player_preferences_services_bytes : instances_required ? instances_services_bytes : navigation_required ? navigation_services_bytes : character_required ? character_services_bytes : layers_required ? animation_layers_services_bytes : animation_required ? animation_services_bytes : services_bytes))
        return "Gameplay must declare the 176-byte baseline or named 192/208/216/224/232/256/264/272/288/304/320/336/352/368/376/392/400/424/448/496-byte extensions.";
    if(std::find(available.features.begin(),available.features.end(),animation_feature)!=available.features.end() && available.services_bytes<animation_services_bytes)
        return "Runtime animation_inertial_v1 requires a 192-byte service allocation.";
    if(std::find(required.features.begin(),required.features.end(),animation_feature)!=required.features.end() && required.services_bytes<animation_services_bytes)
        return "Gameplay animation_inertial_v1 requires a 192-byte service prefix.";
    if(std::find(available.features.begin(),available.features.end(),animation_layers_feature)!=available.features.end()) {
        if(available.services_bytes<animation_layers_services_bytes)return "Runtime animation_layers_v1 requires a 208-byte service allocation.";
        if(std::find(available.features.begin(),available.features.end(),animation_feature)==available.features.end())return "Runtime animation_layers_v1 also requires animation_inertial_v1.";
    }
    if(std::find(available.features.begin(),available.features.end(),character_input_feature)!=available.features.end() && available.services_bytes<character_services_bytes)
        return "Runtime character_input_v1 requires a 216-byte service allocation.";
    if(std::find(available.features.begin(),available.features.end(),navigation_feature)!=available.features.end() && available.services_bytes<navigation_services_bytes)
        return "Runtime navigation_query_v1 requires a 224-byte service allocation.";
    if(std::find(available.features.begin(),available.features.end(),instances_feature)!=available.features.end() && available.services_bytes<instances_services_bytes)
        return "Runtime hierarchical_instances_v1 requires a 232-byte service allocation.";
    if(std::find(available.features.begin(),available.features.end(),player_preferences_feature)!=available.features.end() && available.services_bytes<player_preferences_services_bytes)
        return "Runtime player_preferences_v1 requires a 256-byte service allocation.";
    if(std::find(available.features.begin(),available.features.end(),input_actions_feature)!=available.features.end() && available.services_bytes<input_actions_services_bytes)
        return "Runtime input_actions_v1 requires a 264-byte service allocation.";
    if(std::find(available.features.begin(),available.features.end(),hit_regions_feature)!=available.features.end() && available.services_bytes<hit_regions_services_bytes)
        return "Runtime hit_regions_v1 requires a 272-byte service allocation.";
    if(std::find(available.features.begin(),available.features.end(),character_stance_feature)!=available.features.end() && available.services_bytes<character_stance_services_bytes)
        return "Runtime character_stance_v1 requires a 336-byte service allocation.";
    if(std::find(available.features.begin(),available.features.end(),camera_lens_feature)!=available.features.end() && available.services_bytes<camera_lens_services_bytes)
        return "Runtime camera_lens_v1 requires a 320-byte service allocation.";
    if(std::find(available.features.begin(),available.features.end(),root_motion_feature)!=available.features.end() && available.services_bytes<root_motion_services_bytes)
        return "Runtime animation_root_motion_v1 requires a 304-byte service allocation.";
    if(std::find(available.features.begin(),available.features.end(),two_bone_ik_feature)!=available.features.end() && available.services_bytes<two_bone_ik_services_bytes)
        return "Runtime animation_ik_v1 requires a 288-byte service allocation.";
    if(std::find(available.features.begin(),available.features.end(),two_bone_ik_orientation_feature)!=available.features.end()) {
        if(available.services_bytes<two_bone_ik_orientation_services_bytes)return "Runtime animation_ik_end_orientation_v1 requires a 352-byte service allocation.";
        if(std::find(available.features.begin(),available.features.end(),two_bone_ik_feature)==available.features.end())return "Runtime animation_ik_end_orientation_v1 also requires animation_ik_v1.";
    }
    if(std::find(available.features.begin(),available.features.end(),grounding_control_feature)!=available.features.end()) {
        if(available.services_bytes<grounding_control_services_bytes)return "Runtime animation_grounding_control_v1 requires a 368-byte service allocation.";
        if(std::find(available.features.begin(),available.features.end(),two_bone_ik_orientation_feature)==available.features.end())return "Runtime animation_grounding_control_v1 also requires animation_ik_end_orientation_v1.";
    }
    if(std::find(available.features.begin(),available.features.end(),animation_rate_feature)!=available.features.end()) {
        if(available.services_bytes<animation_rate_services_bytes)return "Runtime animation_rate_v1 requires a 376-byte service allocation.";
        if(std::find(available.features.begin(),available.features.end(),animation_feature)==available.features.end())return "Runtime animation_rate_v1 also requires animation_inertial_v1.";
    }
    if(std::find(available.features.begin(),available.features.end(),camera_pose_feature)!=available.features.end() && available.services_bytes<camera_pose_services_bytes)
        return "Runtime camera_pose_v1 requires a 392-byte service allocation.";
    if(std::find(available.features.begin(),available.features.end(),capsule_cast_feature)!=available.features.end() && available.services_bytes<capsule_cast_services_bytes)
        return "Runtime capsule_cast_v1 requires a 400-byte service allocation.";
    if(std::find(available.features.begin(),available.features.end(),physical_rig_feature)!=available.features.end() && available.services_bytes<physical_rig_services_bytes)
        return "Runtime physical_rig_v1 requires a 424-byte service allocation.";
    if(std::find(available.features.begin(),available.features.end(),physical_rig_drive_feature)!=available.features.end()) {
        if(available.services_bytes<physical_rig_drive_services_bytes)return "Runtime physical_rig_drive_v1 requires a 448-byte service allocation.";
        if(std::find(available.features.begin(),available.features.end(),physical_rig_feature)==available.features.end())return "Runtime physical_rig_drive_v1 also requires physical_rig_v1.";
    }
    if(std::find(available.features.begin(),available.features.end(),physical_rig_animation_feature)!=available.features.end()) {
        if(available.services_bytes<physical_rig_animation_services_bytes)return "Runtime physical_rig_animation_v1 requires a 496-byte service allocation.";
        if(std::find(available.features.begin(),available.features.end(),physical_rig_drive_feature)==available.features.end() ||
            std::find(available.features.begin(),available.features.end(),physical_rig_feature)==available.features.end())
            return "Runtime physical_rig_animation_v1 also requires physical_rig_drive_v1 and physical_rig_v1.";
    }
    for(std::size_t i=0;i<required.features.size();++i) {
        const auto& feature=required.features[i];
        if(feature!=baseline_feature && feature!=persistence_feature && feature!=collections_feature && feature!=animation_feature && feature!=animation_layers_feature && feature!=character_input_feature && feature!=navigation_feature && feature!=instances_feature && feature!=player_preferences_feature && feature!=input_actions_feature && feature!=hit_regions_feature && feature!=two_bone_ik_feature && feature!=root_motion_feature && feature!=camera_lens_feature && feature!=character_stance_feature && feature!=two_bone_ik_orientation_feature && feature!=grounding_control_feature && feature!=animation_rate_feature && feature!=camera_pose_feature && feature!=capsule_cast_feature && feature!=physical_rig_feature && feature!=physical_rig_drive_feature && feature!=physical_rig_animation_feature)return "Unknown required gameplay feature: "+feature;
        const auto prefix_end=required.features.begin()+static_cast<std::vector<std::string>::difference_type>(i);
        if(std::find(required.features.begin(),prefix_end,feature)!=prefix_end)
            return "Duplicate required gameplay feature: "+feature;
        if(std::find(available.features.begin(),available.features.end(),feature)==available.features.end())
            return "Runtime lacks required gameplay feature: "+feature;
    }
    return {};
}
// Prepare an already negotiated 448-byte view. Valid legacy behavior is
// unchanged: feature checks require only requested callbacks and retain the
// original pointers in the frozen prefix. No short allocation is reinterpreted.
inline PoimaGamePhysicalRigDriveServicesV1 physical_rig_drive_view(const Contract& required,const PoimaGameServices& provided) {
    const auto check=[](bool value,const char* message){if(!value)throw std::runtime_error(message);};
    check(provided.version==gameplay_abi::services_version && provided.bytes>=sizeof(PoimaGamePhysicalRigDriveServicesV1),
        "Gameplay requires services epoch 7 and the 448-byte physical rig drive extension.");
    PoimaGamePhysicalRigDriveServicesV1 extended{};
    std::memcpy(&extended,&provided,sizeof(extended));
    auto& physical=extended.physical;auto& capsule=physical.capsule;auto& pose=capsule.camera_pose;auto& rate=pose.animation_rate;auto& grounding=rate.grounding;auto& orientation=grounding.orientation;auto& stance=orientation.character_stance;auto& lens=stance.camera_lens;auto& root=lens.root_motion;auto& ik=root.ik;auto& regions=ik.regions;auto& preferences=regions.actions.preferences;auto& instances=preferences.instances;
    auto& navigation=instances.navigation;auto& character=navigation.character;
    auto& layers=character.animation;auto& animation=layers.animation;
    animation.baseline.bytes=sizeof(extended);
    check(physical.physical_rig_summary && physical.physical_rig_part && physical.physical_rig_command,"Gameplay physical rig callback is absent.");
    check(extended.physical_rig_summary_v2 && extended.physical_rig_drive_get && extended.physical_rig_drive_set,"Gameplay physical rig drive callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::capsule_cast_feature)!=required.features.end())
        check(capsule.capsule_cast,"Gameplay capsule cast callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::camera_pose_feature)!=required.features.end())
        check(pose.camera_pose_get && pose.camera_pose_set,"Gameplay camera pose callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::animation_rate_feature)!=required.features.end())
        check(rate.animation_rate_set,"Gameplay animation rate callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::grounding_control_feature)!=required.features.end())
        check(grounding.grounding_get && grounding.grounding_set,"Gameplay grounding control callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::two_bone_ik_orientation_feature)!=required.features.end())
        check(orientation.ik_orientation_get && orientation.ik_orientation_set,"Gameplay IK orientation extension callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::character_stance_feature)!=required.features.end())
        check(stance.character_stance_get && stance.character_stance_set,"Gameplay character stance extension callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::camera_lens_feature)!=required.features.end())
        check(lens.camera_lens_get && lens.camera_lens_set,"Gameplay camera lens extension callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::root_motion_feature)!=required.features.end())
        check(root.root_motion_get && root.root_motion_set,"Gameplay root motion extension callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::two_bone_ik_feature)!=required.features.end())
        check(ik.ik_get && ik.ik_set,"Gameplay two-bone IK extension callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::hit_regions_feature)!=required.features.end())
        check(regions.hit_test,"Gameplay hit region extension callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::input_actions_feature)!=required.features.end())
        check(regions.actions.input_action,"Gameplay input action extension callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::player_preferences_feature)!=required.features.end())
        check(preferences.preference_snapshot && preferences.preference_patch && preferences.preference_result,"Gameplay player preference extension callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::instances_feature)!=required.features.end())
        check(instances.instance_node,"Gameplay hierarchical instance resolver callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::navigation_feature)!=required.features.end())
        check(navigation.navigation_path,"Gameplay navigation extension callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::character_input_feature)!=required.features.end())
        check(character.character_input,"Gameplay character input extension callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::animation_feature)!=required.features.end())
        check(animation.animation_get_extended && animation.animation_set_extended,"Gameplay animation extension callback is absent.");
    if(std::find(required.features.begin(),required.features.end(),gameplay_abi::animation_layers_feature)!=required.features.end())
        check(layers.animation_layer_get && layers.animation_layer_set,"Gameplay animation layer extension callback is absent.");
    return extended;
}
inline PoimaGameServices& physical_rig_drive_baseline(PoimaGamePhysicalRigDriveServicesV1& value) {
    return value.physical.capsule.camera_pose.animation_rate.grounding.orientation.character_stance.camera_lens.root_motion.ik.regions.actions.preferences.instances.navigation.character.animation.animation.baseline;
}

// A real 496-byte negotiated view. New target-player rights do not grant any
// intermediate ordinary-animation, character, navigation, IK or other feature.
// Only this new view clears unrequested extension callbacks; old views retain
// their frozen prefix behavior.
inline PoimaGamePhysicalRigAnimationServicesV1 physical_rig_animation_view(const Contract& required,const PoimaGameServices& provided) {
    const auto check=[](bool value,const char* message){if(!value)throw std::runtime_error(message);};
    const auto has=[&](const char* feature){return std::find(required.features.begin(),required.features.end(),feature)!=required.features.end();};
    check(required.services_bytes==physical_rig_animation_services_bytes && has(physical_rig_animation_feature) &&
        has(physical_rig_drive_feature) && has(physical_rig_feature),"Physical animation view requires its explicit negotiated features and 496-byte prefix.");
    const auto invalid=compatibility_error(required,required);if(!invalid.empty())throw std::runtime_error(invalid);
    check(provided.version==services_version && provided.bytes>=sizeof(PoimaGamePhysicalRigAnimationServicesV1),
        "Gameplay requires services epoch 7 and the 496-byte physical animation extension.");
    PoimaGamePhysicalRigAnimationServicesV1 extended{};
    std::memcpy(&extended,&provided,sizeof(extended));
    check(extended.physical_rig_animation_get && extended.physical_rig_animation_layer_get &&
        extended.physical_rig_animation_set && extended.physical_rig_animation_rate_set &&
        extended.physical_rig_drive_source_get && extended.physical_rig_drive_source_set,
        "Gameplay physical animation callback is absent.");
    extended.drive=physical_rig_drive_view(required,provided);
    auto& physical=extended.drive.physical;auto& capsule=physical.capsule;auto& pose=capsule.camera_pose;
    auto& rate=pose.animation_rate;auto& grounding=rate.grounding;auto& orientation=grounding.orientation;
    auto& stance=orientation.character_stance;auto& lens=stance.camera_lens;auto& root=lens.root_motion;
    auto& ik=root.ik;auto& regions=ik.regions;auto& preferences=regions.actions.preferences;
    auto& instances=preferences.instances;auto& navigation=instances.navigation;auto& character=navigation.character;
    auto& layers=character.animation;auto& animation=layers.animation;
    if(!has(capsule_cast_feature))capsule.capsule_cast=nullptr;
    if(!has(camera_pose_feature)){pose.camera_pose_get=nullptr;pose.camera_pose_set=nullptr;}
    if(!has(animation_rate_feature))rate.animation_rate_set=nullptr;
    if(!has(grounding_control_feature)){grounding.grounding_get=nullptr;grounding.grounding_set=nullptr;}
    if(!has(two_bone_ik_orientation_feature)){orientation.ik_orientation_get=nullptr;orientation.ik_orientation_set=nullptr;}
    if(!has(character_stance_feature)){stance.character_stance_get=nullptr;stance.character_stance_set=nullptr;}
    if(!has(camera_lens_feature)){lens.camera_lens_get=nullptr;lens.camera_lens_set=nullptr;}
    if(!has(root_motion_feature)){root.root_motion_get=nullptr;root.root_motion_set=nullptr;}
    if(!has(two_bone_ik_feature)){ik.ik_get=nullptr;ik.ik_set=nullptr;}
    if(!has(hit_regions_feature))regions.hit_test=nullptr;
    if(!has(input_actions_feature))regions.actions.input_action=nullptr;
    if(!has(player_preferences_feature)){preferences.preference_snapshot=nullptr;preferences.preference_patch=nullptr;preferences.preference_result=nullptr;}
    if(!has(instances_feature))instances.instance_node=nullptr;
    if(!has(navigation_feature))navigation.navigation_path=nullptr;
    if(!has(character_input_feature))character.character_input=nullptr;
    if(!has(animation_feature)){animation.animation_get_extended=nullptr;animation.animation_set_extended=nullptr;}
    if(!has(animation_layers_feature)){layers.animation_layer_get=nullptr;layers.animation_layer_set=nullptr;}
    animation.baseline.bytes=sizeof(extended);return extended;
}
}
