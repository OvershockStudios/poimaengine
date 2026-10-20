// SPDX-License-Identifier: Apache-2.0
// Independent support-frame geometry, owner-phase probes and complete saves.
#include "poima/runtime.hpp"
#include "poima/runtime_animation.hpp"
#include "poima/runtime_grounding.hpp"
#include "poima/jobs.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <span>
#include <thread>
using namespace poima;
namespace {
using V=std::array<double,3>;using Q=std::array<double,4>;using Json=nlohmann::json;
std::filesystem::path output;unsigned archives=0;const std::string content_hash(64,'e');
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::string id(unsigned n){const auto s=std::to_string(n);return std::string(32-s.size(),'0')+s;}
V subtract(V a,const V& b){for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
double magnitude(const V& a){return std::hypot(a[0],a[1],a[2]);}
double dot(const V& a,const V& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
void near(double actual,double expected,double tolerance=4e-5){check(std::isfinite(actual)&&std::abs(actual-expected)<=tolerance,"Independent planting scalar differs.");}
void near(const V& actual,const V& expected,double tolerance=4e-5){for(unsigned i=0;i<3;++i)near(actual[i],expected[i],tolerance);}
template<class F>void rejects(F&& f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"Expected native rejection absent.");}
std::string retain(const std::string& name,const std::string& bytes){if(!output.empty()){std::ofstream f(output/(std::to_string(++archives)+"-"+name+".json"),std::ios::binary);check(bool(f),"Cannot open complete archive.");f.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));f.close();check(bool(f),"Cannot close complete archive.");}return bytes;}
std::string reseal(Json value){const auto bytes=value.at("payload").dump();value["sha256"]=sha256(std::as_bytes(std::span(bytes.data(),bytes.size())));return value.dump();}
Q yaw(double angle){return {0,std::sin(angle/2),0,std::cos(angle/2)};}
Q tilt(double angle){return {0,0,std::sin(angle/2),std::cos(angle/2)};}
// Independent rigid-body frame: quaternion polynomial, no solver/inverse helper.
Matrix4 rigid(const V& p,const Q& q){const auto x=q[0],y=q[1],z=q[2],w=q[3];return {
    1-2*(y*y+z*z),2*(x*y+w*z),2*(x*z-w*y),0,
    2*(x*y-w*z),1-2*(x*x+z*z),2*(y*z+w*x),0,
    2*(x*z+w*y),2*(y*z-w*x),1-2*(x*x+y*y),0,p[0],p[1],p[2],1};}
V vector(const Matrix4& m,const V& p){V r{};for(unsigned i=0;i<3;++i)r[i]=m[i]*p[0]+m[4+i]*p[1]+m[8+i]*p[2];return r;}
V point(const Matrix4& m,const V& p){auto r=vector(m,p);for(unsigned i=0;i<3;++i)r[i]+=m[12+i];return r;}
V local_point(const Matrix4& m,const V& p){const auto d=subtract(p,{m[12],m[13],m[14]});return {m[0]*d[0]+m[1]*d[1]+m[2]*d[2],m[4]*d[0]+m[5]*d[1]+m[6]*d[2],m[8]*d[0]+m[9]*d[1]+m[10]*d[2]};}
RuntimeDefinition definition(bool planting=true,unsigned feet=2){
    RuntimeDefinition d;d.world_id="foot-planting-native";d.authored_revision=3;
    auto model=std::make_shared<ModelAsset>();model->nodes.resize(feet*3);
    RuntimeEntityDefinition rig;rig.id=id(1);rig.animation_rig=RuntimeAnimationRig{model};
    for(unsigned f=0;f<feet;++f){const auto b=f*3;model->roots.push_back(b);
        for(unsigned j=0;j<3;++j){model->nodes[b+j].name="Analytic planted leg "+std::to_string(b+j);model->nodes[b+j].parent=j?static_cast<int>(b+j-1):-1;}
        model->nodes[b].position={f? .3:-.3,1.8,0};model->nodes[b+1].position={.5,-.7,0};model->nodes[b+2].position={-.5,-.7,0};
        RuntimeTwoBoneIk c;c.slot=f+1;c.root=b;c.middle=b+1;c.end=b+2;c.space=IkSpace::Rig;c.target={0,.4,0};c.pole={1,1,0};c.metric_policy=RuntimeIkMetricPolicy::NearUniformV1;
        c.grounding=RuntimeGroundingDefinition{};c.grounding->sole_offset={0,-.2,0};c.grounding->reference_height=.2;c.grounding->max_correction=.6;c.grounding->plant_lock=planting;rig.animation_rig->ik.push_back(c);
    }
    model->animations.push_back({"Still",1,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0,1},{{-.3,1.8,0,0},{-.3,1.8,0,0}}}}});
    model->animations.push_back({"Late invalid metric",1./32,{{0,AnimationPath::scale,AnimationInterpolation::step,{0,1./64,1./32},{{1,1,1,0},{1,1,1,0},{1.01,1,1,0}}}}});
    d.entities.push_back(rig);
    for(unsigned n=0;n<model->nodes.size();++n){RuntimeEntityDefinition e;e.id=id(10+n);const auto parent=model->nodes[n].parent;e.parent=parent<0?id(1):id(10+static_cast<unsigned>(parent));e.transform={model->nodes[n].position,model->nodes[n].rotation,model->nodes[n].scale};e.rig_node=RuntimeRigNode{id(1),n};d.entities.push_back(e);}
    RuntimeEntityDefinition floor;floor.id=id(100);floor.transform.position={0,-.5,0};floor.collider=BoxCollider{};floor.collider->half_extents={5,.5,5};floor.collider->motion=BodyMotion::Kinematic;d.entities.push_back(floor);return d;
}
RuntimeEntityDefinition& entity(RuntimeDefinition& d,unsigned n){auto i=std::find_if(d.entities.begin(),d.entities.end(),[&](const auto& e){return e.id==id(n);});check(i!=d.entities.end(),"Fixture entity absent.");return *i;}
RuntimeGroundingState state(RuntimeAnimations& a,unsigned slot=1){auto s=a.ik_state(id(1),slot);check(s&&s->grounding,"Missing animation contact state.");return *s->grounding;}
RuntimeGroundingState state(Runtime& a,unsigned slot=1){auto s=a.ik(id(1),slot);check(s&&s->grounding,"Missing native contact state.");return *s->grounding;}
RuntimeFootPlantState planted(const RuntimeGroundingState& s){check(s.planting.has_value(),"Missing configured plant state.");return *s.planting;}
void locked(const RuntimeGroundingState& s){auto p=planted(s);check(s.status=="contact"&&p.status=="locked"&&!p.armed&&p.anchor&&p.target_sole&&p.target_normal&&p.target_forward&&p.position_error&&p.normal_error,"Plant does not retain a complete locked frame.");}
struct Plane {
    Matrix4 frame=rigid({0,-.5,0},{0,0,0,1});unsigned queries=0,frames=0;bool present=true;std::string support=id(100);double error=0;bool available=true;
    const std::thread::id owner=std::this_thread::get_id();
    grounding::Query query(){return [this](const std::string& rig,const RuntimeRay& ray)->std::optional<RuntimeRayHit>{
        check(std::this_thread::get_id()==owner&&rig==id(1),"Probe left the owner phase.");++queries;if(!present)return {};
        const auto n=vector(frame,{0,1,0}),p=point(frame,{0,.5,0});const auto denominator=dot(n,ray.direction);check(std::abs(denominator)>1e-8,"Fixture ray parallel to plane.");
        const auto t=dot(n,subtract(p,ray.origin))/denominator;check(t>=0&&t<=ray.distance,"Fixture plane outside probe.");RuntimeRayHit h;h.entity=support;h.normal=n;h.position=ray.origin;for(unsigned i=0;i<3;++i)h.position[i]+=t*ray.direction[i];h.position[0]+=error;h.distance=t;h.fraction=t/ray.distance;h.triangle=7;return h;};}
    grounding::SupportQuery resolver(){return [this](const std::string& rig,const std::string& key)->std::optional<Matrix4>{check(std::this_thread::get_id()==owner&&rig==id(1)&&key.size()==32,"Resolver owner/support differs.");++frames;if(!available)return {};return frame;};}
};
void portable_owner_barrier_and_queryless_restore(){
    const auto d=definition();jobs::Config config;config.workers=2;auto executor=std::make_shared<jobs::Executor>(config);RuntimeAnimations a(d,executor);auto wrapper=identity_matrix();Plane plane;
    auto world=[&](const std::string&)->const Matrix4&{return wrapper;};
    a.sample(0,world);check(plane.queries==0&&plane.frames==0,"Construction/sample queried physics.");
    const auto initial=retain("initial-v10",a.save_state(0));const auto doc=Json::parse(initial);check(doc["version"]==10&&doc["legacy_version"]==7&&doc["retimed"]==false,"Initial plant profile not canonical v10.");
    a.sample(1,world,plane.query(),plane.resolver());for(unsigned slot=1;slot<=2;++slot){auto s=state(a,slot);locked(s);near(s.input_sole,{slot==1?-.3:.3,.2,0},2e-12);near(planted(s).anchor->local_point,{slot==1?-.3:.3,.5,0},2e-12);check(s.casts==1&&planted(s).anchor->acquired_tick==1,"Wrong acquisition tick/count.");}
    check(plane.queries==2&&plane.frames==2,"Acquisition did not use one probe and frame per foot.");
    const auto saved=retain("animation-locked",a.save_state(1));const auto q=plane.queries,f=plane.frames;
    for(unsigned i=0;i<3;++i){a.sample(1,world);(void)a.ik_state(id(1),1);check(a.save_state(1)==saved,"Inspection altered complete cached plant state.");}
    RuntimeAnimations cold(d);cold.load_state(saved,1);cold.sample(1,world);check(retain("animation-restored",cold.save_state(1))==saved&&plane.queries==q&&plane.frames==f,"Restore/inspection refreshed support frames or probes.");
    auto checkpoint=a.checkpoint();unsigned calls=0;const auto original_query=plane.query();grounding::Query fail=[&](const std::string& key,const RuntimeRay& ray)->std::optional<RuntimeRayHit>{if(++calls==2)throw std::runtime_error("Second-foot query fault");return original_query(key,ray);};
    rejects([&]{a.sample(2,world,fail,plane.resolver());});check(calls==2&&retain("after-second-query-fault",a.save_state(1))==saved,"Second foot published partial controls/anchors/history.");a.restore(checkpoint);check(a.save_state(1)==saved,"Trusted checkpoint lost plant state.");
    auto bad_frame=plane.frame;bad_frame[0]=1.01;grounding::SupportQuery invalid=[&](const std::string&,const std::string&)->std::optional<Matrix4>{return bad_frame;};const auto before_queries=plane.queries;
    rejects([&]{a.sample(2,world,plane.query(),invalid);});check(a.save_state(1)==saved&&plane.queries==before_queries,"Invalid support frame probed/published state.");
    RuntimeIkCommand manual{id(1),1,true,IkSpace::Rig,{0,.2,0},{1,1,0},1};rejects([&]{a.apply_ik({manual});});check(a.save_state(1)==saved,"Manual owned-slot conflict changed complete state.");
}
void portable_release_latch_and_fade(){
    auto d=definition(true,1);const auto& rig=*d.entities[0].animation_rig;auto c=rig.ik[0];std::vector<int> parents;for(const auto& n:rig.model->nodes)parents.push_back(n.parent);
    auto pose=sample_model(*rig.model,std::nullopt,0,false);const auto wrapper=identity_matrix();Plane plane;RuntimeGroundingState initial;initial.planting=RuntimeFootPlantState{};
    auto first=grounding::prepare(parents,pose,c,wrapper,id(1),1,initial,plane.query(),plane.resolver());check(planted(first.state).anchor.has_value(),"Portable low stance did not acquire.");
    // Tiny actual hit disagreement is retained, not overwritten by the target.
    plane.error=.0005;auto tiny=grounding::prepare(parents,pose,c,wrapper,id(1),2,first.state,plane.query(),plane.resolver());grounding::finish(tiny.state,pose,c,wrapper);locked(tiny.state);near(tiny.state.hit->position[0]-(*tiny.state.planting->target_sole)[0],.0005,2e-12);
    auto phase=[&](const RuntimeGroundingState& previous,std::uint64_t tick){return grounding::prepare(parents,pose,c,wrapper,id(1),tick,previous,plane.query(),plane.resolver());};
    plane.error=.002;auto released=phase(first.state,2);check(released.state.status=="plant-anchor-error"&&!planted(released.state).armed&&!planted(released.state).anchor,"Overhang/anchor disagreement did not release and latch.");
    plane.error=0;auto latch=phase(released.state,3);check(latch.state.status=="contact"&&planted(latch.state).status=="disarmed"&&!planted(latch.state).anchor,"Constraint failure ratcheted a new anchor during the same stance.");
    auto lifted=pose;lifted.local[2].position[1]+=.2;lifted.world[2][13]+=.2;
    const auto before=plane.queries;auto swing=grounding::prepare(parents,lifted,c,wrapper,id(1),4,latch.state,plane.query(),plane.resolver());check(swing.state.status=="lifted"&&planted(swing.state).armed&&!planted(swing.state).anchor&&plane.queries==before,"Swing did not rearm without a cast.");
    auto replant=phase(swing.state,5);check(planted(replant.state).anchor&&planted(replant.state).anchor->acquired_tick==5,"New stance did not acquire a new anchor.");
    auto partial=pose;partial.local[2].position[1]+=.09;partial.world[2][13]+=.09;
    auto faded=grounding::prepare(parents,partial,c,wrapper,id(1),6,replant.state,plane.query(),plane.resolver());check(planted(faded.state).anchor.has_value(),"Partial source lift discarded a held anchor.");near(faded.control.weight,.5,2e-12);check(faded.control.end_orientation.has_value(),"Partial stance lost orientation.");near(faded.control.end_orientation->weight,.5,2e-12);near(*faded.state.planting->target_sole,*replant.state.planting->target_sole,0);
    auto disabled=c;disabled.enabled=false;const auto q=plane.queries;auto off=grounding::prepare(parents,pose,disabled,wrapper,id(1),7,replant.state,plane.query(),plane.resolver());check(planted(off.state).armed&&!planted(off.state).anchor&&off.state.status=="disabled"&&plane.queries==q,"Disabled profile did not clear/rearm without probing.");
    auto zero=c;zero.weight=0;auto weight0=grounding::prepare(parents,pose,zero,wrapper,id(1),7,replant.state,plane.query(),plane.resolver());check(planted(weight0.state).armed&&!planted(weight0.state).anchor&&plane.queries==q,"Zero ceiling did not clear/rearm without probing.");
    plane.available=false;const auto q2=plane.queries;auto lost=phase(replant.state,6);check(lost.state.status=="plant-support-lost"&&!lost.state.cast&&!lost.state.hit&&!planted(lost.state).armed&&plane.queries==q2,"Lost support cast or retained an unsafe anchor.");plane.available=true;
    plane.support=id(101);auto changed=phase(replant.state,6);check(changed.state.status=="plant-support-changed"&&changed.state.cast&&changed.state.hit->entity==id(101)&&!planted(changed.state).anchor,"Changed support silently rebound the old anchor.");plane.support=id(100);
    plane.present=false;auto miss=phase(replant.state,6);check(miss.state.status=="miss"&&planted(miss.state).status=="released-miss"&&!planted(miss.state).armed,"Held overhang miss did not latch release.");plane.present=true;
    auto teleported=wrapper;teleported[12]=.7;auto unsafe=grounding::prepare(parents,pose,c,teleported,id(1),6,replant.state,plane.query(),plane.resolver());check(unsafe.state.status=="correction-limit"&&!planted(unsafe.state).anchor&&!planted(unsafe.state).armed,"Large wrapper correction retained unsafe lock.");
}
void strict_v10_and_legacy_profiles(){
    auto d=definition();RuntimeAnimations a(d);Plane p;auto matrix=identity_matrix();auto world=[&](const std::string&)->const Matrix4&{return matrix;};a.sample(1,world,p.query(),p.resolver());
    const auto original=retain("strict-original-v10",a.save_state(1));const auto source=Json::parse(original);
    auto reject_document=[&](Json bad){const auto bytes=retain("malformed-v10",bad.dump());const auto q=p.queries,f=p.frames;rejects([&]{a.load_state(bytes,1);});check(a.save_state(1)==original&&p.queries==q&&p.frames==f,"Malformed v10 changed controls/history or consulted physics.");};
    for(unsigned kind=0;kind<25;++kind){auto bad=source;auto& g=bad["rigs"][0]["ik"][0]["grounding"];auto& plant=g["planting"];
        switch(kind){
        case 0:bad["extra"]=0;break;
        case 1:bad["retimed"]=0;break;
        case 2:bad["legacy_version"]=6;break;
        case 3:g.erase("planting");break;
        case 4:g["planting"]=nullptr;break;
        case 5:plant["extra"]=0;break;
        case 6:plant.erase("armed");break;
        case 7:plant["armed"]=0;break;
        case 8:plant["armed"]=true;break;
        case 9:plant["status"]="invented";break;
        case 10:plant["anchor"]=nullptr;break;
        case 11:plant["anchor"]["acquired_tick"]=2;break;
        case 12:plant["anchor"]["support"]=id(0);break;
        case 13:plant["anchor"]["support"]=id(101);break;
        case 14:plant["anchor"]["triangle"]=true;break;
        case 15:plant["anchor"]["local_point"][0]=4000000.001;break;
        case 16:plant["anchor"]["local_normal"]={0,2,0};break;
        case 17:plant["anchor"]["local_forward"]={0,1,0};break;
        case 18:plant["target_sole"][0]=1000000.001;break;
        case 19:plant["target_normal"]={0,2,0};break;
        case 20:plant["target_forward"]=nullptr;break;
        case 21:plant["position_error"]=-.01;break;
        case 22:plant["normal_error"]=4;break;
        case 23:plant["target_sole"][1]=.01;break;
        case 24:plant["anchor"]["extra"]=0;break;
        }
        reject_document(std::move(bad));
    }
    RuntimeAnimations initial(d);const auto pristine=initial.save_state(0);auto malformed=Json::parse(pristine);malformed["rigs"][0]["ik"][0]["grounding"]["planting"]["anchor"]={{"acquired_tick",0},{"support",id(100)},{"triangle",nullptr},{"local_point",{0,.5,0}},{"local_normal",{0,1,0}},{"local_forward",{0,0,1}}};rejects([&]{initial.load_state(malformed.dump(),0);});check(initial.save_state(0)==pristine,"Malformed initial plant published an anchor.");
    auto mixed=d;mixed.entities[0].animation_rig->ik[1].grounding->plant_lock=false;RuntimeAnimations hybrid(mixed);hybrid.sample(1,world,p.query(),p.resolver());auto hybrid_doc=Json::parse(hybrid.save_state(1));check(hybrid_doc["rigs"][0]["ik"][1]["grounding"]["planting"].is_null(),"V10 nonplanting grounding slot is not explicitly null.");
    auto old=definition(false);RuntimeAnimations v7(old);v7.sample(1,world,p.query());const auto old7=retain("legacy-v7",v7.save_state(1));check(Json::parse(old7)["version"]==7&&old7.find("plant_lock")==std::string::npos&&old7.find("planting")==std::string::npos,"Omitted option changed legacy v7 fields.");
    RuntimeAnimations back7(old);back7.load_state(old7,1);back7.sample(1,world);check(back7.save_state(1)==old7,"Legacy v7 full bytes did not survive cold reconstruction.");rejects([&]{back7.load_state(original,1);});check(back7.save_state(1)==old7,"Plant membership rejection changed legacy state.");
    v7.apply_grounding({{id(1),1,false,.5}});v7.sample(2,world,p.query());const auto old8=retain("legacy-v8",v7.save_state(2));check(Json::parse(old8)["version"]==8&&old8.find("planting")==std::string::npos,"Omitted option changed legacy v8 fields.");RuntimeAnimations back8(old);back8.load_state(old8,2);back8.sample(2,world);check(back8.save_state(2)==old8,"Legacy v8 full bytes did not survive cold reconstruction.");
    AnimationCommand playing{id(1),0,0,1,true,true};v7.apply({playing},2);v7.sample(3,world,p.query());v7.apply({},3,{{id(1),2,{}}});v7.sample(4,world,p.query());const auto old9=retain("legacy-v9",v7.save_state(4));check(Json::parse(old9)["version"]==9&&old9.find("planting")==std::string::npos,"Omitted option changed legacy v9 fields.");RuntimeAnimations back9(old);back9.load_state(old9,4);back9.sample(4,world);check(back9.save_state(4)==old9,"Legacy v9 complete history/rates changed cold.");
    a.apply({playing},1);a.sample(2,world,p.query(),p.resolver());a.apply({},2,{{id(1),2,{}}});a.sample(3,world,p.query(),p.resolver());const auto timed=retain("retimed-v10",a.save_state(3));check(Json::parse(timed)["version"]==10&&Json::parse(timed)["retimed"]==true,"Retimed planting lost its profile/history.");RuntimeAnimations cold(d);cold.load_state(timed,3);cold.sample(3,world);check(cold.save_state(3)==timed,"Retimed v10 complete state changed cold.");
}
V sole(Runtime& r,unsigned slot){return point(r.entity(id(12+3*(slot-1))).world,{0,-.2,0});}
void inspect_locked(Runtime& r,unsigned slot,const Matrix4& support,const V& anchor){
    const auto s=state(r,slot);locked(s);const auto plant=*s.planting;
    near(plant.anchor->local_point,anchor);near(*plant.target_sole,point(support,anchor));near(sole(r,slot),point(support,anchor));near(local_point(support,sole(r,slot)),anchor);
    near(s.actual_sole,sole(r,slot),2e-12);near(*plant.position_error,magnitude(subtract(sole(r,slot),*plant.target_sole)),2e-12);near(*plant.normal_error,0,3e-5);near(*plant.target_normal,vector(support,{0,1,0}));near(*plant.target_forward,vector(support,{0,0,1}));
}
void native_wrapper_drift_positive_control(){
    auto planted_d=definition(),free_d=definition(false);
    for(auto* d:{&planted_d,&free_d}){auto& rig=d->entities[0];rig.collider=BoxCollider{};rig.collider->half_extents={.01,.01,.01};rig.collider->motion=BodyMotion::Kinematic;entity(*d,100).collider->motion=BodyMotion::Static;}
    Runtime locked_runtime(planted_d),ordinary(free_d);locked_runtime.step(1,{});ordinary.step(1,{});
    const auto frame=rigid({0,-.5,0},{0,0,0,1});KinematicTarget move;move.entity=id(1);move.position={.04,0,0};move.duration_ticks=2;
    locked_runtime.step(2,{}, {move});ordinary.step(2,{}, {move});
    for(unsigned slot=1;slot<=2;++slot){const V anchor{slot==1?-.3:.3,.5,0};inspect_locked(locked_runtime,slot,frame,anchor);near(sole(ordinary,slot)[0],anchor[0]+.04);check(std::abs(sole(ordinary,slot)[0]-sole(locked_runtime,slot)[0])>.035,"Unplanted positive control did not visibly drift.");check(state(locked_runtime,slot).casts==3&&state(ordinary,slot).casts==3,"Wrapper test cast more/less than one per foot/tick.");}
    const auto saved=retain("wrapper-locked",locked_runtime.save_snapshot(content_hash));for(unsigned n=0;n<3;++n){(void)locked_runtime.ik(id(1),1);(void)locked_runtime.inspect();check(locked_runtime.save_snapshot(content_hash)==saved&&state(locked_runtime).casts==3,"Native inspection advanced probes/anchors.");}
    // Live disabled policy rearms but never releases ownership to manual IK.
    locked_runtime.step(1,{}, {}, {}, {}, {}, {}, {}, {}, {},{{id(1),1,false,1}});auto disabled=state(locked_runtime);check(disabled.status=="disabled"&&planted(disabled).armed&&!planted(disabled).anchor&&disabled.casts==3,"Native disable did not clear/rearm without probing.");
    const auto before=locked_runtime.save_snapshot(content_hash);RuntimeIkCommand manual{id(1),1,true,IkSpace::Rig,{0,.2,0},{1,1,0},1};rejects([&]{locked_runtime.step(1,{}, {}, {}, {}, {},{manual});});check(locked_runtime.save_snapshot(content_hash)==before,"Disabled automatic slot accepted manual control or lost rollback.");
    locked_runtime.step(1,{}, {}, {}, {}, {}, {}, {}, {}, {},{{id(1),1,true,1}});check(planted(state(locked_runtime)).anchor->acquired_tick==5,"Reenable failed to acquire at a fresh committed stance.");near(planted(state(locked_runtime)).anchor->local_point[0],-.26);
    retain("wrapper-reenabled",locked_runtime.save_snapshot(content_hash));
}
void native_moving_support_and_complete_cold(){
    auto d=definition();entity(d,100).transform.scale={1.3,1,1.2};Runtime warm(d);warm.step(1,{});
    const V left{-.3,.5,0},right{.3,.5,0};for(unsigned s=1;s<=2;++s)inspect_locked(warm,s,rigid({0,-.5,0},{0,0,0,1}),s==1?left:right);
    KinematicTarget move;move.entity=id(100);move.position={.025,-.485,.03};move.rotation=yaw(.08);move.duration_ticks=2;warm.step(2,{}, {move});
    auto support=rigid(move.position,move.rotation);for(unsigned s=1;s<=2;++s){inspect_locked(warm,s,support,s==1?left:right);check(state(warm,s).casts==3&&planted(state(warm,s)).anchor->acquired_tick==1,"Moving support changed anchor identity/counter.");}
    const auto before=retain("support-before-cold",warm.save_snapshot(content_hash));
    for(unsigned kind=0;kind<5;++kind){auto malformed=Json::parse(before);auto& g=malformed["payload"]["animation"]["rigs"][0]["ik"][0]["grounding"];auto& plant=g["planting"];
        if(kind==0)plant["anchor"]["local_point"][0]=plant["anchor"]["local_point"][0].get<double>()+.01;
        if(kind==1){plant["anchor"]["support"]=id(999);g["state"]["hit"]["entity"]=id(999);}
        if(kind==2){plant["anchor"]["support"]=id(1);g["state"]["hit"]["entity"]=id(1);}
        if(kind==3)plant["target_sole"][1]=plant["target_sole"][1].get<double>()+.01;
        if(kind==4)g["state"]["actual_sole"][0]=g["state"]["actual_sole"][0].get<double>()+.01;
        const auto bad=retain("malformed-native-anchor",reseal(std::move(malformed)));rejects([&]{(void)Runtime::from_snapshot(d,content_hash,bad);});check(warm.save_snapshot(content_hash)==before,"Invalid native membership/frame/target restore mutated source state.");
    }
    auto cold=Runtime::from_snapshot(d,content_hash,before);check(retain("support-cold-immediate",cold->save_snapshot(content_hash))==before,"Complete v10 native restore changed original fields.");
    // Tilt is separate from yaw, making the normal and forward basis observable.
    KinematicTarget tilt_move;tilt_move.entity=id(100);tilt_move.position={.03,-.48,.025};tilt_move.rotation=tilt(.04);tilt_move.duration_ticks=2;
    warm.step(2,{}, {tilt_move});cold->step(2,{}, {tilt_move});support=rigid(tilt_move.position,tilt_move.rotation);
    for(unsigned s=1;s<=2;++s){inspect_locked(warm,s,support,s==1?left:right);check(state(warm,s).casts==5,"Cold continuation did not retain one cast per final tick.");}
    check(retain("support-warm-future",warm.save_snapshot(content_hash))==retain("support-cold-future",cold->save_snapshot(content_hash)),"Complete moving-support cold future differs.");
    // An ongoing target is saved too, not just a settled final transform.
    move.position={.045,-.48,.04};move.rotation=yaw(.1);move.duration_ticks=4;warm.step(1,{}, {move});const auto mid=retain("support-mid-target",warm.save_snapshot(content_hash));auto midcold=Runtime::from_snapshot(d,content_hash,mid);check(midcold->save_snapshot(content_hash)==mid,"Ongoing kinematic target changed at cold boundary.");warm.step(3,{});midcold->step(3,{});check(retain("support-mid-warm-future",warm.save_snapshot(content_hash))==retain("support-mid-cold-future",midcold->save_snapshot(content_hash)),"Ongoing platform target/anchor future differs cold.");
    warm.step(1,{}, {}, {}, {}, {}, {}, {}, {}, {},{{id(1),1,true,.5}});const auto partial=state(warm);locked(partial);near(partial.weight,.5,2e-12);check(*partial.planting->position_error>.01&&partial.planting->anchor->acquired_tick==1,"Partial multiplier lost anchor or falsely claimed exact achieved contact.");
    const auto faded=retain("support-partial-policy-v10",warm.save_snapshot(content_hash));check(Json::parse(faded)["payload"]["animation"]["legacy_version"]==8,"Requested partial multiplier lost v8 backing profile.");auto faded_cold=Runtime::from_snapshot(d,content_hash,faded);check(retain("support-partial-cold",faded_cold->save_snapshot(content_hash))==faded,"Partial planting policy changed full cold state.");
    warm.step(1,{}, {}, {}, {}, {}, {}, {}, {}, {},{{id(1),1,true,1}});faded_cold->step(1,{}, {}, {}, {}, {}, {}, {}, {}, {},{{id(1),1,true,1}});check(retain("support-partial-warm-future",warm.save_snapshot(content_hash))==retain("support-partial-cold-future",faded_cold->save_snapshot(content_hash)),"Partial-to-full plant continuation changed complete cold state.");
}
void native_second_tick_whole_rollback(){
    auto d=definition();RuntimeEntityDefinition body;body.id=id(200);body.transform.position={3,5,0};body.collider=BoxCollider{};body.collider->motion=BodyMotion::Dynamic;d.entities.push_back(body);
    Runtime value(d),witness(d);const auto original=retain("before-late-native-failure",value.save_snapshot(content_hash));const auto initial_body=value.entity(id(200)).world;AnimationCommand late{id(1),1,0,1,false,true};
    witness.step(1,{}, {}, {},{late});locked(state(witness));check(witness.entity(id(200)).world!=initial_body,"Witness did not exercise actual physics before fault.");
    rejects([&]{value.step(2,{}, {}, {},{late});});check(retain("after-late-native-failure",value.save_snapshot(content_hash))==original&&state(value).casts==0&&planted(state(value)).status=="uninitialized"&&value.entity(id(200)).world==initial_body,"Second-tick metric failure lost whole native rollback.");
    value.step(1,{});locked(state(value));const auto committed=retain("committed-before-late-failure",value.save_snapshot(content_hash));
    rejects([&]{value.step(2,{}, {}, {},{late});});check(retain("committed-after-late-failure",value.save_snapshot(content_hash))==committed,"Second-tick failure lost a preexisting support anchor or physics state.");
}
}
int main(int argc,char** argv){try{
    if(argc!=1){check(argc==3&&std::string(argv[1])=="--output","Use optional --output FRESH.");output=std::filesystem::absolute(argv[2]);check(!std::filesystem::exists(output)&&std::filesystem::is_directory(output.parent_path()),"Output must be fresh with existing parent.");check(std::filesystem::create_directory(output),"Cannot create checkpoint directory.");}
    portable_owner_barrier_and_queryless_restore();portable_release_latch_and_fade();strict_v10_and_legacy_profiles();
    if(!Runtime::available()){std::cout<<"3 portable foot-planting groups passed; native physics unavailable\n";return 0;}
    native_wrapper_drift_positive_control();native_moving_support_and_complete_cold();native_second_tick_whole_rollback();std::cout<<"6 foot-planting groups passed (3 portable, 3 actual native physics)\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
