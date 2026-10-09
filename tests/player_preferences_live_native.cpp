// SPDX-License-Identifier: Apache-2.0
#include "poima/player_preferences.hpp"
#include "player_settings_store.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using Preferences=poima::PlayerPreferences;
using Json=nlohmann::json;
void check(bool valid,const char* message) {if(!valid)throw std::runtime_error(message);}
void rejects(Preferences& owner,const std::string& patch,int code=-32602) {
    const auto before=owner.snapshot();const auto report=owner.inspect();bool denied=false;
    try {(void)owner.prepare(patch);}catch(const Preferences::Error& error) {denied=error.code==code;}
    check(denied,"Preference patch failed to reject with its documented code.");
    check(owner.snapshot()==before && owner.inspect()==report,"Rejected preparation changed published state.");
}
void isolation() {
    Preferences owner({});
    const auto initial=owner.snapshot();
    const auto candidate=owner.prepare(R"({"expected_revision":0,"set":{"input.sensitivity_x":0.75,"audio.master_gain":0.25,"camera.vertical_fov":95,"ui.scale":1.5}})");
    check(owner.snapshot()==initial && candidate->revision==1,"Preparation prematurely published candidate.");
    check(candidate->values.sensitivity_x==.75 && candidate->values.master_gain==.25 &&
        candidate->values.vertical_fov==95 && candidate->values.ui_scale==1.5f,"Live typed candidate has wrong values.");
    owner.publish(candidate);
    check(owner.snapshot()==candidate && initial->revision==0 && initial->values.master_gain==1,"Publication mutated retained snapshot.");
    const auto no_change=owner.prepare(R"({"expected_revision":1})");
    check(no_change->revision==2 && no_change->values.master_gain==.25,"Accepted empty patch lost overrides or revision increment.");
    owner.publish(no_change);
    rejects(owner,R"({"expected_revision":1,"set":{"audio.master_gain":0.9}})",-32009);
}
void resets_and_sources() {
    Preferences::Values inherited;inherited.sensitivity_x=.3;inherited.sensitivity_y=.4;inherited.invert_y=true;
    std::string values=R"({"camera.vertical_fov":88,"ui.scale":2,"input.sensitivity_x":0.8,"audio.master_gain":0.2})";
    std::string sources=R"({"audio.master_gain":"profile","camera.vertical_fov":"profile"})";
    std::string profile=R"({"source":"profile","revision":7,"content_hash":"original"})";
    Preferences owner(inherited,values,sources,profile);
    values.clear();sources.clear();profile.clear();
    auto report=Json::parse(owner.inspect());
    check(report.at("profile").at("revision")==7 && report.at("fields").at("audio.master_gain").at("source")=="profile",
        "Initial metadata not owned or source lost.");
    const auto candidate=owner.prepare(R"({"expected_revision":0,"reset":["camera.vertical_fov","ui.scale","input.sensitivity_x","audio.master_gain"]})");
    check(!candidate->values.vertical_fov && !candidate->values.ui_scale && candidate->values.master_gain==1 &&
        candidate->values.sensitivity_x==.3 && candidate->values.sensitivity_y==.4 && candidate->values.invert_y,
        "Reset failed to restore original inherited inputs/camera/density/master.");
    check(Json::parse(candidate->requested_json).empty() && Json::parse(candidate->sources_json).empty(),"Reset retained sparse override.");
    owner.publish(candidate);report=Json::parse(owner.inspect());
    check(report.at("fields").at("camera.vertical_fov").at("effective").is_null() &&
        report.at("fields").at("ui.scale").at("source")=="window_density" &&
        report.at("fields").at("audio.master_gain").at("source")=="engine_default", "Inherited null/source semantics differ.");
    owner.publish(owner.prepare(R"({"expected_revision":1,"set":{"input.invert_x":true}})"));
    check(Json::parse(owner.snapshot()->sources_json).at("input.invert_x")=="live_override","Live patch source is missing.");
    // Native embedders may supply a different inherited sink baseline; sparse
    // session overrides do not overwrite that owner's reset policy.
    Preferences::Values custom;custom.master_gain=.5;
    Preferences custom_owner(custom);
    custom_owner.publish(custom_owner.prepare(R"({"expected_revision":0})"));
    check(custom_owner.snapshot()->values.master_gain==.5,"Empty patch changed custom inherited master baseline.");
    custom_owner.publish(custom_owner.prepare(R"({"expected_revision":1,"set":{"audio.master_gain":0.8}})"));
    custom_owner.publish(custom_owner.prepare(R"({"expected_revision":2,"reset":["audio.master_gain"]})"));
    check(custom_owner.snapshot()->values.master_gain==.5,"Reset changed custom inherited master baseline.");
}
void deferred_graphics() {
    Preferences owner({},R"({"graphics.samples":1,"graphics.frames_in_flight":1})");
    check(owner.snapshot()->values.samples==1 && owner.snapshot()->values.frames_in_flight==1,"Initial graphics overrides were not applied.");
    auto change=owner.prepare(R"({"expected_revision":0,"set":{"graphics.samples":4,"graphics.frames_in_flight":2,"audio.master_gain":0}})");
    check(change->values.samples==1 && change->values.frames_in_flight==1 && change->values.master_gain==0,
        "Live graphics reconfigured or master mute failed.");
    auto rows=Json::parse(change->fields_json);
    check(rows.at("graphics.samples").at("requested")==4 && rows.at("graphics.samples").at("effective")==1 &&
        rows.at("graphics.samples").at("next_effective")==4 &&
        rows.at("graphics.frames_in_flight").at("next_effective")==2 &&
        rows.at("graphics.samples").at("requires_next_player")==true &&
        rows.at("graphics.samples").at("application")=="next_player" &&
        rows.at("audio.master_gain").at("application")=="live","Graphics deferral metadata differs.");
    owner.publish(change);
    auto reset=owner.prepare(R"({"expected_revision":1,"reset":["graphics.samples","graphics.frames_in_flight"]})");
    rows=Json::parse(reset->fields_json);
    check(rows.at("graphics.samples").at("requested").is_null() && rows.at("graphics.samples").at("requires_next_player")==true &&
        rows.at("graphics.samples").at("effective")==1 && rows.at("graphics.samples").at("next_effective")==4 &&
        rows.at("graphics.frames_in_flight").at("requested").is_null() &&
        rows.at("graphics.frames_in_flight").at("effective")==1 && rows.at("graphics.frames_in_flight").at("next_effective")==2 &&
        reset->values.samples==1,"Reset lost frozen launch graphics or pending default.");
    owner.publish(reset);
    owner.publish(owner.prepare(R"({"expected_revision":2,"set":{"graphics.samples":1}})"));
    check(Json::parse(owner.snapshot()->fields_json).at("graphics.samples").at("requires_next_player")==false,
        "Intent equal to live graphics reported pending reconfiguration.");
    Preferences next({},change->requested_json);
    check(next.snapshot()->values.samples==4 && next.snapshot()->values.frames_in_flight==2,"Next player did not apply requested graphics.");
}
void invalid_patches() {
    Preferences owner({});
    for(const auto* patch:{
        R"([])",R"({})",R"({"expected_revision":0,"request_id":"ignored"})",
        R"({"expected_revision":0.0})",R"({"expected_revision":true})",R"({"expected_revision":-1})",
        R"({"expected_revision":9007199254740992})",R"({"expected_revision":0,"set":[]})",
        R"({"expected_revision":0,"set":{"audio.master_gain":"0.5"}})",
        R"({"expected_revision":0,"set":{"audio.master_gain":-0.01}})",
        R"({"expected_revision":0,"set":{"audio.master_gain":1.01}})",
        R"({"expected_revision":0,"set":{"input.invert_x":1}})",
        R"({"expected_revision":0,"set":{"graphics.samples":4.0}})",
        R"({"expected_revision":0,"set":{"graphics.frames_in_flight":3}})",
        R"({"expected_revision":0,"set":{"camera.vertical_fov":151}})",
        R"({"expected_revision":0,"set":{"input.sensitivity_x":11}})",
        R"({"expected_revision":0,"set":{"ui.scale":0}})",
        R"({"expected_revision":0,"set":{"unknown":1}})",
        R"({"expected_revision":0,"reset":"audio.master_gain"})",
        R"({"expected_revision":0,"reset":[null]})",
        R"({"expected_revision":0,"reset":["unknown"]})",
        R"({"expected_revision":0,"reset":["audio.master_gain","audio.master_gain"]})",
        R"({"expected_revision":0,"reset":["audio.master_gain"],"set":{"audio.master_gain":1}})",
        R"({"expected_revision":0,"set":{"audio.master_gain":0.5,"audio.master_gain":0.6}})",
        R"({"expected_revision":0,"expected_revision":0})",R"({"expected_revision":0,"set":{"audio.master_gain":1e999}})"})rejects(owner,patch);
    rejects(owner,R"({"expected_revision":9007199254740991})",-32009);
    rejects(owner,std::string(65537,' '));
    rejects(owner,R"({"expected_revision":0,"reset":["camera.vertical_fov","input.sensitivity_x","input.sensitivity_y","input.invert_x","input.invert_y","ui.scale","graphics.samples","graphics.frames_in_flight","audio.master_gain","unknown"]})");
    std::string deep=R"({"expected_revision":0,"set":)";for(int i=0;i<18;++i)deep+='[';deep+='0';for(int i=0;i<18;++i)deep+=']';deep+='}';rejects(owner,deep);
    // Revision exhaustion is a trusted owner test seam, not a wire bypass.
    auto limit=std::make_shared<Preferences::State>(*owner.snapshot());limit->revision=9007199254740991ULL;owner.publish(limit);
    rejects(owner,R"({"expected_revision":9007199254740991})");
}
void initial_validation() {
    unsigned denied=0;
    const auto reject=[&](auto&& call) {try {call();}catch(const Preferences::Error& error) {if(error.code==-32602)++denied;}};
    reject([] {Preferences::Values bad;bad.sensitivity_y=std::numeric_limits<double>::quiet_NaN();Preferences owner(bad);});
    reject([] {Preferences owner({},R"({"audio.master_gain":2})");});
    reject([] {Preferences owner({},"{}",R"({"audio.master_gain":"profile"})");});
    reject([] {Preferences owner({},R"({"audio.master_gain":0.2})",R"({"audio.master_gain":3})");});
    reject([] {Preferences owner({},"{}","{}","[]");});
    check(denied==5,"Invalid constructor values/metadata accepted.");
}
void registry() {
    const auto schema=poima::player_settings::values_schema();
    check(schema.at("maxProperties")==9 && schema.at("properties").size()==9 &&
        schema.at("properties").at("audio.master_gain").at("maximum")==1,"Ninth registry field/schema missing.");
    const auto descriptor=poima::player_settings::describe();bool found=false;
    for(const auto& row:descriptor.at("settings"))if(row.at("id")=="audio.master_gain") {
        found=row.at("default")==1 && row.at("application")=="next_player" && row.at("live_application")=="output_sink_only";
    }
    check(found,"Stored intent/master sink metadata is incorrect.");
    Preferences owner({});
    const auto candidate=owner.prepare(R"({"expected_revision":0,"set":{"camera.vertical_fov":5,"input.sensitivity_x":0,"input.sensitivity_y":10,"input.invert_x":true,"input.invert_y":false,"ui.scale":8,"graphics.samples":1,"graphics.frames_in_flight":1,"audio.master_gain":0}})");
    check(Json::parse(candidate->requested_json).size()==9,"Valid full registry patch rejected or dropped a field.");
    owner.publish(candidate);
    const auto reset=owner.prepare(R"({"expected_revision":1,"reset":["camera.vertical_fov","input.sensitivity_x","input.sensitivity_y","input.invert_x","input.invert_y","ui.scale","graphics.samples","graphics.frames_in_flight","audio.master_gain"]})");
    check(Json::parse(reset->requested_json).empty() && reset->values.master_gain==1 && reset->values.sensitivity_y==.1,
        "Registry-bound nine-ID reset failed.");
}
}
int main() {
    try {
        isolation();resets_and_sources();deferred_graphics();invalid_patches();initial_validation();registry();
        std::cout<<"Live preferences passed six groups: immutable preparation/publication, inheritance/reset, frozen graphics, strict rejected patches, initial validation and nine-field registry. No device/runtime/storage application claim.\n";
        return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
