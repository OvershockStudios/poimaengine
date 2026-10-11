// SPDX-License-Identifier: Apache-2.0
// Independent analytic IK, committed animation state and native lifecycle.
#include "poima/runtime_ik.hpp"
#include "poima/runtime_animation.hpp"
#include "poima/jobs.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <stdexcept>
using namespace poima;
namespace {
using Json=nlohmann::json;using V=std::array<double,3>;
const std::string hash(64,'a');
std::string id(unsigned n){auto value=std::to_string(n);return std::string(32-value.size(),'0')+value;}
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void near(double a,double b,double epsilon=2e-8){check(std::isfinite(a)&&std::abs(a-b)<=epsilon,"Independent IK oracle differs.");}
void near(const V& a,const V& b,double epsilon=2e-8){for(unsigned i=0;i<3;++i)near(a[i],b[i],epsilon);}
template<class F>void rejects(F&& f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"Invalid IK input succeeded.");}
V pos(const Matrix4& m){return {m[12],m[13],m[14]};}
V point(const Matrix4& m,const V& p){V r{};for(unsigned i=0;i<3;++i)r[i]=m[12+i]+m[i]*p[0]+m[4+i]*p[1]+m[8+i]*p[2];return r;}
bool equal(const RuntimeIkState& a,const RuntimeIkState& b){return a.entity==b.entity&&a.slot==b.slot&&a.root==b.root&&a.middle==b.middle&&a.end==b.end&&a.enabled==b.enabled&&a.space==b.space&&a.target==b.target&&a.pole==b.pole&&a.end_position==b.end_position&&a.weight==b.weight&&a.target_error==b.target_error&&a.clamped==b.clamped&&a.pole_fallback==b.pole_fallback;}
const std::vector<int> parents{-1,0,1};
RuntimeTwoBoneIk chain(){return {1,0,1,2,true,IkSpace::Rig,{1,1,0},{0,0,1},1};}
RuntimeIkCommand goal(V target={1,1,0},V pole={0,0,1},double weight=1,IkSpace space=IkSpace::Rig){return {id(1),1,true,space,target,pole,weight};}
ModelPose pose(double first=1,double second=1){ModelPose p;p.local.resize(3);p.local[1].position={0,first,0};p.local[2].position={0,second,0};return p;}
void reachable_and_clamp(){
    auto p=pose();const auto state=ik::solve(parents,p,chain(),goal(),identity_matrix());near(state.end_position,{1,1,0});near(state.target_error,0);check(!state.clamped&&!state.pole_fallback,"Reachable solution incorrectly classified.");
    // Law-of-cosines midpoint is (1/2,1/2,sqrt(1/2)), independent
    // of the solver's quaternion construction.
    near(pos(p.world[1]),{.5,.5,std::sqrt(.5)});
    auto far=pose();const auto outer=ik::solve(parents,far,chain(),goal({0,9,0}),identity_matrix());near(outer.end_position,{0,2,0});near(outer.target_error,7);check(outer.clamped,"Outer reach was not clamped.");
    auto short_link=pose(2,1);const auto inner=ik::solve(parents,short_link,chain(),goal({0,.25,0}),identity_matrix());near(inner.end_position,{0,1,0});near(inner.target_error,.75);check(inner.clamped,"Inner reach was not clamped.");
    auto boundary=pose(2,1);const auto boundary_state=ik::solve(parents,boundary,chain(),goal({0,1,0}),identity_matrix());check(!boundary_state.clamped,"Exact inner boundary classified outside reach.");
}
void fold_antipodal_and_pole(){
    auto folded=pose();const auto fold=ik::solve(parents,folded,chain(),goal({0,0,0},{0,0,0}),identity_matrix());near(fold.end_position,{0,0,0});check(!fold.clamped&&fold.pole_fallback,"Equal-length fold fallback wrong.");
    auto opposite=pose();const auto antipodal=ik::solve(parents,opposite,chain(),goal({0,-2,0},{0,0,1}),identity_matrix());near(antipodal.end_position,{0,-2,0});near(antipodal.target_error,0);
    auto almost=pose();const V target{2e-7,-std::sqrt(4.-4e-14),0};near(ik::solve(parents,almost,chain(),goal(target),identity_matrix()).end_position,target,2e-8);
    auto a=pose(),b=pose();const auto x=ik::solve(parents,a,chain(),goal({1,0,0},{2,0,0}),identity_matrix());const auto y=ik::solve(parents,b,chain(),goal({1,0,0},{2,0,0}),identity_matrix());
    check(x.pole_fallback&&equal(x,y)&&a.local[0].rotation==b.local[0].rotation&&a.local[1].rotation==b.local[1].rotation,"Pole fallback is not deterministic.");near(pos(a.world[1]),{.5,std::sqrt(.75),0});
}
void weight_and_field_fidelity(){
    auto p=pose();p.local[2].rotation={.2,.3,.4,.5};p.local[2].scale={1,2,3};const auto original=p.local;
    auto disabled=goal();disabled.enabled=false;(void)ik::solve(parents,p,chain(),disabled,identity_matrix());
    check(std::memcmp(p.local.data(),original.data(),original.size()*sizeof(NodePose))==0,"Disabled IK changed incoming local bytes.");
    (void)ik::solve(parents,p,chain(),goal({1,1,0},{0,0,1},0),identity_matrix());check(std::memcmp(p.local.data(),original.data(),original.size()*sizeof(NodePose))==0,"Zero-weight IK changed incoming local bytes.");
    (void)ik::solve(parents,p,chain(),goal({1,1,0},{0,0,1},.5),identity_matrix());
    for(unsigned n=0;n<3;++n)check(p.local[n].position==original[n].position&&p.local[n].scale==original[n].scale,"IK altered translation or scale.");check(p.local[2].rotation==original[2].rotation,"IK changed end rotation.");
    // The actual root swing is 60 degrees; weight .5 publishes 30 degrees.
    near(p.world[1][13],std::cos(std::acos(.5)*.5));
}
void affine_spaces(){
    auto world=identity_matrix();world[0]=2;world[4]=.4;world[5]=3;world[10]=.5;world[12]=7;world[13]=-2;world[14]=4;
    const V target=point(world,{1,1,0}),pole=point(world,{0,0,1});auto p=pose();const auto state=ik::solve(parents,p,chain(),goal(target,pole,1,IkSpace::World),world);
    near(state.end_position,target);near(state.target_error,0);check(!state.clamped,"Affine wrapper distorted rig reach.");
    auto rig_pose=pose();near(ik::solve(parents,rig_pose,chain(),goal(),world).end_position,target);
    auto distant=pose();const auto clamped=ik::solve(parents,distant,chain(),goal(point(world,{0,4,0}),pole,1,IkSpace::World),world);near(clamped.end_position,point(world,{0,2,0}));near(clamped.target_error,std::hypot(.8,6.));check(clamped.clamped,"World target not clamped in rig metric.");
    auto reflected=world;reflected[0]=-2;rejects([&]{auto q=pose();ik::solve(parents,q,chain(),goal(),reflected);});
}
void malformed_and_metric(){
    const auto rest=pose().local;ik::validate_chains(parents,rest,std::vector<RuntimeTwoBoneIk>{chain()});
    for(unsigned kind=0;kind<6;++kind){auto c=chain();if(kind==0)c.slot=0;if(kind==1)c.end=1;if(kind==2)c.middle=2;if(kind==3)c.weight=2;if(kind==4)c.target[1]=std::numeric_limits<double>::quiet_NaN();if(kind==5)c.space=static_cast<IkSpace>(2);rejects([&]{ik::validate_chains(parents,rest,std::vector<RuntimeTwoBoneIk>{c});});}
    auto overlap=chain();overlap.slot=2;rejects([&]{ik::validate_chains(parents,rest,std::vector<RuntimeTwoBoneIk>{chain(),overlap});});
    auto zero=pose(0,1);rejects([&]{ik::validate_chains(parents,zero.local,std::vector<RuntimeTwoBoneIk>{chain()});});
    for(const auto scale:{V{2,1,1},V{-1,-1,-1},V{0,0,0}}){auto p=pose();p.local[0].scale=scale;rejects([&]{ik::solve(parents,p,chain(),goal(),identity_matrix());});}
    auto inactive=pose();inactive.local[0].scale={2,1,1};auto c=goal();c.enabled=false;(void)ik::solve(parents,inactive,chain(),c,identity_matrix());
    const std::vector<int> separate{-1,0,1,-1,3,4};auto dual=pose();const auto second_rest=pose().local;dual.local.insert(dual.local.end(),second_rest.begin(),second_rest.end());dual.local[3].position={3,0,0};auto other=chain();other.slot=2;other.root=3;other.middle=4;other.end=5;
    ik::validate_chains(separate,dual.local,std::vector<RuntimeTwoBoneIk>{chain(),other});ik::solve(separate,dual,chain(),goal(),identity_matrix());auto second=goal({4,1,0});second.slot=2;near(ik::solve(separate,dual,other,second,identity_matrix()).end_position,{4,1,0});near(pos(dual.world[2]),{1,1,0});
}
std::shared_ptr<ModelAsset> model(){
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(3);m->roots={0};for(unsigned n=0;n<3;++n){m->nodes[n].name="Joint "+std::to_string(n);m->nodes[n].parent=n?static_cast<int>(n-1):-1;if(n)m->nodes[n].position={0,1,0};}
    m->nodes[0].skin=0;m->nodes[0].primitives={0};auto mesh=std::make_shared<MeshAsset>();MeshVertex vertex;vertex.position={0,2,0};vertex.normal={0,0,1};vertex.tangent={1,0,0,1};mesh->vertices={vertex,vertex,vertex};mesh->indices={0,1,2};mesh->has_uv=true;mesh->influences.assign(3,SkinWeight{{2,0,0,0},{1,0,0,0}});m->primitives={mesh};
    auto mid=identity_matrix(),end=identity_matrix();mid[13]=-1;end[13]=-2;m->skins.push_back({"Three bones",0,{0,1,2},{identity_matrix(),mid,end}});
    m->animations.push_back({"Uniform growth",1,{{0,AnimationPath::scale,AnimationInterpolation::linear,{0,1},{{1,1,1,0},{2,2,2,0}}}}});
    m->animations.push_back({"Invalid later metric",1./32,{{0,AnimationPath::scale,AnimationInterpolation::step,{0,1./64,1./32},{{1,1,1,0},{1,1,1,0},{2,1,1,0}}}}});return m;
}
RuntimeDefinition definition(bool configured=true){
    RuntimeDefinition d;d.world_id="two-bone-native";d.authored_revision=3;const auto m=model();
    RuntimeEntityDefinition rig;rig.id=id(1);rig.animation_rig=RuntimeAnimationRig{m};if(configured)rig.animation_rig->ik={chain()};d.entities.push_back(rig);
    for(unsigned n=0;n<3;++n){RuntimeEntityDefinition node;node.id=id(n+2);node.parent=n?id(n+1):id(1);node.transform.position=m->nodes[n].position;node.rig_node=RuntimeRigNode{id(1),n};d.entities.push_back(node);}
    RuntimeEntityDefinition render;render.id=id(5);render.parent=id(2);render.mesh=RuntimeMesh{};render.mesh->mesh=m->primitives[0];render.skinned_mesh=RuntimeSkinnedMesh{id(1),0};d.entities.push_back(render);
    RuntimeEntityDefinition camera;camera.id=id(6);camera.camera=RuntimeCamera{};camera.transform.position={0,0,8};d.entities.push_back(camera);return d;
}
void animation_state_and_jobs(){
    auto d=definition();RuntimeAnimations a(d);check(a.has_ik(),"Configured rig has no IK.");const auto initial=a.sample(0);const auto state=*a.ik_state(id(1),1);near(state.end_position,{1,1,0});
    const auto before=a.save_state(0);const auto encoded=Json::parse(before);check(encoded["version"]==4&&encoded["rigs"][0]["ik"].size()==1,"IK save is not version4.");check(!encoded["rigs"][0]["ik"][0].contains("end_position"),"Derived diagnostics persisted.");
    const auto cp=a.checkpoint();auto restore=cp;a.apply_ik({goal({-1,1,0})});check(equal(*a.ik_state(id(1),1),state),"Uncommitted command leaked through getter.");a.sample(0);near(a.ik_state(id(1),1)->end_position,{-1,1,0});a.restore(restore);check(a.save_state(0)==before&&equal(*a.ik_state(id(1),1),state),"Checkpoint did not restore controls and diagnostics.");
    for(unsigned kind=0;kind<5;++kind){auto bad=encoded;auto& c=bad["rigs"][0]["ik"][0];if(kind==0)c["end"]=1;if(kind==1)c["weight"]=2;if(kind==2)c["enabled"]=1;if(kind==3)c["target"]=Json::array({1,2});if(kind==4)c["cache"]=1;rejects([&]{a.load_state(bad.dump(),0);});check(a.save_state(0)==before,"Rejected saved control mutated state.");}
    RuntimeAnimations restored(d);restored.load_state(before,0);restored.sample(0);check(restored.save_state(0)==before&&equal(*restored.ik_state(id(1),1),state),"Animation control roundtrip differs.");
    for(unsigned workers:{1u,3u}){jobs::Config config;config.workers=workers;auto executor=std::make_shared<jobs::Executor>(config);RuntimeAnimations parallel(d,executor);const auto owner=std::this_thread::get_id();auto matrix=identity_matrix();unsigned calls=0;
        const auto poses=parallel.sample(0,[&](const std::string& entity)->const Matrix4&{check(std::this_thread::get_id()==owner&&entity==id(1),"Wrapper provider called from job or wrong rig.");++calls;return matrix;});
        check(calls==1&&parallel.save_state(0)==before&&equal(*parallel.ik_state(id(1),1),state)&&poses.size()==initial.size(),"Serial/parallel IK differs.");for(std::size_t i=0;i<poses.size();++i)check(poses[i].entity==initial[i].entity&&poses[i].local.rotation==initial[i].local.rotation,"Parallel local order/result differs.");}
    auto source=std::const_pointer_cast<ModelAsset>(d.entities[0].animation_rig->model);source->nodes[1].parent=-1;source->nodes[1].position={7,8,9};const auto again=a.sample(0);check(equal(*a.ik_state(id(1),1),state)&&again[0].local.rotation==initial[0].local.rotation,"Mutable model alias changed frozen solver.");
}
void layers_histories_and_membership(){
    auto d=definition();RuntimeAnimationLayer layer;layer.slot=1;layer.clip=0;layer.time=.5;layer.playing=false;layer.weight=1;d.entities[0].animation_rig->layers={layer};RuntimeAnimations a(d);a.sample(0);near(a.ik_state(id(1),1)->end_position,{1,1,0});
    const auto history=a.checkpoint();check(history.bases[0].current->poses->at(0).rotation==std::array<double,4>{0,0,0,1},"IK polluted base inertial history.");check(history.layers[0][0].clock.current->poses->at(0).scale==V{1.5,1.5,1.5},"Layer history does not retain pre-IK pose.");
    RuntimeAnimations inertial(d);inertial.sample(0);inertial.sample(1);AnimationCommand transition{id(1),0,.25,1,false,true};transition.blend_ticks=4;transition.transition_mode=AnimationTransitionMode::Inertial;inertial.apply({transition},1);inertial.sample(2);near(inertial.ik_state(id(1),1)->end_position,{1,1,0});const auto inertial_state=inertial.checkpoint();check(inertial_state.bases[0].transition&&inertial_state.bases[0].transition->inertial&&inertial_state.bases[0].current->poses->at(0).rotation==std::array<double,4>{0,0,0,1},"Post-layer IK contaminated inertial state.");
    const auto inertial_bytes=inertial.save_state(2);RuntimeAnimations inertial_copy(d);inertial_copy.load_state(inertial_bytes,2);inertial_copy.sample(2);check(inertial_copy.save_state(2)==inertial_bytes&&equal(*inertial_copy.ik_state(id(1),1),*inertial.ik_state(id(1),1)),"IK plus layer/inertial save differs.");
    a.apply_ik({goal({0,1,0})});a.sample(0);const auto state=*a.ik_state(id(1),1);a.rebind(d,0);check(equal(*a.ik_state(id(1),1),state),"Surviving rebind lost IK state.");
    auto changed=d;changed.entities[0].animation_rig->ik[0].pole={1,0,0};rejects([&]{a.rebind(changed,0);});check(equal(*a.ik_state(id(1),1),state),"Invalid surviving config changed cached state.");
    auto removed=d;removed.entities.clear();a.rebind(removed,0);check(!a.has_ik()&&!a.ik_state(id(1),1),"Removed rig retained controls.");a.rebind(d,0);a.sample(0);near(a.ik_state(id(1),1)->target,{1,1,0});
    auto legacy=definition(false);RuntimeAnimations old(legacy);old.sample(0);const auto old_bytes=old.save_state(0);check(Json::parse(old_bytes)["version"]==1&&!Json::parse(old_bytes)["rigs"][0].contains("ik"),"Omitted IK changed legacy save.");RuntimeAnimations old_copy(legacy);old_copy.load_state(old_bytes,0);old_copy.sample(0);check(old_copy.save_state(0)==old_bytes,"Legacy animation roundtrip changed.");
    auto bad=d;bad.entities[2].parent=id(1);rejects([&]{validate_runtime_animation(bad);});
    auto nested=definition();RuntimeEntityDefinition second=nested.entities[0];second.id=id(20);second.parent=id(3);second.animation_rig->ik.clear();nested.entities.push_back(second);
    for(unsigned n=0;n<3;++n){auto node=nested.entities[n+1];node.id=id(21+n);node.parent=n?id(20+n):id(20);node.rig_node->rig=id(20);nested.entities.push_back(node);}validate_runtime_animation(nested);nested.entities[6].animation_rig->ik={chain()};rejects([&]{validate_runtime_animation(nested);});
}
void native_pose_skin_hit_and_rollback(){
    auto d=definition();d.entities[3].hit_region=RuntimeHitRegion{id(1),"effector",HitRegionShape::Sphere,{}, {.5,.5,.5},.1,0,true};Runtime r(d);near(pos(r.entity(id(4)).world),{1,1,0});const auto snapshot=r.snapshot(id(6));const auto old_palette=snapshot.objects.at(0).skin->palette;
    const auto deformed=deform_mesh(*snapshot.objects[0].mesh,old_palette);near(point(snapshot.objects[0].world,{deformed->vertices[0].position[0],deformed->vertices[0].position[1],deformed->vertices[0].position[2]}),{1,1,0},2e-6);
    RuntimeRay ray;ray.origin={1,1,3};ray.direction={0,0,-1};ray.distance=5;const auto hit=r.hit_test(ray);check(hit&&hit->region=="effector","Hit region did not follow committed IK.");near(hit->distance,2.9,2e-6);
    const auto original=r.save_snapshot(hash);r.step(1,{}, {}, {}, {}, {},{goal({-1,1,0})});near(pos(r.entity(id(4)).world),{-1,1,0});check(snapshot.objects[0].skin->palette==old_palette,"Old skin snapshot changed after IK.");
    const auto bytes=r.save_snapshot(hash);auto fresh=Runtime::from_snapshot(d,hash,bytes);check(fresh->save_snapshot(hash)==bytes&&equal(*fresh->ik(id(1),1),*r.ik(id(1),1)),"Fresh restore IK state differs.");r.step(4,{});fresh->step(4,{});check(fresh->save_snapshot(hash)==r.save_snapshot(hash),"Fresh IK continuation differs.");
    Runtime failed(d);AnimationCommand invalid{id(1),1,0,1,false,true};rejects([&]{failed.step(3,{}, {}, {},{invalid});});check(failed.save_snapshot(hash)==original&&equal(*failed.ik(id(1),1),*Runtime(d).ik(id(1),1)),"Later evaluated-scale failure did not roll back whole runtime.");
    rejects([&]{failed.step(1,{}, {}, {}, {}, {},{goal(),goal()});});check(failed.save_snapshot(hash)==original,"Duplicate IK changed runtime.");
}
void native_physical_wrapper_and_templates(){
    auto d=definition();RuntimeEntityDefinition body;body.id=id(8);body.collider=BoxCollider{};body.collider->motion=BodyMotion::Kinematic;body.collider->half_extents={.1,.1,.1};d.entities[0].parent=body.id;d.entities[0].animation_rig->ik[0].space=IkSpace::World;d.entities.push_back(body);Runtime r(d);
    KinematicTarget motion;motion.entity=body.id;motion.position={.5,0,0};motion.duration_ticks=2;r.step(2,{}, {motion});near(r.ik(id(1),1)->end_position,{1,1,0},2e-6);near(pos(r.entity(id(4)).world),{1,1,0},2e-6);
    r.step(1,{}, {}, {}, {}, {},{goal({1,1,0},{0,0,1},1,IkSpace::Rig)});near(r.ik(id(1),1)->end_position,{1.5,1,0},2e-6);
    auto template_def=definition();RuntimeSpawnTemplate recipe;recipe.id=id(100);recipe.name="Frozen IK graph";recipe.root=id(1);recipe.entities.assign(template_def.entities.begin(),template_def.entities.begin()+5);template_def.entities.erase(template_def.entities.begin(),template_def.entities.begin()+5);template_def.templates={recipe};Runtime spawned(template_def);
    const auto births=spawned.change_structure(0,{{recipe.id,{}},{recipe.id,{}}},{});check(births.spawned.size()==2,"Two IK templates did not spawn.");const auto first=births.spawned[0],second=births.spawned[1];const auto second_state=*spawned.ik(second,1);near(second_state.target,{1,1,0});auto changed=goal({-1,1,0});changed.entity=first;spawned.step(1,{}, {}, {}, {}, {},{changed});near(spawned.ik(first,1)->end_position,{-1,1,0});near(spawned.ik(second,1)->end_position,{1,1,0});
    const auto saved=spawned.save_snapshot(hash);auto restored=Runtime::from_snapshot(template_def,hash,saved);check(restored->save_snapshot(hash)==saved&&equal(*restored->ik(first,1),*spawned.ik(first,1)),"Spawned IK restore lost remapped controls.");
    spawned.change_structure(1,{}, {first});rejects([&]{spawned.ik(first,1);});check(equal(*spawned.ik(second,1),second_state),"Removal altered surviving IK state.");const auto reborn=spawned.change_structure(2,{{recipe.id,{}}},{}).spawned[0];check(reborn!=first,"Retired IK identity reused.");near(spawned.ik(reborn,1)->target,{1,1,0});
}
}
int main(){try{
    reachable_and_clamp();fold_antipodal_and_pole();weight_and_field_fidelity();affine_spaces();malformed_and_metric();animation_state_and_jobs();layers_histories_and_membership();
    if(Runtime::available()){native_pose_skin_hit_and_rollback();native_physical_wrapper_and_templates();std::cout<<"9 native two-bone IK groups passed\n";}
    else std::cout<<"7 portable two-bone IK groups passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
