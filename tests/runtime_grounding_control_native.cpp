// SPDX-License-Identifier: Apache-2.0
// Requested contact multipliers, actual physics and complete saved-state pairs.
#include "poima/runtime.hpp"
#include "poima/runtime_animation.hpp"
#include "poima/runtime_grounding.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
namespace {
using Json=nlohmann::json;
const std::string hash(64,'d');std::filesystem::path output;unsigned archive_count=0;bool unsettled_prop=false;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::string id(unsigned n){const auto text=std::to_string(n);return std::string(32-text.size(),'0')+text;}
void near(double a,double b,double error=2e-6){require(std::isfinite(a)&&std::abs(a-b)<=error,"Independent grounding scalar differs.");}
template<class F>void rejects(F&& f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}require(failed,"Expected original native rejection absent.");}
std::string retain(const std::string& label,const std::string& bytes){
    if(!output.empty()){std::ofstream file(output/(std::to_string(++archive_count)+"-"+label+".json"),std::ios::binary);require(bool(file),"Cannot retain complete native save.");file.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));file.close();require(bool(file),"Cannot close complete native save.");}return bytes;
}
RuntimeDefinition definition(bool actor=false){
    RuntimeDefinition d;d.world_id="grounding-control-native";d.authored_revision=9;
    auto model=std::make_shared<ModelAsset>();model->roots={0};model->nodes.resize(3);
    model->nodes[0].name="Hip";model->nodes[0].position={0,1.8,0};
    model->nodes[1].name="Knee";model->nodes[1].parent=0;model->nodes[1].position={.5,-.7,0};
    model->nodes[2].name="Ankle";model->nodes[2].parent=1;model->nodes[2].position={-.5,-.7,0};
    model->animations.push_back({"Late invalid affine frame",1./32,{{0,AnimationPath::scale,AnimationInterpolation::step,{0,1./64,1./32},{{1,1,1,0},{1,1,1,0},{1.01,1,1,0}}}}});
    RuntimeEntityDefinition rig;rig.id=id(1);rig.animation_rig=RuntimeAnimationRig{model};
    RuntimeTwoBoneIk chain;chain.slot=1;chain.root=0;chain.middle=1;chain.end=2;chain.space=IkSpace::Rig;chain.target={0,.4,0};chain.pole={1,1,0};chain.metric_policy=RuntimeIkMetricPolicy::NearUniformV1;
    chain.grounding=RuntimeGroundingDefinition{};chain.grounding->sole_offset={0,-.2,0};chain.grounding->reference_height=.2;chain.grounding->max_correction=.6;
    rig.animation_rig->ik={chain};if(actor)rig.parent=id(300);d.entities.push_back(rig);
    for(unsigned n=0;n<3;++n){RuntimeEntityDefinition node;node.id=id(10+n);node.parent=n?id(9+n):id(1);node.transform={model->nodes[n].position,model->nodes[n].rotation,model->nodes[n].scale};node.rig_node=RuntimeRigNode{id(1),n};d.entities.push_back(node);}
    RuntimeEntityDefinition floor;floor.id=id(100);floor.transform.position={0,-.5,0};floor.collider=BoxCollider{};floor.collider->half_extents={10,.5,10};d.entities.push_back(floor);
    if(actor){RuntimeEntityDefinition player;player.id=id(300);player.transform.position={0,.02,0};player.character=CharacterController{};player.character->radius=.25f;player.character->height=1.6f;player.character->jump_speed=5;d.entities.push_back(player);}
    RuntimeEntityDefinition body;body.id=id(200);body.transform.position={4,5,0};body.collider=BoxCollider{};body.collider->motion=BodyMotion::Dynamic;d.entities.push_back(body);return d;
}
RuntimeGroundingCommand command(bool enabled=true,double weight=1){return {id(1),1,enabled,weight};}
void advance(Runtime& runtime,std::uint32_t ticks,const std::vector<RuntimeGroundingCommand>& controls={},const std::vector<RuntimeInput>& inputs={},const std::vector<AnimationCommand>& animations={}){
    runtime.step(ticks,inputs,{}, {},animations,{}, {},{}, {},{},controls);
}
RuntimeGroundingState state(Runtime& runtime){const auto s=runtime.ik(id(1),1);require(s&&s->grounding,"Automatic foot state absent.");return *s->grounding;}
void requested(Runtime& runtime,bool enabled,double weight){auto c=runtime.grounding(id(1),1);require(c&&c->entity==id(1)&&c->slot==1&&c->enabled==enabled,"Requested multiplier identity differs.");near(c->weight,weight,0);}
void passive_serialization_and_guards(){
    const auto d=definition();RuntimeAnimations animation(d);
    auto world=identity_matrix();unsigned queries=0;grounding::Query query=[&](const std::string&,const RuntimeRay& ray)->std::optional<RuntimeRayHit>{++queries;RuntimeRayHit hit;hit.entity=id(100);hit.position={ray.origin[0],0,ray.origin[2]};hit.normal=std::array<double,3>{0,1,0};return hit;};
    auto wrapper=[&](const std::string&)->const Matrix4&{return world;};
    animation.sample(0,wrapper);require(queries==0,"Initial queryless sample probed supports.");
    const auto legacy=retain("identity-animation-v7",animation.save_state(0));require(Json::parse(legacy)["version"]==7,"Identity control changed old grounding version.");
    auto original=animation.grounding_control(id(1),1);require(original&&original->enabled&&original->weight==1,"Default requested control differs.");
    // Deliberately invalid input copied from the untouched initial v7 save:
    // nonidentity policy has never produced a final grounding decision.
    auto initial_v8=Json::parse(legacy);initial_v8["version"]=8;initial_v8["rigs"][0]["ik"][0]["grounding"]["control"]={{"enabled",false},{"weight",.375}};
    const auto invalid_initial=retain("invalid-uninitialized-v8-nonidentity",initial_v8.dump());rejects([&]{animation.load_state(invalid_initial,0);});
    const auto retained_control=animation.grounding_control(id(1),1);require(retain("after-invalid-uninitialized-v8",animation.save_state(0))==legacy&&retained_control&&retained_control->enabled&&retained_control->weight==1&&queries==0&&animation.ik_state(id(1),1)->grounding->casts==0&&animation.ik_state(id(1),1)->grounding->status=="uninitialized","Uninitialized nonidentity v8 rejection changed whole state/policy or queried.");
    animation.apply_grounding({command(false,.375)});rejects([&]{(void)animation.save_state(0);});
    require(animation.ik_state(id(1),1)->grounding->casts==0&&queries==0,"Setter performed a support query.");
    animation.sample(0,wrapper);rejects([&]{(void)animation.save_state(0);});
    animation.sample(1,wrapper,query);require(queries==0,"Disabled final decision queried support.");
    const auto changed=retain("requested-animation-v8",animation.save_state(1));auto value=Json::parse(changed);require(value["version"]==8,"Nonidentity control requires animation v8.");
    require(value["rigs"][0]["ik"][0]["grounding"]["control"]==Json{{"enabled",false},{"weight",.375}},"Complete v8 requested control absent.");
    require(animation.ik_state(id(1),1)->grounding->status=="disabled"&&animation.ik_state(id(1),1)->grounding->casts==0,"Disabled final decision state differs.");
    for(unsigned i=0;i<4;++i){animation.apply_grounding({command(false,.375)});(void)animation.grounding_control(id(1),1);require(animation.save_state(1)==changed&&queries==0,"Same-value write/getter changed full state or queried.");}
    auto invalid=command();invalid.weight=std::numeric_limits<double>::quiet_NaN();rejects([&]{animation.apply_grounding({invalid});});
    for(const auto weight:{-1.,1.001,std::numeric_limits<double>::infinity()}){invalid=command(false,weight);rejects([&]{animation.apply_grounding({invalid});});}
    for(const auto slot:{0u,9u,2u}){invalid=command();invalid.slot=slot;rejects([&]{animation.apply_grounding({invalid});});}
    invalid=command();invalid.entity=id(100);rejects([&]{animation.apply_grounding({command(),invalid});});rejects([&]{animation.apply_grounding({command(),command()});});
    std::vector<RuntimeGroundingCommand> excess(65,command());rejects([&]{animation.apply_grounding(excess);});require(animation.save_state(1)==changed,"Rejected staged controls mutated requested state.");
    auto bad=Json::parse(changed);bad["rigs"][0]["ik"][0]["grounding"]["control"]["weight"]=-1;rejects([&]{animation.load_state(bad.dump(),1);});require(animation.save_state(1)==changed,"Malformed v8 load changed state.");
    for(unsigned kind=0;kind<3;++kind){bad=Json::parse(changed);auto& control=bad["rigs"][0]["ik"][0]["grounding"]["control"];if(kind==0)control.erase("enabled");if(kind==1)control["extra"]=0;if(kind==2)control["weight"]=true;rejects([&]{animation.load_state(bad.dump(),1);});require(animation.save_state(1)==changed,"Closed/typed v8 control rejection changed full state.");}
    bad=Json::parse(legacy);bad["version"]=8;bad["rigs"][0]["ik"][0]["grounding"]["control"]={{"enabled",true},{"weight",1}};rejects([&]{animation.load_state(bad.dump(),0);});require(animation.save_state(1)==changed,"Noncanonical allidentity v8 load changed full state.");
    animation.apply_grounding({command(true,.5)});animation.sample(2,wrapper,query);require(queries==1,"Half-weight final sample did not query exactly once.");
    animation.apply_grounding({command()});rejects([&]{(void)animation.save_state(2);});animation.sample(2,wrapper);rejects([&]{(void)animation.save_state(2);});
    animation.sample(3,wrapper,query);require(Json::parse(animation.save_state(3))["version"]==7&&queries==2,"Committed identity reset did not return to v7.");
    RuntimeAnimations cold(d);cold.load_state(changed,1);cold.sample(1,wrapper);require(cold.save_state(1)==changed,"Cold v8 changed complete animation bytes.");
    cold.load_state(legacy,0);cold.sample(0,wrapper);require(cold.grounding_control(id(1),1)->enabled&&cold.grounding_control(id(1),1)->weight==1&&cold.save_state(0)==legacy,"Old v7 load did not reset requested identity.");
    auto manual=d;manual.entities[0].animation_rig->ik[0].grounding.reset();RuntimeAnimations point_only(manual);require(!point_only.grounding_control(id(1),1),"Manual slot fabricated automatic control.");rejects([&]{point_only.apply_grounding({command()});});
}
void authored_ceiling_without_query(){
    for(const auto enabled:{false,true}){auto d=definition();auto& chain=d.entities[0].animation_rig->ik[0];chain.enabled=enabled;chain.weight=.4;RuntimeAnimations animation(d);animation.apply_grounding({command(true,.5)});unsigned casts=0;auto world=identity_matrix();
        grounding::Query query=[&](const std::string&,const RuntimeRay& ray)->std::optional<RuntimeRayHit>{++casts;RuntimeRayHit hit;hit.entity=id(100);hit.position={ray.origin[0],0,ray.origin[2]};hit.normal=std::array<double,3>{0,1,0};return hit;};
        animation.sample(1,[&](const std::string&)->const Matrix4&{return world;},query);const auto s=animation.ik_state(id(1),1);near(s->grounding->weight,enabled?.2:0,2e-12);require(casts==(enabled?1u:0u),"Requested enable escaped authored enable ceiling.");
        const auto c=animation.grounding_control(id(1),1);require(c->enabled&&c->weight==.5,"Effective contact overwrote requested multiplier.");}
}
void contact_controls_ownership_and_cold(){
    const auto d=definition();Runtime runtime(d);advance(runtime,1);requested(runtime,true,1);near(state(runtime).weight,1,0);require(state(runtime).status=="contact","Actual initial contact absent.");
    const auto identity=retain("native-default-v7",runtime.save_snapshot(hash));const auto count=state(runtime).casts;advance(runtime,1,{command(false,.7)});requested(runtime,false,.7);require(state(runtime).status=="disabled"&&state(runtime).casts==count,"Disabled control queried or retained active correction.");
    const auto disabled=retain("native-disabled-v8",runtime.save_snapshot(hash));for(unsigned i=0;i<3;++i){requested(runtime,false,.7);(void)runtime.ik(id(1),1);require(runtime.save_snapshot(hash)==disabled,"Passive getter changed complete native state.");}
    RuntimeIkCommand manual{id(1),1,true,IkSpace::Rig,{0,.2,0},{1,1,0},1};rejects([&]{runtime.step(1,{}, {},{}, {},{}, {manual});});require(runtime.save_snapshot(hash)==disabled,"Disabled automatic slot relinquished ownership or changed state.");
    advance(runtime,1,{command(true,.5)});near(state(runtime).weight,.5,2e-12);near(runtime.ik(id(1),1)->end_orientation->goal.weight,.5,2e-12);requested(runtime,true,.5);
    const auto source=retain("native-fade-source",runtime.save_snapshot(hash));auto cold=Runtime::from_snapshot(d,hash,source);require(retain("native-fade-cold",cold->save_snapshot(hash))==source,"V8 cold reconstruction changed complete state.");
    advance(runtime,3);advance(*cold,3);require(retain("native-fade-future",runtime.save_snapshot(hash))==retain("native-fade-cold-future",cold->save_snapshot(hash)),"V8 cold continuation differs.");
    advance(runtime,1,{command()});requested(runtime,true,1);near(state(runtime).weight,1,0);
    auto legacy=Runtime::from_snapshot(d,hash,identity);requested(*legacy,true,1);require(legacy->save_snapshot(hash)==identity,"Retained old v7 snapshot changed.");
}
void real_jump_and_landing(){
    const auto d=definition(true);Runtime runtime(d);advance(runtime,unsettled_prop?60:120);const auto landed=runtime.entity(id(300));require(landed.ground=="on_ground","Real character failed to land before jump.");const auto y=landed.world[13];
    // The default is a distinct prospective cohort. Retain EVERY field and
    // require the actual unrelated prop to be asleep before the jump branch;
    // portable saves do not retain general active-contact solver history.
    const auto before_jump=retain(unsettled_prop?"pre-jump-original-unsettled":"pre-jump-observed-settled",runtime.save_snapshot(hash));
    if(!unsettled_prop){const auto saved=Json::parse(before_jump);const auto& rows=saved.at("payload").at("entities");const auto found=std::find_if(rows.begin(),rows.end(),[](const auto& row){return row.at("id")==id(200);});require(found!=rows.end(),"Complete pre-jump prop missing.");const auto& body=found->at("body");require(body.at("motion")=="dynamic"&&body.at("active").is_boolean()&&!body.at("active").get<bool>(),"Fixed warmup did not produce an observed inactive prop.");for(const auto* field:{"linear_velocity","angular_velocity"}){const auto& values=body.at(field);require(values.is_array()&&values.size()==3,"Complete pre-jump body velocity missing.");for(const auto& v:values)require(v.is_number()&&v.get<double>()==0,"Inactive prop must have exactly zero pre-jump velocities.");}}
    RuntimeInput jump;jump.entity=id(300);jump.jump=true;advance(runtime,1,{command(false,1)},{jump});require(runtime.entity(id(300)).velocity[1]>0,"Actual controller Jump produced no upward velocity.");advance(runtime,9);require(runtime.entity(id(300)).world[13]>y+.1&&runtime.entity(id(300)).ground!="on_ground","Real controller did not become airborne.");requested(runtime,false,1);require(state(runtime).status=="disabled","Airborne control was not retained.");
    const auto airborne=retain("actual-airborne",runtime.save_snapshot(hash));auto cold=Runtime::from_snapshot(d,hash,airborne);require(cold->save_snapshot(hash)==airborne,"Airborne cold reconstruction differs.");advance(runtime,100);advance(*cold,100);require(retain("landed-warm",runtime.save_snapshot(hash))==retain("landed-cold",cold->save_snapshot(hash)),"Character/contact controls changed cold landing.");require(runtime.entity(id(300)).ground=="on_ground","Character failed to land under native physics.");
    advance(runtime,1,{command(true,.25)});near(state(runtime).weight,.25,2e-12);advance(runtime,1,{command()});require(state(runtime).status=="contact"&&state(runtime).weight==1,"Landing did not restore full contact.");
}
void later_failure_restores_full_batch(){
    const auto d=definition();Runtime runtime(d),first(d);const auto original=retain("before-two-tick-failure",runtime.save_snapshot(hash));const auto body=runtime.entity(id(200)).world;AnimationCommand invalid{id(1),0,0,1,false,true};
    advance(first,1,{command(true,.5)}, {},{invalid});requested(first,true,.5);require(state(first).casts==1&&first.entity(id(200)).world!=body,"Accepted first Tick did not exercise control/contact/physics.");
    rejects([&]{advance(runtime,2,{command(true,.5)}, {},{invalid});});requested(runtime,true,1);require(retain("after-two-tick-failure",runtime.save_snapshot(hash))==original&&runtime.entity(id(200)).world==body&&state(runtime).casts==0,"Later failure lost complete physics/control/cache/history rollback.");
}
}
int main(int argc,char** argv){try{
    for(int i=1;i<argc;++i){const std::string arg=argv[i];if(arg=="--unsettled-prop"){require(!unsettled_prop,"Duplicate --unsettled-prop.");unsettled_prop=true;}else if(arg=="--output"){require(output.empty()&&i+1<argc,"Use --output FRESH once.");output=std::filesystem::absolute(argv[++i]);}else require(false,"Use optional --output FRESH and --unsettled-prop.");}
    if(!output.empty()){require(!std::filesystem::exists(output)&&std::filesystem::is_directory(output.parent_path()),"Output must be fresh with existing parent.");require(std::filesystem::create_directory(output),"Cannot create retained-checkpoint directory.");}
    std::cout<<(unsettled_prop?"Retained original60-tick unsettled-prop cohort; general future-contact equality remains unqualified.\n":"Distinct120-tick cohort; requires observed inactive prop with exact zero velocities before Jump, with every saved field retained.\n");
    passive_serialization_and_guards();authored_ceiling_without_query();
    if(!Runtime::available()){std::cout<<"2 portable grounding-control groups passed; native physics unavailable\n";return 0;}
    contact_controls_ownership_and_cold();real_jump_and_landing();later_failure_restores_full_batch();std::cout<<"5 native grounding-control groups passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
