// SPDX-License-Identifier: Apache-2.0
#include "poima/gameplay.hpp"
#include "poima/gameplay_compatibility.hpp"
#include "poima/native_gameplay_artifact.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace poima;
namespace {
void check(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
template<class F>void rejects(F action) {bool rejected=false;try {action();}catch(const std::exception&) {rejected=true;}check(rejected,"Incompatible ABI unexpectedly accepted.");}
struct Extended {PoimaGameServices services{};std::array<unsigned char,64> opaque;};
static_assert(offsetof(Extended,opaque)==176);
struct Host {unsigned reads=0,events=0,requests=0;};
Extended host(Host& state) {
    Extended value;value.opaque.fill(0xa5);auto& s=value.services;s.version=7;s.bytes=sizeof(value);s.context=&state;
    auto unused=[](auto...)->int32_t {return -1;};
    s.entity=unused;s.raycast=unused;s.move=unused;s.sound=unused;s.animation_get=unused;s.animation_set=unused;
    s.save_info=unused;s.save_request=unused;s.save_result=unused;s.component_query=unused;s.component_get=unused;s.component_set=unused;
    s.entity_alive=unused;s.spawn=unused;s.despawn=unused;s.template_component_get=unused;s.ui_edit=unused;
    s.ui_get=[](void* context,const PoimaUiId* id,PoimaGameUiState* output,char* text,uint32_t capacity,uint32_t* written,PoimaGameError*)->int32_t {
        if(id->high || id->low!=2 || capacity<8)return -1;
        ++static_cast<Host*>(context)->reads;*output={};output->kind=1;output->visible=output->enabled=output->effective_visible=output->effective_enabled=1;
        output->text_bytes=*written=8;std::memcpy(text,"Original",8);return 0;
    };
    s.control_info=[](void* context,PoimaGameUiControlEvent* output,PoimaGameError*)->int32_t {
        ++static_cast<Host*>(context)->events;*output={};output->element.low=3;output->sequence=42;output->action_bytes=5;std::memcpy(output->action,"pause",5);return 0;
    };
    s.control_request=[](void* context,uint32_t intent,PoimaGameError*)->int32_t {if(intent!=2)return -1;++static_cast<Host*>(context)->requests;return 0;};
    return value;
}
void policy() {
    using namespace gameplay_abi;
    const Contract baseline;
    check(compatibility_error(baseline,baseline).empty(),"Baseline compatibility rejected.");
    auto extended=baseline;extended.services_bytes=240;
    check(compatibility_error(baseline,extended).empty(),"Appended host prefix rejected.");
    check(!compatibility_error(extended,baseline).empty(),"Missing required tail accepted.");
    for(bool requirement:{false,true}) {
        auto invalid=[&](Contract changed) {check(!(requirement?compatibility_error(changed,baseline):compatibility_error(baseline,changed)).empty(),"Invalid contract accepted.");};
        auto changed=baseline;changed.call_version=2;invalid(changed);
        changed=baseline;changed.call_bytes=88;invalid(changed);
        changed=baseline;changed.services_version=8;invalid(changed);
        changed=baseline;changed.services_bytes=175;invalid(changed);
        changed=baseline;changed.features.clear();invalid(changed);
    }
    auto changed=baseline;changed.features.push_back("unimplemented_tail");check(!compatibility_error(changed,extended).empty(),"Unknown required feature accepted.");
    changed=baseline;changed.features.push_back(baseline_feature);check(!compatibility_error(changed,baseline).empty(),"Duplicate feature accepted.");
    auto collections=baseline;collections.features.push_back(collections_feature);
    check(!compatibility_error(collections,baseline).empty(),"Collection game accepted by scalar-only runtime.");
    check(compatibility_error(collections,available_contract()).empty(),"Collection game rejected by capable runtime.");
    check(compatibility_error(baseline,available_contract()).empty(),"Collection capability broke old scalar game.");
    check(collections.services_version==7 && collections.services_bytes==176,"Collection feature changed baseline ABI.");
    const auto capable=available_contract();
    check(capable.services_version==7 && capable.services_bytes==304u &&
        std::find(capable.features.begin(),capable.features.end(),animation_feature)!=capable.features.end() &&
        std::find(capable.features.begin(),capable.features.end(),"animation_layers_v1")!=capable.features.end() &&
        std::find(capable.features.begin(),capable.features.end(),player_preferences_feature)!=capable.features.end() &&
        std::find(capable.features.begin(),capable.features.end(),input_actions_feature)!=capable.features.end() &&
        std::find(capable.features.begin(),capable.features.end(),hit_regions_feature)!=capable.features.end() &&
        std::find(capable.features.begin(),capable.features.end(),two_bone_ik_feature)!=capable.features.end(),"Runtime did not declare its actual named service extent.");
    auto animation=baseline;animation.services_bytes=192;animation.features.push_back(animation_feature);
    check(compatibility_error(animation,capable).empty(),"Negotiated animation extension rejected.");
    check(!compatibility_error(animation,baseline).empty(),"Animation extension accepted by baseline host.");
    auto missing_feature=capable;missing_feature.features.erase(std::remove(missing_feature.features.begin(),missing_feature.features.end(),animation_feature),missing_feature.features.end());
    check(!compatibility_error(animation,missing_feature).empty(),"A large service extent granted an undeclared animation feature.");
    auto short_animation=animation;short_animation.services_bytes=176;
    check(!compatibility_error(short_animation,capable).empty(),"Animation requirement omitted its required tail extent.");
    auto short_host=capable;short_host.services_bytes=176;
    check(!compatibility_error(baseline,short_host).empty(),"Runtime declared animation capability without allocating its tail.");
    auto duplicate_animation=animation;duplicate_animation.features.push_back(animation_feature);
    check(!compatibility_error(duplicate_animation,capable).empty(),"Duplicate animation requirement accepted.");
    auto layers=animation;layers.services_bytes=208;layers.features.push_back("animation_layers_v1");
    check(compatibility_error(layers,capable).empty(),"Named layered requirement rejected.");
    check(!compatibility_error(layers,animation).empty(),"Layered game accepted by inertial-only host.");
    for(unsigned bytes:{176u,192u,200u,207u,209u,240u}) {
        auto bad=layers;bad.services_bytes=bytes;
        check(!compatibility_error(bad,capable).empty(),"Layered requirement accepted wrong prefix extent.");
    }
    auto without_inertia=layers;without_inertia.features.erase(std::remove(without_inertia.features.begin(),without_inertia.features.end(),animation_feature),without_inertia.features.end());
    check(!compatibility_error(without_inertia,capable).empty(),"Layers granted without their inertial dependency.");
    auto without_layers=capable;without_layers.features.erase(std::remove(without_layers.features.begin(),without_layers.features.end(),"animation_layers_v1"),without_layers.features.end());
    check(!compatibility_error(layers,without_layers).empty(),"Opaque large extent granted an unnamed layer feature.");
    auto appended=capable;appended.services_bytes=304;appended.features.push_back("future_available_v1");
    check(compatibility_error(layers,appended).empty(),"Larger same-epoch host rejected a known layered prefix.");
    check(std::find(capable.features.begin(),capable.features.end(),character_input_feature)!=capable.features.end(),"Runtime omitted independent character input capability.");
    for(unsigned animation_kind:{0u,1u,2u}) {
        auto actor=baseline;actor.services_bytes=216;actor.features.push_back(character_input_feature);
        if(animation_kind)actor.features.push_back(animation_feature);
        if(animation_kind==2)actor.features.push_back(animation_layers_feature);
        check(compatibility_error(actor,capable).empty(),"Valid character/animation feature combination rejected.");
        auto character_only=baseline;character_only.services_bytes=216;character_only.features.push_back(character_input_feature);
        check(compatibility_error(actor,character_only).empty()==(animation_kind==0),"Character allocation granted undeclared animation features.");
        for(unsigned size:{176u,192u,208u,215u,217u,240u}) {
            auto wrong=actor;wrong.services_bytes=size;
            check(!compatibility_error(wrong,capable).empty(),"Character requirement accepted wrong prefix extent.");
        }
        auto no_character=capable;std::erase(no_character.features,std::string(character_input_feature));
        check(!compatibility_error(actor,no_character).empty(),"Opaque extent granted unnamed character input.");
        auto old_host=capable;old_host.services_bytes=208;std::erase(old_host.features,std::string(character_input_feature));
        check(!compatibility_error(actor,old_host).empty(),"Character game accepted by old layered host.");
    }
    auto invalid_character_host=capable;invalid_character_host.services_bytes=208;
    check(!compatibility_error(baseline,invalid_character_host).empty(),"Host declared character capability without allocated tail.");
    auto duplicate_character=baseline;duplicate_character.services_bytes=216;duplicate_character.features.push_back(character_input_feature);duplicate_character.features.push_back(character_input_feature);
    check(!compatibility_error(duplicate_character,capable).empty(),"Duplicate character feature accepted.");
    auto layers_character_without_inertia=baseline;layers_character_without_inertia.services_bytes=216;
    layers_character_without_inertia.features.push_back(character_input_feature);layers_character_without_inertia.features.push_back(animation_layers_feature);
    check(!compatibility_error(layers_character_without_inertia,capable).empty(),"Character extent bypassed layer dependency.");
    // Navigation is an independent named opt-in: an opaque larger allocation
    // never grants animation, character input, or a missing world binding.
    auto navigation=baseline;navigation.services_bytes=224;navigation.features.push_back(navigation_feature);
    auto synthetic=capable;synthetic.services_bytes=224;std::erase(synthetic.features,std::string(instances_feature));
    std::erase(synthetic.features,std::string(player_preferences_feature));
    std::erase(synthetic.features,std::string(input_actions_feature));
    std::erase(synthetic.features,std::string(hit_regions_feature));
    std::erase(synthetic.features,std::string(two_bone_ik_feature));
    std::erase(synthetic.features,std::string(root_motion_feature));
    if(std::find(synthetic.features.begin(),synthetic.features.end(),navigation_feature)==synthetic.features.end())synthetic.features.push_back(navigation_feature);
    check(compatibility_error(navigation,synthetic).empty(),"Independent navigation profile rejected.");
    check(!compatibility_error(navigation,available_contract(false)).empty(),"Unbound runtime grants navigation.");
    check((std::find(capable.features.begin(),capable.features.end(),navigation_feature)!=capable.features.end())==runtime_navigation_available(),"Navigation advertisement differs from actual runtime backend.");
    for(unsigned features=0;features<8;++features) {
        auto required=navigation;
        if(features&1)required.features.push_back(character_input_feature);
        if(features&2)required.features.push_back(animation_feature);
        if(features&4)required.features.push_back(animation_layers_feature);
        const bool valid=!(features&4) || bool(features&2);
        check(compatibility_error(required,synthetic).empty()==valid,"Navigation profile bypassed named intermediate permissions.");
        for(unsigned bytes:{176u,192u,208u,216u,223u,225u,256u}) {
            auto bad=required;bad.services_bytes=bytes;check(!compatibility_error(bad,synthetic).empty(),"Navigation requirement accepted wrong extent.");
        }
    }
    auto opaque=synthetic;std::erase(opaque.features,std::string(navigation_feature));
    check(!compatibility_error(navigation,opaque).empty(),"Opaque extent granted unnamed navigation.");
    auto short_navigation=synthetic;short_navigation.services_bytes=216;
    check(!compatibility_error(baseline,short_navigation).empty(),"Navigation capability advertised without allocation.");
    auto duplicate_navigation=navigation;duplicate_navigation.features.push_back(navigation_feature);
    check(!compatibility_error(duplicate_navigation,synthetic).empty(),"Duplicate navigation feature accepted.");
    auto larger=synthetic;larger.services_bytes=256;larger.features.push_back("future_available_v1");
    check(compatibility_error(navigation,larger).empty(),"Future available extent broke current navigation requirement.");
    auto navigation_only=baseline;navigation_only.services_bytes=224;navigation_only.features.push_back(navigation_feature);
    check(compatibility_error(navigation,navigation_only).empty(),"Navigation requires unrelated optional services.");
    auto navigation_character=navigation;navigation_character.features.push_back(character_input_feature);
    check(!compatibility_error(navigation_character,navigation_only).empty(),"Navigation allocation granted undeclared character input.");
    auto instances=baseline;instances.services_bytes=232;instances.features.push_back(instances_feature);
    check(compatibility_error(instances,capable).empty(),"Named instance extension rejected.");
    auto instances_only=instances;
    check(compatibility_error(instances,instances_only).empty(),"Instance resolver requires unrelated optional services.");
    for(unsigned combination=0;combination<16;++combination) {
        auto required=instances;
        if(combination&1)required.features.push_back(character_input_feature);
        if(combination&2)required.features.push_back(animation_feature);
        if(combination&4)required.features.push_back(animation_layers_feature);
        if(combination&8)required.features.push_back(navigation_feature);
        auto all=capable;
        if(std::find(all.features.begin(),all.features.end(),navigation_feature)==all.features.end())all.features.push_back(navigation_feature);
        const bool valid=!(combination&4) || bool(combination&2);
        check(compatibility_error(required,all).empty()==valid,"Instance prefix bypassed an independent feature dependency.");
        for(unsigned size:{176u,192u,208u,216u,224u,231u,233u,256u}) {
            auto bad=required;bad.services_bytes=size;
            check(!compatibility_error(bad,all).empty(),"Instance requirement accepted wrong prefix extent.");
        }
    }
    auto no_instances=capable;std::erase(no_instances.features,std::string(instances_feature));
    check(!compatibility_error(instances,no_instances).empty(),"Opaque extent grants unnamed instance resolver.");
    auto short_instances=instances_only;short_instances.services_bytes=224;
    check(!compatibility_error(baseline,short_instances).empty(),"Instance capability advertised without allocation.");
    auto duplicate_instances=instances;duplicate_instances.features.push_back(instances_feature);
    check(!compatibility_error(duplicate_instances,capable).empty(),"Duplicate instance requirement accepted.");
    auto future_instances=capable;future_instances.services_bytes=304;future_instances.features.push_back("future_available_v1");
    check(compatibility_error(instances,future_instances).empty(),"Larger same-epoch host rejects instance prefix.");
    auto preferences=baseline;preferences.services_bytes=256;preferences.features.push_back(player_preferences_feature);
    check(compatibility_error(preferences,capable).empty() && compatibility_error(preferences,preferences).empty(),
        "Independent preference extension rejected or requires unrelated services.");
    auto no_preferences=capable;std::erase(no_preferences.features,std::string(player_preferences_feature));
    check(!compatibility_error(preferences,no_preferences).empty(),"Opaque extent granted unnamed preferences.");
    auto short_preferences=preferences;short_preferences.services_bytes=232;
    check(!compatibility_error(preferences,short_preferences).empty() && !compatibility_error(baseline,short_preferences).empty(),
        "Preference callbacks advertised/read beyond allocated prefix.");
    auto duplicate_preferences=preferences;duplicate_preferences.features.push_back(player_preferences_feature);
    check(!compatibility_error(duplicate_preferences,capable).empty(),"Duplicate preference feature accepted.");
    auto actions=baseline;actions.services_bytes=264;actions.features.push_back(input_actions_feature);
    check(compatibility_error(actions,capable).empty() && compatibility_error(actions,actions).empty(),
        "Input action observations require an unrelated service feature.");
    auto unnamed_actions=capable;std::erase(unnamed_actions.features,std::string(input_actions_feature));
    check(!compatibility_error(actions,unnamed_actions).empty(),"Opaque extent granted undeclared input actions.");
    auto short_actions=actions;short_actions.services_bytes=256;
    check(!compatibility_error(actions,short_actions).empty() && !compatibility_error(baseline,short_actions).empty(),
        "Input actions advertised/read beyond the allocated prefix.");
    for(unsigned combination=0;combination<64;++combination) {
        auto required=actions,all=capable;
        if(combination&1)required.features.push_back(animation_feature);
        if(combination&2)required.features.push_back(animation_layers_feature);
        if(combination&4)required.features.push_back(character_input_feature);
        if(combination&8)required.features.push_back(navigation_feature);
        if(combination&16)required.features.push_back(instances_feature);
        if(combination&32)required.features.push_back(player_preferences_feature);
        if(std::find(all.features.begin(),all.features.end(),navigation_feature)==all.features.end())all.features.push_back(navigation_feature);
        check(compatibility_error(required,all).empty()==(!(combination&2) || bool(combination&1)),
            "Action extent bypassed an independently negotiated feature dependency.");
        for(unsigned bytes:{176u,192u,208u,216u,224u,232u,256u,263u,265u,272u}) {
            auto wrong=required;wrong.services_bytes=bytes;
            check(!compatibility_error(wrong,all).empty(),"Input action requirement accepted a wrong extent.");
        }
    }
    auto duplicate_actions=actions;duplicate_actions.features.push_back(input_actions_feature);
    check(!compatibility_error(duplicate_actions,capable).empty(),"Duplicate action feature accepted.");
    auto regions=baseline;regions.services_bytes=272;regions.features.push_back(hit_regions_feature);
    check(compatibility_error(regions,regions).empty() && compatibility_error(regions,capable).empty(),"Hit regions require an unrelated feature.");
    auto unnamed_regions=capable;std::erase(unnamed_regions.features,std::string(hit_regions_feature));
    check(!compatibility_error(regions,unnamed_regions).empty(),"Opaque extent granted hit regions.");
    auto short_regions=regions;short_regions.services_bytes=264;
    check(!compatibility_error(baseline,short_regions).empty(),"Hit region capability advertised past allocation.");
    auto duplicate_regions=regions;duplicate_regions.features.push_back(hit_regions_feature);
    check(!compatibility_error(duplicate_regions,capable).empty(),"Duplicate hit regions accepted.");
    for(unsigned combination=0;combination<128;++combination) {
        auto required=regions,all=capable;
        const char* optional[]{animation_feature,animation_layers_feature,character_input_feature,navigation_feature,instances_feature,player_preferences_feature,input_actions_feature};
        for(unsigned i=0;i<7;++i)if(combination&(1u<<i))required.features.push_back(optional[i]);
        if(std::find(all.features.begin(),all.features.end(),navigation_feature)==all.features.end())all.features.push_back(navigation_feature);
        check(compatibility_error(required,all).empty()==(!(combination&2) || bool(combination&1)),"Region prefix bypassed independent feature dependency.");
        for(unsigned bytes:{176u,192u,208u,216u,224u,232u,256u,264u,271u,273u}) {
            auto wrong=required;wrong.services_bytes=bytes;
            check(!compatibility_error(wrong,all).empty(),"Hit regions accepted a wrong prefix.");
        }
    }
    auto ik=baseline;ik.services_bytes=288;ik.features.push_back(two_bone_ik_feature);
    check(compatibility_error(ik,ik).empty() && compatibility_error(ik,capable).empty(),"IK requires unrelated features.");
    auto unnamed_ik=capable;std::erase(unnamed_ik.features,std::string(two_bone_ik_feature));
    check(!compatibility_error(ik,unnamed_ik).empty(),"Opaque extent granted IK.");
    auto short_ik=ik;short_ik.services_bytes=272;
    check(!compatibility_error(baseline,short_ik).empty(),"IK advertised past allocation.");
    auto duplicate_ik=ik;duplicate_ik.features.push_back(two_bone_ik_feature);
    check(!compatibility_error(duplicate_ik,capable).empty(),"Duplicate IK feature accepted.");
    for(unsigned combination=0;combination<256;++combination) {
        auto required=ik,all=capable;
        const char* optional[]{animation_feature,animation_layers_feature,character_input_feature,navigation_feature,instances_feature,player_preferences_feature,input_actions_feature,hit_regions_feature};
        for(unsigned i=0;i<8;++i)if(combination&(1u<<i))required.features.push_back(optional[i]);
        if(std::find(all.features.begin(),all.features.end(),navigation_feature)==all.features.end())all.features.push_back(navigation_feature);
        check(compatibility_error(required,all).empty()==(!(combination&2) || bool(combination&1)),"IK prefix bypassed independent feature dependency.");
        for(unsigned bytes:{176u,192u,208u,216u,224u,232u,256u,264u,272u,280u,287u,289u}) {
            auto wrong=required;wrong.services_bytes=bytes;
            check(!compatibility_error(wrong,all).empty(),"IK accepted wrong prefix.");
        }
    }
    auto root=baseline;root.services_bytes=304;root.features.push_back(root_motion_feature);
    check(compatibility_error(root,root).empty() && compatibility_error(root,capable).empty(),"Root motion requires unrelated features.");
    auto unnamed_root=capable;std::erase(unnamed_root.features,std::string(root_motion_feature));
    check(!compatibility_error(root,unnamed_root).empty(),"Opaque extent granted root motion.");
    auto short_root=root;short_root.services_bytes=288;
    check(!compatibility_error(baseline,short_root).empty(),"Root motion advertised beyond allocation.");
    auto duplicate_root=root;duplicate_root.features.push_back(root_motion_feature);
    check(!compatibility_error(duplicate_root,capable).empty(),"Duplicate root motion accepted.");
    for(unsigned combination=0;combination<512;++combination) {
        auto required=root,all=capable;
        const char* optional[]{animation_feature,animation_layers_feature,character_input_feature,navigation_feature,instances_feature,player_preferences_feature,input_actions_feature,hit_regions_feature,two_bone_ik_feature};
        for(unsigned i=0;i<9;++i)if(combination&(1u<<i))required.features.push_back(optional[i]);
        if(std::find(all.features.begin(),all.features.end(),navigation_feature)==all.features.end())all.features.push_back(navigation_feature);
        check(compatibility_error(required,all).empty()==(!(combination&2) || bool(combination&1)),"Root prefix bypassed independent dependency.");
        for(unsigned bytes:{176u,192u,208u,216u,224u,232u,256u,264u,272u,288u,296u,303u,305u}) {
            auto wrong=required;wrong.services_bytes=bytes;
            check(!compatibility_error(wrong,all).empty(),"Root motion accepted wrong prefix.");
        }
    }
    Host context;auto source=host(context);const auto copy=source;
    auto view=baseline_view(source.services);check(view.version==7 && view.bytes==176 && view.context==&context && view.control_request==source.services.control_request,"Baseline view lost prefix semantics.");
    auto expected=source.services;expected.bytes=176;check(std::memcmp(&view,&expected,176)==0,"Baseline view changed callback prefix.");
    source.opaque.fill(0x5a);auto second=baseline_view(source.services);check(std::memcmp(&view,&second,176)==0,"Opaque tail influenced baseline view.");
    check(copy.services.bytes==240 && source.services.bytes==240,"Adapter mutated host extent.");
    PoimaGameAnimationServicesV1 typed{};typed.baseline=source.services;typed.baseline.bytes=sizeof(typed);
    typed.animation_get_extended=[](void*,const PoimaEntityId*,PoimaGameAnimationStateV1*,PoimaGameError*)->int32_t{return -1;};
    typed.animation_set_extended=[](void*,const PoimaGameAnimationCommandV1*,PoimaGameError*)->int32_t{return -1;};
    const auto typed_before=typed;const auto legacy_from_typed=baseline_view(typed.baseline);
    check(std::memcmp(&legacy_from_typed,&view,176)==0,"Actual typed extension changed the legacy callback prefix.");
    check(std::memcmp(&typed,&typed_before,sizeof(typed))==0,"Legacy view mutated the typed extension.");
    typed.animation_get_extended=nullptr;typed.animation_set_extended=nullptr;
    const auto legacy_without_tail=baseline_view(typed.baseline);
    check(std::memcmp(&legacy_without_tail,&legacy_from_typed,176)==0,"Baseline view interpreted extension callback slots.");
    source.services.bytes=175;rejects([&]{baseline_view(source.services);});source.services.bytes=240;source.services.version=8;rejects([&]{baseline_view(source.services);});
}
void compiled(const GameplayConfig& config) {
    Gameplay game(config);Host context;auto source=host(context);const auto before=game.state();
    // Validation must happen before entering either old compiled callback.
    for(auto [version,bytes]:{std::pair{7u,175u},std::pair{8u,240u}}) {
        source.services.version=version;source.services.bytes=bytes;
        rejects([&]{game.tick(source.services,{},11);});rejects([&]{game.control(source.services,11);});
        check(game.state()==before && context.events==0 && context.reads==0 && context.requests==0,"Rejected ABI entered game or mutated state.");
    }
    source.services.version=7;source.services.bytes=sizeof(source);
    game.tick(source.services,{},11);game.control(source.services,11);
    const auto values=nlohmann::json::parse(game.inspect()).at("values");
    check(values.at("Ticks")==1 && values.at("ControlCalls")==1 && values.at("LastAction")==4 && values.at("SeenTick")=="11" && values.at("SeenSequence")=="42","Old compiled Tick/Control did not observe baseline service semantics.");
    check(context.reads==1 && context.events==1 && context.requests==1,"Old compiled service callbacks not invoked exactly once.");
    check(source.services.bytes==sizeof(source) && std::all_of(source.opaque.begin(),source.opaque.end(),[](auto byte){return byte==0xa5;}),"Adapter/game wrote opaque host tail.");
}
}
int main(int argc,char** argv) {try {
    policy();const bool native=argc==3 && std::string(argv[1])=="--native";
    check(argc==1 || argc==4 || native,"Usage: gameplay-compatibility-test [HOSTFXR BRIDGE ASSEMBLY | --native DESCRIPTOR]");
    if(argc!=1) {
        GameplayConfig config;
        if(native) {const auto artifact=load_native_gameplay_artifact(argv[2]);config.native_aot=true;config.native_library=artifact.library;config.native_sha256=artifact.library_sha256;config.native_schema=artifact.schema;config.type=artifact.type;}
        else {config.hostfxr=argv[1];config.bridge=argv[2];config.assembly=argv[3];config.type="Poima.Tests.ManagedUiGame";}
        check(config.type=="Poima.Tests.ManagedUiGame","Wrong preserved compatibility fixture.");compiled(config);
    }
    std::cout<<"Compatibility policy and bounded legacy view passed"<<(argc==1?" (policy only)":native?" with compiled NativeAOT Tick/Control":" with compiled CoreCLR Tick/Control")<<".\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
