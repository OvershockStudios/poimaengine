// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime_animation.hpp"
#include "poima/runtime_root_motion.hpp"
#include "poima/jobs.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
namespace {
using Json=nlohmann::json;using P=root_motion::Planar;
const double pi=std::acos(-1.);const std::string hash(64,'a');
std::string id(unsigned n){const auto s=std::to_string(n);return std::string(32-s.size(),'0')+s;}
void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
void near(double a,double b,double e=2e-7){check(std::isfinite(a)&&std::abs(a-b)<=e,"Independent root motion oracle differs.");}
template<class F>void rejects(F&& f){bool bad=false;try{f();}catch(const std::exception&){bad=true;}check(bad,"Invalid root motion input succeeded.");}
std::array<double,4> yaw(double a){return {0,std::sin(a/2),0,std::cos(a/2)};}
std::array<float,4> curve_key(const std::array<double,4>& q){return {static_cast<float>(q[0]),static_cast<float>(q[1]),static_cast<float>(q[2]),static_cast<float>(q[3])};}
Matrix4 pose_world(const std::vector<RuntimeAnimationPose>& poses,unsigned node){auto m=local_matrix(poses[node].local.position,poses[node].local.rotation,poses[node].local.scale);for(int parent=static_cast<int>(node)-1;parent>=0;--parent)m=multiply(local_matrix(poses[static_cast<std::size_t>(parent)].local.position,poses[static_cast<std::size_t>(parent)].local.rotation,poses[static_cast<std::size_t>(parent)].local.scale),m);return m;}
std::shared_ptr<ModelAsset> model(){
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(3);m->roots={0};m->nodes[0].name="Importer";m->nodes[0].parent=-1;
    m->nodes[1].name="Motion";m->nodes[1].parent=0;m->nodes[1].position={0,1,0};m->nodes[1].rotation=yaw(pi/6);
    m->nodes[2].name="Hand";m->nodes[2].parent=1;m->nodes[2].position={0,1,0};
    m->animations.push_back({"Straight",1,{{1,AnimationPath::translation,AnimationInterpolation::linear,{0,1},{{0,1,0,0},{1,1,0,0}}}}});
    m->animations.push_back({"Turn with offset",1,{
        {1,AnimationPath::translation,AnimationInterpolation::linear,{0,1},{{3,1,2,0},{4,1,2,0}}},
        {1,AnimationPath::rotation,AnimationInterpolation::linear,{0,1},{curve_key(yaw(pi/3)),curve_key(yaw(5*pi/6))}}}});
    m->animations.push_back({"Double speed",1,{{1,AnimationPath::translation,AnimationInterpolation::linear,{0,1},{{7,1,0,0},{9,1,0,0}}}}});
    m->animations.push_back({"Hand only",1,{{2,AnimationPath::translation,AnimationInterpolation::linear,{0,1},{{0,1,0,0},{0,2,0,0}}}}});
    return m;
}
RuntimeDefinition definition(bool character=false,bool turning=false){
    RuntimeDefinition d;d.world_id="root-motion-native";d.authored_revision=3;const auto m=model();
    RuntimeEntityDefinition rig;rig.id=id(1);rig.animation_rig=RuntimeAnimationRig{m};rig.animation_rig->clip=turning?1:0;rig.animation_rig->playing=true;
    rig.animation_rig->root_motion=RuntimeRootMotionDefinition{1,character?RootMotionMode::Character:RootMotionMode::Extract,character?id(100):"",true,turning};
    if(character){rig.parent=id(100);RuntimeEntityDefinition actor;actor.id=id(100);actor.character=CharacterController{};d.entities.push_back(actor);}
    d.entities.push_back(rig);
    for(unsigned n=0;n<3;++n){RuntimeEntityDefinition node;node.id=id(n+2);node.parent=n?id(n+1):id(1);const auto& source=m->nodes[n];node.transform={source.position,source.rotation,source.scale};node.rig_node=RuntimeRigNode{id(1),n};d.entities.push_back(node);}
    return d;
}
void commit(RuntimeAnimations& a,std::uint64_t tick){const auto states=a.prepare_root_motion(tick);a.commit_root_motion(states,tick+1);a.sample(tick+1);}
void algebra_and_metric(){
    const P a{2,3,pi/3},b{4,-1,pi/4};const auto c=root_motion::compose(a,b);
    near(c.x,2+2-std::sqrt(3.)/2);near(c.z,3-2*std::sqrt(3.)-.5);near(c.yaw,7*pi/12);
    const auto i=root_motion::compose(a,root_motion::inverse(a));near(i.x,0);near(i.z,0);near(i.yaw,0);
    const auto squared=root_motion::power({1,0,pi/2},2);near(squared.x,1);near(squared.z,-1);near(squared.yaw,pi);
    const auto four=root_motion::power({1,0,pi/2},4);near(four.x,0);near(four.z,0);near(four.yaw,2*pi);
    auto metric=identity_matrix();metric[0]=2;metric[5]=2;metric[10]=2;root_motion::validate_metric(metric,true);
    metric[4]=.1;rejects([&]{root_motion::validate_metric(metric,true);});metric=identity_matrix();metric[0]=-1;rejects([&]{root_motion::validate_metric(metric);});
    rejects([&]{root_motion::power(a,9007199254740992ULL);});rejects([&]{root_motion::validate_delta({2,0,0});});
}
void straight_loops_and_read_purity(){
    auto d=definition();RuntimeAnimations a(d);a.sample(0);const auto initial=a.save_state(0);check(Json::parse(initial)["version"]==5,"Root save is not v5.");
    const auto p=a.prepare_root_motion(0);const auto second=a.prepare_root_motion(0);check(a.save_state(0)==initial&&p[0].local_delta==second[0].local_delta&&a.root_motion_state(id(1))->to_tick==0,"Preparation or read consumed a Tick.");
    // Clip translation in model +X is converted through inverse start/root rest.
    near(p[0].local_delta[0],Runtime::fixed_dt);near(p[0].local_delta[2],0);
    for(unsigned t=0;t<121;++t){const auto states=a.prepare_root_motion(t);near(states[0].local_delta[0],Runtime::fixed_dt);a.commit_root_motion(states,t+1);const auto poses=a.sample(t+1);const auto world=pose_world(poses,1);near(world[12],0);near(world[13],1);}
    rejects([&]{a.prepare_root_motion(120);});const auto saved=a.save_state(121);RuntimeAnimations fresh(d);fresh.load_state(saved,121);fresh.sample(121);check(fresh.save_state(121)==saved,"Fresh root state continuation changed bytes.");
    check(fresh.prepare_root_motion(121)[0].local_delta==a.prepare_root_motion(121)[0].local_delta,"Fresh root interval differs.");
}
void nonzero_heading_turn_and_cycles(){
    auto d=definition(false,true);RuntimeAnimations a(d);const auto m=model();CompiledAnimation compiled(*m);std::vector<NodePose> baseline;std::vector<int> parents;
    for(const auto& n:m->nodes){baseline.push_back({n.position,n.scale,n.rotation});parents.push_back(n.parent);}
    const auto rest=root_motion::selected(compiled.sample({},0,false,baseline),parents,1,true);
    AnimationCommand control{id(1),1,0,1,true,true};P accumulated;
    for(unsigned t=0;t<120;++t){const auto delta=root_motion::interval(compiled,baseline,parents,1,true,rest,1,control,0,t);accumulated=root_motion::compose(accumulated,delta);near(delta.yaw,pi/120);}
    // Independent 2D matrix construction for two cycles with nonzero clip/rest headings.
    const auto start=local_matrix({3,1,2},yaw(pi/3),{1,1,1}),end=local_matrix({4,1,2},yaw(5*pi/6),{1,1,1});
    const auto cycle=multiply(end,inverse_affine(start));const auto rest_matrix=local_matrix({0,0,0},yaw(pi/6),{1,1,1});
    const auto expected=multiply(multiply(multiply(multiply(rest_matrix,inverse_affine(start)),multiply(cycle,cycle)),start),inverse_affine(rest_matrix));
    near(accumulated.x,expected[12]);near(accumulated.z,expected[14]);near(std::sin(accumulated.yaw),expected[8]);near(std::cos(accumulated.yaw),expected[0]);
    control.time=.999;control.speed=8;const auto fast=root_motion::interval(compiled,baseline,parents,1,true,rest,1,control,0,0);near(fast.yaw,pi/15);
    control.loop=false;control.time=1;near(root_motion::interval(compiled,baseline,parents,1,true,rest,1,control,0,0).x,0);
    control.loop=true;control.speed=0;near(root_motion::interval(compiled,baseline,parents,1,true,rest,1,control,0,0).yaw,0);
    control.speed=8;control.time=1e9;rejects([&]{root_motion::interval(compiled,baseline,parents,1,true,rest,1,control,0,0);});
}
void commands_fades_and_inertial(){
    auto d=definition();RuntimeAnimations a(d);commit(a,0);const auto prior=a.save_state(1);
    AnimationCommand c{id(1),2,0,1,true,true};c.blend_ticks=4;a.apply({c},1);auto states=a.prepare_root_motion(1);near(states[0].local_delta[0],1.125*Runtime::fixed_dt);a.commit_root_motion(states,2);a.sample(2);
    c.clip=0;c.blend_ticks=4;a.apply({c},2);states=a.prepare_root_motion(2);near(states[0].local_delta[0],(1.125*.875+1*.125)*Runtime::fixed_dt);
    const auto interrupted=a.save_state(2);RuntimeAnimations fresh(d);fresh.load_state(interrupted,2);check(fresh.prepare_root_motion(2)[0].local_delta==states[0].local_delta,"Interrupted fade did not restore.");
    c.clip=2;c.blend_ticks=0;c.time=.8;a.apply({c},2);states=a.prepare_root_motion(2);near(states[0].local_delta[0],2*Runtime::fixed_dt);check(a.root_motion_state(id(1))->to_tick==2,"Seek moved committed root state.");
    a.commit_root_motion(states,3);a.sample(3);c.clip=0;c.time=0;c.blend_ticks=4;c.transition_mode=AnimationTransitionMode::Inertial;a.apply({c},3);
    states=a.prepare_root_motion(3);const double average=root_motion::decay_average(0,.25);near(states[0].local_delta[0],(1+average)*Runtime::fixed_dt);
    const auto inertial=a.save_state(3);fresh.load_state(inertial,3);check(fresh.prepare_root_motion(3)[0].local_delta==states[0].local_delta,"Inertial root state did not restore.");
    for(unsigned t=3;t<7;++t)commit(a,t);near(a.prepare_root_motion(7)[0].local_delta[0],Runtime::fixed_dt);
    auto cp=a.checkpoint();a.apply_root_motion({{id(1),false}});check(a.root_motion_state(id(1))->enabled,"Staged enable leaked into getter.");states=a.prepare_root_motion(7);check(!states[0].enabled&&states[0].local_delta==std::array<double,3>{},"Disabled root emitted movement.");a.commit_root_motion(states,8);a.sample(8);near(pose_world(a.sample(8),1)[12],0);a.restore(cp);check(a.root_motion_state(id(1))->to_tick==7,"Root checkpoint did not restore.");
    rejects([&]{a.apply_root_motion({{id(1),true},{id(1),false}});});check(a.root_motion_state(id(1))->to_tick==7,"Rejected command altered state.");(void)prior;
}
void layers_masks_and_yaw_admission(){
    auto d=definition();RuntimeAnimationLayer layer;layer.clip=3;layer.playing=true;layer.mask={{2,1}};d.entities[0].animation_rig->layers={layer};RuntimeAnimations a(d);for(unsigned t=0;t<30;++t)commit(a,t);near(a.sample(30)[2].local.position[1],1.5);
    for(unsigned node:{0u,1u}){auto bad=d;bad.entities[0].animation_rig->layers[0].weight=0;bad.entities[0].animation_rig->layers[0].mask={{node,1}};rejects([&]{RuntimeAnimations r(bad);});}
    auto turning=definition(false,true);auto source=std::const_pointer_cast<ModelAsset>(turning.entities[0].animation_rig->model);
    source->animations[1].channels[1].values[1]=curve_key(yaw(pi+pi/6));rejects([&]{RuntimeAnimations r(turning);}); // 150deg/sec * maximum speed8 exceeds20rad/sec.
    auto with_ik=definition();with_ik.entities[0].animation_rig->ik={{1,0,1,2,true,IkSpace::Rig,{0,2,0},{0,0,1},1}};RuntimeAnimations solved(with_ik);
    for(unsigned tick=0;tick<30;++tick){const auto states=solved.prepare_root_motion(tick);near(states[0].local_delta[0],Runtime::fixed_dt);solved.commit_root_motion(states,tick+1);solved.sample(tick+1);near(solved.ik_state(id(1),1)->end_position[0],0);near(solved.ik_state(id(1),1)->end_position[1],2);}
}
void strict_profiles_and_saved_state(){
    auto d=definition(false,true);auto m=std::const_pointer_cast<ModelAsset>(d.entities[0].animation_rig->model);m->animations[1].channels[1].values[1]=curve_key({std::sin(.1),0,0,std::cos(.1)});rejects([&]{RuntimeAnimations a(d);});
    d=definition(false,true);m=std::const_pointer_cast<ModelAsset>(d.entities[0].animation_rig->model);m->animations[1].channels[1].interpolation=AnimationInterpolation::step;rejects([&]{RuntimeAnimations a(d);});
    d=definition(false,true);m=std::const_pointer_cast<ModelAsset>(d.entities[0].animation_rig->model);m->animations[1].channels[1].values[1]=curve_key(yaw(pi/3+pi));rejects([&]{RuntimeAnimations a(d);});
    d=definition();RuntimeAnimations a(d);commit(a,0);const auto before=a.save_state(1);const auto value=Json::parse(before);
    for(unsigned kind=0;kind<7;++kind){auto bad=value;auto& root=bad["rigs"][0]["root_motion"];if(kind==0)root["binding"]["node"]=1.;if(kind==1)root["binding"]["actor"]=id(100);if(kind==2)root["state"]["to_tick"]=0;if(kind==3)root["enabled"]=1;if(kind==4)root["state"]["world_applied"]=Json::array({1,0,0});if(kind==5)root["last_velocity"]=Json::array({1,2});if(kind==6)root["extra"]=1;rejects([&]{a.load_state(bad.dump(),1);});check(a.save_state(1)==before,"Invalid saved root state published.");}
    auto ordinary=definition();ordinary.entities[0].animation_rig->root_motion.reset();RuntimeAnimations legacy(ordinary);check(Json::parse(legacy.save_state(0))["version"]==1,"Omitted root changed legacy format.");
}
void jobs_membership_and_aliases(){
    auto d=definition();RuntimeAnimations serial(d);commit(serial,0);const auto expected=serial.save_state(1);
    for(unsigned count:{1u,3u}){jobs::Config config;config.workers=count;auto executor=std::make_shared<jobs::Executor>(config);RuntimeAnimations parallel(d,executor);commit(parallel,0);check(parallel.save_state(1)==expected,"Parallel root state differs.");}
    auto model_alias=std::const_pointer_cast<ModelAsset>(d.entities[0].animation_rig->model);model_alias->animations[0].channels[0].values[1][0]=50;near(serial.prepare_root_motion(1)[0].local_delta[0],Runtime::fixed_dt);
    auto clean=definition();RuntimeAnimations current(clean);commit(current,0);const auto cp=current.checkpoint();current.rebind(clean,1);check(current.save_state(1)==expected,"Surviving root playback reset.");
    auto removed=clean;removed.entities.clear();current.rebind(removed,1);check(!current.root_motion_state(id(1)),"Removed root state survived.");current.rebind(clean,1);check(current.root_motion_state(id(1))->from_tick==1&&current.root_motion_state(id(1))->to_tick==1,"Reborn root did not start neutral.");auto restore=cp;current.restore(restore);check(current.save_state(1)==expected,"Membership rollback lost root state.");
}
void character_physics_and_conflicts(){
    auto d=definition(true);RuntimeEntityDefinition floor;floor.id=id(200);floor.transform.position={0,-.5,0};floor.collider=BoxCollider{{20,.5,20},BodyMotion::Static};d.entities.push_back(floor);
    Runtime r(d);r.step(30,{});near(r.entity(id(100)).world[12],.5,.02);near(r.root_motion(id(1))->local_delta[0],Runtime::fixed_dt);check(r.root_motion(id(1))->world_applied[0]>0,"Character did not apply root motion.");
    const auto before=r.save_snapshot(hash);RuntimeInput input;input.entity=id(100);input.move={1,0};rejects([&]{r.step(1,{input});});check(r.save_snapshot(hash)==before,"Character conflict did not roll back.");
    input.move={0,0};input.jump=true;r.step(1,{input});check(r.entity(id(100)).world[13]>0,"Root Character lost jump/vertical velocity.");
    r.step(1,{}, {}, {}, {}, {}, {},{{id(1),false}});check(!r.root_motion(id(1))->enabled&&r.root_motion(id(1))->world_applied==std::array<double,3>{},"Disabled Character diagnostics are not neutral.");
    input.jump=false;input.move={1,0};r.step(2,{input});check(r.entity(id(100)).world[12]>.5,"Disabled root prevented ordinary input.");
    const auto saved=r.save_snapshot(hash);auto fresh=Runtime::from_snapshot(d,hash,saved);check(fresh->save_snapshot(hash)==saved,"Character root snapshot differs after restore.");r.step(2,{});fresh->step(2,{});check(r.save_snapshot(hash)==fresh->save_snapshot(hash),"Character fresh continuation differs.");
}
void wall_yaw_pivot_and_atomic_batch(){
    auto d=definition(true);RuntimeEntityDefinition floor;floor.id=id(200);floor.transform.position={0,-.5,0};floor.collider=BoxCollider{{20,.5,20},BodyMotion::Static};d.entities.push_back(floor);RuntimeEntityDefinition wall;wall.id=id(201);wall.transform.position={1,1,0};wall.collider=BoxCollider{{.1,2,2},BodyMotion::Static};d.entities.push_back(wall);
    d.entities[4].hit_region=RuntimeHitRegion{id(100),"hand",HitRegionShape::Sphere};d.entities[4].hit_region->radius=.2;
    Runtime r(d);r.step(120,{});check(r.entity(id(100)).world[12]<1&&r.root_motion(id(1))->translation_error>.005,"Wall did not separate requested and actual motion.");
    const auto hand=r.entity(id(4)).world;near(hand[12],r.entity(id(100)).world[12],2e-6);RuntimeRay ray;ray.origin={hand[12],hand[13],2};ray.direction={0,0,-1};ray.distance=3;const auto hit=r.hit_test(ray);check(hit&&hit->region&&*hit->region=="hand","Root compensated HitRegion was not queried.");near(hit->distance,1.8,2e-6);
    auto turning=definition(true,true);turning.entities[1].transform.position={.4,0,0};Runtime t(turning);t.step(1,{});near(t.root_motion(id(1))->yaw_delta,pi/120);check(std::abs(t.root_motion(id(1))->world_requested[2])>1e-4,"Pivot conjugation lost transverse motion.");RuntimeInput input;input.entity=id(100);input.look={1,0};const auto before=t.save_snapshot(hash);rejects([&]{t.step(1,{input});});check(t.save_snapshot(hash)==before,"Owned yaw conflict changed state.");
}
void late_failure_and_template_lifetime(){
    auto broken=definition(true);auto source=std::const_pointer_cast<ModelAsset>(broken.entities[1].animation_rig->model);
    source->animations.push_back({"Bad second Tick",1./16,{{1,AnimationPath::scale,AnimationInterpolation::step,{0,1./32,1./16},{{1,1,1,0},{2,1,1,0},{2,1,1,0}}}}});
    broken.entities[1].animation_rig->clip=4;Runtime invalid(broken);const auto before=invalid.save_snapshot(hash);rejects([&]{invalid.step(2,{});});check(invalid.save_snapshot(hash)==before&&invalid.root_motion(id(1))->to_tick==0,"Later metric failure did not restore full batch.");
    const auto base=definition(true);RuntimeSpawnTemplate recipe;recipe.id=id(500);recipe.name="Root character graph";recipe.root=id(100);recipe.entities=base.entities;RuntimeDefinition d;d.world_id="root-template-native";d.templates={recipe};Runtime live(d);
    RuntimeTransform first_pose,second_pose;first_pose.position={0,3,0};second_pose.position={4,3,0};const auto births=live.change_structure(0,{{recipe.id,first_pose},{recipe.id,second_pose}},{});check(births.spawned.size()==2,"Two root Character instances did not spawn.");
    const auto first=births.spawned[0],second=births.spawned[1];const auto rig1=live.instance(first).nodes.at(id(1)),rig2=live.instance(second).nodes.at(id(1));
    check(live.root_motion(rig1)->actor==first&&live.root_motion(rig2)->actor==second,"Template root actor was not remapped.");live.step(2,{}, {}, {}, {}, {}, {},{{rig1,false}});check(!live.root_motion(rig1)->enabled&&live.root_motion(rig2)->enabled,"Root controls leaked between instances.");
    const auto saved=live.save_snapshot(hash);auto fresh=Runtime::from_snapshot(d,hash,saved);check(fresh->save_snapshot(hash)==saved,"Root template fresh restore differs.");live.step(1,{});fresh->step(1,{});check(live.save_snapshot(hash)==fresh->save_snapshot(hash),"Root template fresh continuation differs.");
    live.change_structure(1,{}, {first});rejects([&]{live.root_motion(rig1);});check(live.root_motion(rig2)->enabled,"Despawn changed surviving driver.");const auto reborn=live.change_structure(2,{{recipe.id,first_pose}},{}).spawned[0];check(reborn!=first,"Root actor identity resurrected.");const auto rig3=live.instance(reborn).nodes.at(id(1));check(live.root_motion(rig3)->enabled&&live.root_motion(rig3)->from_tick==live.inspect().tick&&live.root_motion(rig3)->from_tick==live.root_motion(rig3)->to_tick,"Reborn root did not use frozen defaults/neutral cursor.");
}
}
int main(){try{algebra_and_metric();straight_loops_and_read_purity();nonzero_heading_turn_and_cycles();commands_fades_and_inertial();layers_masks_and_yaw_admission();strict_profiles_and_saved_state();jobs_membership_and_aliases();if(Runtime::available()){character_physics_and_conflicts();wall_yaw_pivot_and_atomic_batch();late_failure_and_template_lifetime();std::cout<<"10 native root motion groups passed\n";}else std::cout<<"7 portable root motion groups passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
