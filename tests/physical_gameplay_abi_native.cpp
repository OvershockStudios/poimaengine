// SPDX-License-Identifier: Apache-2.0
// Header/policy qualification only. Actual callback staging and native physics
// require separate engine-hosted gameplay qualification.
#include "poima/gameplay_abi.h"
#include "poima/gameplay_compatibility.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <type_traits>
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
using poima::gameplay_abi::Contract;
using poima::gameplay_abi::compatibility_error;
Contract contract(std::uint32_t bytes,std::vector<std::string> features){Contract c;c.services_bytes=bytes;c.features=std::move(features);return c;}
void prefix_compatibility(){
    const auto physical=contract(424,{"baseline_v7","physical_rig_v1"});
    const auto old=contract(400,{"baseline_v7","capsule_cast_v1"});
    const auto all=contract(424,{"baseline_v7","capsule_cast_v1","physical_rig_v1"});
    check(compatibility_error(Contract{},physical).empty(),"Old176 requirement rejected by new424 allocation.");
    check(compatibility_error(old,all).empty(),"Old400 requirement rejected by new424 allocation.");
    check(compatibility_error(physical,physical).empty(),"Physical-only feature incorrectly requires capsule/camera/animation rights.");
    check(!compatibility_error(physical,old).empty(),"Old400 host granted physical extension.");
    auto future=all;future.services_bytes=480;future.features.push_back("future_unknown_v1");check(compatibility_error(physical,future).empty(),"Unknown available tail invalidated known424 consumer.");
    for(auto bytes:{176u,400u,408u,416u,423u}){auto short_host=physical;short_host.services_bytes=bytes;check(!compatibility_error(physical,short_host).empty(),"Short physical host accepted.");}
    auto extent_only=contract(424,{"baseline_v7"});check(!compatibility_error(physical,extent_only).empty(),"Extent alone granted physical feature.");
    check(!compatibility_error(extent_only,all).empty(),"Unmarked424 requirement accepted instead of canonical176.");
    auto malformed=physical;malformed.services_version=8;check(!compatibility_error(malformed,all).empty(),"Changed epoch accepted.");
    malformed=physical;malformed.features.push_back("future_unknown_v1");check(!compatibility_error(malformed,future).empty(),"Unknown requirement accepted.");
    malformed=physical;malformed.services_bytes=400;check(!compatibility_error(malformed,all).empty(),"Physical requirement lied about readable prefix.");
    auto physical_and_capsule=physical;physical_and_capsule.features.push_back("capsule_cast_v1");check(compatibility_error(physical_and_capsule,all).empty(),"Independent physical/capsule union rejected.");
}
void prefix_bytes(){
    PoimaGamePhysicalRigServicesV1 full{};auto* prefix=reinterpret_cast<PoimaGameServices*>(&full);prefix->version=7;prefix->bytes=424;prefix->context=reinterpret_cast<void*>(std::uintptr_t(0x1234));
    // Intentionally poison bytes outside the baseline. A legacy view copies only
    // its exact prefix and cannot expose new tail fields by accident.
    std::memset(reinterpret_cast<unsigned char*>(&full)+176,0xa5,sizeof(full)-176);
    auto legacy=poima::gameplay_abi::baseline_view(*prefix);check(legacy.bytes==176&&legacy.version==7&&legacy.context==prefix->context,"Legacy prefix values changed.");
    auto expected=*prefix;expected.bytes=176;check(std::memcmp(&legacy,&expected,176)==0,"Baseline view copied or changed wrong bytes.");
    prefix->bytes=175;bool rejected=false;try{(void)poima::gameplay_abi::baseline_view(*prefix);}catch(const std::runtime_error&){rejected=true;}check(rejected,"Short baseline accepted.");
}
void drive_compatibility(){
    const auto drive=contract(448,{"baseline_v7","physical_rig_v1","physical_rig_drive_v1"});
    const auto legacy=contract(424,{"baseline_v7","physical_rig_v1"});
    check(compatibility_error(drive,drive).empty(),"Independent drive extension rejected.");
    check(compatibility_error(legacy,drive).empty(),"Frozen424 consumer rejected by448 host.");
    check(!compatibility_error(drive,legacy).empty(),"Frozen424 host granted drive capability.");
    for(auto bytes:{176u,400u,424u,432u,440u,447u}){auto short_host=drive;short_host.services_bytes=bytes;check(!compatibility_error(drive,short_host).empty(),"Short drive host accepted.");}
    auto missing=drive;missing.features={"baseline_v7","physical_rig_drive_v1"};
    check(!compatibility_error(missing,drive).empty()&&!compatibility_error(Contract{},missing).empty(),"Drive dependency missing from requirement or host.");
    auto unmarked=legacy;unmarked.services_bytes=448;
    check(!compatibility_error(drive,unmarked).empty()&&!compatibility_error(unmarked,drive).empty(),"448 extent granted unnamed drive rights.");
    auto future=drive;future.services_bytes=480;future.features.push_back("future_unknown_v1");
    check(compatibility_error(drive,future).empty(),"Future same-epoch tail rejected448 consumer.");
    auto wrong=drive;wrong.services_bytes=456;check(!compatibility_error(wrong,future).empty(),"Noncanonical drive requirement accepted.");
    wrong=drive;wrong.features.push_back("physical_rig_drive_v1");check(!compatibility_error(wrong,drive).empty(),"Duplicate drive requirement accepted.");
}
Contract target_contract(){return contract(496,{"baseline_v7","physical_rig_v1","physical_rig_drive_v1","physical_rig_animation_v1"});}
void target_compatibility(){
    const auto target=target_contract(),drive=contract(448,{"baseline_v7","physical_rig_v1","physical_rig_drive_v1"});
    check(compatibility_error(target,target).empty(),"Target-only feature incorrectly requires ordinary animation/input rights.");
    check(compatibility_error(drive,target).empty()&&compatibility_error(Contract{},target).empty(),"New496 host rejected frozen448 or176 consumer.");
    check(!compatibility_error(target,drive).empty(),"Old448 host granted target animation.");
    for(auto bytes:{176u,424u,448u,456u,464u,472u,480u,488u,495u}){auto short_host=target;short_host.services_bytes=bytes;check(!compatibility_error(target,short_host).empty(),"Short target allocation accepted.");}
    for(const char* feature:{"physical_rig_v1","physical_rig_drive_v1","physical_rig_animation_v1"}){
        auto missing=target;missing.features.erase(std::remove(missing.features.begin(),missing.features.end(),feature),missing.features.end());
        check(!compatibility_error(missing,target).empty()&&!compatibility_error(target,missing).empty(),"Missing target requirement/dependency accepted.");
        if(std::strcmp(feature,"physical_rig_animation_v1")!=0)check(!compatibility_error(Contract{},missing).empty(),"Host advertised target feature without its physical dependency.");
    }
    auto future=target;future.services_bytes=520;future.features.push_back("future_unknown_v1");check(compatibility_error(target,future).empty(),"Larger same-epoch available target allocation rejected.");
    auto bad=target;bad.services_bytes=504;check(!compatibility_error(bad,future).empty(),"Noncanonical target required extent accepted.");
    bad=target;bad.services_version=8;check(!compatibility_error(bad,target).empty(),"Target extension changed the compatibility epoch.");
    bad=target;bad.features.push_back("physical_rig_animation_v1");check(!compatibility_error(bad,target).empty(),"Duplicate target feature accepted.");
    bad=target;bad.features.push_back("future_unknown_v1");check(!compatibility_error(bad,future).empty(),"Unknown target requirement accepted.");
}
// Real, correctly typed inert callback addresses. These test only negotiation
// and allocation boundaries; none stands in for gameplay or a native solver.
template<class Signature>struct Present;
template<class... Args>struct Present<int32_t (POIMA_CALL *)(Args...)>{static int32_t POIMA_CALL call(Args...){return 0;}};
PoimaGamePhysicalRigAnimationServicesV1 populated_target(){
    PoimaGamePhysicalRigAnimationServicesV1 value{};
    auto set=[](auto& pointer){using Pointer=std::remove_reference_t<decltype(pointer)>;pointer=&Present<Pointer>::call;};
    auto& drive=value.drive;auto& physical=drive.physical;auto& capsule=physical.capsule;auto& pose=capsule.camera_pose;
    auto& rate=pose.animation_rate;auto& grounding=rate.grounding;auto& orientation=grounding.orientation;
    auto& stance=orientation.character_stance;auto& lens=stance.camera_lens;auto& root=lens.root_motion;
    auto& ik=root.ik;auto& regions=ik.regions;auto& preferences=regions.actions.preferences;auto& instances=preferences.instances;
    auto& navigation=instances.navigation;auto& character=navigation.character;auto& layers=character.animation;auto& animation=layers.animation;
    animation.baseline.version=7;animation.baseline.bytes=496;animation.baseline.context=reinterpret_cast<void*>(std::uintptr_t(0x1234));
    set(animation.animation_get_extended);set(animation.animation_set_extended);set(layers.animation_layer_get);set(layers.animation_layer_set);
    set(character.character_input);set(navigation.navigation_path);set(instances.instance_node);
    set(preferences.preference_snapshot);set(preferences.preference_patch);set(preferences.preference_result);set(regions.actions.input_action);
    set(regions.hit_test);set(ik.ik_get);set(ik.ik_set);set(root.root_motion_get);set(root.root_motion_set);
    set(lens.camera_lens_get);set(lens.camera_lens_set);set(stance.character_stance_get);set(stance.character_stance_set);
    set(orientation.ik_orientation_get);set(orientation.ik_orientation_set);set(grounding.grounding_get);set(grounding.grounding_set);
    set(rate.animation_rate_set);set(pose.camera_pose_get);set(pose.camera_pose_set);set(capsule.capsule_cast);
    set(physical.physical_rig_summary);set(physical.physical_rig_part);set(physical.physical_rig_command);
    set(drive.physical_rig_summary_v2);set(drive.physical_rig_drive_get);set(drive.physical_rig_drive_set);
    set(value.physical_rig_animation_get);set(value.physical_rig_animation_layer_get);set(value.physical_rig_animation_set);
    set(value.physical_rig_animation_rate_set);set(value.physical_rig_drive_source_get);set(value.physical_rig_drive_source_set);return value;
}
template<class F>void rejected(F&& f,const char* message){bool caught=false;try{f();}catch(const std::runtime_error&){caught=true;}check(caught,message);}
void target_views(){
    namespace abi=poima::gameplay_abi;
    const auto target=target_contract();auto provided=populated_target();const auto original=provided;
    auto& prefix=abi::physical_rig_drive_baseline(provided.drive);
    auto minimal=abi::physical_rig_animation_view(target,prefix);
    check(abi::physical_rig_drive_baseline(minimal.drive).bytes==496,"Target view advertised a short embedded allocation.");
    const PoimaGamePhysicalRigAnimationServicesV1 zeros{};
    check(std::memcmp(&minimal,&provided,176)==0,"Target view changed the baseline prefix.");
    check(std::memcmp(reinterpret_cast<const unsigned char*>(&minimal)+176,reinterpret_cast<const unsigned char*>(&zeros)+176,224)==0,
        "Target marker granted an unrequested intermediate callback.");
    check(std::memcmp(reinterpret_cast<const unsigned char*>(&minimal)+400,reinterpret_cast<const unsigned char*>(&provided)+400,96)==0,
        "Target view changed its required physical/drive/target callbacks.");
    auto all=target;for(const char* feature:{"animation_inertial_v1","animation_layers_v1","character_input_v1","navigation_query_v1","hierarchical_instances_v1","player_preferences_v1","input_actions_v1","hit_regions_v1","animation_ik_v1","animation_root_motion_v1","camera_lens_v1","character_stance_v1","animation_ik_end_orientation_v1","animation_grounding_control_v1","animation_rate_v1","camera_pose_v1","capsule_cast_v1"})all.features.push_back(feature);
    auto full=abi::physical_rig_animation_view(all,prefix);check(std::memcmp(&full,&provided,496)==0,"Explicit union changed any full service-table byte.");
    const auto drive=contract(448,{"baseline_v7","physical_rig_v1","physical_rig_drive_v1"});auto old=abi::physical_rig_drive_view(drive,prefix);auto expected=provided.drive;abi::physical_rig_drive_baseline(expected).bytes=448;
    check(std::memcmp(&old,&expected,448)==0,"Frozen448 consumer prefix or pointer semantics changed.");
    struct Guarded {PoimaGamePhysicalRigDriveServicesV1 value;std::uint64_t canary[2];};
    Guarded short_table{provided.drive,{0xaabbccddeeff0011ULL,0x1122334455667788ULL}};abi::physical_rig_drive_baseline(short_table.value).bytes=448;
    const auto short_original=short_table;rejected([&]{(void)abi::physical_rig_animation_view(target,abi::physical_rig_drive_baseline(short_table.value));},"Actual short448 allocation granted a496 view.");
    check(std::memcmp(&short_table,&short_original,sizeof(short_table))==0,"Rejected short view changed allocation or canary.");
    PoimaGameServices only_baseline{};only_baseline.version=7;only_baseline.bytes=176;rejected([&]{(void)abi::physical_rig_animation_view(target,only_baseline);},"Actual176 allocation granted a496 view.");
    for(unsigned n=0;n<12;++n){auto missing=provided;switch(n){
        case 0:missing.physical_rig_animation_get=nullptr;break;case 1:missing.physical_rig_animation_layer_get=nullptr;break;
        case 2:missing.physical_rig_animation_set=nullptr;break;case 3:missing.physical_rig_animation_rate_set=nullptr;break;
        case 4:missing.physical_rig_drive_source_get=nullptr;break;case 5:missing.physical_rig_drive_source_set=nullptr;break;
        case 6:missing.drive.physical_rig_summary_v2=nullptr;break;case 7:missing.drive.physical_rig_drive_get=nullptr;break;
        case 8:missing.drive.physical_rig_drive_set=nullptr;break;case 9:missing.drive.physical.physical_rig_summary=nullptr;break;
        case 10:missing.drive.physical.physical_rig_part=nullptr;break;case 11:missing.drive.physical.physical_rig_command=nullptr;break;
    }const auto before=missing;rejected([&]{(void)abi::physical_rig_animation_view(target,abi::physical_rig_drive_baseline(missing.drive));},"Missing required physical animation callback accepted.");check(std::memcmp(&missing,&before,496)==0,"Failed callback negotiation changed provider allocation.");}
    auto missing_ordinary=provided;missing_ordinary.drive.physical.capsule.camera_pose.animation_rate.grounding.orientation.character_stance.camera_lens.root_motion.ik.regions.actions.preferences.instances.navigation.character.animation.animation.animation_get_extended=nullptr;
    (void)abi::physical_rig_animation_view(target,abi::physical_rig_drive_baseline(missing_ordinary.drive));
    rejected([&]{(void)abi::physical_rig_animation_view(all,abi::physical_rig_drive_baseline(missing_ordinary.drive));},"Requested ordinary feature bypassed callback validation.");
    rejected([&]{(void)abi::physical_rig_animation_view(drive,prefix);},"Unrequested target feature received its tail.");
    prefix.version=8;rejected([&]{(void)abi::physical_rig_animation_view(target,prefix);},"Wrong service epoch was reinterpreted.");prefix.version=7;
    struct Future {PoimaGamePhysicalRigAnimationServicesV1 value;std::uint64_t opaque[3];};Future future{provided,{11,22,33}};abi::physical_rig_drive_baseline(future.value.drive).bytes=sizeof(future);const auto future_before=future;
    auto bounded=abi::physical_rig_animation_view(target,abi::physical_rig_drive_baseline(future.value.drive));check(abi::physical_rig_drive_baseline(bounded.drive).bytes==496,"Opaque host tail escaped bounded target view.");check(std::memcmp(&future,&future_before,sizeof(future))==0&&std::memcmp(&provided,&original,496)==0,"View construction changed an original provider.");
}
}
static_assert(sizeof(PoimaGameServices)==176&&sizeof(PoimaGameCapsuleQueryServicesV1)==400&&sizeof(PoimaGamePhysicalRigServicesV1)==424);
static_assert(std::is_standard_layout_v<PoimaGamePhysicalRigSummaryV1>&&std::is_trivially_copyable_v<PoimaGamePhysicalRigSummaryV1>);
static_assert(std::is_standard_layout_v<PoimaGamePhysicalRigPartRequestV1>&&std::is_trivially_copyable_v<PoimaGamePhysicalRigPartRequestV1>);
static_assert(std::is_standard_layout_v<PoimaGamePhysicalRigPartStateV1>&&std::is_trivially_copyable_v<PoimaGamePhysicalRigPartStateV1>);
static_assert(std::is_standard_layout_v<PoimaGamePhysicalRigCommandV1>&&std::is_trivially_copyable_v<PoimaGamePhysicalRigCommandV1>);
static_assert(sizeof(PoimaGamePhysicalRigDriveServicesV1)==448&&sizeof(PoimaGamePhysicalRigDriveRequestV1)==40&&sizeof(PoimaGamePhysicalRigDriveCommandV1)==80&&sizeof(PoimaGamePhysicalRigDriveStateV1)==168);
static_assert(std::is_standard_layout_v<PoimaGamePhysicalRigDriveServicesV1>&&std::is_trivially_copyable_v<PoimaGamePhysicalRigDriveServicesV1>);
static_assert(sizeof(PoimaGamePhysicalRigAnimationServicesV1)==496&&sizeof(PoimaGamePhysicalRigDriveSourceStateV1)==208&&sizeof(PoimaGamePhysicalRigDriveSourceCommandV1)==56);
static_assert(std::is_standard_layout_v<PoimaGamePhysicalRigAnimationServicesV1>&&std::is_trivially_copyable_v<PoimaGamePhysicalRigAnimationServicesV1>);
static_assert(std::is_standard_layout_v<PoimaGamePhysicalRigDriveSourceStateV1>&&std::is_trivially_copyable_v<PoimaGamePhysicalRigDriveSourceStateV1>);
static_assert(std::is_standard_layout_v<PoimaGamePhysicalRigDriveSourceCommandV1>&&std::is_trivially_copyable_v<PoimaGamePhysicalRigDriveSourceCommandV1>);
int main(){try{prefix_compatibility();prefix_bytes();drive_compatibility();target_compatibility();target_views();std::cout<<"physical gameplay ABI:5 header/policy/view groups passed; no native gameplay claim\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
