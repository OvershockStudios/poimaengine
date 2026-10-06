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
    Host context;auto source=host(context);const auto copy=source;
    auto view=baseline_view(source.services);check(view.version==7 && view.bytes==176 && view.context==&context && view.control_request==source.services.control_request,"Baseline view lost prefix semantics.");
    auto expected=source.services;expected.bytes=176;check(std::memcmp(&view,&expected,176)==0,"Baseline view changed callback prefix.");
    source.opaque.fill(0x5a);auto second=baseline_view(source.services);check(std::memcmp(&view,&second,176)==0,"Opaque tail influenced baseline view.");
    check(copy.services.bytes==240 && source.services.bytes==240,"Adapter mutated host extent.");
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
