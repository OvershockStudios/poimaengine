// SPDX-License-Identifier: Apache-2.0
// New bounded-scale Runtime workflows; no edits to the fixed-scale suites.
// The small planar FK oracle is intentionally independent of production FK.
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace poima;using Json=nlohmann::json;namespace pr=physical_rig;
using V=std::array<double,3>;using Q=std::array<double,4>;
constexpr double maximum=1./128.;const std::string content(64,'b');
constexpr V root_scale{1.25,.75,1},helper_scale{1.125,.875,1},tip_scale{.875,1.0625,1},leaf_scale{1.125,.875,1};
constexpr double root_angle=.2,helper_angle=-.4,tip_angle=.3;
void check(bool b,const char* s){if(!b)throw std::runtime_error(s);}
const char* diagnostic_group="startup";
void near(double a,double b,double e=1e-8){
    if(!(std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=e)){
        std::ostringstream message;message.precision(17);
        message<<"Independent bounded Runtime oracle differs: group="<<diagnostic_group
            <<", actual="<<a<<", expected="<<b<<", tolerance="<<e<<'.';
        throw std::runtime_error(message.str());
    }
}
template<class F>void rejects(F&& f){bool bad=false;try{f();}catch(const std::exception&){bad=true;}check(bad,"Invalid bounded Runtime operation succeeded.");}
std::string id(unsigned n){const auto s=std::to_string(n);return std::string(32-s.size(),'0')+s;}
Q zq(double x){return {0,0,std::sin(x/2),std::cos(x/2)};}
V rotate(double a,V p){return {std::cos(a)*p[0]-std::sin(a)*p[1],std::sin(a)*p[0]+std::cos(a)*p[1],p[2]};}
Q product(Q a,Q b){Q q{a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};const double n=std::hypot(std::hypot(q[0],q[1]),std::hypot(q[2],q[3]));for(auto& x:q)x/=n;return q;}
V rotate(Q q,V p){const V t{2*(q[1]*p[2]-q[2]*p[1]),2*(q[2]*p[0]-q[0]*p[2]),2*(q[0]*p[1]-q[1]*p[0])};return {p[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1],p[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2],p[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0]};}
struct Affine {double a=1,b=0,c=0,d=1,x=0,y=0,z=0,zscale=1;};
Affine trs(V p,double a,V s){return {std::cos(a)*s[0],-std::sin(a)*s[1],std::sin(a)*s[0],std::cos(a)*s[1],p[0],p[1],p[2],s[2]};}
Affine chain(Affine p,Affine q){return {p.a*q.a+p.b*q.c,p.a*q.b+p.b*q.d,p.c*q.a+p.d*q.c,p.c*q.b+p.d*q.d,p.x+p.a*q.x+p.b*q.y,p.y+p.c*q.x+p.d*q.y,p.z+p.zscale*q.z,p.zscale*q.zscale};}
RuntimeTransform placed(double x=0,double a=.25){return {{x,6,0},zq(a),{1.4,1.4,1.4}};}
AnimationChannel scale_channel(unsigned node,V a,V b){
    // ModelAsset keys are float32, as in imported animation. Make the existing
    // source encoding explicit; these casts retain the same key bits formerly
    // produced by narrowing in the aggregate initializer.
    return {node,AnimationPath::scale,AnimationInterpolation::linear,{0,2},
        {{static_cast<float>(a[0]),static_cast<float>(a[1]),static_cast<float>(a[2]),0},
         {static_cast<float>(b[0]),static_cast<float>(b[1]),static_cast<float>(b[2]),0}}};
}
std::shared_ptr<ModelAsset> model(){
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(4);m->roots={0};
    m->nodes[0].name="root";m->nodes[0].position={.5,.75,0};m->nodes[0].rotation=zq(root_angle); // model rest differs from initialized scale
    m->nodes[1].name="unmapped helper";m->nodes[1].parent=0;m->nodes[1].position={.25,.5,0};m->nodes[1].rotation=zq(helper_angle);m->nodes[1].scale=helper_scale;
    m->nodes[2].name="tip";m->nodes[2].parent=1;m->nodes[2].position={0,.4,0};m->nodes[2].rotation=zq(tip_angle);m->nodes[2].scale=tip_scale;
    m->nodes[3].name="unmapped leaf";m->nodes[3].parent=2;m->nodes[3].position={.1,.15,.05};m->nodes[3].rotation=zq(-.17);m->nodes[3].scale=leaf_scale;
    const AnimationChannel translation{0,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{.5,.75,0,0},{2.5,.75,0,0}}};
    auto ramp=scale_channel(0,root_scale,{1.25*1.002,.75,1});
    auto helper=scale_channel(1,helper_scale,{1.125,.875*.999,1});auto leaf=scale_channel(3,leaf_scale,{1.125*1.0015,.875,1});
    m->animations.push_back({"micro-scale base",2,{translation,ramp,helper,leaf}}); // 0
    const auto small=scale_channel(0,{1.25*1.004,.75,1},{1.25*1.004,.75,1});
    m->animations.push_back({"in-envelope target",2,{translation,small}}); // 1
    auto late=scale_channel(0,root_scale,root_scale);late.times={0,.05,.1,2};late.values={{1.25,.75,1,0},{1.25,.75,1,0},{1.375,.75,1,0},{1.375,.75,1,0}};
    m->animations.push_back({"later out-of-envelope",2,{translation,late}}); // 2
    m->animations.push_back({"additive ratio target",2,{small}}); // 3
    auto fast=ramp;fast.times={0,.1,2};fast.values={{1.25,.75,1,0},{1.25*1.006,.75,1,0},{1.25*1.006,.75,1,0}};
    m->animations.push_back({"bounded keys with positive log-scale velocity",2,{translation,fast}}); // 4
    return m;
}
pr::Configuration profile(){
    pr::Configuration c;c.pose_frame=pr::PoseFramePolicy::SkeletalBoundedScale;c.max_scale_deviation=maximum;c.profile.root="root";
    pr::Part a;a.name="root";a.node=0;a.shape=pr::Sphere{.12};a.mass=3;a.body_in_bone.position={.08,0,0};a.linear_damping=0;a.angular_damping=.02;auto b=a;b.name="tip";b.node=2;b.mass=1;b.shape=pr::Sphere{.1};c.profile.parts={a,b};
    const auto root=chain(trs({},0,{1.4,1.4,1.4}),trs({.5,.75,0},root_angle,root_scale));const auto tip=chain(chain(root,trs({.25,.5,0},helper_angle,helper_scale)),trs({0,.4,0},tip_angle,tip_scale));
    const auto ro=rotate(root_angle,{.08,0,0}),to=rotate(root_angle+helper_angle+tip_angle,{.08,0,0});V ca{root.x+ro[0],root.y+ro[1],0},cb{tip.x+to[0],tip.y+to[1],0},anchor{},pa{},pb{};
    for(unsigned k=0;k<3;++k){anchor[k]=(ca[k]+cb[k])*.5;pa[k]=anchor[k]-ca[k];pb[k]=anchor[k]-cb[k];}
    pr::Joint j;j.name="spine";j.parent="root";j.child="tip";j.parent_frame.position=rotate(-root_angle,pa);j.child_frame.position=rotate(-(root_angle+helper_angle+tip_angle),pb);j.child_frame.rotation=zq(-helper_angle-tip_angle);j.limits=pr::SwingTwist{1,1,-.8,.8};j.rest_stiffness=.5;j.rest_damping=.1;j.soft_limit_stiffness=.5;c.profile.joints={j};return c;
}
std::vector<RuntimeEntityDefinition> actor(std::shared_ptr<const ModelAsset> m,unsigned first,double phase=0){
    RuntimeEntityDefinition rig;rig.id=id(first);rig.transform=placed();rig.animation_rig=RuntimeAnimationRig{m,0,phase,1,false,true,{}};rig.physical_rig=profile();std::vector<RuntimeEntityDefinition> out{rig};
    for(unsigned n=0;n<4;++n){const auto& node=m->nodes[n];RuntimeEntityDefinition e;e.id=id(first+1+n);e.parent=node.parent<0?rig.id:id(first+1+unsigned(node.parent));e.transform={node.position,node.rotation,node.scale};e.rig_node=RuntimeRigNode{rig.id,n};out.push_back(e);}return out;
}
RuntimeDefinition definition(bool additive=false){
    RuntimeDefinition d;d.world_id="bounded-physical-runtime";d.authored_revision=9;RuntimeEntityDefinition floor;floor.id=id(2);floor.transform.position={0,-.5,0};floor.collider=BoxCollider{};floor.collider->half_extents={30,.5,30};RuntimeEntityDefinition camera;camera.id=id(3);camera.camera=RuntimeCamera{};camera.transform.position={0,5,12};RuntimeEntityDefinition ordinary;ordinary.id=id(4);ordinary.transform.position={-4,3,0};ordinary.collider=BoxCollider{};ordinary.collider->motion=BodyMotion::Dynamic;ordinary.collider->mass=2;d.entities={floor,camera,ordinary};auto m=model();auto a=actor(m,10);
    if(additive){RuntimeAnimationLayer layer;layer.slot=1;layer.mode=AnimationLayerMode::Additive;layer.clip=3;layer.reference_clip=0;layer.weight=0;layer.playing=false;layer.loop=false;layer.mask={{0,1}};a[0].animation_rig->layers={layer};}
    d.entities.insert(d.entities.end(),a.begin(),a.end());RuntimeSpawnTemplate t;t.id=id(900);t.name="bounded actor";t.root=id(1000);t.entities=actor(m,1000,.25);d.templates={t};return d;
}
AnimationCommand command(unsigned clip,double phase=0,bool playing=true){AnimationCommand c;c.entity=id(10);c.clip=clip;c.time=phase;c.playing=playing;c.loop=false;return c;}
void animate(Runtime& r,unsigned ticks,const std::vector<AnimationCommand>& c={}){r.step(ticks,{}, {}, {},c);}
void takeover(Runtime& r,const std::string& rig){r.step(1,{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},{{rig,true,{}}});}
void whole_equal(const Runtime& a,const Runtime& b){
    check(a.save_snapshot(content)==b.save_snapshot(content),"Complete bounded Runtime save differs; no exclusions.");check(a.live_definition().entities.size()==b.live_definition().entities.size(),"Cold membership differs.");
    for(const auto& e:a.live_definition().entities){const auto x=a.entity(e.id),y=b.entity(e.id);check(x.world==y.world&&x.local.position==y.local.position&&x.local.rotation==y.local.rotation&&x.local.scale==y.local.scale,"Whole published local/world state differs.");}
}
void expected_micro(const Runtime& r,const std::vector<std::string>& nodes,RuntimeTransform wrapper,double wrapper_angle,double time){
    // Independent analytic interpolation of declared float32 source endpoints,
    // promoted to double. Ideal F64 percentage formulas describe different keys.
    time=std::clamp(time,0.,2.);const double u=time/2;
    const V rs{1.25+(double(1.2525f)-1.25)*u,.75,1},
        hs{1.125,.875+(double(.874125f)-.875)*u,1},
        ls{1.125+(double(1.1266875f)-1.125)*u,.875,1};
    const auto parent=trs(wrapper.position,wrapper_angle,wrapper.scale);const auto root=chain(parent,trs({.5+time,.75,0},root_angle,rs));const auto tip=chain(chain(root,trs({.25,.5,0},helper_angle,hs)),trs({0,.4,0},tip_angle,tip_scale));
    for(const auto& [id,pose]:std::vector<std::pair<std::string,Affine>>{{nodes[0],root},{nodes[2],tip}}){const auto actual=r.entity(id);near(actual.world[12],pose.x);near(actual.world[13],pose.y);near(actual.world[14],pose.z);}
    for(unsigned k=0;k<3;++k){near(r.entity(nodes[0]).local.scale[k],rs[k],1e-12);near(r.entity(nodes[1]).local.scale[k],hs[k],1e-12);near(r.entity(nodes[3]).local.scale[k],ls[k],1e-12);}
}
void publication(const Runtime& r,const std::string& rig,const std::vector<std::string>& nodes){
    const auto state=r.physical_rig(rig,true,true);check(state&&state->parts.size()==2&&state->joints.size()==1,"Actual two-part native graph differs.");Q q=r.entity(rig).local.rotation;Q logical[4];for(unsigned n=0;n<4;++n){q=product(q,r.entity(nodes[n]).local.rotation);logical[n]=q;}
    for(unsigned n=0;n<2;++n){const unsigned node=n?2:0;const auto e=r.entity(nodes[node]);const auto p=rotate(logical[node],{.08,0,0});for(unsigned k=0;k<3;++k)near(state->parts[n].pose.position[k],e.world[12+k]+p[k],2e-6);}
    near(state->parts[0].mass,3,2e-6);near(state->parts[1].mass,1,2e-6);
    for(unsigned part=0;part<2;++part){const double radius=part?.1:.12,mass=part?1.:3.;for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col)near(state->parts[part].inertia[row*3+col],row==col?.4*mass*radius*radius:0.,1e-8);}
}
const std::vector<std::string> main_nodes{id(11),id(12),id(13),id(14)};
void crossfade_passive_and_cold_future(){
    const auto d=definition();Runtime warm(d);warm.step(12,{});expected_micro(warm,main_nodes,placed(),.25,.2);auto c=command(1,0,false);c.blend_ticks=24;animate(warm,6,{c});
    const double target=double(1.255f),outgoing=1.25+(double(1.2525f)-1.25)*.3/2;near(warm.entity(id(11)).local.scale[0],std::lerp(outgoing,target,.25),1e-12);
    const auto blended=warm.save_snapshot(content);auto cold_blend=Runtime::from_snapshot(d,content,blended);whole_equal(warm,*cold_blend);warm.step(5,{});cold_blend->step(5,{});whole_equal(warm,*cold_blend);
    animate(warm,1,{command(0,.4,true)});animate(*cold_blend,1,{command(0,.4,true)});takeover(warm,id(10));takeover(*cold_blend,id(10));whole_equal(warm,*cold_blend);publication(warm,id(10),main_nodes);
    const auto before=warm.entity(id(11)).local.scale;const auto passive=warm.save_snapshot(content);auto cold=Runtime::from_snapshot(d,content,passive);whole_equal(warm,*cold);
    for(unsigned n=0;n<24;++n){warm.step(1,{});cold->step(1,{});whole_equal(warm,*cold);publication(warm,id(10),main_nodes);}
    check(warm.entity(id(11)).local.scale!=before,"Passive animation scales were silently frozen.");
    const auto snapshot=warm.save_snapshot(content);check(snapshot==warm.save_snapshot(content),"Repeated save changed sampled scales or passive locals.");
}
void template_birth_retirement_and_immutable_baseline(){
    const auto d=definition();Runtime warm(d);warm.step(9,{});const auto placement=placed(4,-.3);const auto born=warm.change_structure(0,{{id(900),placement}},{}).spawned.at(0);std::vector<std::string> nodes;for(unsigned n=1001;n<=1004;++n)nodes.push_back(warm.instance_node(born,id(n)));expected_micro(warm,nodes,placement,-.3,.25);
    auto cold=Runtime::from_snapshot(d,content,warm.save_snapshot(content));whole_equal(warm,*cold);warm.step(7,{});cold->step(7,{});whole_equal(warm,*cold);expected_micro(warm,nodes,placement,-.3,.25+7./60);
    takeover(warm,born);takeover(*cold,born);whole_equal(warm,*cold);for(unsigned n=0;n<12;++n){warm.step(1,{});cold->step(1,{});whole_equal(warm,*cold);publication(warm,born,nodes);}
    auto passive_cold=Runtime::from_snapshot(d,content,warm.save_snapshot(content));whole_equal(warm,*passive_cold);warm.change_structure(warm.structure_revision(),{}, {born});passive_cold->change_structure(passive_cold->structure_revision(),{}, {born});whole_equal(warm,*passive_cold);
    auto retired=Runtime::from_snapshot(d,content,warm.save_snapshot(content));const auto a=warm.change_structure(warm.structure_revision(),{{id(900),placed(-4,.6)}},{}).spawned.at(0);const auto b=retired->change_structure(retired->structure_revision(),{{id(900),placed(-4,.6)}},{}).spawned.at(0);check(a==b&&a!=born,"Cold birth lost retired entity/body lineage.");whole_equal(warm,*retired);
}
void reject_late_composition_and_preserve_whole_batch(){
    {
        const auto d=definition();Runtime r(d);r.step(12,{});const auto prior=r.save_snapshot(content);auto witness=Runtime::from_snapshot(d,content,prior);auto prefix=Runtime::from_snapshot(d,content,prior);animate(*prefix,3,{command(2)});near(prefix->entity(id(11)).local.scale[0],1.25,1e-12);rejects([&]{animate(r,6,{command(2)});});check(r.save_snapshot(content)==prior,"Later source scale failure changed the whole prior state.");whole_equal(r,*witness);
    }
    {
        auto d=definition();d.entities.at(3).animation_rig->clip=4;Runtime r(d);r.step(2,{});near(r.entity(id(11)).local.scale[0],1.25+(double(1.2575f)-1.25)*(2./60)/double(.1f),1e-12);const auto prior=r.save_snapshot(content);auto witness=Runtime::from_snapshot(d,content,prior);auto c=command(1,0,false);c.blend_ticks=60;c.transition_mode=AnimationTransitionMode::Inertial;
        auto first=Runtime::from_snapshot(d,content,prior);animate(*first,1,{c});check(std::abs(first->entity(id(11)).local.scale[0]-1.25)<=maximum*1.25,"Inertial counterexample did not have a legal first tick.");rejects([&]{animate(r,20,{c});});check(r.save_snapshot(content)==prior,"Inertial overshoot failed to roll back prior legal ticks.");whole_equal(r,*witness);
    }
    {
        const auto d=definition(true);Runtime r(d);r.step(3,{});const auto prior=r.save_snapshot(content);auto witness=Runtime::from_snapshot(d,content,prior);auto base=command(1,0,false),layer=command(3,0,false);layer.layer=1;layer.weight=1;layer.weight_blend_ticks=30;
        check((1.004*1.004-1)>maximum,"Additive counterexample must exceed the envelope while each operand is inside.");auto first=Runtime::from_snapshot(d,content,prior);animate(*first,1,{base,layer});rejects([&]{animate(r,40,{base,layer});});check(r.save_snapshot(content)==prior,"Additive later output did not restore entire prior state.");whole_equal(r,*witness);
    }
}
void forged_unmapped_scale_and_passive_future_failure(){
    const auto d=definition();Runtime warm(d);warm.step(8,{});
    const auto reject_forged=[&]{const auto before=warm.save_snapshot(content);auto value=Json::parse(before);bool found=false;for(auto& e:value["payload"]["entities"])if(e["id"]==id(14)){const double v=e["local"]["scale"][2].get<double>();e["local"]["scale"][2]=std::nextafter(v,std::numeric_limits<double>::infinity());found=true;}check(found,"Unmapped saved leaf is missing.");rejects([&]{(void)Runtime::from_snapshot(d,content,value.dump());});check(warm.save_snapshot(content)==before,"Detached invalid saved scale changed the live Runtime.");};
    reject_forged();takeover(warm,id(10));reject_forged();
    // The old ownership restriction remains: passive animation/rate edits are
    // rejected, rather than being used as a back door to freeze or repair scale.
    const auto passive=warm.save_snapshot(content);rejects([&]{animate(warm,1,{command(1)});});check(warm.save_snapshot(content)==passive,"Passive animation ownership conflict changed state.");
    auto delayed=definition();delayed.entities.at(3).animation_rig->clip=2;Runtime later(delayed);takeover(later,id(10));const auto prior=later.save_snapshot(content);auto witness=Runtime::from_snapshot(delayed,content,prior);auto prefix=Runtime::from_snapshot(delayed,content,prior);prefix->step(2,{});near(prefix->entity(id(11)).local.scale[0],1.25,1e-12);rejects([&]{later.step(6,{});});check(later.save_snapshot(content)==prior,"Existing passive clock failure froze or partially advanced state.");whole_equal(later,*witness);
}
}
int main(){try{diagnostic_group="crossfade/passive/cold";crossfade_passive_and_cold_future();diagnostic_group="template birth/retirement";template_birth_retirement_and_immutable_baseline();diagnostic_group="late composition rollback";reject_late_composition_and_preserve_whole_batch();diagnostic_group="saved scale/passive clock";forged_unmapped_scale_and_passive_future_failure();std::cout<<"Bounded physical Runtime: 4 groups passed.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
