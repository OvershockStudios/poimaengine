// SPDX-License-Identifier: Apache-2.0
// Actual native controller/physical ownership and complete checkpoints. The
// two-bone content is analytic; no C#, renderer or callback-preview claim.
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <span>

namespace {
using namespace poima;
using Json=nlohmann::json;
namespace pr=physical_rig;
using V=std::array<double,3>;
const std::string content(64,'c');
unsigned checks=0;
std::string id(unsigned value){char buffer[33];std::snprintf(buffer,sizeof(buffer),"%032x",value);return buffer;}
const auto floor_id=id(1),actor_id=id(100),rig_id=id(101),pelvis_id=id(102),tip_id=id(103),camera_id=id(104);
const auto actor_template=id(900),platform_template=id(901);
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
void near(double actual,double expected,double tolerance=2e-6){check(std::isfinite(actual)&&std::isfinite(expected)&&std::abs(actual-expected)<=tolerance,"Independent controller-handoff oracle differs.");}
template<class F>void rejects(F&& action){bool failed=false;try{action();}catch(const std::exception&){failed=true;}check(failed,"Invalid controller/physical ownership operation succeeded.");}
const char* uncommitted="Controller handoff requires a completed committed locomotion tick; birth plus activation in one tick is unsupported.";
template<class F>void rejects_uncommitted(F&& action){
    bool failed=false;try{action();}catch(const std::exception& error){failed=std::string(error.what()).find(uncommitted)!=std::string::npos;}
    check(failed,"Uncommitted handoff did not fail at the explicit completed-locomotion gate.");
}
std::shared_ptr<ModelAsset> model(){
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(2);m->roots={0};
    m->nodes[0].name="pelvis";m->nodes[1].name="tip";m->nodes[1].parent=0;m->nodes[1].position={0,.5,0};return m;
}
pr::Configuration profile(const std::string& actor){
    pr::Configuration result;result.controller_handoff=pr::ControllerHandoff{actor};result.profile.root="pelvis";
    pr::Part root;root.name="pelvis";root.node=0;root.shape=pr::Sphere{.12};root.mass=3;
    root.linear_damping=0;root.angular_damping=0;
    auto tip=root;tip.name="tip";tip.node=1;tip.shape=pr::Sphere{.1};tip.mass=1;
    result.profile.parts={root,tip};
    pr::Joint joint;joint.name="spine";joint.parent="pelvis";joint.child="tip";
    joint.parent_frame.position={0,.25,0};joint.child_frame.position={0,-.25,0};
    joint.limits=pr::SwingTwist{1,1,-.8,.8};joint.friction_torque=0;result.profile.joints={joint};return result;
}
std::vector<RuntimeEntityDefinition> character(const std::shared_ptr<const ModelAsset>& m,unsigned first,double x=0,double y=.1){
    RuntimeEntityDefinition actor;actor.id=id(first);actor.transform.position={x,y,0};actor.character=CharacterController{};
    actor.character->speed=2;actor.character->locomotion=RuntimeCharacterLocomotionProfile{};
    actor.character->stance=RuntimeCharacterStanceProfile{1,1,.5f};actor.character->camera=id(first+4);
    // Offset the small physical skeleton from the capsule. A shoulder-height
    // side ray can witness the capsule independently of either sphere.
    RuntimeEntityDefinition rig;rig.id=id(first+1);rig.parent=actor.id;rig.transform.position={.8,1,0};
    rig.animation_rig=RuntimeAnimationRig{m};rig.physical_rig=profile(actor.id);
    RuntimeEntityDefinition root;root.id=id(first+2);root.parent=rig.id;root.rig_node=RuntimeRigNode{rig.id,0};
    RuntimeEntityDefinition tip;tip.id=id(first+3);tip.parent=root.id;tip.transform.position={0,.5,0};tip.rig_node=RuntimeRigNode{rig.id,1};
    RuntimeEntityDefinition camera;camera.id=id(first+4);camera.parent=actor.id;camera.transform.position={.05,1.6,.02};camera.camera=RuntimeCamera{};
    return {actor,rig,root,tip,camera};
}
RuntimeDefinition definition(bool two=false,bool raised=false){
    RuntimeDefinition d;d.world_id="controller-physical-native";d.authored_revision=17;
    RuntimeEntityDefinition floor;floor.id=floor_id;floor.transform.position={0,-.5,0};floor.collider=BoxCollider{{30,.5f,30},BodyMotion::Static};d.entities={floor};
    const auto m=model();auto main=character(m,100,two?-4:0,raised?1.1:.1);d.entities.insert(d.entities.end(),main.begin(),main.end());
    if(two){auto other=character(m,200,4);d.entities.insert(d.entities.end(),other.begin(),other.end());}
    RuntimeSpawnTemplate recipe;recipe.id=actor_template;recipe.name="linked virtual actor";recipe.root=id(1000);recipe.entities=character(m,1000);d.templates={recipe};
    RuntimeSpawnTemplate support;support.id=platform_template;support.name="retirable support";support.collider=BoxCollider{{2,.5f,2},BodyMotion::Kinematic};support.transform.position={0,.5,0};d.templates.push_back(support);return d;
}
void step(Runtime& r,std::uint32_t ticks=1,const std::vector<RuntimeInput>& input={},
    const std::vector<PhysicalRigCommand>& physical={},const std::vector<RuntimeCharacterStanceCommand>& stance={},
    const std::vector<RuntimeStructureTick>& structure={}){
    r.step(ticks,input,{}, {}, {},structure,{}, {}, {},stance,{}, {}, {},physical);
}
RuntimeInput input(const std::string& actor=actor_id,float yaw=0,bool jump=false){RuntimeInput i;i.entity=actor;i.move={1,0};i.look={yaw,0};i.jump=jump;return i;}
Json saved(const Runtime& r){return Json::parse(r.save_snapshot(content));}
const Json& physical_row(const Json& envelope,const std::string& rig=rig_id){
    for(const auto& row:envelope.at("payload").at("physical").at("rigs"))if(row.at("entity")==rig)return row;
    throw std::runtime_error("Physical handoff row missing.");
}
const Json& entity_row(const Json& envelope,const std::string& entity=actor_id){
    for(const auto& row:envelope.at("payload").at("entities"))if(row.at("id")==entity)return row;
    throw std::runtime_error("Saved actor row missing.");
}
std::uint32_t body_id(const Json& envelope,const std::string& entity){
    for(const auto& row:envelope.at("payload").at("physical").at("bootstrap").at("bodies"))if(row.at("entity")==entity)return row.at("id").get<std::uint32_t>();
    throw std::runtime_error("Exact ordinary body binding missing.");
}
bool enrolled(const Json& envelope,std::uint32_t raw){
    for(const auto& row:envelope.at("payload").at("physical").at("physics").at("bodies"))if(row.at("id")==raw)return true;return false;
}
RuntimeRay capsule_ray(const Runtime& r,const std::string& actor=actor_id){
    const auto p=r.entity(actor).world;const auto stance=r.character_stance(actor);
    const double height=stance&&stance->crouched?.65:1.25;
    return {{p[12],p[13]+height,p[14]+2},{0,0,-1},4,{floor_id}};
}
void same(const Runtime& a,const Runtime& b){
    check(a.save_snapshot(content)==b.save_snapshot(content),"Complete joined archive differs; no state fields may be excluded.");
    check(a.live_definition().entities.size()==b.live_definition().entities.size(),"Compared live membership differs.");
    for(const auto& definition:a.live_definition().entities){
        const auto x=a.entity(definition.id),y=b.entity(definition.id);
        check(x.world==y.world&&x.local.position==y.local.position&&x.local.rotation==y.local.rotation&&x.local.scale==y.local.scale&&
            x.velocity==y.velocity&&x.ground==y.ground&&x.yaw==y.yaw&&x.pitch==y.pitch,
            "Compared native entity observation differs.");
    }
}
V origin(const Runtime& r,const std::string& entity){const auto m=r.entity(entity).world;return {m[12],m[13],m[14]};}
void check_follow(const Runtime& r,const Json& initial,const std::string& actor=actor_id,const std::string& rig=rig_id){
    const auto now=saved(r);const auto& handoff=physical_row(now,rig).at("handoff");const auto& original=physical_row(initial,rig).at("handoff");
    check(handoff==original,"Suppressed motor proxy, cached support, follow reference or takeover identity changed.");
    const auto a=r.entity(actor),w=r.entity(rig);const auto offset=original.at("passive").at("follow").at("actor_minus_wrapper_world").get<V>();
    for(unsigned axis=0;axis<3;++axis)near(a.world[12+axis],w.world[12+axis]+offset[axis],2e-7);
    const auto& previous=entity_row(initial,actor);near(a.yaw,previous.at("controller").at("yaw").get<double>(),0);near(a.pitch,previous.at("controller").at("pitch").get<double>(),0);
    check(entity_row(now,actor).at("controller")==previous.at("controller"),"Suppressed native motor/stance/view state changed.");
    check(a.velocity==previous.at("controller").at("motor").at("linear_velocity").get<V>(),"Suppressed observation lost the last resolved locomotion velocity.");
    const auto& support=original.at("passive").at("support");
    const std::array<const char*,4> ground_names{"on_ground","steep_ground","not_supported","in_air"};
    const auto ground=support.at("ground_state").get<unsigned>();check(ground<ground_names.size()&&a.ground==ground_names[ground],"Suppressed observation recomputed historical grounding.");
    check(a.locomotion&&a.locomotion->ground_normal==support.at("normal").get<V>()&&a.locomotion->ground_velocity==support.at("velocity").get<V>(),"Suppressed observation changed cached support normal/velocity.");
    const auto q=original.at("passive").at("follow").at("actor_world_rotation").get<std::array<double,4>>();
    const auto heading=local_matrix({0,0,0},q,{1,1,1});
    for(unsigned column=0;column<3;++column)for(unsigned row=0;row<3;++row)near(a.world[column*4+row],heading[column*4+row],2e-7);
    const auto proxy=original.at("passive").at("proxy").at("body_id").get<std::uint32_t>();
    check(body_id(now,actor)==proxy&&!enrolled(now,proxy),"Suppressed capsule was reallocated or re-enrolled.");
}
void held_input_history_and_exact_continuation(){
    const auto d=definition();Runtime seed(d);const auto pristine=seed.save_snapshot(content);
    rejects_uncommitted([&]{step(seed,1,{},{{rig_id,true,{}}});});
    check(seed.save_snapshot(content)==pristine,"Initial uncommitted takeover changed the complete original runtime.");
    step(seed,120);
    check(seed.entity(actor_id).ground=="on_ground","Native controller did not settle before the grounded takeover case.");
    step(seed,1,{}, {},{{actor_id,true}});
    auto first=input(actor_id,12);first.look[1]=19;
    step(seed,1,{first});const std::array<V,2> previous{origin(seed,pelvis_id),origin(seed,tip_id)};
    step(seed,1,{input(actor_id,9)});const std::array<V,2> current{origin(seed,pelvis_id),origin(seed,tip_id)};
    const auto before=seed.save_snapshot(content);const auto before_json=Json::parse(before);
    check(before_json.at("payload").at("physical").at("version")==2u&&physical_row(before_json).at("handoff").at("passive").is_null(),"Animated linked owner did not use explicit extension2 phase.");
    check(entity_row(before_json).at("controller").at("pitch")==19&&entity_row(before_json).at("controller").at("crouched")==true,
        "Nonzero pre-death view and crouched-shape preconditions absent.");
    const auto proxy=body_id(before_json,actor_id);check(enrolled(before_json,proxy),"Locomotion proxy lacks real broadphase membership.");
    const auto ray=seed.raycast(capsule_ray(seed));check(ray&&ray->entity==actor_id,"Independent live capsule ray witness absent.");
    auto held=Runtime::from_snapshot(d,content,before),neutral=Runtime::from_snapshot(d,content,before);same(*held,*neutral);
    auto death=input(actor_id,77,true);death.look[1]=43;
    const auto tick=held->inspect().tick;
    step(*held,1,{death},{{rig_id,true,{}}},{{actor_id,false}});
    step(*neutral,1,{},{{rig_id,true,{}}});same(*held,*neutral);
    const auto activated=saved(*held);const auto& handoff=physical_row(activated).at("handoff");
    check(handoff.at("actor")==actor_id&&!handoff.at("passive").is_null()&&handoff.at("passive").at("takeover_tick")==tick,
        "Activation did not commit the named one-way ownership phase.");
    const auto& proxy_state=handoff.at("passive").at("proxy");const auto& old_body=entity_row(before_json).at("body");
    for(const auto* field:{"position","rotation","linear_velocity","angular_velocity"})check(proxy_state.at(field)==old_body.at(field),"Takeover changed the frozen native query proxy.");
    check(proxy_state.at("body_id")==proxy&&!proxy_state.at("active").get<bool>(),"Takeover lost the existing inactive capsule ID.");
    check(!held->raycast(capsule_ray(*held)),"Removed capsule remains a ghost obstacle in actual native rays.");
    const auto p=held->physical_part(rig_id,"pelvis");check(p&&p->enrolled,"Physical root was not enrolled at takeover.");
    const auto hit=held->raycast({{p->pose.position[0]+2,p->pose.position[1],p->pose.position[2]},{-1,0,0},4,{floor_id}});
    check(hit&&hit->entity==rig_id,"Takeover parts are not visible to actual native rays.");
    const auto parts=held->physical_rig(rig_id,true,false);check(parts&&parts->parts.size()==2,"Two-part native owner missing.");
    // No first-step contact or authored damping exists here. Joint impulses
    // cancel in aggregate; source COM finite differences already include motor
    // displacement and turning, so adding motor velocity again is incorrect.
    for(unsigned axis=0;axis<3;++axis){
        double expected=0,actual=0;for(unsigned n=0;n<2;++n){const double mass=n?1:3;expected+=mass*(current[n][axis]-previous[n][axis])/Runtime::fixed_dt;actual+=parts->parts[n].mass*parts->parts[n].linear_velocity[axis];}
        if(axis==1)expected+=4*static_cast<double>(-9.81f)*Runtime::fixed_dt;
        near(actual,expected,5e-4);
    }
    check_follow(*held,activated);
    const auto camera=held->entity(camera_id).local;
    auto cold=Runtime::from_snapshot(d,content,held->save_snapshot(content));same(*held,*cold);
    bool physical_contact=false;
    for(unsigned frame=0;frame<90;++frame){
        step(*held,1,{death},{},{{actor_id,(frame%2)==0}});step(*cold,1,{},{});same(*held,*cold);check_follow(*held,activated);
        const auto observed=held->entity(camera_id).local;check(observed.position==camera.position&&observed.rotation==camera.rotation&&observed.scale==camera.scale,"Suppressed input moved the owned camera.");
        const auto now=saved(*held);for(const auto& pair:now.at("payload").at("physical").at("physics").at("pairs")){
            const auto a=pair.at("body_a").get<std::uint32_t>(),b=pair.at("body_b").get<std::uint32_t>();
            check(a!=proxy&&b!=proxy,"Suppressed capsule generated a real contact cache pair.");
            for(const auto& part:parts->parts)physical_contact=physical_contact||a==part.body_id||b==part.body_id;
        }
    }
    check(physical_contact,"Cold/future comparison never reached real corpse contacts.");
}
void rollback_lifecycle_and_two_owner_order(){
    const auto d=definition(true);Runtime r(d);step(r,120);step(r,1,{input(actor_id,5),input(id(200),-7)});
    const auto before=r.save_snapshot(content);
    rejects([&]{step(r,3,{input(actor_id,31,true)},{{rig_id,true,{}}},{{actor_id,true}},{{2,99,{{platform_template,{}}},{}}});});
    check(r.save_snapshot(content)==before,"Later batch rejection failed to restore complete locomotion, capsule and rig ownership.");
    const auto restored_ray=r.raycast(capsule_ray(r));check(restored_ray&&restored_ray->entity==actor_id,"Rollback failed to restore actual capsule collision.");
    step(r,1,{},{{id(201),true,{}},{rig_id,true,{}}});
    const auto active=r.save_snapshot(content);auto cold=Runtime::from_snapshot(d,content,active);same(r,*cold);
    check(physical_row(Json::parse(active),rig_id).at("handoff").at("passive").is_object()&&physical_row(Json::parse(active),id(201)).at("handoff").at("passive").is_object(),"Alternating owner activation omitted a handoff.");
    for(unsigned frame=0;frame<45;++frame){step(r);step(*cold);same(r,*cold);}
    const auto start=r.save_snapshot(content);auto prediction=Runtime::from_snapshot(d,content,start);
    RuntimeTransform placement;placement.position={8,.1,0};const auto predicted=prediction->change_structure(prediction->structure_revision(),{{actor_template,placement}},{}).spawned.at(0);
    const auto predicted_rig=prediction->instance_node(predicted,id(1001));
    rejects_uncommitted([&]{step(r,1,{},{{predicted_rig,true,{}}},{},{{0,r.structure_revision(),{{actor_template,placement}},{}}});});
    check(r.save_snapshot(content)==start,"Same-Tick uncommitted birth/death changed existing complete state or allocation frontier.");
    const auto born=r.change_structure(r.structure_revision(),{{actor_template,placement}},{}).spawned.at(0);check(born==predicted,"Rejected birth/death consumed public IDs.");
    const auto born_rig=r.instance_node(born,id(1001));
    check(physical_row(saved(r),born_rig).at("handoff").at("actor")==born,"Hierarchical birth did not remap controller ownership.");
    step(r,3,{input(born)});step(r,1,{input(born,40,true)},{{born_rig,true,{}}});
    const auto all=r.save_snapshot(content);auto all_cold=Runtime::from_snapshot(d,content,all);same(r,*all_cold);
    const auto survivors=r.physical_rig(rig_id,true,false);const auto bodies=r.inspect().bodies;
    r.change_structure(r.structure_revision(),{}, {born});all_cold->change_structure(all_cold->structure_revision(),{}, {born});same(r,*all_cold);
    check(r.inspect().bodies+3==bodies,"Retirement leaked the allocated capsule or physical parts.");
    const auto surviving=r.physical_rig(rig_id,true,false);check(survivors&&surviving&&survivors->parts.size()==surviving->parts.size(),"Retirement lost a surviving owner.");
    for(unsigned n=0;n<survivors->parts.size();++n)check(survivors->parts[n].body_id==surviving->parts[n].body_id&&survivors->parts[n].pose.position==surviving->parts[n].pose.position,"Retiring a linked actor changed surviving body IDs/poses.");
    const auto retired=r.save_snapshot(content);auto reopened=Runtime::from_snapshot(d,content,retired);same(r,*reopened);
    const auto next=r.change_structure(r.structure_revision(),{{actor_template,placement}},{}).spawned.at(0);
    const auto next_cold=reopened->change_structure(reopened->structure_revision(),{{actor_template,placement}},{}).spawned.at(0);
    check(next==next_cold&&next!=born,"Retired linked allocation lineage differs after cold reconstruction.");same(r,*reopened);
}
void cached_retired_support_and_rejections(){
    const auto d=definition(false,true);Runtime r(d);
    const auto support=r.change_structure(0,{{platform_template,{}}},{}).spawned.at(0);step(r,120);
    near(origin(r,actor_id)[1],1,.04);check(r.entity(actor_id).ground=="on_ground","Retirable support was not actually used.");
    const auto before=saved(r);const auto support_body=body_id(before,support);
    step(r,1,{},{{rig_id,true,{}}});const auto takeover=saved(r);
    check(physical_row(takeover).at("handoff").at("passive").at("support").at("ground_body_id")==support_body,"Captured support does not identify the actual native platform.");
    r.change_structure(r.structure_revision(),{}, {support});const auto retired=r.save_snapshot(content);const auto retired_json=Json::parse(retired);
    check(physical_row(retired_json).at("handoff")==physical_row(takeover).at("handoff"),"Retiring support rewrote the frozen locomotion observation.");
    auto cold=Runtime::from_snapshot(d,content,retired);same(r,*cold);
    for(unsigned n=0;n<8;++n){step(r);step(*cold);same(r,*cold);check_follow(r,takeover);}
    const auto original=r.save_snapshot(content);
    for(unsigned mutation=0;mutation<7;++mutation){
        auto bad=Json::parse(original);auto& extension=bad["payload"]["physical"];auto& row=extension["rigs"][0];auto& passive=row["handoff"]["passive"];
        if(mutation==0)passive["proxy"]["body_id"]=std::uint32_t(passive["proxy"]["body_id"].get<std::uint32_t>()+1);
        if(mutation==1)passive["takeover_tick"]=std::uint64_t(r.inspect().tick+1);
        if(mutation==2)passive["proxy"]["linear_velocity"][0]=1.;
        if(mutation==3)passive["follow"]["actor_minus_wrapper_world"][0]=23.;
        if(mutation==4)row["handoff"]["actor"]=floor_id;
        if(mutation==5)row["handoff"]["passive"]=nullptr;
        if(mutation==6)extension["version"]=std::uint32_t(1);
        const auto payload=bad.at("payload").dump();bad["sha256"]=sha256(std::as_bytes(std::span(payload.data(),payload.size())));
        rejects([&]{(void)Runtime::from_snapshot(d,content,bad.dump());});check(r.save_snapshot(content)==original,"Detached invalid handoff archive changed the source runtime.");
    }
    auto malformed=input();malformed.move[0]=std::numeric_limits<float>::quiet_NaN();
    rejects([&]{step(r,1,{malformed});});check(r.save_snapshot(content)==original,"Suppression accepted malformed input or failed whole-state rejection.");
    rejects([&]{step(r,1,{input(),input()});});check(r.save_snapshot(content)==original,"Suppression hid a duplicate caller writer.");
    rejects([&]{step(r,1,{},{{rig_id,true,{}}});});check(r.save_snapshot(content)==original,"Repeated takeover changed complete saved state.");
}
}
int main(){
    try{check(Runtime::available(),"Native runtime is required.");held_input_history_and_exact_continuation();rollback_lifecycle_and_two_owner_order();cached_retired_support_and_rejections();std::cout<<"Controller handoff PASS "<<checks<<" checks across three native groups; no callback/render qualification\n";return 0;}
    catch(const std::exception& error){std::cerr<<"Controller handoff FAIL: "<<error.what()<<'\n';return 1;}
}
