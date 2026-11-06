// SPDX-License-Identifier: Apache-2.0
// Ordinary native Runtime/Jolt integration, with an independent analytic
// animation oracle. No importer, renderer, gameplay stub or test-only controls.
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace poima;
namespace pr=physical_rig;
using J=nlohmann::json;using Q=pr::Quat;using V=pr::Vec3;
const std::string content(64,'a');
unsigned checks=0,pairs=0;
const char* group="startup";
std::string id(unsigned n){char text[33];std::snprintf(text,sizeof(text),"%032x",n);return text;}
const auto rig=id(10),camera=id(3);
void need(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F&& f,const char* why){bool caught=false;try{f();}catch(const std::exception&){caught=true;}need(caught,why);}
void near(double a,double b,double bound=2e-7){need(std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=bound,"Independent physical animation scalar oracle differs.");}
Q axis(unsigned n,double angle){Q q{0,0,0,std::cos(angle/2)};q[n]=std::sin(angle/2);return q;}
Q normalized(Q q){double n=0;for(double x:q)n+=x*x;n=std::sqrt(n);for(double& x:q)x/=n;return q;}
Q inverse(Q q){for(unsigned k=0;k<3;++k)q[k]=-q[k];return q;}
Q product(Q a,Q b){return normalized({a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]});}
V rotate(Q q,V v){const V u{q[0],q[1],q[2]};const auto cross=[](V a,V b){return V{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};};const auto uv=cross(u,v),uuv=cross(u,uv);for(unsigned k=0;k<3;++k)v[k]+=2*(q[3]*uv[k]+uuv[k]);return v;}
double difference(Q a,Q b){a=normalized(a);b=normalized(b);double d=0;for(unsigned k=0;k<4;++k)d+=a[k]*b[k];return 2*std::acos(std::clamp(std::abs(d),0.,1.));}
void rotation_near(Q a,Q b,double bound=2e-6){need(difference(a,b)<=bound,"Independent calibrated joint target differs.");}
Q slerp(Q a,Q b,double t){a=normalized(a);b=normalized(b);double dot=0;for(unsigned k=0;k<4;++k)dot+=a[k]*b[k];if(dot<0){for(double& x:b)x=-x;dot=-dot;}Q q{};if(dot>.9995){for(unsigned k=0;k<4;++k)q[k]=a[k]+t*(b[k]-a[k]);return normalized(q);}const double angle=std::acos(std::clamp(dot,-1.,1.)),den=std::sin(angle);for(unsigned k=0;k<4;++k)q[k]=(std::sin((1-t)*angle)*a[k]+std::sin(t*angle)*b[k])/den;return normalized(q);}
Q from_key(const std::array<float,4>& q){return {q[0],q[1],q[2],q[3]};}
std::array<float,4> key(Q q){return {float(q[0]),float(q[1]),float(q[2]),float(q[3])};}

// Complete original archives retained before assertions, including rejected
// resealed inputs. Neither numeric types nor state fields are projected away.
class Archives {
    std::filesystem::path output;std::uint64_t bytes=0;
    J manifest={{"format","poima.physical-animation-native-archives.v1"},{"entries",J::array()},{"pairs",J::array()}};
    static constexpr std::uint64_t cap=64u*1024u*1024u;
    void commit(){const auto text=manifest.dump(2);need(text.size()<=cap-bytes,"Physical animation manifest exceeds cap.");std::ofstream f(output/"manifest.json",std::ios::binary);f<<text;f.close();need(bool(f),"Cannot retain physical animation manifest.");}
public:
    Archives(){const auto* root=std::getenv("POIMA_TEST_ARTIFACTS");if(!root||!*root)return;output=std::filesystem::path(root)/"physical-animation";std::filesystem::create_directories(output);need(!std::filesystem::exists(output/"manifest.json"),"Physical animation artifacts require a fresh directory.");}
    J retain(const std::string& label,const char* role,const std::string& data){if(output.empty())return nullptr;need(data.size()<=cap&&bytes<=cap-data.size()&&manifest["entries"].size()<512,"Physical animation artifacts exceed 64MiB/512-file cap.");char name[64];std::snprintf(name,sizeof(name),"%04zu-%s.json",manifest["entries"].size()+1,role);need(!std::filesystem::exists(output/name),"Physical animation artifact already exists.");std::ofstream f(output/name,std::ios::binary);f.write(data.data(),static_cast<std::streamsize>(data.size()));f.close();need(bool(f),"Cannot retain physical animation archive.");J row={{"label",label},{"group",group},{"role",role},{"path",name},{"bytes",data.size()},{"sha256",sha256(std::as_bytes(std::span(data.data(),data.size())))}};bytes+=data.size();manifest["entries"].push_back(row);manifest["raw_bytes"]=bytes;commit();return row;}
    void pair(const std::string& label,const std::string& a,const std::string& b){if(output.empty())return;const auto x=retain(label,"source",a),y=retain(label,"restored",b);manifest["pairs"].push_back({{"label",label},{"group",group},{"left",x.at("path")},{"right",y.at("path")},{"complete_byte_equal",a==b},{"excluded_fields",J::array()}});commit();}
};
Archives& records(){static Archives value;return value;}
bool typed(const J& a,const J& b){if(a.type()!=b.type())return false;if(a.is_object()){if(a.size()!=b.size())return false;for(auto p=a.begin();p!=a.end();++p){const auto q=b.find(p.key());if(q==b.end()||!typed(p.value(),*q))return false;}return true;}if(a.is_array()){if(a.size()!=b.size())return false;for(std::size_t n=0;n<a.size();++n)if(!typed(a[n],b[n]))return false;return true;}return a==b;}
std::string saved(const Runtime& r){return r.save_snapshot(content);}
void equal(const std::string& label,const std::string& a,const std::string& b){records().pair(label,a,b);++pairs;need(a==b,"Complete physical animation archive bytes differ.");need(typed(J::parse(a),J::parse(b)),"Complete physical animation typed state differs.");}
std::string reseal(J value){const auto payload=value.at("payload").dump();value["sha256"]=sha256(std::as_bytes(std::span(payload.data(),payload.size())));return value.dump();}

// Distinct body and joint calibration, plus a nontrivial world ancestor. The
// animation keys are authored to produce a known relative joint target, rather
// than borrowing the production desired_joint_rotation or fitting helper.
Q frame(unsigned joint){return joint==0?product(axis(1,.35),axis(2,.2)):Q{0,0,0,1};}
Q body(unsigned part){return product(axis(2,.11*(part+1)),axis(0,-.08*(part+1)));}
Q local_target(unsigned joint,Q target){const auto f=frame(joint);return product(product(f,target),inverse(f));}
pr::Configuration configuration(bool stream=true,pr::TargetLimitPolicy policy=pr::TargetLimitPolicy::Strict){
    pr::Configuration c;c.profile.root="pelvis";if(stream)c.animation_drive=pr::AnimationDrive{policy};
    for(unsigned n=0;n<3;++n){pr::Part p;p.name=std::array{"pelvis","arm","hand"}[n];p.node=n;p.mass=std::array{4.,2.,1.}[n];p.shape=pr::Sphere{std::array{.12,.1,.08}[n]};p.body_in_bone.rotation=body(n);p.linear_damping=p.angular_damping=p.friction=p.restitution=0;c.profile.parts.push_back(p);}
    for(unsigned n=0;n<2;++n){pr::Joint j;j.name=n?"elbow":"shoulder";j.parent=c.profile.parts[n].name;j.child=c.profile.parts[n+1].name;j.parent_frame.position=rotate(inverse(body(n)),{0,.25,0});j.child_frame.position=rotate(inverse(body(n+1)),{0,-.25,0});j.parent_frame.rotation=product(inverse(body(n)),frame(n));j.child_frame.rotation=product(inverse(body(n+1)),frame(n));if(n)j.limits=pr::Hinge{-.8,1.2};else j.limits=pr::SwingTwist{1.2,1.2,-.8,.8};j.drive=pr::DriveProfile{60,6,10};c.profile.joints.push_back(j);}return c;
}
std::shared_ptr<ModelAsset> model(){
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(3);m->roots={0};
    for(unsigned n=0;n<3;++n){m->nodes[n].name=std::array{"pelvis","arm","hand"}[n];if(n){m->nodes[n].parent=n-1;m->nodes[n].position={0,.5,0};}}
    m->nodes[0].skin=0;m->nodes[0].primitives={0};auto mesh=std::make_shared<MeshAsset>();mesh->vertices={{{0,0,0},{0,0,1},{0,0},{1,0,0,1}},{{.15f,.5f,0},{0,0,1},{1,0},{1,0,0,1}},{{0,1,0},{0,0,1},{0,1},{1,0,0,1}}};mesh->indices={0,1,2};mesh->has_uv=true;mesh->influences={SkinWeight{{0,0,0,0},{1,0,0,0}},SkinWeight{{1,0,0,0},{1,0,0,0}},SkinWeight{{2,0,0,0},{1,0,0,0}}};m->primitives={mesh};auto arm=identity_matrix(),hand=identity_matrix();arm[13]=-.5;hand[13]=-1;m->skins.push_back({"calibrated chain",0,{0,1,2},{identity_matrix(),arm,hand}});
    const auto clip=[&](const char* name,float duration,double a,double b,double x,double y){m->animations.push_back({name,duration,{
        {1,AnimationPath::rotation,AnimationInterpolation::linear,{0,duration},{key(local_target(0,axis(2,a))),key(local_target(0,axis(2,b)))}},
        {2,AnimationPath::rotation,AnimationInterpolation::linear,{0,duration},{key(axis(0,x)),key(axis(0,y))}}
    }});};
    clip("source",2,.1,.5,.05,.35);clip("reverse",2,-.35,.25,.2,.5);clip("outside over time",1,0,1.5,0,.3);clip("elbow layer",2,0,0,.6,.6);clip("outside held",2,1.5,1.5,1.4,1.4);return m;
}
std::vector<RuntimeEntityDefinition> entities(const std::shared_ptr<const ModelAsset>& m,unsigned first,bool stream,pr::TargetLimitPolicy policy){
    RuntimeEntityDefinition wrapper;wrapper.id=id(first);wrapper.transform.position={.2,200,.4};wrapper.transform.rotation=axis(2,-.12);wrapper.animation_rig=RuntimeAnimationRig{m,0,0,1,false,true,{}};RuntimeAnimationLayer layer;layer.slot=1;layer.clip=3;layer.weight=0;layer.playing=true;layer.loop=false;layer.mask={{2,1}};wrapper.animation_rig->layers={layer};wrapper.physical_rig=configuration(stream,policy);std::vector<RuntimeEntityDefinition> result{wrapper};
    for(unsigned n=0;n<3;++n){RuntimeEntityDefinition bone;bone.id=id(first+1+n);bone.parent=n?id(first+n):wrapper.id;bone.transform.position=m->nodes[n].position;bone.rig_node=RuntimeRigNode{wrapper.id,n};result.push_back(bone);}RuntimeEntityDefinition skin;skin.id=id(first+4);skin.parent=id(first+1);skin.mesh=RuntimeMesh{};skin.mesh->mesh=m->primitives[0];skin.skinned_mesh=RuntimeSkinnedMesh{wrapper.id,0};result.push_back(skin);return result;
}
RuntimeDefinition definition(bool stream=true,pr::TargetLimitPolicy policy=pr::TargetLimitPolicy::Strict,bool dormant=false){
    RuntimeDefinition d;d.world_id="native-physical-animation";d.authored_revision=1;RuntimeEntityDefinition floor;floor.id=id(2);floor.transform.position={0,-.5,0};floor.transform.scale={40,1,40};floor.collider=BoxCollider{};RuntimeEntityDefinition eye;eye.id=camera;eye.transform.position={0,203,8};eye.camera=RuntimeCamera{};RuntimeEntityDefinition ancestor;ancestor.id=id(9);ancestor.transform.position={4,3,-2};ancestor.transform.rotation=axis(1,.7);d.entities={floor,eye,ancestor};const auto m=model();if(!dormant){auto list=entities(m,10,stream,policy);list[0].parent=ancestor.id;d.entities.insert(d.entities.end(),list.begin(),list.end());}RuntimeSpawnTemplate recipe;recipe.id=id(900);recipe.root=id(1000);recipe.name="calibrated animated target chain";recipe.entities=entities(m,1000,stream,policy);d.templates={recipe};return d;
}
void step(Runtime& r,const std::vector<PhysicalRigCommand>& commands={},std::uint32_t ticks=1,const std::vector<RuntimeStructureTick>& structure={},const std::vector<AnimationCommand>& visible={}){r.step(ticks,{}, {},{},visible,structure,{}, {},{}, {},{}, {},{},commands);}
PhysicalRigCommand activate(const std::string& selected=rig){PhysicalRigCommand c;c.entity=selected;c.activate=true;return c;}
PhysicalRigCommand source(const char* joint,PhysicalRigDriveSource from,double strength,const std::string& selected=rig){PhysicalRigCommand c;c.entity=selected;c.drive_source=PhysicalRigDriveSourceCommand{joint,from,strength};return c;}
PhysicalRigCommand manual(const char* joint,Q target,double strength){PhysicalRigCommand c;c.entity=rig;c.drive=PhysicalRigDriveCommand{joint,target,strength};return c;}
PhysicalRigCommand playback(unsigned clip,double time=0,bool playing=true,std::optional<unsigned> layer={},double weight=1,unsigned blend=0){PhysicalRigCommand c;c.entity=rig;c.animation=AnimationCommand{rig,clip,time,1,false,playing,blend};c.animation->layer=layer;c.animation->weight=weight;return c;}
PhysicalRigCommand rate(double speed,std::optional<unsigned> layer={}){PhysicalRigCommand c;c.entity=rig;c.animation_rate=AnimationRateCommand{rig,speed,layer};return c;}
PhysicalRigAnimationObservation stream(const Runtime& r,const std::string& selected=rig){const auto o=r.physical_animation(selected);need(o.has_value(),"Configured independent target player absent.");need(o->entity==selected&&o->joints.size()==2,"Independent target player membership changed.");need(o->sampled_tick==r.inspect().tick,"Target player sampled_tick is not current committed tick.");return *o;}
const PhysicalRigAnimationJointObservation& joint(const PhysicalRigAnimationObservation& o,const std::string& name){const auto it=std::find_if(o.joints.begin(),o.joints.end(),[&](const auto& j){return j.name==name;});need(it!=o.joints.end(),"Target source observation missing.");return *it;}
PhysicalRigDriveObservation drive(const Runtime& r,const char* name,const std::string& selected=rig){const auto value=r.physical_drive(selected,name);need(value.has_value(),"Configured drive is missing.");return *value;}
Q independent_target(const ModelAsset& m,unsigned clip,double time,unsigned index){const auto& a=m.animations.at(clip);const auto& channel=a.channels.at(index);const double t=std::clamp(time/double(channel.times.back()),0.,1.);const auto local=slerp(from_key(channel.values.front()),from_key(channel.values.back()),t);const auto f=frame(index);return product(product(inverse(f),local),f);}
void targets_match(const Runtime& r,const ModelAsset& m){const auto a=stream(r);need(a.playback.clip.has_value()&&!a.playback.transition,"Analytic target oracle needs a plain base clip.");for(unsigned n=0;n<2;++n){const char* name=n?"elbow":"shoulder";const auto& j=joint(a,name);if(j.source==PhysicalRigDriveSource::Manual){need(!j.sample,"Manual source retained animation attribution.");continue;}need(j.sample.has_value(),"Automatic joint omitted its current sample.");const auto expected=independent_target(m,*a.playback.clip,a.playback.time,n);rotation_near(j.sample->requested,expected);rotation_near(j.sample->target,expected);rotation_near(drive(r,name).target_rotation,expected);need(j.sample->correction_radians==0&&!j.sample->hinge_projected&&!j.sample->twist_clamped&&!j.sample->swing_clamped,"Legal sampled target was fitted.");}}
std::vector<Matrix4> palette(const Runtime& r){for(const auto& item:r.snapshot(camera).objects)if(item.entity_id==id(14)){need(item.skin&&item.skin->palette.size()==3,"Independent skinned presentation missing.");return item.skin->palette;}throw std::runtime_error("Skinned fixture drawable missing.");}
void continuation(const RuntimeDefinition& d,Runtime& r,const std::string& label,unsigned count=4){const auto before=saved(r);records().retain(label,"accepted-source",before);Runtime::validate_snapshot(d,content,before);equal(label+" validation purity",before,saved(r));auto cold=Runtime::from_snapshot(d,content,before);equal(label+" immediate cold restore",before,saved(*cold));for(unsigned n=0;n<count;++n){step(r);step(*cold);equal(label+" continuation "+std::to_string(n),saved(r),saved(*cold));need(palette(r)==palette(*cold),"Cold continuation changed complete skin palette.");}}

void clocks_targets_and_native_response(){
    group="clocks-targets-and-native-response";const auto d=definition();const auto m=d.entities[3].animation_rig->model;Runtime r(d);const auto first=stream(r);need(first.playback.clip==0&&first.playback.layers.size()==1&&first.playback.layers[0].clip==3,"Target bank did not copy original base/layer configuration.");for(const auto& j:first.joints)need(j.source==PhysicalRigDriveSource::Manual&&!j.sample,"Animation source was implicitly selected at startup.");step(r,{},3);near(stream(r).playback.time,3*Runtime::fixed_dt);need(drive(r,"shoulder").strength==0,"Unselected target clock drove physics.");
    const auto before=saved(r);rejects([&]{step(r,{source("shoulder",PhysicalRigDriveSource::Animation,1)});},"Selecting source implicitly activated physics.");equal("unactivated source refusal",before,saved(r));rejects([&]{step(r,{playback(1)});},"Target playback accepted before physical takeover.");equal("unactivated playback refusal",before,saved(r));
    step(r,{activate(),source("shoulder",PhysicalRigDriveSource::Animation,1),source("elbow",PhysicalRigDriveSource::Animation,1)});targets_match(r,*m);need(r.physical_rig(rig)->mode==PhysicalRigMode::Driven,"Automatic positive strengths did not enter Driven.");
    const auto state=saved(r);auto zero=Runtime::from_snapshot(d,content,state);step(*zero,{source("shoulder",PhysicalRigDriveSource::Animation,0),source("elbow",PhysicalRigDriveSource::Animation,0)});step(r);
    const auto initial_skin=palette(r);double largest=0;
    for(unsigned n=0;n<45;++n){step(r);step(*zero);targets_match(r,*m);const auto p=r.physical_part(rig,"arm"),q=zero->physical_part(rig,"arm");need(p&&q&&p->enrolled&&q->enrolled,"Physical response lost native membership.");largest=std::max(largest,difference(p->pose.rotation,q->pose.rotation));for(double x:p->angular_velocity)need(std::isfinite(x),"Target response has nonfinite velocity.");need(!p->at_linear_velocity_limit&&!p->at_angular_velocity_limit&&p->pose.position[1]>100,"Free-fall target fixture reached a safety cap or floor.");}
    need(largest>.01&&palette(r)!=initial_skin,"Changing target stream did not produce a meaningful native/skinned response against zero strength.");need(zero->physical_rig(rig)->mode==PhysicalRigMode::Passive,"Zero-strength automatic targets did not remain Passive.");
    const auto query=saved(r);for(unsigned n=0;n<3;++n){(void)stream(r);(void)r.physical_rig(rig,true,true);(void)palette(r);}equal("target inspection purity",query,saved(r));continuation(d,r,"streaming targets");
}
void manual_hold_and_independent_playback(){
    group="manual-hold-and-independent-playback";const auto d=definition();const auto m=d.entities[3].animation_rig->model;Runtime r(d);step(r,{},2);step(r,{activate(),source("shoulder",PhysicalRigDriveSource::Animation,.7),source("elbow",PhysicalRigDriveSource::Animation,.4)});step(r,{},6);const auto shoulder=drive(r,"shoulder"),elbow=drive(r,"elbow");const auto original=*r.animation(rig);
    step(r,{source("shoulder",PhysicalRigDriveSource::Manual,shoulder.strength),source("elbow",PhysicalRigDriveSource::Manual,elbow.strength),playback(1,.2)});const auto target=stream(r);need(target.playback.clip==1&&r.animation(rig)->clip==original.clip,"Target playback changed source animation clip.");near(r.animation(rig)->time,original.time+Runtime::fixed_dt);step(r,{},5);need(drive(r,"shoulder").target_rotation==shoulder.target_rotation&&drive(r,"elbow").target_rotation==elbow.target_rotation,"Manual hold changed retained target bytes while independent clock advanced.");for(const auto& j:stream(r).joints)need(j.source==PhysicalRigDriveSource::Manual&&!j.sample,"Manual hold retained false automatic sample attribution.");near(stream(r).playback.time,.2+6*Runtime::fixed_dt);continuation(d,r,"manual held targets");
    step(r,{source("shoulder",PhysicalRigDriveSource::Animation,.7),source("elbow",PhysicalRigDriveSource::Animation,.4)});targets_match(r,*m);need(difference(drive(r,"shoulder").target_rotation,shoulder.target_rotation)>.05,"Resuming source reused stale held target.");
    step(r,{manual("shoulder",axis(2,.15),.5)});need(joint(stream(r),"shoulder").source==PhysicalRigDriveSource::Manual&&!joint(stream(r),"shoulder").sample,"Manual SetDrive failed to claim manual ownership.");rotation_near(drive(r,"shoulder").target_rotation,axis(2,.15));step(r,{},3);rotation_near(drive(r,"shoulder").target_rotation,axis(2,.15));
    // Retain the known non-axis readback regression from the drive contract.
    // Manual takeover removes attribution, but must not add another hidden
    // normalization when exactly resubmitting committed binary control values.
    const Q non_axis{.13101032055011172,.009135762026470012,.13557831134064172,.9820241113412504};step(r,{manual("shoulder",non_axis,.5)});const auto readback=drive(r,"shoulder");auto unchanged=Runtime::from_snapshot(d,content,saved(r));step(r,{manual("shoulder",readback.target_rotation,readback.strength)});step(*unchanged);equal("detached player exact manual readback no-op",saved(*unchanged),saved(r));
    step(r,{source("shoulder",PhysicalRigDriveSource::Manual,0),source("elbow",PhysicalRigDriveSource::Manual,0)});need(r.physical_rig(rig)->mode==PhysicalRigMode::Passive,"Release changed ownership or failed to remove active torque.");continuation(d,r,"manual released targets");
    const auto passive=saved(r);AnimationCommand ordinary{rig,1,0,1,false,true,0};rejects([&]{step(r,{},1,{}, {ordinary});},"Target opt-in permitted ordinary animation to overwrite physical pose.");equal("ordinary animation ownership refusal",passive,saved(r));
}
void layers_rates_and_atomic_failures(){
    group="layers-rates-and-atomic-failures";const auto d=definition();Runtime r(d);step(r,{},2);step(r,{activate(),source("shoulder",PhysicalRigDriveSource::Animation,.6),source("elbow",PhysicalRigDriveSource::Animation,.6)});step(r,{playback(3,.4,false,1,1),rate(.5)});const auto a=stream(r);need(a.playback.layers.size()==1&&a.playback.layers[0].clip==3&&a.playback.layers[0].weight==1,"Target layer control did not reach detached layer.");rotation_near(drive(r,"elbow").target_rotation,axis(0,.6));need(r.animation_layer(rig,1)->weight==0,"Target layer modified visible/source layer.");near(a.playback.speed,.5);const auto t=a.playback.time;step(r,{},4);near(stream(r).playback.time,t+2*Runtime::fixed_dt);rotation_near(drive(r,"elbow").target_rotation,axis(0,.6));
    step(r,{playback(1,.1,true,{},1,6)});need(stream(r).playback.transition.has_value(),"Detached crossfade was not observable.");continuation(d,r,"target crossfade",2);
    const auto before=saved(r);const std::vector<std::vector<PhysicalRigCommand>> bad={
        {source("missing",PhysicalRigDriveSource::Animation,.5)},
        {source("shoulder",static_cast<PhysicalRigDriveSource>(5),.5)},
        {source("shoulder",PhysicalRigDriveSource::Animation,1.1)},
        {source("shoulder",PhysicalRigDriveSource::Manual,std::numeric_limits<double>::quiet_NaN())},
        {source("shoulder",PhysicalRigDriveSource::Animation,.4),manual("shoulder",axis(2,.2),.4)},
        {manual("shoulder",axis(2,.2),.4),source("shoulder",PhysicalRigDriveSource::Manual,.4)},
        {source("shoulder",PhysicalRigDriveSource::Animation,.4),source("shoulder",PhysicalRigDriveSource::Animation,.4)},
        {playback(1),playback(1)}, {playback(1),rate(.5)}, {rate(.5),rate(.5)},
        {playback(1,0,true,2)}, {playback(999)}, {rate(std::numeric_limits<double>::quiet_NaN())}
    };
    for(std::size_t n=0;n<bad.size();++n){rejects([&]{step(r,bad[n]);},"Invalid or duplicate target operation accepted.");equal("target refusal "+std::to_string(n),before,saved(r));}
    RuntimeTransform at;at.position={5,200,0};rejects([&]{step(r,{playback(1,.2)},4,{{2,1,{{id(900),at}}, {}}});},"Late structural conflict accepted.");equal("target clock and solver late rollback",before,saved(r));
    // This first pair of steps is valid; the later sample crosses the hard
    // shoulder limit. A multi-step failure must undo earlier real solves.
    step(r,{playback(3,0,false,1,0),playback(2,.75),source("shoulder",PhysicalRigDriveSource::Animation,.8)});const auto crossing=saved(r);auto trial=Runtime::from_snapshot(d,content,crossing);step(*trial,{},1);need(stream(*trial).playback.time>stream(r).playback.time,"Strict crossing never reached an actual valid earlier step.");records().retain("strict valid precursor","accepted-source",saved(*trial));rejects([&]{step(r,{},10);},"Strict sampled target outside limits was silently fitted.");equal("strict sampled failure full rollback",crossing,saved(r));
}
void fitting_and_batch_equivalence(){
    group="fitting-and-batch-equivalence";const auto d=definition(true,pr::TargetLimitPolicy::Fit);Runtime r(d);step(r,{},2);step(r,{activate(),playback(4,0,false),source("shoulder",PhysicalRigDriveSource::Animation,.7),source("elbow",PhysicalRigDriveSource::Animation,.7)});const auto a=stream(r);need(a.limit_policy==pr::TargetLimitPolicy::Fit,"Explicit fit policy lost.");for(const auto& j:a.joints){need(j.sample&&j.sample->correction_radians>.1,"Outside target lacks explicit fit correction.");rotation_near(j.sample->target,j.name=="shoulder"?axis(2,1.2):axis(0,1.2));rotation_near(drive(r,j.name.c_str()).target_rotation,j.sample->target);}need(joint(a,"shoulder").sample->swing_clamped&&joint(a,"elbow").sample->twist_clamped,"Fitting diagnostics omit active limit channels.");const auto before=saved(r);rejects([&]{step(r,{manual("elbow",axis(0,1.4),.7)});},"Sample fitting weakened manual target admission.");equal("fit manual refusal",before,saved(r));continuation(d,r,"fitted target");
    step(r,{playback(1,.2),source("shoulder",PhysicalRigDriveSource::Animation,.6),source("elbow",PhysicalRigDriveSource::Animation,.6)});const auto start=saved(r);auto singles=Runtime::from_snapshot(d,content,start);step(r,{},12);for(unsigned n=0;n<12;++n)step(*singles);equal("target batch equals twelve single ticks",saved(r),saved(*singles));need(palette(r)==palette(*singles),"Batch target progression changed skin matrices.");continuation(d,r,"fitted-policy legal target");
}

void save_contract_and_templates(){
    group="save-contract-and-templates";const auto d=definition();Runtime r(d);step(r,{},2);step(r,{activate(),source("shoulder",PhysicalRigDriveSource::Animation,.7),source("elbow",PhysicalRigDriveSource::Animation,.4),playback(1,.3)});step(r,{},3);const auto original=saved(r);const auto document=J::parse(original);const auto& physical=document.at("payload").at("physical");need(physical.at("version")==6&&physical.at("rigs").size()==1,"Opted-in runtime did not select physical extension six.");const auto& animation=physical.at("rigs")[0].at("animation");need(animation.at("version")==1&&animation.at("entity")==rig&&animation.at("tick")==r.inspect().tick&&animation.at("joints").size()==2,"Complete target checkpoint identity differs.");records().retain("untouched automatic save","accepted-source",original);
    for(unsigned mutation=0;mutation<31;++mutation){auto bad=document;auto& row=bad["payload"]["physical"]["rigs"][0];auto& a=row["animation"];
        switch(mutation){
        case 0:a=nullptr;break;
        case 1:row.erase("animation");break;
        case 2:a["entity"]=id(999);break;
        case 3:a["tick"]=r.inspect().tick-1;break;
        case 4:a["tick"]=double(r.inspect().tick);break;
        case 5:a["player"]=J::object();break;
        case 6:std::swap(a["joints"][0],a["joints"][1]);break;
        case 7:a["joints"][0]["source"]="unknown";break;
        case 8:a["joints"][0]["source"]="manual";break;
        case 9:a["joints"][0]["sample"]=nullptr;break;
        case 10:a["joints"][0]["sample"]["target"]={0.,0.,0.,1.};break;
        case 11:a["joints"][0]["sample"]["requested"]={0.,0.,0.,1.};break;
        case 12:a["joints"][0]["sample"]["correction_radians"]=.1;break;
        case 13:a["joints"][0]["sample"]["chart_condition"]=.1;break;
        case 14:a["joints"][0]["sample"]["swing_clamped"]=true;break;
        case 15:row["membership"]["drives"][0]["target_rotation"]={0.,0.,0.,1.};break;
        case 16:a["joints"][0]["strength"]=.7;break;
        case 17:a["version"]=2;break;
        case 18:a["joints"][0]["sample"]["correction_radians"]=-1e-13;break;
        case 19:a["player"]["history"]["current"]=nullptr;break;
        case 20:a["player"]["history"]["previous"]=nullptr;break;
        case 21:a["player"]["history"]["current"]=nullptr;a["player"]["history"]["previous"]=nullptr;break;
        case 22:a["player"]["layer_history"][0]["current"]=nullptr;break;
        case 23:a["player"]["layer_history"][0]["previous"]=nullptr;break;
        case 24:a["player"]["layer_history"][0]["current"]=nullptr;a["player"]["layer_history"][0]["previous"]=nullptr;break;
        case 25:a["player"]["history"]["current"]["poses"][0]["position"][0]=.125;break;
        case 26:a["player"]["history"]["current"]["poses"][1]["rotation"]=axis(2,.11);break;
        case 27:a["player"]["history"]["current"]["poses"][0]["scale"][0]=1.125;break;
        case 28:a["player"]["layer_history"][0]["current"]["poses"][0]["position"][0]=.125;break;
        case 29:a["player"]["layer_history"][0]["current"]["poses"][1]["rotation"]=axis(2,.11);break;
        case 30:a["player"]["layer_history"][0]["current"]["poses"][0]["scale"][0]=1.125;break;
        }
        if(mutation>=19){
            // If a generic inertial record also retains histories, preserve its
            // exact duplicate agreement. The new physical boundary must still
            // reject missing or changed current histories; physical poses are intact.
            auto& player=a["player"]["player"]["rigs"][0];
            if(player.contains("history"))player["history"]=a["player"]["history"];
            for(std::size_t n=0;n<player["layers"].size();++n)if(player["layers"][n].contains("history"))player["layers"][n]["history"]={{"current",a["player"]["layer_history"][n]["current"]},{"previous",a["player"]["layer_history"][n]["previous"]}};
            need(typed(row.at("pose"),document.at("payload").at("physical").at("rigs")[0].at("pose")),"Target-history corruption touched original physical pose histories.");
        }
        const auto bytes=reseal(std::move(bad));records().retain("resealed target mutation "+std::to_string(mutation),"negative",bytes);rejects([&]{Runtime::validate_snapshot(d,content,bytes);},"Resealed invalid animation checkpoint validated.");rejects([&]{(void)Runtime::from_snapshot(d,content,bytes);},"Resealed invalid animation checkpoint loaded.");equal("detached target rejection purity "+std::to_string(mutation),original,saved(r));
    }
    // A complete donor player is internally valid but belongs to a different
    // actual clock. Keeping the recipient's sample must reject this graft.
    auto donor=std::make_unique<Runtime>(d);step(*donor,{},2);step(*donor,{activate(),source("shoulder",PhysicalRigDriveSource::Animation,.7),source("elbow",PhysicalRigDriveSource::Animation,.4),playback(0,.9)});step(*donor,{},3);need(donor->inspect().tick==r.inspect().tick,"Donor clock graft must use the same committed tick.");const auto donor_bytes=saved(*donor);records().retain("independent donor clock","accepted-source",donor_bytes);auto graft=document;graft["payload"]["physical"]["rigs"][0]["animation"]["player"]=J::parse(donor_bytes).at("payload").at("physical").at("rigs")[0].at("animation").at("player");const auto graft_bytes=reseal(std::move(graft));records().retain("different clock graft","negative",graft_bytes);rejects([&]{(void)Runtime::from_snapshot(d,content,graft_bytes);},"Different target clock was accepted with original sample.");equal("donor graft source purity",original,saved(r));equal("donor graft donor purity",donor_bytes,saved(*donor));
    const auto legacy=definition(false);Runtime old(legacy);step(old,{},2);need(!old.physical_animation(rig),"Unconfigured old drive acquired a target bank.");const auto old_before=saved(old);rejects([&]{step(old,{activate(),source("shoulder",PhysicalRigDriveSource::Animation,.5)});},"Legacy profile acquired animation permission.");equal("legacy source refusal",old_before,saved(old));step(old,{activate(),manual("shoulder",axis(2,.2),.4)});const auto old_bytes=saved(old);const auto old_document=J::parse(old_bytes);need(old_document.at("payload").at("physical").at("version")==5&&!old_document.at("payload").at("physical").at("rigs")[0].contains("animation"),"Unconfigured legacy save acquired extension-six fields.");continuation(legacy,old,"legacy manual-drive record",3);
    const auto empty_definition=definition(true,pr::TargetLimitPolicy::Strict,true);Runtime empty(empty_definition);const auto dormant=saved(empty);const auto dormant_physical=J::parse(dormant).at("payload").at("physical");need(dormant_physical.at("version")==6&&dormant_physical.at("rigs").empty(),"Dormant target template lost its extension-six contract.");auto cold=Runtime::from_snapshot(empty_definition,content,dormant);equal("dormant target template",dormant,saved(*cold));RuntimeTransform at;at.position={5,200,0};const auto born=empty.change_structure(0,{{id(900),at}},{}).spawned.at(0);const auto mirrored=cold->change_structure(0,{{id(900),at}},{}).spawned.at(0);need(born==mirrored,"Target template restore changed allocation frontier.");step(empty,{},2);step(*cold,{},2);const std::vector requests{activate(born),source("shoulder",PhysicalRigDriveSource::Animation,.5,born),source("elbow",PhysicalRigDriveSource::Animation,.3,born)};step(empty,requests);step(*cold,requests);equal("born target streams",saved(empty),saved(*cold));need(stream(empty,born).joints.size()==2,"Born rig lost target membership.");for(unsigned n=0;n<3;++n){step(empty);step(*cold);equal("born target future "+std::to_string(n),saved(empty),saved(*cold));}empty.change_structure(empty.structure_revision(),{}, {born});cold->change_structure(cold->structure_revision(),{}, {born});equal("retired target streams",saved(empty),saved(*cold));rejects([&]{(void)empty.physical_animation(born);},"Retired target stream remained queryable.");const auto retired=saved(empty);auto revived=Runtime::from_snapshot(empty_definition,content,retired);equal("retired target checkpoint",retired,saved(*revived));const auto next=empty.change_structure(empty.structure_revision(),{{id(900),at}},{}).spawned.at(0);const auto next_cold=revived->change_structure(revived->structure_revision(),{{id(900),at}},{}).spawned.at(0);need(next==next_cold&&next!=born,"Retired target stream changed the next incarnation identity.");equal("next target incarnation",saved(empty),saved(*revived));
}
RuntimeDefinition velocity_definition(pr::TargetLimitPolicy policy=pr::TargetLimitPolicy::Strict,bool dormant=false){
    auto d=definition(true,policy,dormant);
    const auto enable=[](auto& entities){for(auto& e:entities)if(e.physical_rig&&e.physical_rig->animation_drive)e.physical_rig->animation_drive->velocity_tracking=true;};
    enable(d.entities);for(auto& t:d.templates)enable(t.entities);return d;
}
V independent_interval(Q start,Q end){
    auto delta=product(end,inverse(start));if(delta[3]<0)for(double& v:delta)v=-v;
    const double n=std::sqrt(delta[0]*delta[0]+delta[1]*delta[1]+delta[2]*delta[2]);
    if(n==0)return {};
    const double scale=2*std::atan2(n,delta[3])/(n*Runtime::fixed_dt);
    return {delta[0]*scale,delta[1]*scale,delta[2]*scale};
}
void vector_near(V a,V b,double bound=2e-7){for(unsigned n=0;n<3;++n)near(a[n],b[n],bound);}
void interval_oracle(const Runtime& r,const ModelAsset& model,unsigned clip,double start,double end){
    const auto current=stream(r);need(current.velocity_tracking,"Authored target velocity permission missing from observation.");
    const auto native=r.physical_rig(rig,true,true);need(native&&native->joints.size()==2,"Velocity oracle requires complete native joints.");
    for(unsigned n=0;n<2;++n){const auto& row=current.joints[n];need(row.sample&&row.motion,"Animation velocity sample/interval absent.");
        const auto a=independent_target(model,clip,start,n),b=independent_target(model,clip,end,n);
        rotation_near(row.motion->start_rotation,a);rotation_near(row.sample->target,b);
        vector_near(row.motion->angular_velocity,independent_interval(a,b));
        need(native->joints[n].drive_target_angular_velocity==std::optional<V>{row.motion->angular_velocity},"Backend target velocity differs from exact committed interval.");
    }
}
void velocity_intervals_and_switches(){
    group="velocity-intervals-and-switches";const auto d=velocity_definition();const auto model=d.entities[3].animation_rig->model;Runtime r(d);
    const auto initial=stream(r);need(initial.velocity_tracking,"New immutable permission absent.");for(const auto& row:initial.joints)need(!row.sample&&!row.motion,"Unselected source acquired a motion interval.");
    step(r,{},2);step(r,{activate(),source("shoulder",PhysicalRigDriveSource::Animation,.7),source("elbow",PhysicalRigDriveSource::Animation,.4)});
    interval_oracle(r,*model,0,2*Runtime::fixed_dt,3*Runtime::fixed_dt);
    const auto old=joint(stream(r),"shoulder").sample->target;
    step(r,{playback(1,.1)});interval_oracle(r,*model,1,.1,.1+Runtime::fixed_dt);
    need(difference(old,joint(stream(r),"shoulder").motion->start_rotation)>.2,"Immediate-restart fixture did not replace a distinct old pose.");
    auto time=stream(r).playback.time;step(r,{rate(2)});interval_oracle(r,*model,1,time,time+2*Runtime::fixed_dt);
    time=stream(r).playback.time;step(r,{source("shoulder",PhysicalRigDriveSource::Animation,.2)});interval_oracle(r,*model,1,time,time+2*Runtime::fixed_dt);
    need(drive(r,"shoulder").strength==.2,"Strength-only control failed to preserve requested strength.");
    time=stream(r).playback.time;step(r,{rate(0)});interval_oracle(r,*model,1,time,time);
    for(const auto& row:stream(r).joints)need(row.motion->angular_velocity==V{},"Frozen plain clip acquired nonzero target velocity.");
    step(r,{manual("shoulder",axis(2,.7),.2)});const auto held=stream(r);need(!joint(held,"shoulder").motion&&!joint(held,"shoulder").sample,"Manual source retained an interval.");
    need(!r.physical_rig(rig,true,true)->joints[0].drive_target_angular_velocity,"Manual source retained backend feed-forward.");
    time=stream(r).playback.time;step(r,{source("shoulder",PhysicalRigDriveSource::Animation,.2),rate(1)});interval_oracle(r,*model,1,time,time+Runtime::fixed_dt);
    need(difference(axis(2,.7),joint(stream(r),"shoulder").motion->start_rotation)>.2,"Manual-to-stream fixture did not distinguish unrelated manual orientation.");
    continuation(d,r,"velocity after source/rate changes",4);
    const auto before=saved(r);rejects([&]{step(r,{playback(0,.2)},4,{{2,1,{{id(900),RuntimeTransform{}}},{}}});},"Late structural conflict accepted in velocity mode.");equal("velocity full late-transaction rollback",before,saved(r));
    const auto base=saved(r);auto single=Runtime::from_snapshot(d,content,base);step(r,{},6);for(unsigned n=0;n<6;++n)step(*single);equal("velocity six-tick batch equals singles",saved(r),saved(*single));
}
void velocity_fades_and_native_response(){
    group="velocity-fades-and-native-response";const auto d=velocity_definition();Runtime r(d);step(r,{},2);step(r,{activate(),source("shoulder",PhysicalRigDriveSource::Animation,.6),source("elbow",PhysicalRigDriveSource::Animation,.6)});
    auto layer=playback(3,0,false,1,1);layer.animation->weight_blend_ticks=2;step(r,{layer});
    const auto prior=joint(stream(r),"elbow").sample->target;need(stream(r).playback.layers[0].weight_transition.has_value(),"Layer retirement fixture has no live weight transition.");
    step(r);const auto end=stream(r);need(!end.playback.layers[0].weight_transition&&end.playback.layers[0].weight==1,"Final layer fade was not retired.");
    rotation_near(joint(end,"elbow").motion->start_rotation,prior);
    vector_near(joint(end,"elbow").motion->angular_velocity,independent_interval(prior,joint(end,"elbow").sample->target));
    need(std::abs(joint(end,"elbow").motion->angular_velocity[0])>1,"Final fade interval was flattened by post-sample weight cleanup.");
    continuation(d,r,"retired layer interval",4);
    auto fade=playback(1,.2,true,{},1,4);fade.animation->transition_mode=AnimationTransitionMode::Inertial;step(r,{fade});
    const auto start=saved(r);auto cold=Runtime::from_snapshot(d,content,start);for(unsigned n=0;n<4;++n){step(r);step(*cold);equal("velocity inertial cold interval "+std::to_string(n),saved(r),saved(*cold));}
    const auto fit_definition=velocity_definition(pr::TargetLimitPolicy::Fit);Runtime fitted(fit_definition);step(fitted,{},2);step(fitted,{activate(),playback(4,0,false),source("shoulder",PhysicalRigDriveSource::Animation,.5),source("elbow",PhysicalRigDriveSource::Animation,.5)});
    for(const auto& row:stream(fitted).joints){need(row.sample->correction_radians>.1,"Constant fitted fixture did not encounter original constraints.");need(row.motion->angular_velocity==V{},"Two identical fitted endpoints acquired a hidden velocity.");}
    continuation(fit_definition,fitted,"constant fitted interval",2);
    // The legacy and velocity-enabled fixtures have identical authored mechanics
    // and receive identical source controls; only feed-forward is opted in.
    Runtime passive_new(d),passive_old(definition());step(passive_new,{},2);step(passive_old,{},2);
    const std::vector zero{activate(),source("shoulder",PhysicalRigDriveSource::Animation,0),source("elbow",PhysicalRigDriveSource::Animation,0)};
    step(passive_new,zero);step(passive_old,zero);
    for(unsigned n=0;n<8;++n){step(passive_new);step(passive_old);const auto x=J::parse(saved(passive_new)),y=J::parse(saved(passive_old));
        need(typed(x.at("payload").at("physical").at("physics"),y.at("payload").at("physical").at("physics")),"Zero-strength opt-in changed complete serialized native physics.");need(palette(passive_new)==palette(passive_old),"Zero-strength target velocity changed skin output.");}
    Runtime tracking(d),holding(definition());step(tracking,{},2);step(holding,{},2);
    const std::vector positive{activate(),source("shoulder",PhysicalRigDriveSource::Animation,.6),source("elbow",PhysicalRigDriveSource::Animation,.6)};
    step(tracking,positive);step(holding,positive);double response=0;
    for(unsigned n=0;n<12;++n){step(tracking);step(holding);const auto a=tracking.physical_part(rig,"arm"),b=holding.physical_part(rig,"arm");need(a&&b,"Native velocity comparison lost body.");response=std::max(response,difference(a->pose.rotation,b->pose.rotation));
        for(double v:a->angular_velocity)need(std::isfinite(v),"Tracking response is nonfinite.");need(!a->at_angular_velocity_limit&&!a->at_linear_velocity_limit,"Tracking fixture hit native speed caps.");}
    need(response>1e-4,"Opted-in interval velocity made no measurable native orientation difference.");
}
void velocity_saves_and_refusals(){
    group="velocity-saves-and-refusals";const auto d=velocity_definition();Runtime r(d);step(r,{},2);step(r,{activate(),source("shoulder",PhysicalRigDriveSource::Animation,.7),source("elbow",PhysicalRigDriveSource::Animation,.4)});step(r,{},3);
    const auto original=saved(r);const auto document=J::parse(original);const auto& physical=document.at("payload").at("physical");need(physical.at("version")==7&&physical.at("rigs")[0].at("animation").at("version")==2,"Velocity owner did not select extension7/target2.");
    for(unsigned mutation=0;mutation<13;++mutation){auto bad=document;auto& row=bad["payload"]["physical"]["rigs"][0];auto& a=row["animation"];auto& first=a["joints"][0];
        switch(mutation){
        case 0:a["version"]=1;break;
        case 1:first.erase("motion");break;
        case 2:first["motion"]=nullptr;break;
        case 3:first["motion"]["extra"]=true;break;
        case 4:first["motion"]["angular_velocity"]={0.,0.};break;
        case 5:first["motion"]["angular_velocity"][0]=.125;break;
        case 6:first["motion"]["angular_velocity"][0]=pr::max_relative_angular_velocity+1;break;
        case 7:first["motion"]["start_rotation"]={0.,0.,0.,2.};break;
        case 8:first["motion"]["start_rotation"]=axis(2,2.);break;
        case 9:first["source"]="manual";first["sample"]=nullptr;break;
        case 10:row["membership"]["drives"][0].erase("target_angular_velocity");break;
        case 11:row["membership"]["drives"][0]["target_angular_velocity"]={.125,0.,0.};break;
        case 12:bad["payload"]["physical"]["version"]=6;break;
        }
        const auto text=reseal(std::move(bad));records().retain("velocity resealed mutation "+std::to_string(mutation),"negative",text);
        rejects([&]{Runtime::validate_snapshot(d,content,text);},"Invalid motion archive validated.");rejects([&]{(void)Runtime::from_snapshot(d,content,text);},"Invalid motion archive restored.");equal("velocity refusal purity "+std::to_string(mutation),original,saved(r));
    }
    continuation(d,r,"complete target velocity",4);
    step(r,{source("shoulder",PhysicalRigDriveSource::Manual,0),source("elbow",PhysicalRigDriveSource::Manual,0)});
    const auto released=J::parse(saved(r)).at("payload").at("physical").at("rigs")[0];for(const auto& row:released.at("animation").at("joints"))need(row.at("motion").is_null()&&row.at("sample").is_null(),"Released source retained motion attribution.");
    for(const auto& row:released.at("membership").at("drives"))need(!row.contains("target_angular_velocity"),"Released backend retained target velocity.");continuation(d,r,"released velocity owner",2);
    // A distinct test clip spans exactly pi within one native interval, with
    // both hinge endpoints individually legal. No ambiguous velocity is chosen.
    auto ambiguous=velocity_definition();auto model_copy=std::make_shared<ModelAsset>(*ambiguous.entities[3].animation_rig->model);
    constexpr float duration=1.f/120.f;
    auto& clip=model_copy->animations[0];clip.duration=duration;
    for(auto& channel:clip.channels)channel.times={0.f,duration};
    clip.channels[0].values={key(local_target(0,axis(2,.1))),key(local_target(0,axis(2,.1)))};
    clip.channels[1].values={key(axis(0,-std::acos(-1.)/2)),key(axis(0,std::acos(-1.)/2))};
    ambiguous.entities[3].animation_rig->model=model_copy;ambiguous.entities[3].physical_rig->profile.joints[1].limits=pr::Hinge{-2,2};
    Runtime pi(ambiguous);step(pi,{},2);const auto old=saved(pi);bool pi_refused=false;
    try{step(pi,{activate(),playback(0),source("elbow",PhysicalRigDriveSource::Animation,.5)});}catch(const std::exception& error){pi_refused=std::string(error.what()).find("ambiguous near a pi rotation")!=std::string::npos;}
    need(pi_refused,"The actual ambiguous pi interval did not produce its specific refusal.");equal("ambiguous interval restores activation and entire state",old,saved(pi));
    const auto dormant_definition=velocity_definition(pr::TargetLimitPolicy::Strict,true);Runtime empty(dormant_definition);const auto dormant=saved(empty);need(J::parse(dormant).at("payload").at("physical").at("version")==7,"Dormant velocity template lost extension7.");auto restored=Runtime::from_snapshot(dormant_definition,content,dormant);equal("dormant velocity template cold",dormant,saved(*restored));
    RuntimeTransform at;at.position={5,200,0};const auto born=empty.change_structure(0,{{id(900),at}},{}).spawned.at(0);const auto mirror=restored->change_structure(0,{{id(900),at}},{}).spawned.at(0);need(born==mirror,"Velocity template allocator changed.");step(empty,{},2);step(*restored,{},2);const std::vector controls{activate(born),source("shoulder",PhysicalRigDriveSource::Animation,.5,born),source("elbow",PhysicalRigDriveSource::Animation,.3,born)};step(empty,controls);step(*restored,controls);equal("born velocity targets",saved(empty),saved(*restored));empty.change_structure(empty.structure_revision(),{}, {born});restored->change_structure(restored->structure_revision(),{}, {born});equal("retired velocity targets",saved(empty),saved(*restored));
    auto explicit_false=definition();explicit_false.entities[3].physical_rig->animation_drive->velocity_tracking=false;Runtime omitted(definition()),disabled(explicit_false);step(omitted,{},2);step(disabled,{},2);equal("explicit false retains complete legacy archive",saved(omitted),saved(disabled));
    const auto legacy=J::parse(saved(disabled)).at("payload").at("physical");need(legacy.at("version")==6&&legacy.at("rigs")[0].at("animation").at("version")==1,"Legacy target record changed version.");for(const auto& row:legacy.at("rigs")[0].at("animation").at("joints"))need(!row.contains("motion"),"Legacy v1 target acquired motion field.");
}

}
int main(){try{need(Runtime::available(),"Actual native simulation is required.");clocks_targets_and_native_response();manual_hold_and_independent_playback();layers_rates_and_atomic_failures();fitting_and_batch_equivalence();save_contract_and_templates();velocity_intervals_and_switches();velocity_fades_and_native_response();velocity_saves_and_refusals();std::cout<<"Runtime physical animation: 8 groups, "<<checks<<" checks, "<<pairs<<" complete archive pairs passed; no balance or recovery claim.\n";return 0;}catch(const std::exception& e){std::cerr<<"Runtime physical animation group "<<group<<": "<<e.what()<<'\n';return 1;}}
