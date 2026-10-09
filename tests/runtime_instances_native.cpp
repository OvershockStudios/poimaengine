// SPDX-License-Identifier: Apache-2.0
// Original analytic graph: independent native ownership, motion and provenance.
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace poima;
namespace {
using Json=nlohmann::json;
const std::string hash(64,'a'),type(32,'d');
std::string local(unsigned n) {return std::string(31,'a')+std::to_string(n);}
std::string field(unsigned n) {return std::string(31,'b')+std::to_string(n);}
std::string authored(unsigned n) {return std::string(31,'0')+std::to_string(n);}
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void near(double a,double b,double epsilon=1e-6) {check(std::isfinite(a) && std::abs(a-b)<epsilon,"Independent instance motion oracle differs.");}
template<class F>void rejects(F&& f) {bool failed=false;try {f();}catch(const std::exception&){failed=true;}check(failed,"Invalid instance operation succeeded.");}
components::Schema schema() {
    const Json fields=Json::array({{{"id",field(1)},{"name","Target"},{"kind","entity"},{"default",std::string(32,'0')}},
        {{"id",field(2)},{"name","Rig"},{"kind","entity"},{"default",std::string(32,'0')}},
        {{"id",field(3)},{"name","Members"},{"kind","array"},{"element_kind","entity"},{"capacity",2},{"default",Json::array()}}});
    return components::parse_schema(Json{{"id",type},{"name","Instance links"},{"version",2},{"fields",fields}}.dump());
}
std::shared_ptr<ModelAsset> model() {
    auto result=std::make_shared<ModelAsset>();result->roots={0};result->nodes.resize(1);
    result->nodes[0].name="Original joint";result->nodes[0].skin=0;result->nodes[0].primitives={0};
    auto mesh=std::make_shared<MeshAsset>();mesh->vertices={
        {{0,0,0},{0,0,1},{0,0},{1,0,0,1}},{{1,0,0},{0,0,1},{1,0},{1,0,0,1}},{{0,1,0},{0,0,1},{0,1},{1,0,0,1}}};
    mesh->indices={0,1,2};mesh->has_uv=true;mesh->influences.assign(3,SkinWeight{{0,0,0,0},{1,0,0,0}});
    result->primitives={mesh};result->skins.push_back({"Original bind",0,{0},{identity_matrix()}});
    result->animations.push_back({"Original translation",2,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,0,0,0},{2,0,0,0}}}}});
    return result;
}
RuntimeDefinition definition() {
    RuntimeDefinition result;result.world_id="native-instance-oracle";result.authored_revision=8;result.component_schemas={schema()};
    RuntimeEntityDefinition floor;floor.id=authored(1);floor.transform.position={0,-.5,0};floor.transform.scale={40,1,40};floor.collider=BoxCollider{};
    RuntimeEntityDefinition observer;observer.id=authored(9);observer.camera=RuntimeCamera{};observer.transform.position={0,3,10};
    result.entities={floor,observer};
    RuntimeSpawnTemplate recipe;recipe.id=std::string(32,'c');recipe.name="Original complete actor";recipe.root=local(1);
    RuntimeEntityDefinition root;root.id=local(1);root.character=CharacterController{};root.character->camera=local(2);
    root.components[type]=components::parse_values(result.component_schemas[0],Json{{field(1),authored(9)},{field(2),local(3)},
        {field(3),Json::array({local(1),local(3)})}}.dump());
    RuntimeEntityDefinition camera;camera.id=local(2);camera.parent=root.id;camera.camera=RuntimeCamera{};camera.transform.position={0,1.6,0};
    const auto asset=model();RuntimeEntityDefinition rig;rig.id=local(3);rig.parent=root.id;
    rig.animation_rig=RuntimeAnimationRig{asset,0,0,1,true,true,{}};
    RuntimeEntityDefinition joint;joint.id=local(4);joint.parent=rig.id;joint.rig_node=RuntimeRigNode{rig.id,0};
    RuntimeEntityDefinition skin;skin.id=local(5);skin.parent=joint.id;skin.mesh=RuntimeMesh{};skin.mesh->mesh=asset->primitives[0];skin.skinned_mesh=RuntimeSkinnedMesh{rig.id,0};
    RuntimeEntityDefinition audio;audio.id=local(6);audio.parent=root.id;auto clip=std::make_shared<AudioClip>();clip->samples.assign(1600,.125f);
    audio.emitter=AudioEmitter{"original-test-tone",clip,1,true,true};
    RuntimeEntityDefinition light;light.id=local(7);light.parent=root.id;light.light=Light{};
    RuntimeEntityDefinition acoustic;acoustic.id=local(8);acoustic.parent=root.id;acoustic.mesh=RuntimeMesh{};acoustic.acoustics=AcousticMaterial{};
    recipe.entities={skin,light,root,joint,acoustic,camera,audio,rig};result.templates={recipe};return result;
}
RuntimeTransform at(double x) {RuntimeTransform result;result.position={x,0,0};return result;}
std::string birth(Runtime& runtime,const RuntimeDefinition& d,double x) {return runtime.change_structure(runtime.structure_revision(),{{d.templates[0].id,at(x)}},{}).spawned.at(0);}
Json links(const Runtime& runtime,const std::string& root) {return Json::parse(components::values_json(runtime.component_schemas()[0],*runtime.component_read(type,root)));}
void membership_and_motion() {
    const auto d=definition();Runtime runtime(d);runtime.step(30,{});
    const auto first=birth(runtime,d,-4);const auto second=birth(runtime,d,4);
    const auto a=runtime.instance(first),b=runtime.instance(second);
    check(a.nodes.size()==8 && a.root==first && a.template_id==d.templates[0].id && a.initial.position[0]==-4,"Complete provenance missing.");
    check(runtime.inspect().entities==18 && runtime.inspect().characters==2 && runtime.inspect().bodies==3,"Complete native membership differs.");
    check(runtime.player_controllers()==std::vector<std::pair<std::string,std::string>>{{first,a.nodes.at(local(2))},{second,b.nodes.at(local(2))}},"Current player selectors differ.");
    check(runtime.camera_ids().size()==3 && runtime.is_player_controller(first) && !runtime.is_player_controller(a.nodes.at(local(2))),"Current camera/controller selectors differ.");
    check(runtime.live_definition().entities.size()==18,"Live baseline metadata omitted members.");
    for(const auto& instance:{a,b}) {
        const auto values=links(runtime,instance.root);
        check(values.at(field(1))==authored(9) && values.at(field(2))==instance.nodes.at(local(3)) &&
            values.at(field(3))==Json::array({instance.root,instance.nodes.at(local(3))}),"Local or external component references were remapped incorrectly.");
        check(runtime.entity(instance.nodes.at(local(2))).id==instance.nodes.at(local(2)),"Mapped camera absent.");
        near(runtime.animation(instance.nodes.at(local(3)))->time,0);
    }
    const auto audio=runtime.audio_snapshot(authored(9));check(audio.sources.size()==2 && audio.geometry.size()==2,"Current audio membership omitted children.");
    RuntimeInput move;move.entity=first;move.move={0,.5f};
    AnimationCommand fast;fast.entity=b.nodes.at(local(3));fast.clip=0;fast.playing=true;fast.loop=true;fast.speed=2;
    runtime.step(30,{move},{},{},{fast});
    near(runtime.animation(a.nodes.at(local(3)))->time,.5);near(runtime.animation(b.nodes.at(local(3)))->time,1);
    near(runtime.entity(a.nodes.at(local(4))).local.position[0],.5);near(runtime.entity(b.nodes.at(local(4))).local.position[0],1);
    check(runtime.entity(first).local.position[2]<-.5 && runtime.entity(second).local.position[2]==0,"Controllers did not move independently along local -Z.");
    const auto scene=runtime.snapshot(authored(9));std::size_t skins=0;for(const auto& object:scene.objects)if(object.skin)++skins;
    check(skins==2,"Runtime renderer observation omitted instanced skin palettes.");
    rejects([&]{runtime.instance_node(first,local(9));});rejects([&]{runtime.instance(a.nodes.at(local(3)));});
    // Native callers author the tree root once; legacy mirror fields do not
    // replace its default transform when no spawn override is supplied.
    auto positioned=d;
    for(auto& member:positioned.templates[0].entities)if(member.id==local(1))member.transform.position={7,0,3};
    Runtime defaults(positioned);
    const auto default_root=defaults.change_structure(0,{{positioned.templates[0].id,std::nullopt}},{}).spawned.at(0);
    check(defaults.entity(default_root).local.position==std::array<double,3>{7,0,3} &&
        defaults.instance(default_root).initial.position==std::array<double,3>{7,0,3},"Hierarchical default root transform was replaced by legacy mirror values.");
}
void atomic_failure_and_reuse() {
    auto d=definition();auto invalid=d.templates[0];invalid.id=std::string(32,'e');
    invalid.entities[2].components[type]=components::parse_values(d.component_schemas[0],Json{{field(1),std::string(32,'f')},{field(2),local(3)},{field(3),Json::array({local(1),local(3)})}}.dump());
    d.templates.push_back(invalid);Runtime failed(d),control(d);const auto before=failed.save_snapshot(hash);
    rejects([&]{failed.change_structure(0,{{d.templates[0].id,at(-4)},{invalid.id,at(4)}},{});});
    check(failed.save_snapshot(hash)==before,"Post-character component rejection changed complete state.");
    const auto first=birth(failed,d,-4),other=birth(control,d,-4);check(first==other,"Failed detached character admission consumed public IDs.");
    RuntimeInput move;move.entity=first;move.move={0,.5f};failed.step(24,{move});control.step(24,{move});
    check(failed.save_snapshot(hash)==control.save_snapshot(hash),"Failed character allocation changed later physics or animation.");
    const auto stable=failed.save_snapshot(hash);
    // A full birth and its retirement occur before a later stale edit; the
    // entire batch must restore original owners, clock/history and allocator.
    const auto next=control.change_structure(1,{{d.templates[0].id,at(4)}},{}).spawned[0];
    rejects([&]{failed.step(4,{}, {}, {}, {},{{0,1,{{d.templates[0].id,at(4)}},{}},{1,2,{}, {next}},{3,0,{{d.templates[0].id,at(6)}},{}}});});
    check(failed.save_snapshot(hash)==stable,"Later failed batch retained hierarchical birth/despawn state.");
    const auto resumed=birth(failed,d,4);check(resumed==next,"Failed hierarchical batch consumed node identities.");
}
void removal_and_survivors() {
    const auto d=definition();Runtime runtime(d);const auto first=birth(runtime,d,-4),second=birth(runtime,d,4);
    const auto a=runtime.instance(first),b=runtime.instance(second);
    auto value=links(runtime,second);value[field(1)]=a.nodes.at(local(4));runtime.component_edit(type,second,components::parse_values(d.component_schemas[0],value.dump()));
    runtime.step(17,{}, {},{{false,a.nodes.at(local(6)),0,1},{false,b.nodes.at(local(6)),0,1}});
    const auto before=runtime.save_snapshot(hash);rejects([&]{runtime.change_structure(2,{}, {first});});check(runtime.save_snapshot(hash)==before,"Incoming child reference did not protect whole-instance removal.");
    value[field(1)]=authored(9);runtime.component_edit(type,second,components::parse_values(d.component_schemas[0],value.dump()));
    const auto rig=runtime.animation(b.nodes.at(local(3)));const auto pose=runtime.entity(second);const auto voice=runtime.sound_state().voices().at(1).id;
    runtime.change_structure(2,{}, {first});
    check(runtime.inspect().entities==10 && runtime.inspect().characters==1 && runtime.inspect().bodies==2,"Whole removal retained native owners.");
    for(const auto& [local_id,id]:a.nodes) {(void)local_id;rejects([&]{runtime.entity(id);});}
    check(runtime.animation(b.nodes.at(local(3)))->time==rig->time && runtime.entity(second).world==pose.world,"Removal reset surviving clock or character.");
    check(runtime.sound_state().voices().size()==1 && runtime.sound_state().voices()[0].id==voice && runtime.sound_state().next_id()==3,"Emitter retirement changed surviving voice or allocator.");
    check(runtime.audio_snapshot(authored(9)).sources.size()==1 && runtime.audio_snapshot(authored(9)).geometry.size()==1,"Audio retained removed graph membership.");
    runtime.change_structure(3,{}, {second});check(runtime.inspect().entities==2 && runtime.inspect().bodies==1 && runtime.sound_state().voices().empty(),"Final instance deletion leaked membership.");
    const auto later=birth(runtime,d,0);check(later>second,"Removed hierarchy root identity was reused.");
}
void saves_and_provenance() {
    const auto d=definition();Runtime runtime(d);const auto root=birth(runtime,d,0);runtime.step(23,{});
    const auto bytes=runtime.save_snapshot(hash);check(Json::parse(bytes).at("version")==6,"Hierarchical snapshot lacks version6 provenance.");
    auto restored=Runtime::from_snapshot(d,hash,bytes);check(restored->save_snapshot(hash)==bytes && restored->instance(root).nodes==runtime.instance(root).nodes,"Complete hierarchy did not round-trip canonically.");
    RuntimeInput move;move.entity=root;move.move={.25f,.5f};runtime.step(31,{move});restored->step(31,{move});
    check(restored->save_snapshot(hash)==runtime.save_snapshot(hash),"Fresh owner continuation differs in physics/animation/provenance.");
    check(birth(*restored,d,4)==birth(runtime,d,4),"Fresh owner node allocation frontier differs.");
    for(unsigned kind=0;kind<4;++kind) {
        auto bad=Json::parse(bytes);auto& nodes=bad["payload"]["structure"]["spawned"][0]["nodes"];
        if(kind==0)nodes.erase(local(4));
        if(kind==1)nodes[local(4)]=nodes.at(local(3));
        if(kind==2)std::swap(nodes[local(4)],nodes[local(5)]);
        if(kind==3)nodes[local(4)]=authored(9);
        const auto body=bad.at("payload").dump();bad["sha256"]=sha256(std::as_bytes(std::span(body.data(),body.size())));
        rejects([&]{Runtime::from_snapshot(d,hash,bad.dump());});
    }
    auto legacy=d;legacy.templates.clear();RuntimeSpawnTemplate prop;prop.id=std::string(32,'c');prop.name="Legacy single prop";prop.collider=BoxCollider{};
    legacy.templates={prop};Runtime old(legacy);const auto oldroot=birth(old,legacy,0);const auto oldbytes=old.save_snapshot(hash);
    check(Json::parse(oldbytes).at("version")==3,"Legacy root-only writer changed snapshot format.");
    auto oldrestore=Runtime::from_snapshot(legacy,hash,oldbytes);check(oldrestore->save_snapshot(hash)==oldbytes && oldrestore->instance_node(oldroot,prop.id)==oldroot,"Legacy root-only save or resolver changed.");
}
}
int main() {
    try {membership_and_motion();atomic_failure_and_reuse();removal_and_survivors();saves_and_provenance();
        std::cout<<"4 hierarchical runtime groups passed: complete native graph, remapping, detached-character rollback, survivor/audio retirement, saves and legacy roots.\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
