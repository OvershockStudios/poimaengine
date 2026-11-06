// SPDX-License-Identifier: Apache-2.0
// Actual native grounded/planted controller→physics ownership. Seven tiny
// spherical proxies represent an analytic pelvis/two-leg tree, not a humanoid.
// Frozen pre-execution oracles: 11kg total, .04m spheres, zero damping; independent
// consecutive COM/quaternion history2e-6; free first-step aggregate P1e-3 and
// angular momentum5e-3. Real floor contacts are required while Animated, but
// first physical P/L assertions require all seven proxies contact-free.
// Complete save bytes/caches are never filtered. No C#/AOT/render/perf claim.
#include "physical_grounded_gameplay_fixture.hpp"
#include "physical_grounded_test_artifacts.hpp"
#include "poima/runtime.hpp"
#include "poima/runtime_animation.hpp"
#include "poima/runtime_grounding.hpp"
#include "poima/assets.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>

namespace {
using namespace poima;using J=nlohmann::json;using V=std::array<double,3>;using Q=std::array<double,4>;
namespace pr=physical_rig;namespace gt=grounded_test;
const std::string content(64,'f');unsigned checks=0;
std::string id(unsigned n){char b[33];std::snprintf(b,sizeof(b),"%032x",n);return b;}
const auto actor=id(100),rig=id(101),camera=id(109),other=id(201),support_recipe=id(900),actor_recipe=id(901);
const std::array<std::string,7> names{"pelvis","left_hip","left_knee","left_foot","right_hip","right_knee","right_foot"};
const std::array<double,7> masses{4,2,1,.5,2,1,.5};
void need(bool b,const char* message){++checks;if(!b)throw std::runtime_error(message);}
void near(double a,double b,double e=2e-7){++checks;if(std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=e)return;
    std::ostringstream s;s.precision(17);s<<"Grounded independent oracle differs: actual="<<a<<" expected="<<b<<" bound="<<e;throw std::runtime_error(s.str());}
void near(const V& a,const V& b,double e=2e-7){for(unsigned n=0;n<3;++n)near(a[n],b[n],e);}
template<class F>void rejects(F&& f){bool bad=false;try{f();}catch(const std::exception&){bad=true;}need(bad,"Unsupported grounded ownership operation was accepted.");}
V subtract(V a,V b){for(unsigned n=0;n<3;++n)a[n]-=b[n];return a;}
double norm(V v){return std::hypot(v[0],v[1],v[2]);}
V cross(V a,V b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
Q yaw(double angle){return {0,std::sin(angle/2),0,std::cos(angle/2)};}
Q product(Q a,Q b){Q q{a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
    a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
    double length=0;for(double x:q)length+=x*x;for(double& x:q)x/=std::sqrt(length);return q;}
Matrix4 rigid(V p,Q q){const auto x=q[0],y=q[1],z=q[2],w=q[3];return {
    1-2*(y*y+z*z),2*(x*y+w*z),2*(x*z-w*y),0,2*(x*y-w*z),1-2*(x*x+z*z),2*(y*z+w*x),0,
    2*(x*z+w*y),2*(y*z-w*x),1-2*(x*x+y*y),0,p[0],p[1],p[2],1};}
V point(const Matrix4& m,V p){V v{};for(unsigned n=0;n<3;++n)v[n]=m[12+n]+m[n]*p[0]+m[4+n]*p[1]+m[8+n]*p[2];return v;}
V vector(const Matrix4& m,V p){return subtract(point(m,p),{m[12],m[13],m[14]});}
V position(const Runtime& r,const std::string& entity){const auto w=r.entity(entity).world;return {w[12],w[13],w[14]};}
Q rotation(const Runtime& r,std::string entity){Q q{0,0,0,1};while(!entity.empty()){
    q=product(r.entity(entity).local.rotation,q);const auto& defs=r.live_definition().entities;
    const auto found=std::find_if(defs.begin(),defs.end(),[&](const auto& d){return d.id==entity;});need(found!=defs.end(),"Rotation oracle lost ancestor.");entity=found->parent;}return q;}
V angular(Q a,Q b){a={-a[0],-a[1],-a[2],a[3]};auto q=product(b,a);if(q[3]<0)for(double& x:q)x=-x;
    const double v=std::hypot(q[0],q[1],q[2]);if(v<1e-14)return {};const double k=2*std::atan2(v,q[3])/(v*Runtime::fixed_dt);return {q[0]*k,q[1]*k,q[2]*k};}
std::shared_ptr<ModelAsset> legs(){
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(7);m->roots={0};m->nodes[0].name=names[0];m->nodes[0].position={0,1,0};
    for(unsigned foot=0;foot<2;++foot){const unsigned b=1+3*foot;
        for(unsigned n=0;n<3;++n){m->nodes[b+n].name=names[b+n];m->nodes[b+n].parent=n?int(b+n-1):0;}
        m->nodes[b].position={foot?.3:-.3,.8,0};m->nodes[b+1].position={.5,-.7,0};m->nodes[b+2].position={-.5,-.7,0};}
    m->animations.push_back({"Original bounded source",2,{
        {0,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,1,0,0},{.015f,1,0,0}}},
        {0,AnimationPath::scale,AnimationInterpolation::linear,{0,2},{{1,1,1,0},{1.000004f,1.000004f,1.000004f,0}}}}});return m;
}
RuntimeTwoBoneIk foot_chain(unsigned slot,bool planted=true){
    RuntimeTwoBoneIk c;c.slot=slot;c.root=1+3*(slot-1);c.middle=c.root+1;c.end=c.root+2;c.space=IkSpace::Rig;c.target={slot==1?-.3:.3,.4,0};
    c.pole={slot==1?-1.:1.,1,0};c.metric_policy=RuntimeIkMetricPolicy::NearUniformV1;c.grounding=RuntimeGroundingDefinition{};
    c.grounding->sole_offset={0,-.2,0};c.grounding->reference_height=.2;c.grounding->max_correction=.6;c.grounding->plant_lock=planted;return c;
}
pr::Configuration profile(){
    pr::Configuration c;c.controller_handoff=pr::ControllerHandoff{actor};c.procedural_pose=pr::ProceduralPosePolicy::GroundedAnimatedOnly;
    c.pose_frame=pr::PoseFramePolicy::SkeletalBoundedScale;c.max_scale_deviation=1e-5;c.profile.root=names[0];const auto m=legs();
    for(unsigned n=0;n<7;++n){pr::Part p;p.name=names[n];p.node=n;p.mass=masses[n];p.shape=pr::Sphere{.04};p.linear_damping=p.angular_damping=0;c.profile.parts.push_back(p);
        if(n){pr::Joint j;j.name="joint"+std::to_string(n);j.parent=names[unsigned(m->nodes[n].parent)];j.child=names[n];j.parent_frame.position=m->nodes[n].position;
            j.limits=pr::SwingTwist{3,3,-3,3};j.friction_torque=0;c.profile.joints.push_back(j);}}
    return c;
}
RuntimeDefinition definition(bool planted=true){
    RuntimeDefinition d;d.world_id="native-grounded-physical";d.authored_revision=27;
    RuntimeEntityDefinition a;a.id=actor;a.transform.position={.6,.1,.25};a.character=CharacterController{};a.character->speed=2;a.character->locomotion=RuntimeCharacterLocomotionProfile{};
    a.character->stance=RuntimeCharacterStanceProfile{1,1,.5};a.character->camera=camera;d.entities.push_back(a);
    const auto m=legs();RuntimeEntityDefinition w;w.id=rig;w.parent=actor;w.animation_rig=RuntimeAnimationRig{m,0,0,1,true,true,{}};
    w.animation_rig->ik={foot_chain(1,planted),foot_chain(2,planted)};w.physical_rig=profile();d.entities.push_back(w);
    for(unsigned n=0;n<7;++n){RuntimeEntityDefinition e;e.id=id(102+n);e.parent=n?id(102+unsigned(m->nodes[n].parent)):rig;e.transform={m->nodes[n].position,m->nodes[n].rotation,m->nodes[n].scale};e.rig_node=RuntimeRigNode{rig,n};d.entities.push_back(e);}
    RuntimeEntityDefinition eye;eye.id=camera;eye.parent=actor;eye.transform.position={.05,1.6,.02};eye.camera=RuntimeCamera{};d.entities.push_back(eye);
    auto independent=std::make_shared<ModelAsset>();independent->nodes.resize(3);independent->roots={0};independent->nodes[0].position={0,1.8,0};
    independent->nodes[1].parent=0;independent->nodes[1].position={.5,-.7,0};independent->nodes[2].parent=1;independent->nodes[2].position={-.5,-.7,0};
    RuntimeEntityDefinition o;o.id=other;o.transform.position={8,0,0};o.animation_rig=RuntimeAnimationRig{independent};auto chain=foot_chain(1,false);chain.root=0;chain.middle=1;chain.end=2;chain.pole={1,1,0};o.animation_rig->ik={chain};d.entities.push_back(o);
    for(unsigned n=0;n<3;++n){RuntimeEntityDefinition e;e.id=id(202+n);e.parent=n?id(201+n):other;e.transform.position=independent->nodes[n].position;e.rig_node=RuntimeRigNode{other,n};d.entities.push_back(e);}
    RuntimeEntityDefinition floor;floor.id=id(300);floor.transform.position={8,-.5,0};floor.collider=BoxCollider{{2,.5f,2},BodyMotion::Static};d.entities.push_back(floor);
    RuntimeSpawnTemplate support;support.id=support_recipe;support.name="retirable real support";support.transform.position={0,-.5,0};support.transform.scale={1.5,1,1.25};support.collider=BoxCollider{{3,.5f,3},BodyMotion::Kinematic};d.templates={support};return d;
}
RuntimeEntityDefinition& def(RuntimeDefinition& d,const std::string& entity){for(auto& e:d.entities)if(e.id==entity)return e;throw std::runtime_error("Definition entity absent.");}
void step(Runtime& r,unsigned ticks=1,const std::vector<PhysicalRigCommand>& physical={},const std::vector<RuntimeGroundingCommand>& requested={},
    const std::vector<RuntimeStructureTick>& structure={},const std::vector<RuntimeInput>& inputs={},const std::vector<KinematicTarget>& motions={}){
    r.step(ticks,inputs,motions,{}, {},structure,{}, {}, {}, {},requested,{}, {},physical);
}
std::string add_support(Runtime& r,std::optional<RuntimeTransform> at={}){return r.change_structure(r.structure_revision(),{{support_recipe,at}},{}).spawned.at(0);}
J saved(const Runtime& r){return J::parse(r.save_snapshot(content));}
const J& physical_row(const J& document,const std::string& selected=rig){for(const auto& row:document.at("payload").at("physical").at("rigs"))if(row.at("entity")==selected)return row;throw std::runtime_error("Physical saved row absent.");}
std::uint32_t body_id(const J& document,const std::string& entity){for(const auto& row:document.at("payload").at("physical").at("bootstrap").at("bodies"))if(row.at("entity")==entity)return row.at("id").get<std::uint32_t>();throw std::runtime_error("Saved native body binding absent.");}
J ground_json(const RuntimeGroundingState& g){
    J hit=nullptr,plant=nullptr;if(g.hit)hit={{"entity",g.hit->entity},{"position",g.hit->position},{"normal",g.hit->normal},{"triangle",g.hit->triangle?J(*g.hit->triangle):J(nullptr)}};
    if(g.planting){const auto& p=*g.planting;J anchor=nullptr;if(p.anchor){const auto& a=*p.anchor;anchor={{"acquired_tick",a.acquired_tick},{"support",a.support},{"triangle",a.triangle?J(*a.triangle):J(nullptr)},
        {"local_point",a.local_point},{"local_normal",a.local_normal},{"local_forward",a.local_forward}};}
        plant={{"armed",p.armed},{"status",p.status},{"anchor",anchor},{"target_sole",p.target_sole?J(*p.target_sole):J(nullptr)},
            {"target_normal",p.target_normal?J(*p.target_normal):J(nullptr)},{"target_forward",p.target_forward?J(*p.target_forward):J(nullptr)},
            {"position_error",p.position_error?J(*p.position_error):J(nullptr)},{"normal_error",p.normal_error?J(*p.normal_error):J(nullptr)}};}
    return {{"decision_tick",g.decision_tick},{"casts",g.casts},{"status",g.status},{"cast",g.cast},{"weight",g.weight},{"input_sole",g.input_sole},
        {"ray_origin",g.ray_origin},{"actual_sole",g.actual_sole},{"actual_normal",g.actual_normal},{"hit",hit},
        {"position_error",g.position_error?J(*g.position_error):J(nullptr)},{"normal_error",g.normal_error?J(*g.normal_error):J(nullptr)},{"planting",plant}};
}
J diagnostic(const RuntimeIkState& s){
    J result={{"entity",s.entity},{"slot",s.slot},{"root",s.root},{"middle",s.middle},{"end",s.end},{"enabled",s.enabled},{"space",s.space==IkSpace::World?"world":"rig"},
        {"target",s.target},{"pole",s.pole},{"weight",s.weight},{"end_position",s.end_position},{"target_error",s.target_error},{"clamped",s.clamped},{"pole_fallback",s.pole_fallback},
        {"end_orientation",nullptr},{"metric",nullptr},{"grounding",s.grounding?ground_json(*s.grounding):J(nullptr)}};
    if(s.end_orientation){const auto& e=*s.end_orientation;result["end_orientation"]={{"rotation",e.goal.rotation},{"weight",e.goal.weight},{"actual_rotation",e.actual_rotation},{"angular_error",e.angular_error}};}
    if(s.metric){const auto& m=*s.metric;result["metric"]={{"evaluated",m.evaluated},{"local_scale_error",m.local_scale_error},{"frame_error",m.frame_error},{"gram_error",m.gram_error},
        {"unweighted_position_error",m.unweighted_position_error?J(*m.unweighted_position_error):J(nullptr)},{"unweighted_position_limit",m.unweighted_position_limit?J(*m.unweighted_position_limit):J(nullptr)}};}return result;
}
J caches(const Runtime& r,const std::string& selected=rig){J states=J::array();for(unsigned slot=1;slot<=2;++slot){const auto s=r.ik(selected,slot);need(bool(s),"Complete configured slot cache absent.");const auto c=r.grounding(selected,slot);need(bool(c)==bool(s->grounding),"Requested multiplier/automatic slot ownership differs.");
    J requested=nullptr;if(c)requested={{"entity",c->entity},{"slot",c->slot},{"enabled",c->enabled},{"weight",c->weight}};states.push_back({{"state",diagnostic(*s)},{"requested",requested}});}return states;}
void same(const Runtime& a,const Runtime& b,const std::string& label,const std::string& selected=rig){
    const auto warm=a.save_snapshot(content),cold=b.save_snapshot(content);gt::artifacts().pair(label,warm,cold);
    if(warm!=cold){const auto diff=J::diff(J::parse(warm),J::parse(cold));std::cerr<<"Whole grounded mismatch "<<label<<" "<<(diff.empty()?"raw JSON bytes":diff.front().dump().substr(0,700))<<'\n';}
    need(warm==cold,"Complete grounded native save differs; no fields may be removed.");
    for(const auto& e:a.live_definition().entities){const auto x=a.entity(e.id),y=b.entity(e.id);need(x.world==y.world&&x.local.position==y.local.position&&x.local.rotation==y.local.rotation&&x.local.scale==y.local.scale&&x.velocity==y.velocity&&x.yaw==y.yaw&&x.pitch==y.pitch,"Complete native entity observation differs after restore.");}
    if(!selected.empty()){need(caches(a,selected)==caches(b,selected),"Complete public frozen foot/requested diagnostics differ.");const auto x=a.procedural_pose(selected),y=b.procedural_pose(selected);need(x&&y&&x->suspended==y->suspended&&x->sampled_tick==y->sampled_tick&&x->slots==y->slots,"Procedural phase/sample tick differs.");}
    need(diagnostic(*a.ik(other,1))==diagnostic(*b.ik(other,1)),"Unrelated live ground cache differs.");
}
std::unique_ptr<Runtime> restore(const RuntimeDefinition& d,const Runtime& r,const std::string& label,const std::string& selected=rig){
    const auto bytes=r.save_snapshot(content);(void)gt::artifacts().retain(label,"source",bytes);auto cold=Runtime::from_snapshot(d,content,bytes);same(r,*cold,label,selected);return cold;
}
RuntimeGroundingState contact(const Runtime& r,const std::string& support,unsigned slot=1,bool planted=true,const std::string& selected=rig){
    const auto s=r.ik(selected,slot);need(s&&s->grounding&&s->grounding->status=="contact"&&s->grounding->cast&&s->enabled&&s->weight>0,"Actual enabled native ground contact is absent.");
    const auto& g=*s->grounding;need(g.decision_tick==r.inspect().tick&&g.casts>0&&g.hit&&g.hit->entity==support,"Contact tick/count/actual support differs.");
    near(g.actual_sole[1],0,2e-5);near(g.actual_normal,{0,1,0},2e-5);
    if(planted){need(g.planting&&g.planting->status=="locked"&&!g.planting->armed&&g.planting->anchor&&g.planting->anchor->support==support&&g.planting->target_sole&&g.planting->target_normal&&g.planting->target_forward,"Complete real plant lock absent.");}
    else need(!g.planting,"Unplanted profile unexpectedly acquired an anchor.");return g;
}
void source_scales(const Runtime& r,const std::string& selected=rig){const double t=double(r.inspect().tick)/120;need(t<1,"Source-scale oracle exceeded its frozen interval.");
    for(const auto& e:r.live_definition().entities)if(e.rig_node&&e.rig_node->rig==selected){const double expected=e.rig_node->node==0?1+(double(1.000004f)-1)*t:1;near(r.entity(e.id).local.scale,{expected,expected,expected},3e-15);}}
void inspect_capture(const Runtime& r,const std::string& support,bool planted=true,const std::string& selected=rig){
    const auto document=saved(r);need(document.at("payload").at("physical").at("version")==4u,"Grounded policy did not select explicit physical extension4.");
    const auto& row=physical_row(document,selected);const auto& c=row.at("procedural_capture");need(c.is_object()&&c.at("version")==2u&&c.at("captured_tick")==r.inspect().tick&&c.at("grounding").at("prepared")==true,"Animated grounded capture is incomplete/not current.");
    need(row.at("procedural_terminal").is_null(),"Animated owner unexpectedly stores a passive terminal.");unsigned rays=0,frames=0;
    const auto state=r.entity(support);const auto expected=rigid(position(r,support),state.local.rotation);const auto raw=body_id(document,support);
    for(const auto& q:c.at("grounding").at("journal")){
        need(q.at("owner")==selected,"Ground witness owner differs.");if(q.at("kind")=="ray"){++rays;need(!q.at("hit").is_null()&&q.at("hit").at("entity")==support,"Actual successful contact ray omitted its hit.");}
        else {need(q.at("kind")=="support","Unknown recorded query kind.");++frames;}
        const auto& b=q.at("binding");need(b.is_object()&&b.at("entity")==support&&b.at("body_id")==raw&&b.at("motion")=="kinematic","Native captured support identity/kind differs.");
        const auto frame=b.at("frame").get<Matrix4>();for(unsigned n=0;n<16;++n)near(frame[n],expected[n],2e-7);
        need(std::abs(state.world[0]-frame[0])>.2,"Support test failed to distinguish rigid native frame from authored render scale.");
    }
    need(rays==2&&frames==(planted?2u:0u),"Final two-foot contact/support query population differs.");
}
void settle(Runtime& r,const std::string& support,bool planted=true){
    step(r,30);need(r.entity(actor).ground=="on_ground","Virtual actor did not establish actual native support.");
    for(unsigned slot=1;slot<=2;++slot){const auto g=contact(r,support,slot,planted);need(g.input_sole[1]>.15,"Grounding fixture had no substantial source-to-surface correction.");}
    (void)contact(r,id(300),1,false,other);inspect_capture(r,support,planted);
}
void admission_initial_and_actual_contacts(){
    for(bool planted:{false,true}){const auto d=definition(planted);Runtime r(d);const auto support=add_support(r);const auto initial=saved(r);
        need(initial.at("payload").at("physical").at("version")==4u&&!physical_row(initial).at("procedural_capture").at("grounding").at("prepared").get<bool>(),"Initial new-policy capture is not explicit unprepared extension4.");
        auto cold=restore(d,r,"initial-unprepared");const auto before=r.save_snapshot(content);rejects([&]{step(r,1,{{rig,true,{}}});});need(r.save_snapshot(content)==before,"Premature activation changed complete unprepared state.");
        step(*cold,30);settle(r,support,planted);same(r,*cold,"prepared-alive-complete");source_scales(r);
        const auto stable=r.save_snapshot(content);for(unsigned n=0;n<4;++n){(void)r.ik(rig,1);(void)r.ik(rig,2);(void)r.grounding(rig,1);(void)r.procedural_pose(rig);}
        need(r.save_snapshot(content)==stable,"Inspection refreshed contact/plant decisions.");
    }
    const auto good=definition();for(unsigned kind=0;kind<6;++kind){auto d=good;auto& w=def(d,rig);
        if(kind==0)w.physical_rig->procedural_pose=pr::ProceduralPosePolicy::AnimatedOnly;
        if(kind==1)for(auto& c:w.animation_rig->ik)c.grounding.reset();
        if(kind==2)w.physical_rig->controller_handoff.reset();if(kind==3)w.physical_rig->initially_passive=true;
        if(kind==4)w.animation_rig->root_motion=RuntimeRootMotionDefinition{0,RootMotionMode::Extract,{},false,false};
        if(kind==5)def(d,id(103)).camera=RuntimeCamera{};
        rejects([&]{Runtime rejected(d);});}
}
struct Sample {std::array<V,7> position;std::array<Q,7> rotation;};
Sample sample(const Runtime& r){Sample s;for(unsigned n=0;n<7;++n){s.position[n]=position(r,id(102+n));s.rotation[n]=rotation(r,id(102+n));}return s;}
void moving_support_corrected_momentum(){
    const auto d=definition();Runtime r(d);const auto support=add_support(r);settle(r,support);
    const auto static_soles=std::array<V,2>{r.ik(rig,1)->grounding->actual_sole,r.ik(rig,2)->grounding->actual_sole};
    const KinematicTarget motion{support,{.6,-.5,.2},yaw(.3),60};step(r,1,{}, {}, {}, {},{motion});step(r,18);const auto previous=sample(r);step(r);const auto current=sample(r);
    need(r.inspect().tick==50,"Moving support sample did not reach its predeclared twentieth motion tick.");
    const auto motor=r.entity(actor);need(motor.locomotion&&norm(motor.locomotion->ground_velocity)>.1,"Native moving support velocity precondition is absent.");
    near(position(r,support),{.2,-.5,.2/3},3e-6);for(unsigned slot=1;slot<=2;++slot){const auto g=contact(r,support,slot);need(norm(subtract(g.actual_sole,static_soles[slot-1]))>.05,"Actual locked foot did not follow translated/rotated support.");}
    inspect_capture(r,support);const auto before=saved(r);const auto& h=physical_row(before).at("pose");
    need(h.at("previous").at("tick")==49u&&h.at("current").at("tick")==50u,"Final corrected history does not span the actual moving ticks.");
    V expected_p{},expected_l{};double rotation_budget=0;
    for(unsigned n=0;n<7;++n){V velocity{};const auto omega=angular(previous.rotation[n],current.rotation[n]);
        for(unsigned k=0;k<3;++k){velocity[k]=(current.position[n][k]-previous.position[n][k])/Runtime::fixed_dt;
            near(h.at("current").at("bodies").at(n).at("position").at(k).get<double>(),current.position[n][k],2e-8);
            near(h.at("current").at("bodies").at(n).at("linear_velocity").at(k).get<double>(),velocity[k],2e-6);
            near(h.at("current").at("bodies").at(n).at("angular_velocity").at(k).get<double>(),omega[k],2e-6);rotation_budget+=std::abs(omega[k]);}
        need(current.position[n][1]>.1,"Proxy is too close to the floor for a free first-step momentum oracle.");
        for(unsigned other_index=0;other_index<n;++other_index)need(norm(subtract(current.position[n],current.position[other_index]))>.085,"Initial physical proxy population can self-contact.");
        velocity[1]+=double(-9.81f)*Runtime::fixed_dt;const auto orbital=cross(current.position[n],velocity);
        for(unsigned k=0;k<3;++k){expected_p[k]+=masses[n]*velocity[k];expected_l[k]+=masses[n]*orbital[k]+.4*masses[n]*.04*.04*omega[k];}
    }
    need(rotation_budget>.1&&norm(expected_p)>.1,"Transfer oracle lacks actual source/platform momentum.");
    const auto frozen=caches(r);auto cold_alive=restore(d,r,"moving-alive-cold");RuntimeInput held;held.entity=actor;held.move={1,-1};held.look={37,19};held.jump=true;
    step(r,1,{{rig,true,{}}},{},{},{held});step(*cold_alive,1,{{rig,true,{}}});same(r,*cold_alive,"moving-activation-held-neutral");
    const auto phase=r.procedural_pose(rig);need(phase&&phase->suspended&&phase->sampled_tick==50&&caches(r)==frozen,"Activation failed to freeze the last actual complete planted decision.");
    const auto native=r.physical_rig(rig,true,false);need(native&&native->parts.size()==7&&native->mode==PhysicalRigMode::Passive,"Physical takeover did not enroll all seven parts.");
    V actual_p{},actual_l{};std::vector<std::uint32_t> ids;
    for(const auto& p:native->parts){need(p.enrolled&&p.linear_damping==0&&p.angular_damping==0,"Momentum damping/enrollment precondition differs.");ids.push_back(p.body_id);const auto orbital=cross(p.pose.position,p.linear_velocity);
        for(unsigned k=0;k<3;++k){actual_p[k]+=p.mass*p.linear_velocity[k];actual_l[k]+=p.mass*orbital[k]+.4*p.mass*.04*.04*p.angular_velocity[k];}}
    const auto activated=saved(r);for(const auto& p:activated.at("payload").at("physical").at("physics").at("pairs"))
        need(std::find(ids.begin(),ids.end(),p.at("body_a").get<std::uint32_t>())==ids.end()&&std::find(ids.begin(),ids.end(),p.at("body_b").get<std::uint32_t>())==ids.end(),"First physical momentum tick had an actual contact on a rig proxy.");
    near(actual_p,expected_p,1e-3);near(actual_l,expected_l,5e-3);
    need(physical_row(activated).at("procedural_capture")==physical_row(activated).at("procedural_terminal"),"Passive capture and terminal must be the same literal historical witness.");
    auto cold=restore(d,r,"moving-passive-cold");auto active_casts=r.ik(other,1)->grounding->casts;
    for(unsigned frame=0;frame<8;++frame){step(r,1,{}, {}, {},{held});step(*cold);same(r,*cold,"moving-passive-future-"+std::to_string(frame));source_scales(r);
        need(caches(r)==frozen&&r.procedural_pose(rig)->sampled_tick==50,"Passive sampling changed historical contact/plant/requested state.");
        const auto current_casts=contact(r,id(300),1,false,other).casts;need(current_casts==active_casts+1,"Suspension stopped unrelated live contact queries.");active_casts=current_casts;}
    const auto current_support=rigid(position(r,support),r.entity(support).local.rotation);const auto old=r.ik(rig,1)->grounding;
    need(norm(subtract(point(current_support,old->planting->anchor->local_point),*old->planting->target_sole))>.02,"Moving support did not diverge from its deliberately historical frozen frame.");
}
void retired_support(bool same_batch){
    const auto d=definition();Runtime r(d);const auto support=add_support(r);settle(r,support);const auto frozen=caches(r);const auto raw=body_id(saved(r),support);
    if(same_batch)step(r,1,{{rig,true,{}}},{},{{0,r.structure_revision(),{}, {support}}});
    else {step(r,1,{{rig,true,{}}});r.change_structure(r.structure_revision(),{}, {support});}
    rejects([&]{(void)r.entity(support);});need(caches(r)==frozen,"Support retirement changed frozen contact/plant witnesses.");
    const auto retired=r.save_snapshot(content);(void)gt::artifacts().retain(same_batch?"activation-batch-retirement":"post-death-retirement","source",retired);
    auto cold=Runtime::from_snapshot(d,content,retired);same(r,*cold,"retired-support-cold");
    RuntimeTransform placement;placement.position={-8,-.5,0};placement.scale={1.5,1,1.25};
    const auto replacement=add_support(r,placement),twin=add_support(*cold,placement);need(replacement==twin&&replacement!=support,"Retired support public identity was reused or cold allocation diverged.");
    const auto new_raw=body_id(saved(r),replacement);need((new_raw&0x007fffffu)==(raw&0x007fffffu)&&new_raw!=raw,"Negative control did not actually reuse a native body slot with a different generation.");
    same(r,*cold,"retired-support-reused-slot");
    // New causal negative, added after native-1 closed. The complete original
    // current-generation world must cold-load successfully first. Rewrite only
    // the old entity's complete journal binding IDs to a VALID ID now owned by
    // the different live replacement, in both capture and terminal. The old
    // retired ID remains valid history; the foreign current owner is not.
    const std::string graft_label=same_batch?"activation-batch-foreign-live-owner":"post-death-foreign-live-owner";
    const auto legitimate=r.save_snapshot(content);(void)gt::artifacts().retain(graft_label+"-legitimate","source",legitimate);
    auto legitimate_cold=Runtime::from_snapshot(d,content,legitimate);same(r,*legitimate_cold,graft_label+"-legitimate-cold");
    auto graft=J::parse(legitimate);auto capture=physical_row(graft).at("procedural_capture");unsigned replaced=0;
    for(auto& query:capture["grounding"]["journal"]){auto& binding=query["binding"];
        if(!binding.is_null()&&binding.at("entity")==support){need(binding.at("body_id")==raw,"Legitimate historical binding lost its original raw generation.");binding["body_id"]=new_raw;++replaced;}}
    need(replaced==4,"Foreign-owner negative did not rewrite every actual two-foot support/ray binding.");
    graft["payload"]["physical"]["rigs"][0]["procedural_capture"]=capture;graft["payload"]["physical"]["rigs"][0]["procedural_terminal"]=capture;
    const auto payload=graft.at("payload").dump();graft["sha256"]=sha256(std::as_bytes(std::span(payload.data(),payload.size())));const auto graft_bytes=graft.dump();
    (void)gt::artifacts().retain(graft_label,"negative",graft_bytes);bool rejected=false;
    try{(void)Runtime::from_snapshot(d,content,graft_bytes);}catch(const std::exception& error){rejected=true;std::cout<<graft_label<<" rejected: "<<error.what()<<'\n';}
    need(rejected,"Valid current native ID owned by a different live entity was accepted as the retired support binding.");
    need(r.save_snapshot(content)==legitimate,"Detached foreign-owner graft probe changed the original legitimate source.");
    for(unsigned frame=0;frame<8;++frame){step(r);step(*cold);same(r,*cold,"retired-support-future-"+std::to_string(frame));need(caches(r)==frozen,"Historical foot silently rebound to replacement/current support.");
        (void)contact(r,id(300),1,false,other);source_scales(r);}
}
void requested_multipliers_mixed_slots_and_rollback(){
    auto d=definition();Runtime r(d);const auto support=add_support(r);settle(r,support);
    step(r,1,{},{{rig,1,true,.65}});const auto requested=r.grounding(rig,1);const auto effective=r.ik(rig,1);
    need(requested&&requested->enabled&&requested->weight==.65&&effective&&effective->grounding&&effective->grounding->status=="contact"&&effective->weight==.65,
        "Requested multiplier and generated contact weight were not independently applied.");
    const auto before=r.save_snapshot(content);const auto frozen=caches(r);
    rejects([&]{step(r,1,{{rig,true,{}}},{{rig,1,true,.65}});});need(r.save_snapshot(content)==before,"Same-tick identical requested edit bypassed activation conflict or changed state.");
    rejects([&]{step(r,4,{{rig,true,{}}},{},{{2,999,{}, {support}}});});
    need(r.save_snapshot(content)==before&&caches(r)==frozen&&!r.procedural_pose(rig)->suspended,"Late failure did not roll back native phase, generated controls and requested multipliers.");
    RuntimeGroundingCommand malformed{rig,1,true,std::numeric_limits<double>::quiet_NaN()};rejects([&]{step(r,1,{}, {malformed});});need(r.save_snapshot(content)==before,"Malformed multiplier changed current source state.");
    auto alive=restore(d,r,"nonidentity-requested-alive");step(r,1,{{rig,true,{}}});step(*alive,1,{{rig,true,{}}});same(r,*alive,"nonidentity-requested-activation");
    const auto passive=r.save_snapshot(content);rejects([&]{step(r,1,{},{{rig,1,false,0}});});need(r.save_snapshot(content)==passive,"Passive multiplier write changed frozen state.");
    auto cold=restore(d,r,"nonidentity-requested-passive");for(unsigned n=0;n<4;++n){step(r);step(*cold);same(r,*cold,"nonidentity-requested-future-"+std::to_string(n));need(caches(r)==frozen,"Passive requested multiplier or generated controls changed.");}
    auto mixed=definition();auto& chain=def(mixed,rig).animation_rig->ik[1];chain.grounding.reset();chain.target={.3,.3,0};chain.end_orientation=RuntimeIkEndOrientation{yaw(.2),.35};
    Runtime m(mixed);const auto main=add_support(m);step(m,30);(void)contact(m,main,1,true);need(!m.ik(rig,2)->grounding&&!m.grounding(rig,2),"Disjoint manual branch was granted automatic ownership.");
    RuntimeIkCommand control{rig,2,true,IkSpace::Rig,{.35,.35,.05},{1,1,0},1,RuntimeIkEndOrientation{yaw(.2),.35}};m.step(1,{}, {}, {}, {}, {}, {control});
    need(m.ik(rig,2)->target==control.target&&m.ik(rig,2)->target_error<2e-6,"Mixed manual chain setter did not solve beside the planted foot.");
    const auto mixed_frozen=caches(m);step(m,1,{{rig,true,{}}});auto m_cold=restore(mixed,m,"mixed-grounded-manual-passive");
    for(unsigned n=0;n<4;++n){step(m);step(*m_cold);same(m,*m_cold,"mixed-grounded-manual-future-"+std::to_string(n));need(caches(m)==mixed_frozen,"Mixed ground/manual terminal changed after takeover.");}
}
void born_yaw_and_last_spawned_retirement(){
    auto d=definition();RuntimeSpawnTemplate recipe;recipe.id=actor_recipe;recipe.name="Grounded analytic actor";recipe.root=actor;
    std::set<std::string> primary{actor,rig,camera};for(unsigned n=0;n<7;++n)primary.insert(id(102+n));
    for(const auto& e:d.entities)if(primary.contains(e.id))recipe.entities.push_back(e);std::erase_if(d.entities,[&](const auto& e){return primary.contains(e.id);});d.templates.push_back(recipe);
    Runtime r(d);const auto support=add_support(r);need(saved(r).at("payload").at("physical").at("version")==4u&&saved(r).at("payload").at("physical").at("rigs").empty(),"Dormant grounded recipe did not retain explicit extension4.");
    RuntimeTransform placement;placement.position={.6,.1,.25};placement.rotation=yaw(-.527);const auto born=r.change_structure(r.structure_revision(),{{actor_recipe,placement}},{}).spawned.at(0);
    const auto selected=r.instance_node(born,rig);const auto initial=r.save_snapshot(content);rejects([&]{step(r,1,{{selected,true,{}}});});need(r.save_snapshot(content)==initial,"Born grounded owner transferred before a committed movement/contact tick.");
    auto initial_cold=restore(d,r,"born-nonidentity-unprepared",selected);step(r,12);step(*initial_cold,12);same(r,*initial_cold,"born-nonidentity-prepared",selected);
    near(r.entity(born).world[0],std::cos(-.527),2e-6);near(r.entity(born).world[2],-std::sin(-.527),2e-6);for(unsigned slot=1;slot<=2;++slot)(void)contact(r,support,slot,true,selected);
    const auto frozen=caches(r,selected);step(r,1,{{selected,true,{}}});step(*initial_cold,1,{{selected,true,{}}});same(r,*initial_cold,"born-nonidentity-activation",selected);
    auto cold=restore(d,r,"born-nonidentity-passive",selected);for(unsigned n=0;n<4;++n){step(r);step(*cold);same(r,*cold,"born-passive-future-"+std::to_string(n),selected);need(caches(r,selected)==frozen,"Born owner lost historical lock/cache.");}
    r.change_structure(r.structure_revision(),{}, {born});cold->change_structure(cold->structure_revision(),{}, {born});same(r,*cold,"last-spawned-owner-retired",{});
    const auto retired=saved(r);need(retired.at("payload").at("physical").at("version")==4u&&retired.at("payload").at("physical").at("rigs").empty(),"Last spawned retirement dropped dormant recipe's extension4 boundary.");
    auto final_cold=restore(d,r,"last-owner-retired-cold",{});step(r,2);step(*final_cold,2);same(r,*final_cold,"last-owner-retired-future",{});
}
std::string reseal(J document){const auto payload=document.at("payload").dump();document["sha256"]=sha256(std::as_bytes(std::span(payload.data(),payload.size())));return document.dump();}
void historical_capture_corruption(){
    const auto d=definition();Runtime r(d);const auto support=add_support(r);settle(r,support);step(r,1,{{rig,true,{}}});const auto pristine=r.save_snapshot(content);const auto original=J::parse(pristine);
    (void)gt::artifacts().retain("corruption-original","source",pristine);auto valid=Runtime::from_snapshot(d,content,pristine);same(r,*valid,"corruption-original-valid");
    for(unsigned kind=0;kind<10;++kind){auto document=original;auto changed=physical_row(original).at("procedural_terminal");auto& g=changed["grounding"];
        if(kind==0)changed["captured_tick"]=changed["captured_tick"].get<std::uint64_t>()+1;
        if(kind==1)g["requested"][0]["weight"]=.5;
        if(kind==2)g["previous"][0]["grounding"]["planting"]["anchor"]["local_point"][0]=g["previous"][0]["grounding"]["planting"]["anchor"]["local_point"][0].get<double>()+.25;
        if(kind==3){for(auto& query:g["journal"])if(query["kind"]=="ray"){query["ray"]["origin"][0]=query["ray"]["origin"][0].get<double>()+.01;break;}}
        if(kind==4)g["journal"][0]["binding"]["body_id"]=g["journal"][0]["binding"]["body_id"].get<std::uint32_t>()^0x01000000u;
        if(kind==5)g["journal"][0]["binding"]["frame"][0]=g["journal"][0]["binding"]["frame"][0].get<double>()*1.2;
        if(kind==6)changed["states"][0]["grounding"]["state"]["actual_sole"][1]=changed["states"][0]["grounding"]["state"]["actual_sole"][1].get<double>()+.01;
        if(kind==7)g["previous_controls"][0]["enabled"]=false;
        if(kind==8)g["previous_requested"][0]["weight"]=.5;
        if(kind==9){bool changed_normal=false;for(auto& query:g["journal"])if(query["kind"]=="ray"){
            need(!query["hit"].is_null()&&query["hit"]["normal"][1]==1.0,"Precision negative lacks its actual exact flat-floor normal.");
            query["hit"]["normal"][1]=std::nextafter(1.0,2.0);changed_normal=true;break;}
            need(changed_normal,"Precision negative did not edit one actual native ray normal.");}
        document["payload"]["physical"]["rigs"][0]["procedural_capture"]=changed;document["payload"]["physical"]["rigs"][0]["procedural_terminal"]=changed;
        const auto bytes=reseal(document);(void)gt::artifacts().retain("ground-witness-corruption-"+std::to_string(kind),"negative",bytes);
        rejects([&]{(void)Runtime::from_snapshot(d,content,bytes);});need(r.save_snapshot(content)==pristine,"Detached corruption probe mutated original native state.");
    }
}
void strict_terminal_codec_corruption(){
    const auto d=definition();Runtime source(d);const auto support=add_support(source);settle(source,support);
    const std::array<const char*,28> labels{
        "missing-state-field","extra-state-field","numeric-boolean","floating-slot","reordered-states","extra-state",
        "target-duplicate-one-ulp","weight-duplicate-one-ulp","orientation-goal-duplicate-one-ulp","zero-goal-weight",
        "noncanonical-actual-rotation","metric-residual-membership","numeric-metric-boolean","metric-bound",
        "ground-cache-duplicate-one-ulp","plant-anchor-duplicate-one-ulp","missing-ground-cache-field",
        "reordered-poses","extra-pose-field","zero-pose-quaternion","nonpositive-pose-scale","text-pose-coordinate",
        "integer-pose-scale","foreign-pose-node","nonobject-state","extra-metric-field","short-end-position",
        "short-orientation-diagnostic"};
    for(unsigned phase=0;phase<2;++phase){
        if(phase)step(source,1,{{rig,true,{}}});
        const auto pristine=source.save_snapshot(content);const auto original=J::parse(pristine);
        const std::string prefix=phase?"codec-passive":"codec-active";
        (void)gt::artifacts().retain(prefix+"-original","source",pristine);
        auto valid=Runtime::from_snapshot(d,content,pristine);same(source,*valid,prefix+"-original-valid");
        const auto& actual_capture=physical_row(original).at("procedural_capture");
        need(actual_capture.at("states").size()==2&&actual_capture.at("poses").size()==7,
            "Codec negative source lacks its complete real two-foot capture.");
        const auto& first=actual_capture.at("states")[0];
        need(!first.at("end_orientation").is_null()&&!first.at("metric").is_null()&&
            !first.at("metric").at("unweighted_position_error").is_null()&&
            !first.at("grounding").at("planting").at("anchor").is_null(),
            "Codec negative source lacks actual orientation, metric or locked-plant diagnostics.");
        for(unsigned kind=0;kind<labels.size();++kind){
            auto document=original;auto changed=actual_capture;auto& state=changed["states"][0];
            auto& pose=changed["poses"][0];
            if(kind==0)state.erase("target_error");
            if(kind==1)state["unknown"]=true;
            if(kind==2)state["enabled"]=1;
            if(kind==3)state["slot"]=double(state["slot"].get<std::uint32_t>());
            if(kind==4)std::swap(changed["states"][0],changed["states"][1]);
            if(kind==5)changed["states"].push_back(state);
            if(kind==6)state["target"][0]=std::nextafter(state["target"][0].get<double>(),std::numeric_limits<double>::infinity());
            if(kind==7)state["weight"]=std::nextafter(state["weight"].get<double>(),0.);
            if(kind==8)state["end_orientation"]["goal"]["rotation"][0]=std::nextafter(
                state["end_orientation"]["goal"]["rotation"][0].get<double>(),std::numeric_limits<double>::infinity());
            if(kind==9)state["end_orientation"]["goal"]["weight"]=0.;
            if(kind==10)for(auto& component:state["end_orientation"]["actual_rotation"])component=-component.get<double>();
            if(kind==11)state["metric"]["unweighted_position_limit"]=nullptr;
            if(kind==12)state["metric"]["evaluated"]=1;
            if(kind==13)state["metric"]["frame_error"]=std::nextafter(1e-4,1.);
            if(kind==14)state["grounding"]["state"]["actual_sole"][0]=std::nextafter(
                state["grounding"]["state"]["actual_sole"][0].get<double>(),std::numeric_limits<double>::infinity());
            if(kind==15)state["grounding"]["planting"]["anchor"]["local_point"][0]=std::nextafter(
                state["grounding"]["planting"]["anchor"]["local_point"][0].get<double>(),std::numeric_limits<double>::infinity());
            if(kind==16)state["grounding"]["state"].erase("casts");
            if(kind==17)std::swap(changed["poses"][0],changed["poses"][1]);
            if(kind==18)pose["unknown"]=false;
            if(kind==19)pose["rotation"]=J::array({0.,0.,0.,0.});
            if(kind==20)pose["scale"][0]=0.;
            if(kind==21)pose["position"][0]="0.0";
            if(kind==22)pose["scale"][0]=1;
            if(kind==23)pose["entity"]=other;
            if(kind==24)changed["states"][0]=nullptr;
            if(kind==25)state["metric"]["unknown"]=0.;
            if(kind==26)state["end_position"]=J::array({0.,0.});
            if(kind==27)state["end_orientation"]["actual_rotation"]=J::array({0.,0.,1.});
            need(changed.dump()!=actual_capture.dump(),"Codec negative did not modify the real captured witness.");
            auto& row=document["payload"]["physical"]["rigs"][0];
            need(row.at("entity")==rig,"Codec negative selected the wrong physical owner.");
            row["procedural_capture"]=changed;if(phase)row["procedural_terminal"]=changed;
            const auto bytes=reseal(document);
            (void)gt::artifacts().retain(prefix+"-"+labels[kind],"negative",bytes);
            rejects([&]{(void)Runtime::from_snapshot(d,content,bytes);});
            need(source.save_snapshot(content)==pristine,"Detached codec corruption probe changed its source runtime.");
        }
    }
}
void synthetic_rounding_rollback_preserves_original_locals(){
    auto d=definition();def(d,rig).transform.position[0]=.125;
    Runtime source(d);const auto support=add_support(source);settle(source,support);
    step(source,1,{{rig,true,{}}});const auto pristine=source.save_snapshot(content);const auto original=J::parse(pristine);
    (void)gt::artifacts().retain("synthetic-rounding-original-native-source","source",pristine);
    auto control=Runtime::from_snapshot(d,content,pristine);same(source,*control,"synthetic-rounding-original-valid");
    const std::array<std::string,3> selected{actor,rig,id(102)};
    const std::array<const char*,3> labels{"linked-actor","rig-wrapper","mapped-pelvis"};
    for(std::size_t kind=0;kind<selected.size();++kind){
        auto document=original;J* changed=nullptr;
        for(auto& row:document["payload"]["entities"])if(row.at("id")==selected[kind]){changed=&row;break;}
        need(changed!=nullptr,"Synthetic rounding witness selected an absent owner node.");
        const auto position=(*changed).at("local").at("position").get<V>();std::size_t axis=0;
        for(std::size_t i=1;i<position.size();++i)if(std::abs(position[i])>std::abs(position[axis]))axis=i;
        auto& component=(*changed)["local"]["position"][axis];const auto before=component.get<double>();
        const auto adjacent=std::nextafter(before,std::numeric_limits<double>::infinity());
        need(std::isnormal(before)&&std::abs(before)>=1e-4&&std::isfinite(adjacent)&&adjacent!=before&&
            std::abs(adjacent-before)<=128*std::numeric_limits<double>::epsilon()*std::max(1.,std::abs(before)),
            "Synthetic adjacent-double witness exceeds the reader's small-value rounding resolution.");
        component=adjacent;
        // This is a deliberately constructed rounding regression, not a
        // platform-produced save and not cross-platform qualification evidence.
        const auto synthetic=reseal(document);need(synthetic!=pristine,"Synthetic rounding witness is unchanged.");
        const std::string prefix=std::string("synthetic-rounding-")+labels[kind];
        (void)gt::artifacts().retain(prefix+"-input","synthetic-rounding-witness",synthetic);
        auto restored=Runtime::from_snapshot(d,content,synthetic);
        const auto immediate=restored->save_snapshot(content);gt::artifacts().pair(prefix+"-load",synthetic,immediate);
        need(immediate==synthetic&&restored->entity(selected[kind]).local.position[axis]==adjacent,
            "Admitted synthetic rounding local was regenerated during restore.");
        const auto tick=restored->inspect().tick;const auto revision=restored->structure_revision();
        bool late_step_rejected=false;
        try{step(*restored,4,{}, {},{{2,revision+1,{}, {support}}});}
        catch(const std::exception& error){late_step_rejected=std::string(error.what()).find("Stale runtime structure revision")!=std::string::npos;}
        need(late_step_rejected,"Synthetic rounding step did not reach its intended late structural rejection.");
        const auto after_step=restored->save_snapshot(content);gt::artifacts().pair(prefix+"-late-step-rollback",synthetic,after_step);
        need(after_step==synthetic&&restored->inspect().tick==tick&&restored->structure_revision()==revision,
            "Rejected step regenerated an admitted original local or changed complete saved state.");
        // A valid outer revision enters change_structure's checkpoint/try/catch;
        // the absent spawned root rejects inside publish_structure. Even this
        // no-op failure must not let rollback synchronization change any local.
        bool structure_rejected=false;
        try{(void)restored->change_structure(revision,{}, {id(777)});}
        catch(const std::exception& error){structure_rejected=std::string(error.what()).find("Removal needs a unique live spawned instance root")!=std::string::npos;}
        need(structure_rejected,"Synthetic rounding structural probe did not enter its intended rejection path.");
        const auto after_structure=restored->save_snapshot(content);gt::artifacts().pair(prefix+"-structure-rollback",synthetic,after_structure);
        need(after_structure==synthetic&&restored->inspect().tick==tick&&restored->structure_revision()==revision,
            "Rejected structure edit regenerated an admitted original local or changed complete saved state.");
        auto reopened=Runtime::from_snapshot(d,content,after_structure);
        same(*restored,*reopened,prefix+"-reopened-after-rejections");
        need(source.save_snapshot(content)==pristine,"Synthetic rounding probe mutated the original native source.");
    }
}
void noncontact_decision_case(unsigned kind){
    static const std::array<const char*,7> labels{"requested-disabled","requested-zero-weight","authored-lifted","actual-ray-miss","correction-limit","tilted-support-slope","actual-plant-support-lost"};
    static const std::array<const char*,7> statuses{"disabled","disabled","lifted","miss","correction-limit","slope","plant-support-lost"};
    const std::string label=std::string("noncontact-")+labels.at(kind);auto d=definition();
    for(auto& chain:def(d,rig).animation_rig->ik){auto& g=*chain.grounding;
        if(kind==2)g.reference_height=-1;if(kind==3)g.probe_below=.01;if(kind==4)g.max_correction=.01;if(kind==5)g.max_slope_degrees=1;}
    Runtime r(d);std::optional<RuntimeTransform> placement;
    if(kind==5){RuntimeTransform at;at.position={0,-.5,0};at.rotation={0,0,std::sin(.05),std::cos(.05)};at.scale={1.5,1,1.25};placement=at;}
    const auto support=add_support(r,placement);step(r,30);need(r.entity(actor).ground=="on_ground","Noncontact fixture lacks real virtual-controller support before its decision.");
    if(kind==0||kind==1||kind==6){for(unsigned slot=1;slot<=2;++slot)(void)contact(r,support,slot,true);
        (void)gt::artifacts().retain(label+"-last-real-contact","source",r.save_snapshot(content));}
    const bool casts=kind==3||kind==4||kind==5,hit=kind==4||kind==5;
    auto inspect=[&]{
        const auto document=saved(r);const auto& capture=physical_row(document).at("procedural_capture");const auto& ground=capture.at("grounding");
        need(ground.at("prepared")==true&&capture.at("captured_tick")==r.inspect().tick,"Noncontact decision was not a completed current capture.");
        unsigned rays=0,support_calls=0;for(const auto& query:ground.at("journal")){
            if(query.at("kind")=="ray"){++rays;need(query.at("hit").is_null()==!hit,"Actual noncontact journal hit membership differs.");
                if(hit)need(query.at("hit").at("entity")==support&&!query.at("binding").is_null(),"Rejected native hit lost its actual support identity.");
                else need(query.at("binding").is_null(),"Miss acquired a fabricated native support binding.");}
            else {need(query.at("kind")=="support","Unknown noncontact journal query kind.");++support_calls;
                need(kind==6&&query.at("entity")==support&&query.at("frame").is_null()&&query.at("binding").is_null(),"Lost plant lookup did not preserve actual absent support.");}}
        need(rays==(casts?2u:0u)&&support_calls==(kind==6?2u:0u),"Noncontact owner-phase ray/support population differs.");
        for(unsigned slot=1;slot<=2;++slot){const auto state=r.ik(rig,slot);need(state&&state->grounding&&!state->enabled&&state->weight==0&&!state->end_orientation,"Noncontact decision retained an effective IK correction.");
            const auto& g=*state->grounding;need(g.status==statuses[kind]&&g.decision_tick==r.inspect().tick&&g.cast==casts&&g.hit.has_value()==hit&&g.weight==0,
                "Actual noncontact status/tick/cast/hit/weight differs from its fixed scenario.");
            need(g.casts==(kind==2?0u:casts?r.inspect().tick:30u),"Noncontact cast history was advanced or discarded.");
            need(g.position_error.has_value()==hit&&g.normal_error.has_value()==hit,"Noncontact residual membership differs from actual hit state.");
            need(g.planting&&!g.planting->anchor&&!g.planting->target_sole&&!g.planting->target_normal&&!g.planting->target_forward,
                "Noncontact decision retained a stale anchor/plant target.");
            const std::string plant=kind<=1?"disabled":kind==2?"lifted":kind==6?"released-support-lost":"seeking";
            need(g.planting->status==plant&&g.planting->armed==(kind!=6),"Noncontact planting latch/status differs.");
            if(kind==4)need(*g.position_error>.1,"Correction-limit scenario lacks a substantial measured uncorrected sole residual.");
            if(kind==5){need(g.hit->normal[1]<std::cos(std::acos(-1.)/180.)&&std::abs(g.hit->normal[0])>.09,"Slope rejection did not use the actual nonflat native normal.");
                near(g.hit->normal,{-std::sin(.1),std::cos(.1),0},3e-6);}
            const auto requested=r.grounding(rig,slot);need(requested&&requested->enabled==(kind!=0)&&requested->weight==(kind==1?0.:1.),"Requested multiplier was confused with generated noncontact controls.");
        }
        near(r.entity(actor).local.rotation[0],0,0);near(r.entity(actor).local.rotation[2],0,0);(void)contact(r,id(300),1,false,other);
    };
    if(kind<=1){step(r,1,{},{{rig,1,kind!=0,kind==1?0.:1.},{rig,2,kind!=0,kind==1?0.:1.}});
        (void)gt::artifacts().retain(label+"-transition","source",r.save_snapshot(content));inspect();auto transition=restore(d,r,label+"-transition-cold");
        step(r);step(*transition);same(r,*transition,label+"-stable-noncontact");}
    if(kind==6)step(r,1,{}, {},{{0,r.structure_revision(),{}, {support}}});
    (void)gt::artifacts().retain(label+"-alive-decision","source",r.save_snapshot(content));inspect();
    auto alive=restore(d,r,label+"-alive-cold");const auto frozen=caches(r);const auto tick=r.inspect().tick;const auto independent=r.ik(other,1)->grounding->casts;
    step(r,1,{{rig,true,{}}});step(*alive,1,{{rig,true,{}}});same(r,*alive,label+"-immediate-takeover");
    need(caches(r)==frozen&&r.procedural_pose(rig)->suspended&&r.procedural_pose(rig)->sampled_tick==tick,"Takeover changed noncontact decision/requested/plant history.");
    need(r.ik(other,1)->grounding->casts==independent+1,"Noncontact takeover stopped unrelated live grounding.");
    auto cold=restore(d,r,label+"-passive-cold");for(unsigned frame=0;frame<4;++frame){step(r);step(*cold);same(r,*cold,label+"-passive-future-"+std::to_string(frame));source_scales(r);
        need(caches(r)==frozen&&r.procedural_pose(rig)->sampled_tick==tick,"Passive noncontact cache was recast/released/relabeled.");
        need(contact(r,id(300),1,false,other).casts==independent+2+frame,"Unrelated current ground decision did not advance exactly once.");}
}
void noncontact_decision_handoffs(){
    unsigned failures=0;for(unsigned kind=0;kind<7;++kind){try{noncontact_decision_case(kind);std::cout<<"noncontact scenario "<<kind<<" PASS\n";}
        catch(const std::exception& error){++failures;std::cerr<<"noncontact scenario "<<kind<<" FAIL: "<<error.what()<<'\n';}}
    need(failures==0,"One or more fixed actual noncontact scenarios failed; all were attempted.");
}
Matrix4 multiply_independent(const Matrix4& a,const Matrix4& b){Matrix4 r{};for(unsigned c=0;c<4;++c)for(unsigned row=0;row<4;++row)for(unsigned k=0;k<4;++k)r[4*c+row]+=a[4*k+row]*b[4*c+k];return r;}
Matrix4 local_independent(const RuntimeTransform& t){auto m=rigid(t.position,t.rotation);for(unsigned c=0;c<3;++c)for(unsigned row=0;row<3;++row)m[4*c+row]*=t.scale[c];return m;}
void actual_journal_replay_and_queryless_preview(){
    const auto full=definition();Runtime native(full);const auto support=add_support(native);auto isolated=full;
    std::erase_if(isolated.entities,[](const auto& e){return e.id==other||e.id==id(202)||e.id==id(203)||e.id==id(204)||e.id==id(300);});
    RuntimeAnimations sampler(isolated);Matrix4 wrapper=native.entity(rig).world;
    auto world=[&](const std::string& owner)->const Matrix4&{need(owner==rig,"Isolated sampler requested another rig wrapper.");return wrapper;};
    (void)sampler.sample(0,world);sampler.record_procedural_final(rig,0,wrapper);
    unsigned rays=0,frames=0,bindings=0;
    for(unsigned tick=1;tick<=30;++tick){step(native);wrapper=native.entity(rig).world;const auto literal=native.save_snapshot(content);(void)gt::artifacts().retain("actual-journal-input-"+std::to_string(tick),"source",literal);
        const auto document=J::parse(literal);const auto capture=physical_row(document).at("procedural_capture");const auto& journal=capture.at("grounding").at("journal");std::size_t next=0;const J* last=nullptr;
        auto take=[&](const char* kind,const std::string& owner)->const J&{need(next<journal.size(),"Isolated replay requested an unrecorded query.");last=&journal.at(next++);need(last->at("kind")==kind&&last->at("owner")==owner,"Isolated actual query order/owner differs.");return *last;};
        grounding::Query query=[&](const std::string& owner,const RuntimeRay& ray)->std::optional<RuntimeRayHit>{++rays;const auto& row=take("ray",owner);
            const J actual={{"origin",ray.origin},{"direction",ray.direction},{"distance",ray.distance},{"ignore",ray.ignore}};need(actual==row.at("ray"),"Isolated prepare ray differs from actual native capture.");
            const auto& hit=row.at("hit");if(hit.is_null())return {};RuntimeRayHit h;h.entity=hit.at("entity").get<std::string>();h.fraction=hit.at("fraction").get<double>();h.distance=hit.at("distance").get<double>();h.position=hit.at("position").get<V>();
            if(!hit.at("normal").is_null())h.normal=hit.at("normal").get<V>();if(!hit.at("triangle").is_null())h.triangle=hit.at("triangle").get<std::uint32_t>();return h;};
        grounding::SupportQuery frame=[&](const std::string& owner,const std::string& entity)->std::optional<Matrix4>{++frames;const auto& row=take("support",owner);need(row.at("entity")==entity,"Isolated support lookup entity differs.");if(row.at("frame").is_null())return {};return row.at("frame").get<Matrix4>();};
        RuntimeAnimations::ProceduralSupportQuery binding=[&](const std::string& owner,const std::string& entity)->std::optional<RuntimeAnimations::ProceduralSupportBinding>{++bindings;
            need(last&&last->at("owner")==owner,"Native identity lookup has no actual query context.");const auto& b=last->at("binding");if(b.is_null())return {};need(b.at("entity")==entity,"Captured binding owner differs.");
            need(b.at("motion")=="kinematic"||b.at("motion")=="static","Captured support kind is unsupported.");return RuntimeAnimations::ProceduralSupportBinding{entity,b.at("body_id").get<std::uint32_t>(),b.at("motion")=="kinematic"?BodyMotion::Kinematic:BodyMotion::Static,b.at("frame").get<Matrix4>()};};
        (void)sampler.sample(tick,world,query,frame,binding);need(next==journal.size(),"Isolated prepare failed to consume every literal native query.");sampler.record_procedural_final(rig,tick,wrapper);
        need(J::parse(*sampler.procedural_witness(rig))==capture,"Pure captured-journal replay differs from actual complete native witness.");
    }
    need(rays==60&&frames==60&&bindings==120,"Real two-foot journal population did not exercise the expected owner/provider barriers.");
    const auto before=sampler.save_state(30),witness=*sampler.procedural_witness(rig);const auto counts=std::array{rays,frames,bindings};
    auto candidate=wrapper;candidate=multiply_independent(rigid(position(native,actor),yaw(.07)),rigid({0,0,0},{0,0,0,1}));
    const auto model=legs();for(unsigned repeat=0;repeat<32;++repeat){const auto preview=sampler.preview_procedural(rig,30,candidate);need(preview.poses.size()==7&&preview.states.size()==2,"Preview omitted complete solved nodes/slots.");
        std::map<std::string,RuntimeTransform> locals;for(const auto& p:preview.poses)locals.emplace(p.entity,p.local);std::array<Matrix4,7> worlds;
        for(unsigned n=0;n<7;++n){worlds[n]=local_independent(locals.at(id(102+n)));worlds[n]=multiply_independent(n?worlds[unsigned(model->nodes[n].parent)]:candidate,worlds[n]);}
        for(unsigned slot=0;slot<2;++slot){const auto& m=worlds[3+3*slot];near(preview.states[slot].end_position,{m[12],m[13],m[14]},2e-7);}
        need(sampler.save_state(30)==before&&*sampler.procedural_witness(rig)==witness&&std::array{rays,frames,bindings}==counts,"Prepared preview changed history, generated/requested state or recast/resolved support.");
    }
    (void)contact(native,support,1,true);std::cout<<"Pure preview: actual captured native ray/support/binding calls="<<rays<<'/'<<frames<<'/'<<bindings<<",32 previews add zero provider calls\n";
}
GameplayConfig callback_config(const std::string& library){
    const auto path=std::filesystem::canonical(library);std::ifstream input(path,std::ios::binary);const std::string bytes((std::istreambuf_iterator<char>(input)),{});need(!bytes.empty(),"Grounded callback module is empty.");
    GameplayConfig c;c.native_aot=true;c.native_library=path.string();c.native_sha256=sha256(std::as_bytes(std::span(bytes.data(),bytes.size())));c.native_schema=gt::schema().dump();c.type="Poima.Tests.CppGroundedPhysicalCallback";c.native_requirements=parse_gameplay_service_contract(gt::requirements().dump());return c;
}
J values(const Runtime& r){return J::parse(r.gameplay_inspect()).at("values");}
void actual_compiled_callback(const std::string& library){
    need(Gameplay::native_available(),"Actual native gameplay transport is required for compiled C++ callback.");const auto d=definition();Runtime r(d);const auto support=add_support(r);settle(r,support);const auto config=callback_config(library);r.gameplay_load(config);
    r.gameplay_edit("{\"expected_yaw\":7,\"mode\":\"0\"}");const auto before_casts=r.ik(rig,1)->grounding->casts;RuntimeInput look;look.entity=actor;look.look={7,0};step(r,1,{}, {}, {},{look});
    need(values(r).at("calls")=="1"&&values(r).at("checks")=="32"&&values(r).at("endpoint_gap").get<double>()<2e-7,"Actual callback did not execute joined repeated FK/IK reads.");
    need(r.ik(rig,1)->grounding->casts==before_casts+1,"Callback read path performed extra committed ground decisions.");
    r.gameplay_edit("{\"expected_yaw\":9,\"mode\":\"1\"}");const auto before=r.save_snapshot(content);look.look={2,0};rejects([&]{step(r,1,{}, {}, {},{look});});need(r.save_snapshot(content)==before,"Compiled multiplier/activation conflict did not roll back full state.");
    r.gameplay_edit("{\"expected_yaw\":7,\"mode\":\"3\"}");step(r);need(r.grounding(rig,1)->weight==.75&&values(r).at("requested_weight")==1.0,"Compiled staged requested multiplier did not retain old getter then publish replacement.");
    const auto frozen=caches(r);r.gameplay_edit("{\"expected_yaw\":12,\"mode\":\"2\"}");look.look={5,0};step(r,1,{}, {}, {},{look});need(r.procedural_pose(rig)->suspended&&caches(r)==frozen,"Compiled activation transferred a fresh candidate/decision instead of the committed cache.");
    r.gameplay_edit("{\"mode\":\"0\"}");step(r);need(values(r).at("passive_reads")=="1","Compiled callback did not read historical ground/IK state.");
    const auto bytes=r.save_snapshot(content);(void)gt::artifacts().retain("actual-callback-passive","source",bytes);auto cold=Runtime::from_snapshot(d,content,bytes,config);same(r,*cold,"compiled-passive-cold");
    for(unsigned n=0;n<4;++n){step(r);step(*cold);same(r,*cold,"compiled-passive-future-"+std::to_string(n));need(caches(r)==frozen,"Compiled passive history changed during continuation.");}
}
}
int main(int argc,char** argv){
    if(argc>2){std::cerr<<"Usage: physical-grounded-test [CPP_ABI_FIXTURE_LIBRARY]\n";return 2;}unsigned failures=0;
    auto run=[&](const char* name,const std::function<void()>& f){gt::current_group=name;try{f();std::cout<<name<<" PASS\n";}catch(const std::exception& e){++failures;std::cerr<<name<<" FAIL: "<<e.what()<<'\n';}};
    run("admission/unprepared/actual ground and plant",admission_initial_and_actual_contacts);
    run("moving rigid support/corrected momentum/exact continuation",moving_support_corrected_momentum);
    run("historical support retirement after takeover",[]{retired_support(false);});
    run("support retirement inside activation batch",[]{retired_support(true);});
    run("requested multipliers/mixed slots/full rollback",requested_multipliers_mixed_slots_and_rollback);
    run("born nonidentity yaw/last spawned owner retirement",born_yaw_and_last_spawned_retirement);
    run("historical complete capture corruption",historical_capture_corruption);
    run("strict complete terminal codec corruption",strict_terminal_codec_corruption);
    run("synthetic rounding/full-state rollback",synthetic_rounding_rollback_preserves_original_locals);
    run("actual noncontact decision handoffs",noncontact_decision_handoffs);
    run("actual native journal replay/queryless preview",actual_journal_replay_and_queryless_preview);
    if(argc==2)run("actual compiled C++ callback",[&]{actual_compiled_callback(argv[1]);});
    std::cout<<"Grounded physical ownership "<<(failures?"FAIL":"PASS")<<" "<<checks<<" checks; "<<(argc==2?"actual C++ callback requested":"no callback execution")<<"; no C#/AOT/render/performance qualification\n";return failures?1:0;
}
