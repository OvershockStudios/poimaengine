// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime_animation.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

using namespace poima;
namespace {
using Json=nlohmann::json;
std::filesystem::path output;
unsigned checks=0,archives=0;
const std::string content(64,'a');
void check(bool value,const char* message) { ++checks;if(!value)throw std::runtime_error(message); }
void near(double actual,double expected,double epsilon=2e-7) {
    check(std::isfinite(actual)&&std::abs(actual-expected)<=epsilon,"Independent animation-rate equation differs.");
}
template<class F> void rejects(F&& f) {
    bool failed=false;try {f();}catch(const std::exception&) {failed=true;}
    check(failed,"Malformed animation-rate operation succeeded.");
}
std::string id(unsigned n) {const auto s=std::to_string(n);return std::string(32-s.size(),'0')+s;}
void archive(const std::string& name,const std::string& bytes) {
    ++archives;if(output.empty())return;
    const auto path=output/(std::to_string(archives)+"-"+name+".json");
    std::ofstream stream(path,std::ios::binary);stream.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    check(bool(stream),"Could not retain complete animation-rate archive.");
}
void same(const std::string& a,const std::string& b,const char* message) {
    archive("left",a);archive("right",b);check(a==b,message);
}
void same_future(const std::string& a,const std::string& b,unsigned tick,const char* message) {
    // Compare EVERY boundary, retaining complete representative operands rather
    // than duplicating hundreds of nearby JSON files in the optional output.
    if(tick%15==0||a!=b)same(a,b,message);else check(a==b,message);
}
bool equal_pose(const RuntimeTransform& a,const RuntimeTransform& b) {
    return a.position==b.position&&a.scale==b.scale&&a.rotation==b.rotation;
}
std::shared_ptr<ModelAsset> model(bool root=false) {
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(3);m->roots={0};
    for(unsigned n=0;n<3;++n) {m->nodes[n].name="node"+std::to_string(n);m->nodes[n].parent=n?int(n)-1:-1;m->nodes[n].position={0,n?1.:0.,0};}
    const unsigned node=root?0:2;const float y=root?0.f:1.f;
    m->animations.push_back({"Source",8,{{node,AnimationPath::translation,AnimationInterpolation::linear,{0,8},{{0,y,0,0},{16,y,0,0}}}}});
    m->animations.push_back({"Destination",8,{
        {node,AnimationPath::translation,AnimationInterpolation::linear,{0,8},{{10,y,0,0},{42,y,0,0}}},
        {node,AnimationPath::scale,AnimationInterpolation::linear,{0,8},{{3,3,3,0},{3,3,3,0}}}}});
    m->animations.push_back({"Constant",8,{{node,AnimationPath::translation,AnimationInterpolation::linear,{0,8},{{20,y,0,0},{20,y,0,0}}}}});
    m->animations.push_back({"Short",2,{{node,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,y,0,0},{4,y,0,0}}}}});
    m->animations.push_back({"Still",0,{{node,AnimationPath::translation,AnimationInterpolation::linear,{0},{{0,y,0,0}}}}});return m;
}
RuntimeDefinition definition(bool layered=false,bool root=false,unsigned rigs=1) {
    RuntimeDefinition d;d.world_id="animation-rate-native";d.authored_revision=7;const auto m=model(root);
    for(unsigned k=0;k<rigs;++k) {
        const unsigned first=1+k*10;RuntimeEntityDefinition r;r.id=id(first);r.animation_rig=RuntimeAnimationRig{m};
        r.animation_rig->clip=0;r.animation_rig->playing=true;r.animation_rig->loop=false;
        if(root)r.animation_rig->root_motion=RuntimeRootMotionDefinition{0,RootMotionMode::Extract,"",true,false};
        if(layered)for(unsigned slot=1;slot<=3;++slot) {
            RuntimeAnimationLayer l;l.slot=slot;l.clip=slot==1?1:2;l.time=slot==1?.25:0;l.playing=slot==1;l.loop=false;
            l.weight=slot==1?.2:slot==2?.25:0;l.mode=slot==2?AnimationLayerMode::Additive:AnimationLayerMode::Override;
            l.mask={{2,slot==1?.5:slot==2?.4:1}};r.animation_rig->layers.push_back(l);
        }
        d.entities.push_back(r);
        for(unsigned n=0;n<3;++n) {RuntimeEntityDefinition e;e.id=id(first+n+1);e.parent=n?id(first+n):r.id;
            e.transform={m->nodes[n].position,m->nodes[n].rotation,m->nodes[n].scale};e.rig_node=RuntimeRigNode{r.id,n};d.entities.push_back(e);}
    }return d;
}
AnimationCommand full(unsigned clip=1,std::uint32_t duration=120,AnimationTransitionMode mode=AnimationTransitionMode::Crossfade) {
    AnimationCommand c{id(1),clip,.25,1,false,true};c.blend_ticks=duration;c.transition_mode=mode;return c;
}
AnimationRateCommand rate(double speed,std::optional<std::uint32_t> layer={}) {return {id(1),speed,layer};}
RuntimeTransform pose(RuntimeAnimations& a,std::uint64_t tick) {
    for(const auto& p:a.sample(tick))if(p.entity==id(4))return p.local;
    throw std::runtime_error("Rate fixture tip is absent.");
}
void warm(RuntimeAnimations& a,std::uint64_t from,std::uint64_t to) {for(auto t=from;t<=to;++t)(void)a.sample(t);}
void boundary(RuntimeAnimations& a,std::uint64_t tick,double speed) {
    const auto before=pose(a,tick);const auto old=a.checkpoint();a.apply({},tick,{rate(speed)});
    check(equal_pose(before,pose(a,tick)),"Retiming changed the complete boundary pose.");
    const auto next=a.checkpoint();check(next[0].anchor_tick==old[0].anchor_tick,"Retiming replaced destination anchor.");
    check(next[0].control.time==old[0].control.time&&next[0].control.speed==old[0].control.speed,"Retiming rewrote immutable command.");
    if(old[0].transition)check(next[0].transition&&next[0].transition->start_tick==old[0].transition->start_tick&&
        next[0].transition->duration_ticks==old[0].transition->duration_ticks&&next[0].transition->inertial==old[0].transition->inertial&&
        next[0].transition->frozen_source==old[0].transition->frozen_source,"Retiming rebuilt transition/correction.");
}
void crossfade_phase_and_completion() {
    auto d=definition();RuntimeAnimations a(d);warm(a,0,30);a.apply({full()},30);warm(a,30,60);
    near(pose(a,60).position[0],4.75);boundary(a,60,2);near(a.state(id(1),60)->time,.75);
    warm(a,61,75);near(pose(a,75).position[0],7.1875);boundary(a,75,0);
    warm(a,76,90);near(pose(a,90).position[0],9);check(a.state(id(1),90)->playing,"Zero rate changed Playing.");boundary(a,90,.5);
    const auto saved=a.save_state(90);RuntimeAnimations b(d);b.load_state(saved,90);same(saved,b.save_state(90),"Active retimed crossfade cold bytes differ.");
    for(unsigned t=91;t<=150;++t) {pose(a,t);pose(b,t);same_future(a.save_state(t),b.save_state(t),t,"Crossfade future archive differs.");}
    near(pose(a,150).position[0],17);near(pose(a,150).scale[0],3);
    check(!a.state(id(1),150)->transition,"Retiming extended original fade deadline.");
}
double f(double u) {return 1-10*u*u*u+15*std::pow(u,4)-6*std::pow(u,5);}
double g(double u) {return u-6*std::pow(u,3)+8*std::pow(u,4)-3*std::pow(u,5);}
double integral_f(double u) {return u-2.5*std::pow(u,4)+3*std::pow(u,5)-std::pow(u,6);}
void inertial_piecewise_phase_and_history() {
    auto d=definition();RuntimeAnimations a(d);warm(a,0,30);a.apply({full(1,120,AnimationTransitionMode::Inertial)},30);
    const auto corrections=a.checkpoint()[0].transition->inertial;
    auto oracle=[&](unsigned t,double phase) {const double u=double(t-30)/120;const auto p=pose(a,t);
        near(p.position[0],10+4*phase-10*f(u)-4*g(u),3e-6);near(p.scale[0],3*std::exp(-std::log(3.)*f(u)),3e-6);};
    warm(a,30,60);oracle(60,.75);boundary(a,60,2);warm(a,61,75);oracle(75,1.25);boundary(a,75,0);
    warm(a,76,90);oracle(90,1.25);boundary(a,90,.5);check(a.checkpoint()[0].transition->inertial==corrections,"Retiming discarded original quintic corrections.");
    const auto saved=a.save_state(90);RuntimeAnimations b(d);b.load_state(saved,90);same(saved,b.save_state(90),"Inertial cold state changed.");
    for(unsigned t=91;t<=150;++t) {pose(a,t);pose(b,t);same_future(a.save_state(t),b.save_state(t),t,"Retimed quintic future differs.");}oracle(150,1.75);
    // v9 profile 1 must retain output velocity history before the FIRST inertial command.
    RuntimeAnimations live(d);warm(live,0,30);live.apply({},30,{rate(2)});warm(live,31,40);
    const auto plain=live.save_state(40);check(Json::parse(plain).at("legacy_version")==1,"Rate-only clock lost legacy profile1.");
    RuntimeAnimations cold(d);cold.load_state(plain,40);same(plain,cold.save_state(40),"Rate-only history was lost on cold load.");
    auto replace=full(1,60,AnimationTransitionMode::Inertial);live.apply({replace},40);cold.apply({replace},40);
    same(live.save_state(40),cold.save_state(40),"First post-load inertial command lost pre-save velocity history.");
    for(unsigned t=40;t<=100;++t) {pose(live,t);pose(cold,t);same_future(live.save_state(t),cold.save_state(t),t,"First inertial cold continuation differs.");}
}
void clock_edges_and_atomic_admission() {
    auto d=definition();RuntimeAnimations a(d);warm(a,0,10);const auto original=a.save_state(10);
    a.apply({},10,{rate(1)});same(original,a.save_state(10),"Same-rate command was not a byte-exact no-op.");check(Json::parse(original).at("version")==1,"Legacy baseline unexpectedly upgraded.");
    for(double bad:{-1.,8.0001,std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        rejects([&]{a.apply({},10,{rate(bad)});});same(original,a.save_state(10),"Invalid rate partially committed.");
    }
    auto absent=rate(2);absent.entity=id(999);rejects([&]{a.apply({},10,{rate(2),absent});});same(original,a.save_state(10),"Late invalid target committed earlier rate.");
    rejects([&]{a.apply({},10,{rate(2),rate(1)});});rejects([&]{a.apply({full()},10,{rate(1)});});
    for(unsigned layer:{0u,1u,5u})rejects([&]{a.apply({},10,{rate(2,layer)});});
    auto c=full(3,0);c.time=1.9;c.loop=true;a.apply({c},10);a.sample(10);a.apply({},10,{rate(2)});near(a.state(id(1),16)->time,.1);
    a.apply({},16,{rate(.5)});near(a.checkpoint()[0].rate->time,2.1);near(a.state(id(1),28)->time,.2);
    const auto segmented=a.save_state(16);a.apply({},16,{rate(.5)});same(segmented,a.save_state(16),"Repeated current rate reanchored an existing segment.");
    c=full(0,0);c.time=.7;c.playing=false;a.apply({c},20);a.apply({},20,{rate(8)});near(a.state(id(1),80)->time,.7);check(!a.state(id(1),80)->playing,"Rate resumed paused clock.");
    c.time=7.99;c.playing=true;a.apply({c},80);a.sample(81);a.apply({},81,{rate(8)});near(a.state(id(1),100)->time,8);check(!a.state(id(1),100)->playing,"Rate restarted ended nonloop clip.");
    c=full(4,0);a.apply({c},100);a.apply({},100,{rate(0)});near(a.state(id(1),120)->time,0);check(!a.state(id(1),120)->playing,"Zero-duration clip became playing.");
    c.clip.reset();a.apply({c},120);a.apply({},120,{rate(8)});near(a.state(id(1),130)->time,0);check(!a.state(id(1),130)->playing,"Rest became playing.");
    // The legal authored seek bound is NOT a bound on a later unwrapped loop
    // segment: even one ordinary Tick after a 1e9 seek must remain retimeable.
    RuntimeAnimations large(d);auto seek=full(3,0);seek.loop=true;seek.time=1e9;large.apply({seek},0);warm(large,0,1);
    const auto high_pose=pose(large,1);const auto high_state=large.state(id(1),1);large.apply({},1,{rate(2)});
    check(equal_pose(high_pose,pose(large,1)),"Large legal loop retime changed boundary pose.");near(large.state(id(1),1)->time,high_state->time,0);
    check(large.checkpoint()[0].rate->time>1e9,"Large loop rate discarded unwrapped phase.");
    const auto high_bytes=large.save_state(1);RuntimeAnimations high_cold(d);high_cold.load_state(high_bytes,1);same(high_bytes,high_cold.save_state(1),"Large loop rate cannot cold roundtrip.");
    auto authored_overflow=seek;authored_overflow.time=1e9+1;rejects([&]{large.apply({authored_overflow},1);});same(high_bytes,large.save_state(1),"Internal phase range weakened authored seek bound.");
    auto segment_overflow=Json::parse(high_bytes);segment_overflow["rigs"][0]["rate"]["time"]=1e20;archive("overflow-rate",segment_overflow.dump());
    rejects([&]{large.load_state(segment_overflow.dump(),1);});same(high_bytes,large.save_state(1),"Unbounded internal rate phase was accepted.");
    for(unsigned t=2;t<=5;++t){pose(large,t);pose(high_cold,t);same(large.save_state(t),high_cold.save_state(t),"Large loop cold continuation differs.");}
}
void masked_layers_and_target_conflicts() {
    auto d=definition(true);RuntimeAnimations a(d);warm(a,0,30);auto c=full();c.layer=1;c.time=.75;c.weight=.8;c.weight_blend_ticks=120;a.apply({c},30);warm(a,30,60);
    const auto old=a.checkpoint();const auto p=pose(a,60);a.apply({},60,{rate(.5),rate(2,1)});check(equal_pose(p,pose(a,60)),"Layer/base retime changed composed boundary pose.");
    const auto next=a.checkpoint();check(next.layers[0][0].weight_transition->start_tick==old.layers[0][0].weight_transition->start_tick,"Retiming restarted layer weight ramp.");
    near(a.layer_state(id(1),2,60)->time,0);warm(a,61,75);
    const double weight=(.2+.6*.375)*.5;near(pose(a,75).position[0],(1-weight)*2.25+weight*16.375+2,3e-6);
    const auto saved=a.save_state(75);RuntimeAnimations b(d);b.load_state(saved,75);same(saved,b.save_state(75),"Layered rate cold bytes differ.");
    for(unsigned t=76;t<=150;++t){pose(a,t);pose(b,t);same_future(a.save_state(t),b.save_state(t),t,"Layer/mask/weight-ramp cold continuation differs.");}
    RuntimeAnimations many(definition(true,false,17));many.sample(0);std::vector<AnimationRateCommand> rates;
    for(unsigned r=0;r<16;++r)for(unsigned slot=0;slot<4;++slot)rates.push_back({id(1+r*10),2,slot?std::optional<std::uint32_t>{slot}:std::nullopt});
    many.apply({},0,rates);const auto accepted=many.save_state(0);rates.push_back({id(161),2,{}});
    rejects([&]{many.apply({},0,rates);});same(accepted,many.save_state(0),"65th valid rate target escaped atomic quota.");
    rates.pop_back();auto extra=full(0,0);extra.entity=id(161);rejects([&]{many.apply({extra},0,rates);});same(accepted,many.save_state(0),"Combined full/rate writes did not share 64 limit.");
    rates.pop_back();many.apply({extra},0,rates);check(many.state(id(161),0)->clip==0,"63 rates plus one full command was rejected.");
}
void commit(RuntimeAnimations& a,unsigned tick) {const auto prepared=a.prepare_root_motion(tick);a.commit_root_motion(prepared,tick+1);a.sample(tick+1);}
void root_interval_and_outgoing_replacement() {
    auto d=definition(false,true);RuntimeAnimations a(d);a.sample(0);for(unsigned t=0;t<30;++t)commit(a,t);a.apply({full()},30);for(unsigned t=30;t<60;++t)commit(a,t);
    const auto old=a.root_motion_state(id(1));a.apply({},60,{rate(2)});check(a.root_motion_state(id(1))->local_delta==old->local_delta,"Retiming consumed root interval.");
    const double w=30.5/120;near(a.prepare_root_motion(60)[0].local_delta[0],((1-w)*2+w*8)/60);commit(a,60);
    a.apply({},61,{rate(0)});near(a.prepare_root_motion(61)[0].local_delta[0],(1-31.5/120)*2/60);
    for(unsigned t=61;t<150;++t)commit(a,t);a.apply({},150,{rate(2)});commit(a,150);
    auto next=full(0,60);next.time=0;a.apply({next},151);
    near(a.prepare_root_motion(151)[0].local_delta[0],((1-.5/60)*8+(.5/60)*2)/60);
    const auto live=a.save_state(151);check(!Json::parse(live).at("rigs")[0].at("root_motion").at("fade").at("source").at("rate").is_null(),"Replacement lost retimed outgoing root source.");
    RuntimeAnimations cold(d);cold.load_state(live,151);same(live,cold.save_state(151),"Retimed root source cold bytes differ.");
    for(unsigned t=151;t<211;++t){check(a.prepare_root_motion(t)[0].local_delta==cold.prepare_root_motion(t)[0].local_delta,"Root future interval differs.");commit(a,t);commit(cold,t);same_future(a.save_state(t+1),cold.save_state(t+1),t+1,"Root cold complete future differs.");}
    RuntimeAnimations inertial(d);inertial.sample(0);for(unsigned t=0;t<30;++t)commit(inertial,t);inertial.apply({full(1,120,AnimationTransitionMode::Inertial)},30);for(unsigned t=30;t<60;++t)commit(inertial,t);
    inertial.apply({},60,{rate(2)});const double u=30./120,v=31./120,mean=(integral_f(v)-integral_f(u))/(v-u);
    near(inertial.prepare_root_motion(60)[0].local_delta[0],(8-2*mean)/60,3e-6);
}
void nested_save_versions_and_strict_restore() {
    auto d=definition();RuntimeAnimations a(d);warm(a,0,30);a.apply({},30,{rate(2)});warm(a,31,40);
    const auto valid=a.save_state(40);const auto j=Json::parse(valid);check(j.at("version")==9&&j.at("legacy_version")==1,"Conditional rate profile not selected.");
    check(j.at("rigs")[0].contains("history")&&j.at("rigs")[0].contains("inertial_ever_used"),"v9 legacy1 dropped future inertial history.");
    auto invalid=[&](auto mutate) {auto bad=j;mutate(bad);const auto bytes=bad.dump();archive("malformed",bytes);rejects([&]{a.load_state(bytes,40);});same(valid,a.save_state(40),"Malformed rate save changed live state.");};
    invalid([](auto& x){x["legacy_version"]=0;});invalid([](auto& x){x["legacy_version"]=9;});invalid([](auto& x){x["legacy_version"]=true;});
    invalid([](auto& x){x["rigs"][0].erase("rate");});invalid([](auto& x){x["rigs"][0]["rate"]=nullptr;});
    invalid([](auto& x){x["rigs"][0]["rate"]["anchor_tick"]=41;});invalid([](auto& x){x["rigs"][0]["rate"]["anchor_tick"]=30.5;});
    invalid([](auto& x){x["rigs"][0]["rate"]["time"]=-1;});invalid([](auto& x){x["rigs"][0]["rate"]["time"]=false;});
    invalid([](auto& x){x["rigs"][0]["rate"]["speed"]=9;});invalid([](auto& x){x["rigs"][0]["rate"]["speed"]=true;});
    invalid([](auto& x){x["rigs"][0]["rate"]["extra"]=0;});invalid([](auto& x){x["rigs"][0]["history"]["current"]["tick"]=41;});
    invalid([](auto& x){x["rigs"][0]["control"]["transition_mode"]="crossfade";});
    // A live source carries its own older segment, never one anchored AFTER fade start.
    a.apply({full()},40);a.sample(40);const auto faded=a.save_state(40);auto bad=Json::parse(faded);bad["rigs"][0]["transition"]["source"]["rate"]["anchor_tick"]=41;
    rejects([&]{a.load_state(bad.dump(),40);});same(faded,a.save_state(40),"Future outgoing source rate was accepted.");
    // Old v1 is loaded without inventing a rate overlay. Legacy transition guards remain strict.
    RuntimeAnimations legacy(d);warm(legacy,0,20);const auto v1=legacy.save_state(20);legacy.load_state(v1,20);same(v1,legacy.save_state(20),"Legacy1 bytes changed on load.");
    legacy.apply({full()},20);legacy.sample(20);const auto old=legacy.save_state(20);auto oldbad=Json::parse(old);oldbad["rigs"][0]["anchor_tick"]=19;
    rejects([&]{legacy.load_state(oldbad.dump(),20);});same(old,legacy.save_state(20),"Rate feature relaxed legacy anchor validation.");
    RuntimeAnimations cleared(d);cleared.sample(0);cleared.apply({},0,{rate(2)});cleared.apply({full(0,0)},0);cleared.sample(0);
    check(Json::parse(cleared.save_state(0)).at("version")==1&&!cleared.checkpoint()[0].rate,"Immediate full replacement retained obsolete rate profile.");
    // Generate genuine old profiles through their public controls/bindings,
    // rather than relabeling one JSON document as several historical versions.
    for(unsigned version=1;version<=8;++version) {
        auto historical=definition(version==3,false);auto& rig=*historical.entities[0].animation_rig;
        if(version==5)rig.root_motion=RuntimeRootMotionDefinition{0,RootMotionMode::Extract,"",true,false};
        if(version==4||version>=6) {
            RuntimeTwoBoneIk ik;ik.root=0;ik.middle=1;ik.end=2;ik.enabled=false;ik.space=IkSpace::Rig;
            ik.target={0,2,0};ik.pole={1,1,0};
            if(version==6)ik.end_orientation=RuntimeIkEndOrientation{{0,0,0,1},1};
            if(version>=7){ik.metric_policy=RuntimeIkMetricPolicy::NearUniformV1;ik.grounding=RuntimeGroundingDefinition{};}
            rig.ik={ik};
        }
        RuntimeAnimations old_profile(historical);
        if(version==2)old_profile.apply({full(1,60,AnimationTransitionMode::Inertial)},0);
        if(version==8)old_profile.apply_grounding({{id(1),1,false,.5}});
        const auto query=[](const std::string&,const RuntimeRay&)->std::optional<RuntimeRayHit>{return std::nullopt;};
        if(version>=7)old_profile.sample(0,{},query);else old_profile.sample(0);
        const auto bytes=old_profile.save_state(0);check(Json::parse(bytes).at("version")==version,"Generated historical profile has wrong version.");
        RuntimeAnimations restored(historical);restored.load_state(bytes,0);
        same(bytes,restored.save_state(0),"Legacy profile changed on load before retiming.");
        // Original v1 deliberately has no output-history fields. Reconstruct its
        // initial current pose at the same tick before creating a NEW v9 state;
        // this does not fabricate an unavailable previous-tick velocity.
        if(version==1)restored.sample(0);
        old_profile.apply({},0,{rate(2)});restored.apply({},0,{rate(2)});
        const auto upgraded=old_profile.save_state(0);const auto wire=Json::parse(upgraded);
        check(wire.at("version")==9&&wire.at("legacy_version")==version,"Retiming did not preserve historical profile selection.");
        same(upgraded,restored.save_state(0),"Retiming a restored legacy profile changed complete state.");
        RuntimeAnimations roundtrip(historical);roundtrip.load_state(upgraded,0);same(upgraded,roundtrip.save_state(0),"Retimed historical profile failed cold roundtrip.");
    }
}
void step(Runtime& r,unsigned ticks,const std::vector<AnimationCommand>& commands={},const std::vector<AnimationRateCommand>& rates={},const std::vector<RuntimeStructureTick>& edits={}) {
    r.step(ticks,{}, {}, {},commands,edits,{}, {}, {}, {}, {},rates);
}
void native_partition_rollback_and_cold() {
    const auto d=definition(true);Runtime a(d),b(d);step(a,30);step(b,30);auto c=full();step(a,30,{c});step(b,30,{c});
    const auto before=a.save_snapshot(content);std::vector<RuntimeStructureTick> fail{{1,99,{},{}}};
    rejects([&]{step(a,3,{}, {rate(2),rate(.5,1)},fail);});same(before,a.save_snapshot(content),"Later native Tick failure left rate/physics/history state changed.");
    step(a,1,{}, {rate(2),rate(.5,1)});step(a,29);step(b,30,{}, {rate(2),rate(.5,1)});
    same(a.save_snapshot(content),b.save_snapshot(content),"Native rate depends on batch partition.");
    const auto saved=a.save_snapshot(content);auto cold=Runtime::from_snapshot(d,content,saved);same(saved,cold->save_snapshot(content),"Full native cold rate archive differs.");
    auto replacement=full(2,60,AnimationTransitionMode::Inertial);step(a,1,{replacement});step(*cold,1,{replacement});
    for(unsigned n=0;n<4;++n) {step(a,15);step(*cold,15);same(a.save_snapshot(content),cold->save_snapshot(content),"Full native retimed/inertial future archive differs.");}
}
}
int main(int argc,char** argv) {
    try {
        if(argc==3&&std::string_view(argv[1])=="--output") {output=std::filesystem::path(argv[2]);check(output.is_absolute()&&!std::filesystem::exists(output)&&std::filesystem::is_directory(output.parent_path()),"Output must be fresh absolute directory with existing parent.");std::filesystem::create_directory(output);}
        else check(argc==1,"Usage: runtime-animation-rate-test [--output FRESH_DIRECTORY]");
        const std::pair<const char*,void(*)()> portable[]{
            {"crossfade_phase_and_completion",crossfade_phase_and_completion},{"inertial_piecewise_phase_and_history",inertial_piecewise_phase_and_history},
            {"clock_edges_and_atomic_admission",clock_edges_and_atomic_admission},{"masked_layers_and_target_conflicts",masked_layers_and_target_conflicts},
            {"root_interval_and_outgoing_replacement",root_interval_and_outgoing_replacement},{"nested_save_versions_and_strict_restore",nested_save_versions_and_strict_restore}};
        for(const auto& [name,test]:portable){test();std::cout<<"PASS "<<name<<'\n';}
        if(Runtime::available()){native_partition_rollback_and_cold();std::cout<<"PASS native_partition_rollback_and_cold\n";}
        else std::cout<<"SKIP native_partition_rollback_and_cold: simulation unavailable\n";
        std::cout<<"Animation rate: "<<checks<<" checks, "<<archives<<" complete archive operands, 6 portable groups, "<<(Runtime::available()?1:0)<<" native groups.\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
