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
}
static_assert(sizeof(PoimaGameServices)==176&&sizeof(PoimaGameCapsuleQueryServicesV1)==400&&sizeof(PoimaGamePhysicalRigServicesV1)==424);
static_assert(std::is_standard_layout_v<PoimaGamePhysicalRigSummaryV1>&&std::is_trivially_copyable_v<PoimaGamePhysicalRigSummaryV1>);
static_assert(std::is_standard_layout_v<PoimaGamePhysicalRigPartRequestV1>&&std::is_trivially_copyable_v<PoimaGamePhysicalRigPartRequestV1>);
static_assert(std::is_standard_layout_v<PoimaGamePhysicalRigPartStateV1>&&std::is_trivially_copyable_v<PoimaGamePhysicalRigPartStateV1>);
static_assert(std::is_standard_layout_v<PoimaGamePhysicalRigCommandV1>&&std::is_trivially_copyable_v<PoimaGamePhysicalRigCommandV1>);
static_assert(sizeof(PoimaGamePhysicalRigDriveServicesV1)==448&&sizeof(PoimaGamePhysicalRigDriveRequestV1)==40&&sizeof(PoimaGamePhysicalRigDriveCommandV1)==80&&sizeof(PoimaGamePhysicalRigDriveStateV1)==168);
static_assert(std::is_standard_layout_v<PoimaGamePhysicalRigDriveServicesV1>&&std::is_trivially_copyable_v<PoimaGamePhysicalRigDriveServicesV1>);
int main(){try{prefix_compatibility();prefix_bytes();drive_compatibility();std::cout<<"physical gameplay ABI:3 header/policy groups passed; no native gameplay claim\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
