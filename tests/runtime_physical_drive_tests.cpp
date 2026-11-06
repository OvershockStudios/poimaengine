// SPDX-License-Identifier: Apache-2.0
// Independent, ordinary Runtime integration of bounded physical joint drives.
// Three analytic sphere proxies and a skinned triangle; no importer/GPU needed.
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
using Json=nlohmann::json;
using V=pr::Vec3;
using Q=pr::Quat;
const std::string content_hash(64,'d');
unsigned checks=0,pairs=0;
const char* group="startup";
std::string id(unsigned n){const auto s=std::to_string(n);return std::string(32-s.size(),'0')+s;}
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& operation,const char* why){bool failed=false;try{operation();}catch(const std::exception&){failed=true;}check(failed,why);}
double norm(V a){return std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);}
V add(V a,V b){for(unsigned n=0;n<3;++n)a[n]+=b[n];return a;}
V scale(V a,double s){for(double& v:a)v*=s;return a;}
Q axis(unsigned n,double angle){Q q{0,0,0,std::cos(angle/2)};q[n]=std::sin(angle/2);return q;}
Q conjugate(Q q){for(unsigned n=0;n<3;++n)q[n]=-q[n];return q;}
// Independent quaternion arithmetic rather than the production frame helper.
Q multiply(Q a,Q b){return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};}
double angle_between(Q a,Q b){double dot=0,aa=0,bb=0;for(unsigned n=0;n<4;++n){dot+=a[n]*b[n];aa+=a[n]*a[n];bb+=b[n]*b[n];}return 2*std::acos(std::clamp(std::abs(dot)/std::sqrt(aa*bb),0.0,1.0));}

// Optional complete artifacts from this execution, retaining both sides even
// when an equality assertion fails. No archive fields are removed or coerced.
class ArchiveLog {
    std::filesystem::path output;
    Json manifest={{"format","poima.physical-drive-native-archives.v1"},{"entries",Json::array()},{"pairs",Json::array()}};
    std::uint64_t bytes=0;
    static constexpr std::uint64_t cap=32u*1024u*1024u;
    void commit(){const auto text=manifest.dump(2);if(text.size()>cap-bytes)throw std::runtime_error("Drive archive manifest exceeds cap.");std::ofstream f(output/"manifest.json",std::ios::binary);f<<text;f.close();if(!f)throw std::runtime_error("Cannot finish drive archive manifest.");}
public:
    ArchiveLog(){const auto* root=std::getenv("POIMA_TEST_ARTIFACTS");if(!root||!*root)return;output=std::filesystem::path(root)/"physical-drive";std::filesystem::create_directories(output);if(std::filesystem::exists(output/"manifest.json"))throw std::runtime_error("Drive artifacts require a fresh directory.");}
    Json retain(const std::string& label,const std::string& role,const std::string& value){
        if(output.empty())return nullptr;
        if(value.size()>cap||bytes>cap-value.size()||manifest["entries"].size()>=512)throw std::runtime_error("Drive artifacts exceed 32MiB/512-file bounds.");
        char name[64];std::snprintf(name,sizeof(name),"%04zu-%s.json",manifest["entries"].size()+1,role.c_str());const auto path=output/name;
        if(std::filesystem::exists(path))throw std::runtime_error("Drive artifact path already exists.");
        std::ofstream f(path,std::ios::binary);f.write(value.data(),static_cast<std::streamsize>(value.size()));f.close();if(!f)throw std::runtime_error("Cannot finish drive archive.");
        Json row={{"label",label},{"group",group},{"role",role},{"path",name},{"bytes",value.size()},{"sha256",sha256(std::as_bytes(std::span(value.data(),value.size())))}};
        bytes+=value.size();manifest["entries"].push_back(row);manifest["raw_bytes"]=bytes;commit();return row;
    }
    void pair(const std::string& label,const std::string& a,const std::string& b){if(output.empty())return;const auto left=retain(label,"source",a),right=retain(label,"restored",b);manifest["pairs"].push_back({{"label",label},{"group",group},{"left",left.at("path")},{"right",right.at("path")},{"complete_byte_equal",a==b},{"excluded_fields",Json::array()}});commit();}
};
ArchiveLog& artifacts(){static ArchiveLog value;return value;}
bool typed_equal(const Json& a,const Json& b){
    if(a.type()!=b.type())return false;
    if(a.is_object()){if(a.size()!=b.size())return false;for(auto it=a.begin();it!=a.end();++it){const auto found=b.find(it.key());if(found==b.end()||!typed_equal(it.value(),*found))return false;}return true;}
    if(a.is_array()){if(a.size()!=b.size())return false;for(std::size_t i=0;i<a.size();++i)if(!typed_equal(a[i],b[i]))return false;return true;}
    return a==b;
}
void equal_archive(const std::string& label,const std::string& source,const std::string& restored){artifacts().pair(label,source,restored);++pairs;check(source==restored,"Complete drive archive bytes differ.");check(typed_equal(Json::parse(source),Json::parse(restored)),"Complete drive archive typed values differ.");}
std::string archive(const Runtime& r){return r.save_snapshot(content_hash);}

std::shared_ptr<ModelAsset> model(){
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(3);m->roots={0};
    m->nodes[0].name="pelvis";m->nodes[0].skin=0;m->nodes[0].primitives={0};
    m->nodes[1].name="arm";m->nodes[1].parent=0;m->nodes[1].position={0,.5,0};
    m->nodes[2].name="hand";m->nodes[2].parent=1;m->nodes[2].position={0,.5,0};
    auto mesh=std::make_shared<MeshAsset>();mesh->vertices={
        {{0,0,0},{0,0,1},{0,0},{1,0,0,1}},{{.15f,.5f,0},{0,0,1},{1,0},{1,0,0,1}},{{0,1,0},{0,0,1},{0,1},{1,0,0,1}}};
    mesh->indices={0,1,2};mesh->has_uv=true;
    mesh->influences={SkinWeight{{0,0,0,0},{1,0,0,0}},SkinWeight{{1,0,0,0},{1,0,0,0}},SkinWeight{{2,0,0,0},{1,0,0,0}}};m->primitives={mesh};
    auto arm=identity_matrix(),hand=identity_matrix();arm[13]=-.5;hand[13]=-1;
    m->skins.push_back({"three-part bind",0,{0,1,2},{identity_matrix(),arm,hand}});
    m->animations.push_back({"still",2,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,0,0,0},{0,0,0,0}}}}});return m;
}
pr::Configuration profile(bool floor_case=false,bool enabled=true){
    pr::Configuration result;result.profile.root="pelvis";
    for(unsigned n=0;n<3;++n){pr::Part part;part.name=std::array{"pelvis","arm","hand"}[n];part.node=n;part.mass=std::array{4.0,2.0,1.0}[n];part.shape=pr::Sphere{std::array{.12,.1,.08}[n]};part.linear_damping=part.angular_damping=floor_case?.2:0;part.friction=floor_case?1:0;part.restitution=0;result.profile.parts.push_back(part);}
    pr::Joint shoulder;shoulder.name="shoulder";shoulder.parent="pelvis";shoulder.child="arm";shoulder.parent_frame.position={0,.25,0};shoulder.child_frame.position={0,-.25,0};shoulder.limits=pr::SwingTwist{1.2,1.2,-.8,.8};
    auto elbow=shoulder;elbow.name="elbow";elbow.parent="arm";elbow.child="hand";elbow.limits=pr::Hinge{-.8,1.2};
    if(enabled){shoulder.drive=pr::DriveProfile{60,6,10};elbow.drive=pr::DriveProfile{60,6,10};}
    result.profile.joints={shoulder,elbow};return result;
}
std::vector<RuntimeEntityDefinition> actor(std::shared_ptr<const ModelAsset> m,unsigned first,double height,bool floor_case=false,bool enabled=true){
    RuntimeEntityDefinition rig;rig.id=id(first);rig.transform.position={0,height,0};rig.animation_rig=RuntimeAnimationRig{m,0,0,1,false,true,{}};rig.physical_rig=profile(floor_case,enabled);
    std::vector<RuntimeEntityDefinition> result{rig};
    for(unsigned n=0;n<3;++n){RuntimeEntityDefinition bone;bone.id=id(first+1+n);bone.parent=n?id(first+n):rig.id;bone.transform.position={0,n?.5:0,0};bone.rig_node=RuntimeRigNode{rig.id,n};result.push_back(bone);}
    RuntimeEntityDefinition skin;skin.id=id(first+4);skin.parent=id(first+1);skin.mesh=RuntimeMesh{};skin.mesh->mesh=m->primitives[0];skin.skinned_mesh=RuntimeSkinnedMesh{rig.id,0};result.push_back(skin);return result;
}
RuntimeDefinition definition(bool floor_case=false,bool enabled=true,bool dormant_only=false){
    RuntimeDefinition result;result.world_id="native-physical-drive";result.authored_revision=1;
    RuntimeEntityDefinition floor;floor.id=id(2);floor.transform.position={0,-.5,0};floor.transform.scale={40,1,40};floor.collider=BoxCollider{};
    RuntimeEntityDefinition camera;camera.id=id(3);camera.transform.position={0,3,8};camera.camera=RuntimeCamera{};result.entities={floor,camera};
    const auto m=model();if(!dormant_only){auto nodes=actor(m,10,floor_case?1.5:200,floor_case,enabled);result.entities.insert(result.entities.end(),nodes.begin(),nodes.end());}
    RuntimeSpawnTemplate recipe;recipe.id=id(900);recipe.name="three-part driven actor";recipe.root=id(1000);recipe.entities=actor(m,1000,0,floor_case,true);result.templates={recipe};return result;
}
PhysicalRigCommand drive(const std::string& joint,Q target,double strength,bool activate=false,const std::string& rig=id(10)){return {rig,activate,{},PhysicalRigDriveCommand{joint,target,strength}};}
void step(Runtime& r,const std::vector<PhysicalRigCommand>& commands={},std::uint32_t ticks=1,const std::vector<RuntimeStructureTick>& structure={}){r.step(ticks,{}, {},{}, {},structure,{}, {},{}, {},{}, {},{},commands);}
PhysicalRigObservation observed(const Runtime& r,const std::string& rig=id(10)){auto result=r.physical_rig(rig,true,true);check(result.has_value(),"Physical rig is absent.");check(result->parts.size()==3&&result->joints.size()==2,"Physical drive fixture membership changed.");return *result;}
const PhysicalRigJointObservation& joint(const PhysicalRigObservation& rig,const std::string& name){const auto at=std::find_if(rig.joints.begin(),rig.joints.end(),[&](const auto& j){return j.name==name;});check(at!=rig.joints.end(),"Expected joint observation missing.");return *at;}
const PhysicalRigPartObservation& part(const PhysicalRigObservation& rig,const std::string& name){const auto at=std::find_if(rig.parts.begin(),rig.parts.end(),[&](const auto& p){return p.name==name;});check(at!=rig.parts.end(),"Expected part observation missing.");return *at;}
Q relative(const PhysicalRigObservation& rig,const std::string& name){const auto& j=joint(rig,name);return multiply(conjugate(part(rig,j.parent).pose.rotation),part(rig,j.child).pose.rotation);}
V momentum(const PhysicalRigObservation& rig){V p{};for(const auto& b:rig.parts)p=add(p,scale(b.linear_velocity,b.mass));return p;}
void finite_and_capped(const PhysicalRigObservation& rig){
    for(const auto& p:rig.parts){check(p.enrolled,"Physical drive left a part unenrolled.");for(double v:p.pose.position)check(std::isfinite(v),"Drive position is not finite.");for(double v:p.angular_velocity)check(std::isfinite(v),"Drive angular velocity is not finite.");check(!p.at_linear_velocity_limit&&!p.at_angular_velocity_limit,"Drive reached a native velocity safety cap.");}
    for(const auto& j:rig.joints){check(j.drive.has_value(),"Configured drive observation is absent.");check(angle_between(relative(rig,j.name),j.relative_rotation)<1e-6,"Observed joint rotation disagrees with independent body-frame composition.");const auto& d=*j.drive;check(norm(j.evaluated_drive_child_torque)<=d.strength*d.max_torque+1e-10,"Evaluated drive torque exceeds authored strength-scaled norm cap.");check(std::isfinite(j.drive_angular_error)&&j.drive_saturation_scale>=0&&j.drive_saturation_scale<=1,"Drive diagnostic is not finite/bounded.");if(d.strength==0)check(j.evaluated_drive_child_torque==V{},"Zero drive produced nonzero active torque.");}
}
std::vector<Matrix4> palette(const Runtime& r,const std::string& skin=id(14)){const auto scene=r.snapshot(id(3));for(const auto& object:scene.objects)if(object.entity_id==skin){check(object.skin&&object.skin->palette.size()==3,"Drive skin palette missing.");return object.skin->palette;}throw std::runtime_error("Drive skinned drawable missing.");}
void roundtrip_continue(const RuntimeDefinition& d,Runtime& r,const std::string& label,unsigned ticks=8){
    const auto saved=archive(r);artifacts().retain(label+" before validation","accepted-source",saved);Runtime::validate_snapshot(d,content_hash,saved);equal_archive(label+" validation purity",saved,archive(r));
    auto cold=Runtime::from_snapshot(d,content_hash,saved);equal_archive(label+" immediate restore",saved,archive(*cold));
    for(unsigned n=0;n<ticks;++n){step(r);step(*cold);equal_archive(label+" future "+std::to_string(n),archive(r),archive(*cold));check(palette(r)==palette(*cold),"Complete cold drive continuation changed published skin matrices.");}
}

void transitions_and_complete_saves(){
    group="transitions-and-complete-saves";const auto d=definition();Runtime r(d);step(r,{},2);
    auto initial=r.physical_drive(id(10),"shoulder");check(initial&&initial->name=="shoulder"&&initial->strength==0&&initial->target_rotation==Q{0,0,0,1},"Drive did not begin at neutral/zero.");check(initial->stiffness==60&&initial->damping==6&&initial->max_torque==10,"Readback gains differ from frozen authoring.");
    const auto animated=archive(r);rejects([&]{step(r,{drive("shoulder",axis(2,.45),1)});},"Drive implicitly activated an animated rig.");equal_archive("animated refusal",animated,archive(r));
    step(r,{drive("shoulder",axis(2,.45),.5,true),drive("elbow",axis(0,.35),.25)});
    check(r.physical_rig(id(10))->mode==PhysicalRigMode::Driven,"Activation plus drive did not enter Driven.");finite_and_capped(observed(r));
    const auto before_queries=archive(r);(void)r.physical_drive(id(10),"elbow");(void)observed(r);(void)palette(r);equal_archive("inspection purity",before_queries,archive(r));
    roundtrip_continue(d,r,"driven");
    // This admitted non-axis quaternion can move by an ULP when normalized
    // again. Writing the exact committed readback must equal no write at all.
    const Q non_axis{.13101032055011172,.009135762026470012,.13557831134064172,.9820241113412504};
    step(r,{drive("shoulder",non_axis,.5)});
    const auto readback=*r.physical_drive(id(10),"shoulder");
    auto unchanged=Runtime::from_snapshot(d,content_hash,archive(r));
    step(r,{drive("shoulder",readback.target_rotation,readback.strength)});step(*unchanged);
    equal_archive("exact drive readback is a no-op",archive(*unchanged),archive(r));
    const auto at=archive(r);auto cold=Runtime::from_snapshot(d,content_hash,at);
    for(unsigned n=0;n<6;++n){const auto commands=std::vector{drive("shoulder",axis(2,-.2),n<3?.75:0),drive("elbow",axis(0,.2),n<5?.5:0)};step(r,commands);step(*cold,commands);equal_archive("target and strength changes "+std::to_string(n),archive(r),archive(*cold));const auto expected=n<5?PhysicalRigMode::Driven:PhysicalRigMode::Passive;check(r.physical_rig(id(10))->mode==expected,"Mode does not reflect all configured joint strengths.");finite_and_capped(observed(r));}
    const auto disabled=r.physical_drive(id(10),"shoulder");check(disabled&&disabled->strength==0&&angle_between(disabled->target_rotation,axis(2,-.2))<1e-7,"Disable discarded the requested target.");
    roundtrip_continue(d,r,"disabled after drive");
}
void zero_and_convergence(){
    group="zero-and-convergence";const auto d=definition();Runtime r(d);step(r,{},2);step(r,{{id(10),true,{},{}}});
    const auto start=archive(r);auto zero=Runtime::from_snapshot(d,content_hash,start),baseline=Runtime::from_snapshot(d,content_hash,start),driven=Runtime::from_snapshot(d,content_hash,start);
    step(*zero,{drive("shoulder",{0,0,0,1},0),drive("elbow",{0,0,0,1},0)});step(*baseline);equal_archive("zero strength exact passive boundary",archive(*baseline),archive(*zero));
    const Q shoulder_target=axis(2,.45),elbow_target=axis(0,.35);const auto before_skin=palette(*driven);
    step(*driven,{drive("shoulder",shoulder_target,1),drive("elbow",elbow_target,1)});
    double late_driven=0,late_passive=0;
    // Frozen before measurement: 180 ticks, final error <0.15rad and last-second
    // combined error <40% of the passive control. No contact or passive springs.
    for(unsigned n=0;n<180;++n){step(*driven);step(*baseline);const auto active=observed(*driven),passive=observed(*baseline);finite_and_capped(active);
        for(const auto& p:active.parts)check(p.pose.position[1]>100,"Free-fall drive comparison accidentally reached the floor.");
        const auto p=momentum(active);check(std::abs(p[0])<5e-4&&std::abs(p[2])<5e-4,"Internal joint drives injected net horizontal momentum.");
        if(n>=120){late_driven+=angle_between(relative(active,"shoulder"),shoulder_target)+angle_between(relative(active,"elbow"),elbow_target);late_passive+=angle_between(relative(passive,"shoulder"),shoulder_target)+angle_between(relative(passive,"elbow"),elbow_target);}
    }
    const auto final=observed(*driven);check(angle_between(relative(final,"shoulder"),shoulder_target)<.15&&angle_between(relative(final,"elbow"),elbow_target)<.15,"Actual native drive did not converge within the declared 180-tick/0.15rad bound.");check(late_driven<.4*late_passive,"Driven response did not materially outperform the undriven baseline.");check(palette(*driven)!=before_skin,"Solved drive motion did not reach skinned presentation.");
    roundtrip_continue(d,*driven,"converged free fall",4);
}
void perturbation_response(){
    group="perturbation-response";const auto d=definition();Runtime source(d);step(source,{},2);step(source,{{id(10),true,{},{}}});const auto initial=archive(source);auto controlled=Runtime::from_snapshot(d,content_hash,initial),passive=Runtime::from_snapshot(d,content_hash,initial);
    const auto arm=source.physical_part(id(10),"arm");check(arm.has_value(),"Perturbation arm absent.");auto point=arm->pose.position;point[1]+=.1;
    PhysicalRigCommand hit{id(10),false,PhysicalRigImpulse{"arm",point,{.04,0,0}}, {}};
    step(*controlled,{hit,drive("shoulder",{0,0,0,1},1),drive("elbow",{0,0,0,1},1)});step(*passive,{hit});
    double active_error=0,passive_error=0;
    for(unsigned n=0;n<180;++n){step(*controlled);step(*passive);const auto a=observed(*controlled),b=observed(*passive);finite_and_capped(a);
        const auto p=momentum(a);check(std::abs(p[0]-.04)<5e-4&&std::abs(p[2])<5e-4,"Drive changed externally supplied horizontal impulse momentum.");
        if(n>=120){active_error+=angle_between(relative(a,"shoulder"),{0,0,0,1})+angle_between(relative(a,"elbow"),{0,0,0,1});passive_error+=angle_between(relative(b,"shoulder"),{0,0,0,1})+angle_between(relative(b,"elbow"),{0,0,0,1});}
    }
    check(passive_error/60>.005,"Impulse counterfactual did not produce meaningful unpowered joint motion.");check(active_error<.5*passive_error,"Drive did not suppress the same localized perturbation relative to the passive baseline.");
    roundtrip_continue(d,*controlled,"perturbed recovery",4);
}
void rollback_and_malformed_state(){
    group="rollback-and-malformed-state";const auto d=definition();Runtime r(d);step(r,{},2);step(r,{drive("shoulder",axis(2,.2),.5,true)});const auto before=archive(r);
    const std::vector<PhysicalRigCommand> invalid={drive("absent",{0,0,0,1},1),drive("shoulder",{0,0,0,2},1),drive("shoulder",axis(2,1.3),1),drive("elbow",axis(1,.1),1),drive("elbow",axis(0,1.3),1),drive("shoulder",{0,0,0,1},-.1),drive("shoulder",{0,0,0,1},1.1),drive("shoulder",{0,0,0,1},std::numeric_limits<double>::quiet_NaN())};
    for(std::size_t n=0;n<invalid.size();++n){rejects([&]{step(r,{invalid[n]});},"Invalid target/strength/joint accepted.");equal_archive("invalid command "+std::to_string(n),before,archive(r));}
    rejects([&]{step(r,{drive("shoulder",axis(2,.2),.5),drive("shoulder",axis(2,.2),.5)});},"Duplicate joint ownership writes accepted.");equal_archive("duplicate drive rollback",before,archive(r));
    // Actual first-tick drive and physics must roll back when a later scheduled
    // structural revision is stale. Do not replace this with parser rejection.
    RuntimeTransform at;at.position={5,200,0};
    rejects([&]{step(r,{drive("shoulder",axis(2,-.3),1)},4,{{2,1,{{id(900),at}}, {}}});},"Late structural conflict accepted.");equal_archive("late batch rollback",before,archive(r));
    const auto unconfigured=definition(false,false);Runtime no_drive(unconfigured);step(no_drive,{},2);step(no_drive,{{id(10),true,{},{}}});const auto no_permission=archive(no_drive);
    check(!no_drive.physical_drive(id(10),"shoulder"),"Unconfigured joint acquired drive permission.");rejects([&]{step(no_drive,{drive("shoulder",axis(2,.2),1)});},"Absent authored drive permission accepted.");equal_archive("unconfigured rejection",no_permission,archive(no_drive));
    for(unsigned mutation=0;mutation<8;++mutation){auto bad=Json::parse(before);auto& membership=bad["payload"]["physical"]["rigs"][0]["membership"];
        if(mutation==0)membership.erase("drives");
        if(mutation==1)membership["drives"][0]["strength"]=1.1;
        if(mutation==2)membership["drives"][0]["strength"]="0.5";
        if(mutation==3)membership["drives"][0]["target_rotation"]={0,0,0,2};
        if(mutation==4)membership["drives"][0]["joint"]="unknown";
        if(mutation==5)std::swap(membership["drives"][0],membership["drives"][1]);
        if(mutation==6)membership["mode"]="passive";
        if(mutation==7)membership["drives"][0]["stiffness"]=60;
        const auto payload=bad.at("payload").dump();bad["sha256"]=sha256(std::as_bytes(std::span(payload.data(),payload.size())));const auto bytes=bad.dump();artifacts().retain("resealed drive mutation "+std::to_string(mutation),"negative",bytes);
        rejects([&]{(void)Runtime::from_snapshot(d,content_hash,bytes);},"Malformed resealed drive state restored.");equal_archive("negative archive source purity "+std::to_string(mutation),before,archive(r));
    }
}
void contacts_sleep_and_wake(){
    group="contacts-sleep-and-wake";const auto d=definition(true);Runtime r(d);step(r,{},2);step(r,{drive("shoulder",axis(2,.2),.5,true),drive("elbow",axis(0,.3),.5)});step(r,{},30);step(r,{drive("shoulder",{0,0,0,1},0),drive("elbow",{0,0,0,1},0)});
    bool sleeping=false,contact=false;
    // Bounded settling cohort, fixed before execution; no forced sleep or edits
    // to solver records. Strong body/contact damping belongs to this fixture.
    for(unsigned n=0;n<50;++n){step(r,{},30);const auto current=observed(r);finite_and_capped(current);const auto saved=Json::parse(archive(r));const auto& contact_pairs=saved.at("payload").at("physical").at("physics").at("pairs");contact=contact||!contact_pairs.empty();sleeping=std::all_of(current.parts.begin(),current.parts.end(),[](const auto& p){return p.sleeping;});if(sleeping)break;}
    check(contact,"Settling fixture never recorded an actual contact pair.");check(sleeping,"Released physical rig did not naturally sleep within 1500 settling ticks.");
    const auto saved=archive(r);auto cold=Runtime::from_snapshot(d,content_hash,saved);equal_archive("sleep immediate restore",saved,archive(*cold));
    const auto read_before=archive(r);(void)r.physical_drive(id(10),"shoulder");(void)observed(r);equal_archive("sleep read purity",read_before,archive(r));
    const auto zero_target=drive("shoulder",axis(2,-.1),0);step(r,{zero_target});step(*cold,{zero_target});equal_archive("sleep zero target retained",archive(r),archive(*cold));
    const auto still_sleeping=observed(r);check(std::all_of(still_sleeping.parts.begin(),still_sleeping.parts.end(),[](const auto& p){return p.sleeping;}),"Zero-strength target-only update woke a passive rig.");
    step(r,{drive("shoulder",axis(2,.3),.5)});step(*cold,{drive("shoulder",axis(2,.3),.5)});equal_archive("positive drive wake",archive(r),archive(*cold));
    const auto awake=observed(r);check(awake.mode==PhysicalRigMode::Driven&&std::any_of(awake.parts.begin(),awake.parts.end(),[](const auto& p){return p.active;}),"Changed positive drive did not wake the physical rig.");
    roundtrip_continue(d,r,"ground contact driven",8);
}
void dormant_template_lifecycle(){
    group="dormant-template-lifecycle";const auto d=definition(false,true,true);Runtime r(d);const auto empty=archive(r);
    check(Json::parse(empty).at("payload").at("physical").at("version")==5,"Dormant drive recipe did not select the complete drive save contract.");auto cold=Runtime::from_snapshot(d,content_hash,empty);equal_archive("dormant recipe restore",empty,archive(*cold));
    RuntimeTransform at;at.position={5,200,0};const auto born=r.change_structure(0,{{id(900),at}},{}).spawned.at(0);const auto mirrored=cold->change_structure(0,{{id(900),at}},{}).spawned.at(0);check(born==mirrored,"Drive recipe restore changed public allocation frontier.");
    step(r,{},2);step(*cold,{},2);const auto request=drive("elbow",axis(0,.3),.5,true,born);step(r,{request});step(*cold,{request});equal_archive("spawned driven instance",archive(r),archive(*cold));check(r.physical_rig(born)->mode==PhysicalRigMode::Driven,"Spawned drive did not become active.");
    for(unsigned n=0;n<5;++n){step(r);step(*cold);equal_archive("spawned continuation "+std::to_string(n),archive(r),archive(*cold));}
    const auto before_remove=archive(r);r.change_structure(r.structure_revision(),{}, {born});cold->change_structure(cold->structure_revision(),{}, {born});equal_archive("retired driven instance",archive(r),archive(*cold));check(archive(r)!=before_remove,"Retiring a driven instance did not change full state.");rejects([&]{(void)r.physical_drive(born,"elbow");},"Retired joint still inspectable.");
    const auto retired=archive(r);auto restored=Runtime::from_snapshot(d,content_hash,retired);equal_archive("retired-drive restore",retired,archive(*restored));const auto next=r.change_structure(r.structure_revision(),{{id(900),at}},{}).spawned.at(0);const auto next_cold=restored->change_structure(restored->structure_revision(),{{id(900),at}},{}).spawned.at(0);check(next==next_cold&&next!=born,"Retired drive allocation frontier was reused or diverged.");equal_archive("next incarnation",archive(r),archive(*restored));
}
}
int main(){
    try{check(Runtime::available(),"Native simulation is required.");transitions_and_complete_saves();zero_and_convergence();perturbation_response();rollback_and_malformed_state();contacts_sleep_and_wake();dormant_template_lifecycle();std::cout<<"Runtime physical drives: 6 native groups, "<<checks<<" checks, "<<pairs<<" complete archive pairs passed; no balance/recovery claim.\n";return 0;}
    catch(const std::exception& error){std::cerr<<"Runtime physical drive group "<<group<<": "<<error.what()<<'\n';return 1;}
}
