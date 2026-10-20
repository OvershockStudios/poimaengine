// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime_animation.hpp"
#include "../src/runtime_hierarchy.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace poima;
namespace {
using Json=nlohmann::json;
using V=std::array<double,3>;
using Q=std::array<double,4>;
const std::string content(64,'a');
std::filesystem::path output;
std::size_t checks=0,serial=0;
Json evidence={{"format","poima.runtime-hierarchy-test.v1"},{"groups",Json::array()},
    {"pairs",Json::array()},{"observations",Json::array()}};
void check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
void near(double actual,double expected,double tolerance=2e-8) {
    check(std::isfinite(actual)&&std::isfinite(expected)&&std::abs(actual-expected)<=tolerance,
        "Independent affine/analytic expectation differs.");
}
template<class F>std::string rejects(F action) {
    try {action();}catch(const std::exception& e){return e.what();}
    throw std::runtime_error("Invalid operation was admitted.");
}
std::string id(unsigned n) {const auto text=std::to_string(n);return std::string(32-text.size(),'0')+text;}
Q axis(V a,double angle) {const double norm=std::hypot(a[0],a[1],a[2]),s=std::sin(angle/2)/norm;return {a[0]*s,a[1]*s,a[2]*s,std::cos(angle/2)};}
bool bits(const Matrix4& a,const Matrix4& b) {return std::memcmp(a.data(),b.data(),sizeof(double)*a.size())==0;}
void write(const std::filesystem::path& p,const std::string& bytes) {
    std::ofstream stream(p,std::ios::binary);stream.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    if(!stream)throw std::runtime_error("Cannot retain hierarchy test operand: "+p.string());
}
void pair(const std::string& label,const std::string& before,const std::string& after) {
    const auto stem=std::to_string(++serial)+"-"+label;
    if(!output.empty()){write(output/(stem+"-before.json"),before);write(output/(stem+"-after.json"),after);}
    evidence["pairs"].push_back({{"label",label},{"before",stem+"-before.json"},{"after",stem+"-after.json"},
        {"before_bytes",before.size()},{"after_bytes",after.size()},{"equal",before==after}});
    check(before==after,"Complete original saved operands differ.");
}
void group(const char* name,auto run) {run();evidence["groups"].push_back(name);}

// An independent row-major affine map. Expected basis columns are constructed
// by quaternion-vector cross products, not the production TRS/multiply helpers.
struct Affine {std::array<std::array<double,3>,3> a{{{1,0,0},{0,1,0},{0,0,1}}};V t{};};
V cross(V a,V b) {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
V rotate(Q q,V v) {
    double norm=0;for(double x:q)norm+=x*x;norm=std::sqrt(norm);for(double& x:q)x/=norm;
    const V u{q[0],q[1],q[2]},uv=cross(u,v),uuv=cross(u,uv);
    for(unsigned r=0;r<3;++r)v[r]+=2*q[3]*uv[r]+2*uuv[r];return v;
}
Affine independent(const RuntimeTransform& local) {
    Affine result;result.t=local.position;
    for(unsigned c=0;c<3;++c){V basis{};basis[c]=local.scale[c];const auto rotated=rotate(local.rotation,basis);
        for(unsigned r=0;r<3;++r)result.a[r][c]=rotated[r];}
    return result;
}
Affine compose(const Affine& parent,const Affine& local) {
    Affine result;result.t=parent.t;
    for(unsigned r=0;r<3;++r){for(unsigned c=0;c<3;++c){result.a[r][c]=0;
            for(unsigned k=0;k<3;++k)result.a[r][c]+=parent.a[r][k]*local.a[k][c];}
        for(unsigned k=0;k<3;++k)result.t[r]+=parent.a[r][k]*local.t[k];}
    return result;
}
void matrix(const Matrix4& actual,const Affine& expected) {
    for(unsigned c=0;c<3;++c)for(unsigned r=0;r<3;++r)near(actual[4*c+r],expected.a[r][c]);
    for(unsigned r=0;r<3;++r){near(actual[12+r],expected.t[r]);check(actual[4*r+3]==0,"Affine bottom row changed.");}
    check(actual[15]==1,"Affine homogeneous entry changed.");
}
void observe(Runtime& runtime,const std::string& label) {
    const auto saved=runtime.save_snapshot(content);const auto definition=runtime.live_definition();
    std::map<std::string,RuntimeEntityDefinition> definitions;std::map<std::string,Affine> expected;
    for(const auto& item:definition.entities)definitions.emplace(item.id,item);
    const auto visit=[&](auto&& self,const std::string& name)->Affine {
        if(const auto found=expected.find(name);found!=expected.end())return found->second;
        const auto local=independent(runtime.entity(name).local);const auto& parent=definitions.at(name).parent;
        const auto result=parent.empty()?local:compose(self(self,parent),local);expected.emplace(name,result);return result;
    };
    Json rows=Json::array();
    for(unsigned repeat=0;repeat<3;++repeat)for(const auto& [name,definition_row]:definitions) {
        (void)definition_row;const auto state=runtime.entity(name);matrix(state.world,visit(visit,name));
        if(repeat==0)rows.push_back({{"id",name},{"position",state.local.position},{"rotation",state.local.rotation},
            {"scale",state.local.scale},{"world",state.world}});
    }
    for(const auto& name:runtime.camera_ids())check(bits(runtime.snapshot(name).camera_world,runtime.entity(name).world),"Presentation camera differs from committed hierarchy.");
    evidence["observations"].push_back({{"label",label},{"tick",runtime.inspect().tick},{"rows",std::move(rows)}});
    pair(label+"-read-purity",saved,runtime.save_snapshot(content));
}

void exact_keys_and_publication() {
    detail::RuntimeHierarchyCache cache;RuntimeTransform local;Matrix4 world{};
    check(cache.update(local,nullptr,false,world),"First update did not compute.");
    const auto initial=world;check(!cache.update(local,nullptr,false,world)&&bits(world,initial),"Unchanged key changed output.");
    local.position[0]=-0.;check(cache.update(local,nullptr,false,world),"Negative-zero key was treated as positive zero.");
    check(std::signbit(world[12]),"Root negative-zero translation was canonicalized.");
    local.position[0]=0.;check(cache.update(local,nullptr,false,world),"Positive-zero return did not invalidate exact key.");
    local.rotation={-0.,-0.,-0.,-1.};check(cache.update(local,nullptr,false,world),"Equivalent opposite quaternion bits were ignored.");
    local.rotation={0,0,0,1};cache.update(local,nullptr,false,world);
    local.position[0]=std::numeric_limits<double>::denorm_min();
    check(cache.update(local,nullptr,false,world)&&world[12]==local.position[0],"Smallest representable translation was approximated away.");
    local.position[0]=1;cache.update(local,nullptr,false,world);const auto committed=world;
    for(double bad:{std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),std::nextafter(1e12,std::numeric_limits<double>::infinity())}) {
        auto candidate=local;candidate.position[0]=bad;rejects([&]{cache.update(candidate,nullptr,false,world);});
        check(bits(world,committed),"Rejected candidate partially published world.");
        check(!cache.update(local,nullptr,false,world),"Rejected candidate replaced valid key.");
        rejects([&]{cache.update(candidate,nullptr,false,world);});
    }
    auto invalid_rotation=local;invalid_rotation.rotation={0,0,0,0};
    rejects([&]{cache.update(invalid_rotation,nullptr,false,world);});
    check(bits(world,committed)&&!cache.update(local,nullptr,false,world),"Invalid rotation changed matrix/key.");
    auto boundary=local;boundary.position[0]=1e12;check(cache.update(boundary,nullptr,false,world)&&world[12]==1e12,"Inclusive hierarchy range boundary changed.");
    cache.invalidate();check(cache.update(boundary,nullptr,false,world),"Explicit invalidation did not recompute.");
    // A bad parent composition must also leave both published fields intact.
    Matrix4 parent{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    local.position={0,0,0};cache.invalidate();cache.update(local,&parent,true,world);const auto old=world;
    auto huge=parent;huge[0]=1e308;local.scale[0]=1e308;
    rejects([&]{cache.update(local,&huge,true,world);});check(bits(world,old),"Overflowing product changed published matrix.");
    local.scale[0]=1;check(!cache.update(local,&parent,false,world),"Overflow replaced prior complete key.");
}
void independent_tree_and_parent_signal() {
    const double pi=std::acos(-1.);RuntimeTransform p,c,g;
    p.position={3,4,5};p.rotation=axis({0,1,0},pi/2);p.scale={2,3,4};
    c.position={1,2,3};c.rotation=axis({0,0,1},pi/4);c.scale={.5,2,1};g.position={.2,.3,.4};
    detail::RuntimeHierarchyCache pc,cc,gc;Matrix4 pw{},cw{},gw{};
    bool changed=pc.update(p,nullptr,false,pw);changed=cc.update(c,&pw,changed,cw);gc.update(g,&cw,changed,gw);
    const auto expected=compose(compose(independent(p),independent(c)),independent(g));matrix(gw,expected);
    // Handwritten quarter-turn anchor, before the noncommuting child rotation.
    near(pw[0],0);near(pw[2],-2);near(pw[8],4);near(pw[12],3);
    check(!pc.update(p,nullptr,false,pw)&&!cc.update(c,&pw,false,cw)&&!gc.update(g,&cw,false,gw),"Stationary tree did not retain exact keys.");
    // Conservative propagation is required even if a recomputed parent's bits agree.
    check(cc.update(c,&pw,true,cw)&&gc.update(g,&cw,true,gw),"Parent recomputation was not propagated.");
    p.position[0]=7;changed=pc.update(p,nullptr,false,pw);changed=cc.update(c,&pw,changed,cw);gc.update(g,&cw,changed,gw);
    matrix(gw,compose(compose(independent(p),independent(c)),independent(g)));
    auto alternate=pw;alternate[12]=-8;cc.invalidate();check(cc.update(c,&alternate,false,cw),"Rebound parent invalidation ignored.");
    p.position[0]=-8;matrix(cw,compose(independent(p),independent(c)));
}
RuntimeEntityDefinition node(unsigned number,unsigned parent=0) {RuntimeEntityDefinition e;e.id=id(number);if(parent)e.parent=id(parent);return e;}
RuntimeDefinition physical_definition() {
    RuntimeDefinition d;d.world_id="hierarchy-physical";d.authored_revision=7;
    auto root=node(1);root.collider=BoxCollider{};root.collider->motion=BodyMotion::Kinematic;root.transform.position={0,4,0};
    auto child=node(2,1);child.transform.position={1,2,3};child.transform.rotation=axis({0,0,1},.6);child.transform.scale={2,1,.5};
    auto leaf=node(3,2);leaf.transform.position={.25,.5,.75};leaf.transform.rotation=axis({1,0,0},.4);
    auto unrelated=node(4);unrelated.transform.position={20,3,0};
    auto falling=node(5);falling.transform.position={10,20,0};falling.collider=BoxCollider{};falling.collider->motion=BodyMotion::Dynamic;
    auto attachment=node(6,5);attachment.transform.position={.5,1,0};
    d.entities={leaf,child,root,unrelated,attachment,falling};return d;
}
void physical_descendants_and_cold() {
    const auto d=physical_definition();Runtime a(d),control(d);observe(a,"physical-initial");const auto untouched=a.entity(id(4)).world;
    const KinematicTarget target{id(1),{2,5,-1},axis({0,1,0},.7),12};
    a.step(12,{}, {target});control.step(1,{}, {target});control.step(11,{});observe(a,"physical-moved");
    check(a.entity(id(1)).local.position[0]>1&&a.entity(id(5)).local.position[1]<20,"Physical roots did not actually change.");
    check(bits(a.entity(id(4)).world,untouched),"Unrelated stationary branch changed.");pair("physical-partition",a.save_snapshot(content),control.save_snapshot(content));
    const auto before=a.save_snapshot(content);const auto locals=a.entity(id(3));
    rejects([&]{a.step(3,{}, {}, {}, {},{{1,99,{},{}}});});pair("physical-late-reject",before,a.save_snapshot(content));
    check(bits(a.entity(id(3)).world,locals.world),"Rollback retained descendant matrix.");
    a.step(2,{});control.step(2,{});pair("physical-retry",a.save_snapshot(content),control.save_snapshot(content));
    // Keep the cold exact-byte operand contact-free with an identity physical
    // rotation; arbitrary restored contact/float-quaternion equivalence is not
    // a new portable-save guarantee. The earlier yaw propagation stays tested.
    const KinematicTarget unturn{id(1),{2,5,-1},{0,0,0,1},3};
    a.step(3,{}, {unturn});control.step(3,{}, {unturn});observe(a,"physical-unturned");
    pair("physical-unturn",a.save_snapshot(content),control.save_snapshot(content));
    const auto saved=a.save_snapshot(content);auto cold=Runtime::from_snapshot(d,content,saved);observe(*cold,"physical-cold");
    pair("physical-cold-immediate",saved,cold->save_snapshot(content));a.step(3,{});cold->step(3,{});
    pair("physical-contact-free-future",a.save_snapshot(content),cold->save_snapshot(content));
}
std::shared_ptr<ModelAsset> three_bones() {
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(3);m->roots={0};
    for(unsigned n=0;n<3;++n){m->nodes[n].name="Original bone "+std::to_string(n);m->nodes[n].parent=n?static_cast<int>(n-1):-1;if(n)m->nodes[n].position={0,1,0};}
    m->animations.push_back({"Across",1,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0,1},{{0,0,0,0},{1,0,0,0}}}}});
    m->animations.push_back({"Large middle",1,{{1,AnimationPath::scale,AnimationInterpolation::linear,{0,1},{{1,1,1,0},{100000000,1,1,0}}}}});
    return m;
}
void add_rig(RuntimeDefinition& d,unsigned rig_id,unsigned first,const std::shared_ptr<ModelAsset>& m,bool with_ik) {
    auto rig=node(rig_id);rig.animation_rig=RuntimeAnimationRig{m};
    if(with_ik){RuntimeTwoBoneIk chain;chain.root=0;chain.middle=1;chain.end=2;chain.enabled=false;chain.space=IkSpace::Rig;chain.target={1,1,0};rig.animation_rig->ik={chain};}
    d.entities.push_back(rig);
    for(unsigned n=0;n<3;++n){auto e=node(first+n,n?first+n-1:rig_id);e.transform={m->nodes[n].position,m->nodes[n].rotation,m->nodes[n].scale};e.rig_node=RuntimeRigNode{id(rig_id),n};d.entities.push_back(e);}
}
void animation_ik_and_hierarchy_overflow() {
    RuntimeDefinition d;d.world_id="hierarchy-animated";d.authored_revision=5;const auto model=three_bones();add_rig(d,10,11,model,true);
    d.entities[0].transform.position={3,0,0};auto attached=node(14,13);attached.transform.position={.2,.3,.4};d.entities.push_back(attached);
    Runtime a(d),control(d);const AnimationCommand across{id(10),0,0,1,false,true};a.step(30,{}, {}, {},{across});control.step(30,{}, {}, {},{across});
    observe(a,"animation-playing");near(a.entity(id(11)).local.position[0],.5);near(a.entity(id(13)).world[12],3.5);
    RuntimeIkCommand goal{id(10),1,true,IkSpace::Rig,{1,1,0},{0,0,1},1};
    a.step(1,{}, {}, {}, {}, {}, {goal});control.step(1,{}, {}, {}, {}, {}, {goal});observe(a,"ik-published");
    near(a.entity(id(13)).world[12],4);near(a.entity(id(13)).world[13],1);check(a.ik(id(10),1)->target_error<2e-8,"IK endpoint witness failed.");
    const auto saved=a.save_snapshot(content);rejects([&]{a.step(3,{}, {}, {}, {},{{1,99,{},{}}});});pair("ik-late-reject",saved,a.save_snapshot(content));
    a.step(2,{});control.step(2,{});pair("ik-retry",a.save_snapshot(content),control.save_snapshot(content));
    const auto committed=a.save_snapshot(content);auto cold=Runtime::from_snapshot(d,content,committed);observe(*cold,"ik-cold");pair("ik-cold-immediate",committed,cold->save_snapshot(content));
    a.step(2,{});cold->step(2,{});pair("ik-cold-future",a.save_snapshot(content),cold->save_snapshot(content));
    RuntimeDefinition huge;huge.world_id="hierarchy-live-overflow";add_rig(huge,20,21,model,false);
    auto scaled=node(24,22);scaled.transform.scale={100000000,1,1};huge.entities.push_back(scaled);
    Runtime overflow(huge);overflow.step(1,{});observe(overflow,"overflow-valid-held");const auto before=overflow.save_snapshot(content);
    const auto error=rejects([&]{overflow.step(1,{}, {}, {},{{id(20),1,0,1,false,true}});});
    check(error=="Runtime hierarchy matrix exceeds the supported range.","Live overflow did not reach the actual hierarchy range guard.");
    pair("live-hierarchy-overflow",before,overflow.save_snapshot(content));observe(overflow,"overflow-rollback");
}
void character_root_motion() {
    RuntimeDefinition d;d.world_id="hierarchy-root-motion";const auto m=three_bones();auto actor=node(100);actor.transform.position={0,10,0};actor.character=CharacterController{};d.entities.push_back(actor);
    add_rig(d,20,21,m,false);d.entities[1].parent=id(100);d.entities[1].animation_rig->clip=0;d.entities[1].animation_rig->playing=true;
    d.entities[1].animation_rig->root_motion=RuntimeRootMotionDefinition{0,RootMotionMode::Character,id(100),true,false};
    Runtime a(d),control(d);a.step(30,{});control.step(12,{});control.step(18,{});observe(a,"character-root-motion");
    near(a.entity(id(100)).world[12],.5,.02);check(a.root_motion(id(20))->world_applied[0]>0,"Root motion did not move physical wrapper.");
    near(a.entity(id(21)).world[12],a.entity(id(100)).world[12],2e-6);pair("root-motion-partition",a.save_snapshot(content),control.save_snapshot(content));
    const auto saved=a.save_snapshot(content);RuntimeInput input;input.entity=id(100);input.move={1,0};rejects([&]{a.step(1,{input});});pair("root-motion-conflict",saved,a.save_snapshot(content));
    auto cold=Runtime::from_snapshot(d,content,saved);observe(*cold,"root-motion-cold");pair("root-motion-cold-immediate",saved,cold->save_snapshot(content));
    a.step(2,{});cold->step(2,{});pair("root-motion-freeflight-future",a.save_snapshot(content),cold->save_snapshot(content));
}
void camera_pitch_stance_rollback() {
    RuntimeDefinition d;d.world_id="hierarchy-camera";auto actor=node(1);actor.transform.position={0,10,0};actor.character=CharacterController{};actor.character->camera=id(2);actor.character->stance=RuntimeCharacterStanceProfile{1,2,.8f};
    auto camera=node(2,1);camera.camera=RuntimeCamera{};camera.transform.position={.05,1.6,.02};auto child=node(3,2);child.transform.position={.3,.2,-.5};auto leaf=node(4,3);leaf.transform.position={.1,.2,.3};d.entities={leaf,child,camera,actor};
    Runtime a(d),control(d);RuntimeInput look;look.entity=id(1);look.look={0,30};a.step(1,{look});control.step(1,{look});observe(a,"camera-pitch-only");
    check(a.entity(id(1)).yaw==0&&a.entity(id(1)).pitch!=0,"Pitch witness changed yaw or did not pitch.");
    const auto eye=a.entity(id(2));const auto basis=rotate(eye.local.rotation,{0,1,0});check(std::abs(basis[2])>.1,"Pitch did not affect camera local rotation.");
    a.step(1,{}, {}, {}, {}, {}, {}, {}, {},{{id(1),true}});control.step(1,{}, {}, {}, {}, {}, {}, {}, {},{{id(1),true}});observe(a,"camera-crouched");
    near(a.entity(id(2)).local.position[1],1.6-static_cast<double>(.8f));const auto low=a.entity(id(2)).local.position;
    a.step(2,{}, {}, {}, {}, {}, {}, {}, {},{{id(1),true}});control.step(2,{}, {}, {}, {}, {}, {}, {}, {},{{id(1),true}});
    check(a.entity(id(2)).local.position==low,"Repeated stance accumulated camera displacement.");
    const auto saved=a.save_snapshot(content);rejects([&]{a.step(3,{}, {}, {}, {},{{1,99,{},{}}}, {}, {}, {},{{id(1),false}});});pair("camera-late-reject",saved,a.save_snapshot(content));observe(a,"camera-rollback");
    a.step(1,{}, {}, {}, {}, {}, {}, {}, {},{{id(1),false}});control.step(1,{}, {}, {}, {}, {}, {}, {}, {},{{id(1),false}});near(a.entity(id(2)).local.position[1],1.6);pair("camera-retry",a.save_snapshot(content),control.save_snapshot(content));
    const auto committed=a.save_snapshot(content);auto cold=Runtime::from_snapshot(d,content,committed);observe(*cold,"camera-cold");pair("camera-cold-immediate",committed,cold->save_snapshot(content));
}
void topology_replacement_and_rollback() {
    RuntimeDefinition d;d.world_id="hierarchy-topology";auto base=node(900);base.transform.position={30,2,1};d.entities={base};
    RuntimeSpawnTemplate recipe;recipe.id=id(500);recipe.name="Original affine graph";recipe.root=id(1);
    auto root=node(1),child=node(2,1),leaf=node(3,2);child.transform.position={1,2,0};child.transform.rotation=axis({0,0,1},.4);child.transform.scale={2,1,1};leaf.transform.position={.2,.3,.4};recipe.entities={leaf,child,root};d.templates={recipe};
    Runtime a(d),control(d);RuntimeTransform first;first.position={2,3,4};first.rotation=axis({0,1,0},.7);
    const auto born=a.change_structure(0,{{recipe.id,first}},{}),other=control.change_structure(0,{{recipe.id,first}},{});check(born.spawned==other.spawned,"Initial generated IDs differ.");observe(a,"hierarchy-born");
    const auto root_id=born.spawned.at(0),leaf_id=a.instance_node(root_id,id(3));const auto saved=a.save_snapshot(content);
    // Removal is published before a later stale revision rejects the whole batch.
    rejects([&]{a.step(3,{}, {}, {}, {},{{0,1,{}, {root_id}},{1,99,{{recipe.id,first}}, {}}});});pair("topology-remove-rollback",saved,a.save_snapshot(content));
    check(!a.instance(root_id).nodes.empty()&&!a.entity(leaf_id).id.empty(),"Rollback lost restored parent bindings.");observe(a,"hierarchy-remove-rollback");
    RuntimeTransform second;second.position={-4,5,2};second.rotation=axis({1,0,0},.3);second.scale={.5,2,1};
    // Birth is independently rolled back; the next valid request must reuse the
    // allocator's original next identities, not the discarded native owners.
    rejects([&]{a.step(3,{}, {}, {}, {},{{0,1,{{recipe.id,second}},{}},{1,99,{},{}}});});pair("topology-birth-rollback",saved,a.save_snapshot(content));
    const auto next=a.change_structure(1,{{recipe.id,second}},{root_id}),same=control.change_structure(1,{{recipe.id,second}},{root_id});check(next.spawned==same.spawned&&next.spawned.at(0)!=root_id,"Retry consumed or resurrected public IDs.");
    rejects([&]{(void)a.entity(leaf_id);});observe(a,"hierarchy-replaced");pair("topology-retry",a.save_snapshot(content),control.save_snapshot(content));
    const auto committed=a.save_snapshot(content);auto cold=Runtime::from_snapshot(d,content,committed);observe(*cold,"hierarchy-cold");pair("topology-cold-immediate",committed,cold->save_snapshot(content));
    a.step(4,{});cold->step(4,{});pair("topology-cold-future",a.save_snapshot(content),cold->save_snapshot(content));
    a.change_structure(2,{},next.spawned);check(a.inspect().entities==1,"Removed branch retained membership.");observe(a,"hierarchy-removed");
}
}
int main(int argc,char** argv) {
    try {
        if(argc==3&&std::string(argv[1])=="--output") {output=std::filesystem::absolute(argv[2]);
            if(!std::filesystem::is_directory(output.parent_path())||std::filesystem::exists(output))throw std::runtime_error("--output requires a fresh path under an existing directory.");
            std::filesystem::create_directory(output);
        }else if(argc!=1)throw std::runtime_error("Usage: runtime-hierarchy-test [--output FRESH_DIRECTORY]");
        group("exact_keys_and_publication",exact_keys_and_publication);group("independent_tree_and_parent_signal",independent_tree_and_parent_signal);
        if(Runtime::available()) {group("physical_descendants_and_cold",physical_descendants_and_cold);group("animation_ik_and_hierarchy_overflow",animation_ik_and_hierarchy_overflow);group("character_root_motion",character_root_motion);group("camera_pitch_stance_rollback",camera_pitch_stance_rollback);group("topology_replacement_and_rollback",topology_replacement_and_rollback);}
        evidence["native_available"]=Runtime::available();evidence["checks"]=checks;evidence["passed"]=true;
        if(!output.empty())write(output/"report.json",evidence.dump(2)+"\n");
        std::cout<<(Runtime::available()?"7 native hierarchy groups passed":"2 portable hierarchy groups passed; 5 native groups skipped (simulation unavailable)")<<"; "<<checks<<" checks\n";return 0;
    }catch(const std::exception& error){evidence["passed"]=false;evidence["checks"]=checks;evidence["error"]=error.what();
        if(!output.empty())try{write(output/"report.json",evidence.dump(2)+"\n");}catch(const std::exception& retained){std::cerr<<retained.what()<<'\n';}
        std::cerr<<error.what()<<'\n';return 1;}
}
