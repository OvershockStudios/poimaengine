// SPDX-License-Identifier: Apache-2.0
// Actual Runtime joins for fixed-scale skeletal frames. Source-only until the
// integrating owner compiles/runs it; no full-character or performance claim.
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace poima;using Json=nlohmann::json;namespace pr=physical_rig;
using V=std::array<double,3>;using Q=std::array<double,4>;
const std::string content(64,'c');
std::string id(unsigned n){const auto text=std::to_string(n);return std::string(32-text.size(),'0')+text;}
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void near(double a,double b,double tolerance=1e-8){check(std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=tolerance,"Skeletal Runtime independent analytic oracle differs.");}
template<class F>void rejects(F&& f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"Invalid skeletal Runtime operation succeeded.");}
Q zq(double a){return {0,0,std::sin(a/2),std::cos(a/2)};}
V rz(double a,V p){return {std::cos(a)*p[0]-std::sin(a)*p[1],std::sin(a)*p[0]+std::cos(a)*p[1],p[2]};}
Q product(Q a,Q b){
    Q q{a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
    const double norm=std::hypot(std::hypot(q[0],q[1]),std::hypot(q[2],q[3]));check(norm>0,"Invalid independent rotation input.");
    for(auto& x:q)x/=norm;return q;
}
V rotate(Q q,V v){
    const V t{2*(q[1]*v[2]-q[2]*v[1]),2*(q[2]*v[0]-q[0]*v[2]),2*(q[0]*v[1]-q[1]*v[0])};
    return {v[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1],v[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2],v[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0]};
}
// Independent planar affine arithmetic; never calls the production FK/inverse.
struct Affine {double a=1,b=0,c=0,d=1,x=0,y=0,z=0,zscale=1;};
Affine trs(V p,double q,V s){return {std::cos(q)*s[0],-std::sin(q)*s[1],std::sin(q)*s[0],std::cos(q)*s[1],p[0],p[1],p[2],s[2]};}
Affine chain(Affine p,Affine q){return {p.a*q.a+p.b*q.c,p.a*q.b+p.b*q.d,p.c*q.a+p.d*q.c,p.c*q.b+p.d*q.d,
    p.x+p.a*q.x+p.b*q.y,p.y+p.c*q.x+p.d*q.y,p.z+p.zscale*q.z,p.zscale*q.zscale};}
constexpr V root_scale{1.25,.75,1},helper_scale{1.125,.875,1},tip_scale{.875,1.0625,1};
constexpr double root_angle=.2,helper_angle=-.4,tip_angle=.3;
RuntimeTransform placed(double x=0,double angle=.25){return {{x,6,0},zq(angle),{1.4,1.4,1.4}};}

std::shared_ptr<ModelAsset> model(){
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(3);m->roots={0};
    m->nodes[0].name="root";m->nodes[0].position={.5,.75,0};m->nodes[0].rotation=zq(root_angle); // rest scale is1, not the initialized clip's scale
    m->nodes[1].name="unmapped shearing helper";m->nodes[1].parent=0;m->nodes[1].position={.25,.5,0};m->nodes[1].rotation=zq(helper_angle);m->nodes[1].scale=helper_scale;
    m->nodes[2].name="tip";m->nodes[2].parent=1;m->nodes[2].position={0,.4,0};m->nodes[2].rotation=zq(tip_angle);m->nodes[2].scale=tip_scale;
    const AnimationChannel translation{0,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{.5,.75,0,0},{2.5,.75,0,0}}};
    const AnimationChannel fixed{0,AnimationPath::scale,AnimationInterpolation::linear,{0,2},{{1.25,.75,1,0},{1.25,.75,1,0}}};
    m->animations.push_back({"initialized fixed scale",2,{translation,fixed}});
    auto differing=fixed;differing.values={{1.375,.75,1,0},{1.375,.75,1,0}};
    m->animations.push_back({"different constant scale",2,{translation,differing}});
    auto delayed=fixed;delayed.times={0,.0625,2};delayed.values={{1.25,.75,1,0},{1.25,.75,1,0},{1.5,.75,1,0}};
    m->animations.push_back({"scale changes after three committed ticks",2,{translation,delayed}});
    return m;
}
pr::Configuration profile(){
    pr::Configuration c;c.pose_frame=pr::PoseFramePolicy::SkeletalFixedScale;c.profile.root="root";
    pr::Part a;a.name="root";a.node=0;a.shape=pr::Sphere{.12};a.mass=3;a.body_in_bone.position={.08,0,0};a.linear_damping=0;a.angular_damping=.02;
    auto b=a;b.name="tip";b.node=2;b.mass=1;b.shape=pr::Sphere{.1};c.profile.parts={a,b};
    // Initial calibrated anchors share one metric point for the authored1.4
    // wrapper scale. Translation/rotation births preserve this calibration.
    const auto root=chain(trs({},0,{1.4,1.4,1.4}),trs({.5,.75,0},root_angle,root_scale));
    const auto tip=chain(chain(root,trs({.25,.5,0},helper_angle,helper_scale)),trs({0,.4,0},tip_angle,tip_scale));
    const auto ra=rz(root_angle,a.body_in_bone.position),rb=rz(root_angle+helper_angle+tip_angle,b.body_in_bone.position);
    V ca{root.x+ra[0],root.y+ra[1],0},cb{tip.x+rb[0],tip.y+rb[1],0},anchor{};
    for(unsigned k=0;k<3;++k)anchor[k]=(ca[k]+cb[k])*.5;
    V pa{},pb{};for(unsigned k=0;k<3;++k){pa[k]=anchor[k]-ca[k];pb[k]=anchor[k]-cb[k];}
    pr::Joint j;j.name="spine";j.parent="root";j.child="tip";j.parent_frame.position=rz(-root_angle,pa);
    j.child_frame.position=rz(-(root_angle+helper_angle+tip_angle),pb);j.child_frame.rotation=zq(-helper_angle-tip_angle);
    j.limits=pr::SwingTwist{1,1,-.8,.8};j.rest_damping=.1;j.rest_stiffness=.5;j.soft_limit_stiffness=.5;c.profile.joints={j};return c;
}
std::vector<RuntimeEntityDefinition> actor(std::shared_ptr<const ModelAsset> m,unsigned first){
    RuntimeEntityDefinition rig;rig.id=id(first);rig.transform=placed();rig.animation_rig=RuntimeAnimationRig{m,0,0,1,false,true,{}};rig.physical_rig=profile();
    std::vector<RuntimeEntityDefinition> result{rig};
    for(unsigned n=0;n<3;++n){const auto& node=m->nodes[n];RuntimeEntityDefinition e;e.id=id(first+1+n);e.parent=n?result[n].id:rig.id;
        e.transform={node.position,node.rotation,node.scale};e.rig_node=RuntimeRigNode{rig.id,n};result.push_back(e);}
    return result;
}
RuntimeDefinition definition(){
    RuntimeDefinition d;d.world_id="skeletal-runtime-integration";d.authored_revision=9;
    RuntimeEntityDefinition floor;floor.id=id(2);floor.transform.position={0,-.5,0};floor.collider=BoxCollider{};floor.collider->half_extents={30,.5,30};
    RuntimeEntityDefinition camera;camera.id=id(3);camera.camera=RuntimeCamera{};camera.transform.position={0,5,12};d.entities={floor,camera};
    const auto m=model();auto a=actor(m,10);d.entities.insert(d.entities.end(),a.begin(),a.end());
    RuntimeSpawnTemplate recipe;recipe.id=id(900);recipe.name="fixed-scale skeletal actor";recipe.root=id(1000);recipe.entities=actor(m,1000);d.templates={recipe};return d;
}
void physical_step(Runtime& r,const std::vector<PhysicalRigCommand>& commands){r.step(1,{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},commands);}
std::vector<std::string> authored_nodes(){return {id(11),id(12),id(13)};}
Json physical_current(const Runtime& r,const std::string& rig){
    const auto complete=Json::parse(r.save_snapshot(content));
    for(const auto& row:complete.at("payload").at("physical").at("rigs"))if(row.at("entity")==rig)return row.at("pose").at("current");
    throw std::runtime_error("Complete archive omitted physical pose history.");
}
void expected_animated(const Runtime& r,const std::string& rig,const std::vector<std::string>& nodes,RuntimeTransform placement,double wrapper_angle,double seconds){
    check(nodes.size()==3,"Analytic fixture mapping must be complete.");
    const auto wrapper=trs(placement.position,wrapper_angle,placement.scale);
    const auto a=chain(wrapper,trs({.5+seconds,.75,0},root_angle,root_scale));
    const auto h=chain(a,trs({.25,.5,0},helper_angle,helper_scale));
    const auto b=chain(h,trs({0,.4,0},tip_angle,tip_scale));
    const Affine expected[]{a,h,b};const auto history=physical_current(r,rig);
    for(unsigned n=0;n<3;++n){const auto node=r.entity(nodes[n]);near(node.world[12],expected[n].x);near(node.world[13],expected[n].y);near(node.world[14],expected[n].z);}
    check(r.entity(nodes[0]).local.scale==root_scale && r.entity(nodes[1]).local.scale==helper_scale && r.entity(nodes[2]).local.scale==tip_scale,
        "Original selected clip scales were normalized or replaced by model rest scales.");
    for(unsigned n=0;n<2;++n){const auto bone=n?b:a;const double angle=wrapper_angle+root_angle+(n?helper_angle+tip_angle:0);
        const auto offset=rz(angle,{.08,0,0});const auto& body=history.at("bodies").at(n);
        near(body.at("position").at(0).get<double>(),bone.x+offset[0]);near(body.at("position").at(1).get<double>(),bone.y+offset[1]);
        const auto q=zq(angle);for(unsigned k=0;k<4;++k)near(body.at("rotation").at(k).get<double>(),q[k]);
    }
    const auto& shear=r.entity(nodes[2]).world;
    check(std::abs(shear[0]*shear[4]+shear[1]*shear[5])>.01,"Core fixture did not exercise actual affine shear.");
}
void publication(const Runtime& r,const std::string& rig,const std::vector<std::string>& nodes){
    Q q=r.entity(rig).local.rotation;std::vector<Q> rotations;
    for(const auto& id:nodes){q=product(q,r.entity(id).local.rotation);rotations.push_back(q);}
    const auto full=r.physical_rig(rig,true,true);check(full && full->parts.size()==2 && full->joints.size()==1,"Actual articulated projection incomplete.");
    for(unsigned n=0;n<2;++n){const unsigned bone=n?2:0;const auto state=r.entity(nodes[bone]);const auto offset=rotate(rotations[bone],{.08,0,0});const auto& body=full->parts[n];
        for(unsigned k=0;k<3;++k)near(body.pose.position[k],state.world[12+k]+offset[k],2e-6);
        double dot=0;for(unsigned k=0;k<4;++k)dot+=body.pose.rotation[k]*rotations[bone][k];near(std::abs(dot),1,1e-7);
    }
    check(r.entity(nodes[0]).local.scale==root_scale && r.entity(nodes[1]).local.scale==helper_scale && r.entity(nodes[2]).local.scale==tip_scale,"Passive publication changed exact node scales.");
}
void whole_equal(const Runtime& a,const Runtime& b){
    check(a.save_snapshot(content)==b.save_snapshot(content),"Complete native Runtime archive differs; no fields may be excluded.");
    check(a.live_definition().entities.size()==b.live_definition().entities.size(),"Cold live graph population changed.");
    for(const auto& e:a.live_definition().entities){const auto x=a.entity(e.id),y=b.entity(e.id);
        check(x.world==y.world && x.local.position==y.local.position && x.local.rotation==y.local.rotation && x.local.scale==y.local.scale,
            "Native full world/local publication differs despite archive comparison.");}
}
void initialized_clip_takeover_and_cold_future(){
    const auto d=definition();Runtime warm(d);expected_animated(warm,id(10),authored_nodes(),placed(),.25,0);
    warm.step(12,{});expected_animated(warm,id(10),authored_nodes(),placed(),.25,.2);
    const auto animated=warm.save_snapshot(content);auto animated_cold=Runtime::from_snapshot(d,content,animated);whole_equal(warm,*animated_cold);
    warm.step(3,{});animated_cold->step(3,{});whole_equal(warm,*animated_cold);expected_animated(warm,id(10),authored_nodes(),placed(),.25,.25);
    physical_step(warm,{{id(10),true,{}}});physical_step(*animated_cold,{{id(10),true,{}}});whole_equal(warm,*animated_cold);publication(warm,id(10),authored_nodes());
    const auto p=warm.physical_rig(id(10),true,false);V momentum{};for(const auto& body:p->parts)for(unsigned k=0;k<3;++k)momentum[k]+=body.mass*body.linear_velocity[k];
    near(momentum[0],4*1.4*std::cos(.25),2e-4);near(momentum[1],4*1.4*std::sin(.25)-4*9.81/60,2e-4);
    const auto passive=warm.save_snapshot(content);Runtime::validate_snapshot(d,content,passive);auto cold=Runtime::from_snapshot(d,content,passive);whole_equal(warm,*cold);
    for(unsigned n=0;n<24;++n){warm.step(1,{});cold->step(1,{});whole_equal(warm,*cold);publication(warm,id(10),authored_nodes());}
}
void template_birth_bootstrap_and_retired_frontier(){
    const auto d=definition();Runtime warm(d);warm.step(9,{});const auto birth=placed(4,-.3);
    const auto born=warm.change_structure(0,{{id(900),birth}},{}).spawned.at(0);std::vector<std::string> nodes;
    for(unsigned n=1001;n<=1003;++n)nodes.push_back(warm.instance_node(born,id(n)));
    expected_animated(warm,born,nodes,birth,-.3,0);
    const auto animated=warm.save_snapshot(content);auto cold_animated=Runtime::from_snapshot(d,content,animated);whole_equal(warm,*cold_animated);
    warm.step(7,{});cold_animated->step(7,{});whole_equal(warm,*cold_animated);expected_animated(warm,born,nodes,birth,-.3,7./60);
    physical_step(warm,{{born,true,{}}});physical_step(*cold_animated,{{born,true,{}}});whole_equal(warm,*cold_animated);publication(warm,born,nodes);
    const auto passive=warm.save_snapshot(content);auto cold=Runtime::from_snapshot(d,content,passive);whole_equal(warm,*cold);
    for(unsigned n=0;n<12;++n){warm.step(1,{});cold->step(1,{});whole_equal(warm,*cold);publication(warm,born,nodes);}
    warm.change_structure(warm.structure_revision(),{}, {born});const auto retired=warm.save_snapshot(content);auto retired_cold=Runtime::from_snapshot(d,content,retired);whole_equal(warm,*retired_cold);
    const auto next=warm.change_structure(warm.structure_revision(),{{id(900),placed(-4,.6)}},{}).spawned.at(0);
    const auto other=retired_cold->change_structure(retired_cold->structure_revision(),{{id(900),placed(-4,.6)}},{}).spawned.at(0);
    check(next==other && next!=born,"Skeletal cold expanded graph lost retired allocator history.");whole_equal(warm,*retired_cold);
}
void rejected_scale_changes_preserve_entire_prior_state(){
    const auto d=definition();Runtime r(d);r.step(12,{});const auto prior=r.save_snapshot(content);auto witness=Runtime::from_snapshot(d,content,prior);
    const auto command=[](std::optional<std::uint32_t> clip){return AnimationCommand{id(10),clip,0,1,false,true};};
    rejects([&]{r.step(1,{}, {}, {}, {command(1)});});check(r.save_snapshot(content)==prior,"Different constant-scale clip escaped whole rollback.");whole_equal(r,*witness);
    // First three new clip samples have the exact admitted scale. A later
    // positive scale change must roll back all already-executed physics/history.
    rejects([&]{r.step(6,{}, {}, {}, {command(2)});});check(r.save_snapshot(content)==prior,"Later scale failure lost complete multi-tick rollback.");whole_equal(r,*witness);
    auto rest=command(std::nullopt);rest.playing=false;
    rejects([&]{r.step(1,{}, {}, {}, {rest});});check(r.save_snapshot(content)==prior,"Returning to different model-rest scale was silently allowed.");whole_equal(r,*witness);
    auto bad_birth=placed(5);bad_birth.scale={1.4,1.5,1.4};rejects([&]{(void)r.change_structure(0,{{id(900),bad_birth}},{});});
    check(r.save_snapshot(content)==prior,"Invalid skeletal birth consumed body/entity IDs or changed whole state.");whole_equal(r,*witness);
    for(auto policy:{pr::PoseFramePolicy::RigidUniform,static_cast<pr::PoseFramePolicy>(999)}){
        auto invalid=d;for(auto& e:invalid.entities)if(e.physical_rig)e.physical_rig->pose_frame=policy;
        rejects([&]{Runtime candidate(invalid);});check(r.save_snapshot(content)==prior,"Detached invalid-policy construction changed existing runtime.");
    }
    // Same-scale animation command remains legal; the new policy must not turn
    // all clip iteration into an unsupported path to make failures look green.
    const AnimationCommand accepted{id(10),0,.5,1,false,false};r.step(1,{}, {}, {}, {accepted});expected_animated(r,id(10),authored_nodes(),placed(),.25,.5);
    r.step(1,{}, {}, {}, {AnimationCommand{id(10),0,.5,1,false,true}});physical_step(r,{{id(10),true,{}}});publication(r,id(10),authored_nodes());
}
}
int main(){
    try{check(Runtime::available(),"Actual native simulation required.");initialized_clip_takeover_and_cold_future();template_birth_bootstrap_and_retired_frontier();rejected_scale_changes_preserve_entire_prior_state();
        std::cout<<"Skeletal Runtime integration: 3 native groups passed.\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
