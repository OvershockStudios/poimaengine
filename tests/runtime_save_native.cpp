// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/animation.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace poima;
using Json=nlohmann::json;
namespace {
const std::string content(64,'a');
void check(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
void near(double a,double b,double tolerance,const char* message) { check(std::isfinite(a) && std::isfinite(b) && std::abs(a-b)<=tolerance,message); }
template<class F> void rejects(F action) { bool rejected=false;try { action(); }catch(const std::exception&) { rejected=true; }check(rejected,"Malformed snapshot or binding was accepted."); }
RuntimeEntityDefinition box(const std::string& id,std::array<double,3> position,std::array<double,3> scale,BodyMotion motion) {
    RuntimeEntityDefinition entity;entity.id=id;entity.transform.position=position;entity.transform.scale=scale;
    entity.collider=BoxCollider{};entity.collider->motion=motion;return entity;
}
RuntimeDefinition definition() {
    RuntimeDefinition result;result.world_id="save-native-fixture";result.authored_revision=17;
    result.entities={box("floor",{0,-.5,0},{40,1,40},BodyMotion::Static),
        box("door",{0,1.5,-3},{2,3,.4},BodyMotion::Kinematic),
        box("falling",{7,40,0},{1,1,1},BodyMotion::Dynamic)};
    RuntimeEntityDefinition player;player.id="player";player.transform.position={0,0,4};player.character=CharacterController{};player.character->camera="camera";
    RuntimeEntityDefinition camera;camera.id="camera";camera.parent="player";camera.transform.position={0,1.6,0};camera.camera=RuntimeCamera{};
    RuntimeEntityDefinition handle;handle.id="handle";handle.parent="door";handle.transform.position={.4,0,0};
    result.entities.push_back(player);result.entities.push_back(camera);result.entities.push_back(handle);return result;
}
Json& row(Json& document,const std::string& id) {
    for(auto& value:document.at("payload").at("entities"))if(value.at("id")==id)return value;
    throw std::runtime_error("Snapshot entity row absent.");
}
std::string sealed(Json document) {
    const auto payload=document.at("payload").dump();
    document["sha256"]=sha256(std::as_bytes(std::span(payload.data(),payload.size())));return document.dump();
}
void equal_entity(const Runtime& a,const Runtime& b,const std::string& id,double tolerance=1e-5,bool support=true) {
    const auto x=a.entity(id),y=b.entity(id);
    check(x.id==y.id && x.has_body==y.has_body && x.is_character==y.is_character && x.motion==y.motion,"Restored entity classification differs.");
    for(std::size_t i=0;i<16;++i)near(x.world[i],y.world[i],tolerance,"Restored world transform differs.");
    for(std::size_t i=0;i<3;++i) {
        near(x.local.position[i],y.local.position[i],tolerance,"Restored local position differs.");
        near(x.local.scale[i],y.local.scale[i],1e-12,"Restored local scale differs.");
        near(x.velocity[i],y.velocity[i],tolerance,"Restored linear velocity differs.");
    }
    // Quaternion sign is semantically equivalent; compare through the world matrix above.
    near(x.yaw,y.yaw,1e-12,"Character yaw differs.");near(x.pitch,y.pitch,1e-12,"Character pitch differs.");
    if(support && x.is_character)check(x.ground==y.ground,"Reconstructed character support differs.");
    check(x.motion_remaining_ticks==y.motion_remaining_ticks && x.kinematic_target.has_value()==y.kinematic_target.has_value(),"Kinematic progress differs.");
    if(x.kinematic_target) {
        check(x.kinematic_target->entity==y.kinematic_target->entity && x.kinematic_target->duration_ticks==y.kinematic_target->duration_ticks,"Kinematic command identity differs.");
        for(std::size_t i=0;i<3;++i)near(x.kinematic_target->position[i],y.kinematic_target->position[i],1e-12,"Kinematic target position differs.");
        for(std::size_t i=0;i<4;++i)near(x.kinematic_target->rotation[i],y.kinematic_target->rotation[i],1e-12,"Kinematic target rotation differs.");
    }
}
void roundtrip_motion() {
    const auto d=definition();Runtime source(d);source.step(120,{});
    check(source.entity("player").ground=="on_ground","Character fixture did not reach the floor.");
    RuntimeInput walking;walking.entity="player";walking.move={.2f,.8f};walking.look={25,-12};
    const double q=std::sqrt(.5);const KinematicTarget target{"door",{3,1.5,-3},{0,q,0,q},120};
    source.step(37,{walking},{target});const auto bytes=source.save_snapshot(content);
    const auto payload=Json::parse(bytes);
    check(payload.at("format")=="poima.runtime-snapshot" && payload.at("version")==1,"Snapshot envelope differs.");
    check(payload.at("payload").at("tick")==157 && payload.at("payload").at("authored_revision")==17,"Snapshot clock/revision differs.");
    check(source.entity("falling").velocity[1]<-10 && source.entity("falling").world[13]>1,"Dynamic fixture must still be falling when saved.");
    auto restored=Runtime::from_snapshot(d,content,bytes);
    check(restored->inspect().tick==source.inspect().tick,"Restored clock differs.");
    for(const auto& entity:d.entities)equal_entity(source,*restored,entity.id);
    check(restored->snapshot("camera").camera_world==restored->entity("camera").world,"Restored presentation camera is stale.");
    check(source.save_snapshot(content)==bytes,"Saving/loading modified source runtime.");
    RuntimeInput jump;jump.entity="player";jump.jump=true;
    source.step(1,{jump});restored->step(1,{jump});
    check(source.entity("player").velocity[1]>1 && restored->entity("player").velocity[1]>1,"Reconstructed grounded support cannot jump.");
    equal_entity(source,*restored,"player",1e-4,false);
    source.step(9,{});restored->step(9,{});
    equal_entity(source,*restored,"falling",1e-4,false);equal_entity(source,*restored,"door");equal_entity(source,*restored,"handle");
    // Contacts rebuild solver caches; this is a physical/pose tolerance test,
    // deliberately not a claim of bit-identical future contact trajectories.
    source.step(73,{});restored->step(73,{});
    equal_entity(source,*restored,"door");equal_entity(source,*restored,"handle");
    near(restored->entity("door").world[12],3,1e-5,"Door failed to reach its original endpoint.");
    check(!restored->entity("door").kinematic_target && restored->entity("door").motion_remaining_ticks==0,"Restored door did not finish at the original duration.");
    near(source.entity("falling").world[13],restored->entity("falling").world[13],.03,"Post-contact body diverged beyond three centimeters.");
    check(std::abs(restored->entity("falling").world[13]-.5)<.05,"Restored dynamic body failed to settle on the floor.");
    const auto later=source.save_snapshot(content);
    for(int i=0;i<8;++i) {
        auto staged=Runtime::from_snapshot(d,content,later);
        for(const auto& entity:d.entities)equal_entity(source,*staged,entity.id,1e-5,false);
        staged->step(1,{});
        check(source.save_snapshot(content)==later,"Repeated staging mutated the live source.");
    }
}
RuntimeDefinition animated_definition() {
    auto d=definition();auto model=std::make_shared<ModelAsset>();model->nodes.resize(1);
    model->nodes[0].position={0,1,0};model->roots={0};
    model->animations.push_back({"Across",2,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,1,0,0},{4,1,0,0}}}}});
    model->animations.push_back({"Above",2,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,3,0,0},{-2,3,0,0}}}}});
    RuntimeEntityDefinition rig;rig.id="rig";rig.transform.position={-8,0,0};rig.animation_rig=RuntimeAnimationRig{};rig.animation_rig->model=model;d.entities.push_back(rig);
    RuntimeEntityDefinition bone;bone.id="bone";bone.parent="rig";bone.transform.position={0,1,0};bone.rig_node=RuntimeRigNode{"rig",0};d.entities.push_back(bone);
    auto clip=std::make_shared<AudioClip>();clip->samples.assign(audio_rate,.1f);
    RuntimeEntityDefinition emitter;emitter.id="emitter";emitter.transform.position={4,1,0};emitter.emitter=AudioEmitter{};
    emitter.emitter->asset=std::string(64,'c');emitter.emitter->clip=clip;emitter.emitter->loop=true;d.entities.push_back(emitter);
    return d;
}
void equal_animation(const Runtime& a,const Runtime& b) {
    const auto x=a.animation("rig"),y=b.animation("rig");check(x && y,"Restored animation missing.");
    check(x->entity==y->entity && x->clip==y->clip && x->loop==y->loop && x->playing==y->playing && x->transition.has_value()==y->transition.has_value(),"Animation control differs.");
    near(x->time,y->time,1e-12,"Animation clock differs.");near(x->speed,y->speed,1e-12,"Animation speed differs.");near(x->duration,y->duration,1e-12,"Animation duration differs.");
    if(x->transition) {
        const auto& s=*x->transition;const auto& t=*y->transition;
        check(s.start_tick==t.start_tick && s.duration_ticks==t.duration_ticks && s.elapsed_ticks==t.elapsed_ticks && s.source_frozen==t.source_frozen,"Animation transition progress differs.");
        near(s.weight,t.weight,1e-12,"Animation blend weight differs.");
        if(!s.source_frozen) {
            check(s.source_clip==t.source_clip && s.source_loop==t.source_loop && s.source_playing==t.source_playing,"Outgoing animation metadata differs.");
            near(s.source_time,t.source_time,1e-12,"Outgoing animation clock differs.");near(s.source_speed,t.source_speed,1e-12,"Outgoing animation speed differs.");
        }
    }
    equal_entity(a,b,"bone",1e-10);
}
void equal_sound(const Runtime& a,const Runtime& b) {
    const auto& x=a.sound_state();const auto& y=b.sound_state();check(x.next_id()==y.next_id() && x.voices().size()==y.voices().size(),"Logical sound identity/count differs.");
    for(std::size_t i=0;i<x.voices().size();++i) {
        const auto& s=x.voices()[i];const auto& t=y.voices()[i];
        check(s.id==t.id && s.start_tick==t.start_tick && s.stop_sample==t.stop_sample && s.emitter==t.emitter && s.gain==t.gain,"Logical voice state differs.");
        check(s.sound.asset==t.sound.asset && s.sound.gain==t.sound.gain && s.sound.loop==t.sound.loop && s.sound.enabled==t.sound.enabled,"Frozen voice emitter settings differ.");
        check(s.sound.clip && t.sound.clip && s.sound.clip->samples==t.sound.clip->samples,"Restored voice content reference differs.");
        check(s.end_sample()==t.end_sample() && s.emitting(a.inspect().tick*audio_tick_frames)==t.emitting(b.inspect().tick*audio_tick_frames),"Restored sound cursor/emission differs.");
    }
}
void animation_and_sound() {
    const auto d=animated_definition();Runtime source(d);
    source.step(12,{}, {},{{false,"emitter",0,.6f}},{{"rig",0,0,1,true,true,0}});
    source.step(13,{}, {},{{false,"emitter",0,.8f}},{{"rig",1,0,1.25,true,true,60}});
    // Save an ordinary two-clock transition independently of the frozen case.
    auto outgoing=Runtime::from_snapshot(d,content,source.save_snapshot(content));
    equal_animation(source,*outgoing);equal_sound(source,*outgoing);
    source.step(9,{}, {},{{true,"",1,1}},{{"rig",0,.5,.75,true,true,40}});
    check(source.animation("rig")->transition && source.animation("rig")->transition->source_frozen,"Fixture did not create an interrupted frozen-source fade.");
    check(source.sound_state().voices().size()==2 && source.sound_state().voices()[0].stop_sample.has_value(),"Fixture did not retain stopped/playing logical voices.");
    const auto bytes=source.save_snapshot(content);auto restored=Runtime::from_snapshot(d,content,bytes);
    equal_animation(source,*restored);equal_sound(source,*restored);
    source.step(5,{}, {},{{false,"emitter",0,.3f}});restored->step(5,{}, {},{{false,"emitter",0,.3f}});
    check(restored->sound_state().next_id()==4,"Restored sound identity counter was reset.");
    equal_animation(source,*restored);equal_sound(source,*restored);
    source.step(26,{}, {},{{true,"",2,1}});restored->step(26,{}, {},{{true,"",2,1}});
    check(!restored->animation("rig")->transition,"Restored frozen fade did not finish on its original tick.");
    equal_animation(source,*restored);equal_sound(source,*restored);
    source.step(120,{});restored->step(120,{});equal_animation(source,*restored);equal_sound(source,*restored);
    auto staged=Runtime::from_snapshot(d,content,bytes);equal_animation(*staged,*Runtime::from_snapshot(d,content,bytes));
    check(staged->inspect().tick==34,"Staged snapshot bytes changed after independent runtimes advanced.");
}
void angular_and_sleep() {
    RuntimeDefinition d;d.world_id="rotating-contact-save";d.authored_revision=3;
    d.entities={box("floor",{0,-.5,0},{20,1,20},BodyMotion::Static),box("tilted",{0,2,0},{1,1,1},BodyMotion::Dynamic)};
    const double angle=.19;d.entities[1].transform.rotation={std::sin(angle),0,0,std::cos(angle)};
    Runtime source(d);bool rotating=false;Json saved;
    for(int tick=0;tick<120;++tick) {
        source.step(1,{});saved=Json::parse(source.save_snapshot(content));
        const auto velocity=row(saved,"tilted").at("body").at("angular_velocity").get<std::array<double,3>>();
        if(std::abs(velocity[0])+std::abs(velocity[1])+std::abs(velocity[2])>.1) { rotating=true;break; }
    }
    check(rotating,"Tilted falling body failed to acquire angular velocity at contact.");
    auto restored=Runtime::from_snapshot(d,content,saved.dump());auto again=Json::parse(restored->save_snapshot(content));
    const auto original=row(saved,"tilted").at("body"),copied=row(again,"tilted").at("body");
    check(original.at("active")==true && copied.at("active")==true,"Active rotating body was put to sleep on restoration.");
    for(std::size_t i=0;i<3;++i)near(original.at("angular_velocity")[i].get<double>(),copied.at("angular_velocity")[i].get<double>(),1e-5,"Angular velocity did not round-trip.");
    const auto before=restored->entity("tilted").world;restored->step(1,{});
    check(restored->entity("tilted").world!=before,"Restored rotating body failed to continue moving.");
    source.step(600,{});saved=Json::parse(source.save_snapshot(content));
    check(row(saved,"tilted").at("body").at("active")==false,"Settled fixture never entered sleeping state.");
    restored=Runtime::from_snapshot(d,content,saved.dump());again=Json::parse(restored->save_snapshot(content));
    check(row(again,"tilted").at("body").at("active")==false,"Snapshot restoration woke a sleeping body.");
    equal_entity(source,*restored,"tilted");
}
void export_boundaries() {
    RuntimeDefinition falling;falling.world_id="export-position-boundary";
    falling.entities={box("falling",{0,-1e6,0},{1,1,1},BodyMotion::Dynamic)};
    Runtime source(falling);const auto valid=source.save_snapshot(content);
    check(Runtime::from_snapshot(falling,content,valid)->inspect().tick==0,"Inclusive position boundary failed valid round-trip.");
    source.step(1,{});const auto before=source.entity("falling");
    check(before.world[13]<-1e6,"Boundary fixture did not move beyond the supported snapshot range.");
    rejects([&]{(void)source.save_snapshot(content);});
    check(source.inspect().tick==1 && source.entity("falling").world==before.world && source.entity("falling").velocity==before.velocity,"Rejected export changed the live body.");
    auto d=definition();
    for(auto& entity:d.entities)if(entity.id=="player")entity.transform.rotation={0,1,0,0};
    Runtime turned(d);check(std::abs(turned.entity("player").yaw)>179.99,"Initial character yaw fixture is not 180 degrees.");
    auto restored=Runtime::from_snapshot(d,content,turned.save_snapshot(content));
    equal_entity(turned,*restored,"player",1e-5,false);equal_entity(turned,*restored,"camera",1e-5,false);
    for(const auto& identity:{std::string{},std::string(257,'x')}) {
        auto invalid=definition();invalid.world_id=identity;
        rejects([&]{Runtime bad(invalid);(void)bad.save_snapshot(content);});
        invalid=definition();invalid.entities.front().id=identity;
        rejects([&]{Runtime bad(invalid);(void)bad.save_snapshot(content);});
    }
    Runtime velocity_source(definition());const auto bytes=velocity_source.save_snapshot(content);
    for(const auto* field:{"linear_velocity","angular_velocity"}) {
        auto candidate=Json::parse(bytes);row(candidate,"falling")["body"][field]={1000,0,0};
        // Each component is within the JSON +/-1e6 limit, but the vector is
        // above the body's Jolt limit and must not be silently clamped.
        rejects([&]{(void)Runtime::from_snapshot(definition(),content,sealed(candidate));});
        check(velocity_source.save_snapshot(content)==bytes,"Rejected clamped-velocity load changed source.");
    }
}
std::unique_ptr<Runtime> exchange_fixture(const RuntimeDefinition& d) {
    auto runtime=std::make_unique<Runtime>(d);runtime->step(120,{});
    RuntimeInput walking;walking.entity="player";walking.move={.2f,.8f};walking.look={25,-12};
    const double q=std::sqrt(.5);
    runtime->step(12,{walking},{{"door",{3,1.5,-3},{0,q,0,q},120}},{{false,"emitter",0,.6f}},{{"rig",0,0,1,true,true,0}});
    runtime->step(13,{}, {},{{false,"emitter",0,.8f}},{{"rig",1,0,1.25,true,true,60}});
    runtime->step(9,{}, {},{{true,"",1,1}},{{"rig",0,.5,.75,true,true,40}});
    check(runtime->inspect().tick==154 && runtime->animation("rig")->transition->source_frozen,"Process-exchange fixture is not at its expected live state.");
    return runtime;
}
void fixture_file(const std::string& mode,const std::filesystem::path& path) {
    const auto d=animated_definition();auto expected=exchange_fixture(d);
    if(mode=="--write-fixture") {
        check(!std::filesystem::exists(path),"Fixture output already exists.");
        const auto bytes=expected->save_snapshot(content);std::ofstream output(path,std::ios::binary);
        check(bool(output),"Cannot create snapshot test fixture.");output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));output.close();
        check(bool(output),"Could not finish snapshot test fixture.");
        std::cout<<"Wrote test-only portable snapshot fixture at tick 154 ("<<bytes.size()<<" bytes).\n";return;
    }
    check(mode=="--read-fixture","Expected --write-fixture PATH or --read-fixture PATH.");
    const auto size=std::filesystem::file_size(path);check(size>0 && size<=64U*1024U*1024U,"Fixture file exceeds snapshot limit.");
    std::string bytes(static_cast<std::size_t>(size),'\0');std::ifstream input(path,std::ios::binary);
    input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));check(bool(input),"Cannot read complete snapshot test fixture.");
    auto restored=Runtime::from_snapshot(d,content,bytes);
    check(restored->inspect().tick==154,"Fresh-process restore changed the simulation clock.");
    for(const auto& entity:d.entities)equal_entity(*expected,*restored,entity.id,1e-5,false);
    equal_animation(*expected,*restored);equal_sound(*expected,*restored);
    check(restored->save_snapshot(content)==bytes,"Fresh-process round-trip changed canonical snapshot bytes.");
    expected->step(31,{});restored->step(31,{});
    equal_entity(*expected,*restored,"door");equal_entity(*expected,*restored,"handle");
    equal_animation(*expected,*restored);equal_sound(*expected,*restored);
    check(!restored->animation("rig")->transition,"Fresh-process restored fade missed its original endpoint.");
    std::cout<<"Read fresh-process fixture: exact canonical bytes, all live entities, animation/sound state and future logical motion passed.\n";
}
RuntimeDefinition lifecycle_definition() {
    auto d=definition();
    RuntimeEntityDefinition reserved;reserved.id=std::string(31,'0')+"1";d.entities.push_back(reserved);
    for(int i=0;i<4;++i) {
        RuntimeSpawnTemplate recipe;recipe.id=std::string(31,'a')+char('1'+i);recipe.name="Save prop";
        recipe.transform.position={double(10+i*4),20,0};
        if(i<3) {recipe.collider=BoxCollider{};recipe.collider->motion=i==0 ? BodyMotion::Dynamic : i==1 ? BodyMotion::Static : BodyMotion::Kinematic;}
        d.templates.push_back(recipe);
    }
    return d;
}
void lifecycle_roundtrip() {
    const auto d=lifecycle_definition();Runtime source(d);
    check(Json::parse(source.save_snapshot(content)).at("version")==1,"Unused catalog changed legacy snapshot version.");
    RuntimeTransform override;override.position={18,4,0};override.scale={2,1,1};
    const auto born=source.change_structure(0,{{d.templates[0].id,{}},{d.templates[1].id,override},{d.templates[2].id,{}},{d.templates[3].id,{}}},{}).spawned;
    source.step(15,{},{{born[2],{20,20,0},{0,0,0,1},90}});
    check(born.front()==std::string(31,'0')+"2","Generated ID failed to skip authored reservation.");
    const auto bytes=source.save_snapshot(content);const auto saved=Json::parse(bytes);
    check(saved.at("version")==3 && saved.at("payload").contains("components"),"Lifecycle snapshot needs version 3 and component state.");
    check(saved.at("payload").at("structure").at("revision")==1 && saved.at("payload").at("structure").at("spawned").size()==4,"Lifecycle provenance missing.");
    auto restored=Runtime::from_snapshot(d,content,bytes);
    check(restored->structure_revision()==1 && restored->inspect().tick==source.inspect().tick,"Restored lifecycle revision/clock differs.");
    for(const auto& e:d.entities)equal_entity(source,*restored,e.id);
    for(const auto& id:born)equal_entity(source,*restored,id);
    source.step(7,{});restored->step(7,{});
    for(const auto& id:born)equal_entity(source,*restored,id,1e-4);
    const auto next=source.change_structure(1,{{d.templates[3].id,{}}},{}).spawned;
    check(restored->change_structure(1,{{d.templates[3].id,{}}},{}).spawned==next,"Restored generated identity frontier differs.");
    // Removing every generated entity must still retain allocator history.
    auto all=born;all.insert(all.end(),next.begin(),next.end());
    source.change_structure(2,{},all);
    const auto empty_bytes=source.save_snapshot(content);const auto empty=Json::parse(empty_bytes);
    check(empty.at("version")==3 && empty.at("payload").at("structure").at("spawned").empty(),"Empty survivor set discarded lifecycle history.");
    auto empty_restore=Runtime::from_snapshot(d,content,empty_bytes);
    check(empty_restore->structure_revision()==3,"Empty survivor restore reset structural revision.");
    const auto later=source.change_structure(3,{{d.templates[3].id,{}}},{}).spawned;
    check(empty_restore->change_structure(3,{{d.templates[3].id,{}}},{}).spawned==later && later.front()>next.front(),"Empty survivor restore reused a retired identity.");
}
void lifecycle_components() {
    RuntimeDefinition d;d.world_id="lifecycle-reference-save";
    const std::string type(32,'b'),field(32,'c'),target=std::string(31,'0')+"1";
    const auto schema=components::parse_schema("{\"id\":\""+type+"\",\"name\":\"Target\",\"version\":1,\"fields\":[{\"id\":\""+field+"\",\"name\":\"Entity\",\"kind\":\"entity\",\"default\":\"00000000000000000000000000000000\"}]}");
    d.component_schemas={schema};
    RuntimeSpawnTemplate plain;plain.id=std::string(32,'d');plain.name="Target";
    RuntimeSpawnTemplate referencing;referencing.id=std::string(32,'e');referencing.name="Referrer";
    referencing.components[type]=components::parse_values(schema,Json{{field,target}}.dump());d.templates={plain,referencing};
    Runtime source(d);check(Json::parse(source.save_snapshot(content)).at("version")==2,"Unchanged custom world lost legacy snapshot version.");
    const auto born=source.change_structure(0,{{plain.id,{}},{referencing.id,{}}},{}).spawned;
    check(born.front()==target,"Reference fixture allocation assumption changed.");
    source.component_edit(type,born[1],components::defaults(schema));
    source.change_structure(1,{}, {born[0]});
    const auto bytes=source.save_snapshot(content);auto restored=Runtime::from_snapshot(d,content,bytes);
    check(restored->component_read(type,born[1])==source.component_read(type,born[1]) && restored->component_revision()==source.component_revision(),"Repaired saved reference did not replace stale template default.");
    rejects([&]{(void)restored->entity(born[0]);});
    check(restored->component_query(type,"",64)==std::vector<std::string>{born[1]},"Restored component membership differs.");
    const auto original=Json::parse(bytes);
    auto invalid=[&](auto change) {auto j=original;change(j);rejects([&]{(void)Runtime::from_snapshot(d,content,sealed(j));});check(source.save_snapshot(content)==bytes,"Rejected component save mutated source.");};
    invalid([](auto& j){j["payload"]["components"]["types"][0]["fingerprint"]=std::string(64,'0');});
    invalid([](auto& j){j["payload"]["components"]["types"][0]["instances"].clear();});
    invalid([&](auto& j){j["payload"]["components"]["types"][0]["instances"][0]["entity"]=born[0];});
    invalid([&](auto& j){j["payload"]["components"]["types"][0]["instances"][0]["values"][0]=born[0];});
    invalid([](auto& j){j["payload"].erase("components");});
    source.change_structure(2,{}, {born[1]});
    const auto empty_bytes=source.save_snapshot(content);const auto empty=Json::parse(empty_bytes);
    check(empty.at("payload").at("entities").empty() && empty.at("payload").at("structure").at("spawned").empty(),"Zero-live fixture retained entities.");
    auto empty_restore=Runtime::from_snapshot(d,content,empty_bytes);
    check(empty_restore->structure_revision()==3 && empty_restore->component_query(type,"",64).empty(),"Zero-live restoration retained stale membership or reset revision.");
    const auto next=source.change_structure(3,{{plain.id,{}}},{}).spawned;
    check(empty_restore->change_structure(3,{{plain.id,{}}},{}).spawned==next && next.front()>born.back(),"Zero-live restoration reused one of its two retired identities.");
}
void lifecycle_fixture_file(const std::string& mode,const std::filesystem::path& path) {
    const auto d=lifecycle_definition();Runtime expected(d);
    const auto born=expected.change_structure(0,{{d.templates[0].id,{}},{d.templates[1].id,{}},{d.templates[2].id,{}},{d.templates[3].id,{}}},{}).spawned;
    expected.step(12,{},{{born[2],{20,20,0},{0,0,0,1},90}});
    expected.change_structure(1,{}, {born[3]});
    if(mode=="--write-lifecycle-fixture") {
        check(!std::filesystem::exists(path),"Lifecycle fixture output already exists.");
        const auto bytes=expected.save_snapshot(content);std::ofstream output(path,std::ios::binary);
        check(bool(output),"Cannot create lifecycle snapshot fixture.");output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));output.close();
        check(bool(output),"Could not finish lifecycle snapshot fixture.");
        std::cout<<"Wrote test-only version 3 lifecycle snapshot at tick 12 ("<<bytes.size()<<" bytes).\n";return;
    }
    check(mode=="--read-lifecycle-fixture","Expected --write-lifecycle-fixture PATH or --read-lifecycle-fixture PATH.");
    const auto size=std::filesystem::file_size(path);check(size>0 && size<=64U*1024U*1024U,"Lifecycle fixture exceeds snapshot limit.");
    std::string bytes(static_cast<std::size_t>(size),'\0');std::ifstream input(path,std::ios::binary);
    input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));check(bool(input),"Cannot read complete lifecycle snapshot fixture.");
    check(Json::parse(bytes).at("version")==3,"Lifecycle exchange fixture is not version 3.");
    auto restored=Runtime::from_snapshot(d,content,bytes);
    check(restored->inspect().tick==12 && restored->structure_revision()==2,"Fresh-process lifecycle clock or revision changed.");
    for(const auto& e:d.entities)equal_entity(expected,*restored,e.id,1e-5,false);
    for(std::size_t i=0;i<3;++i)equal_entity(expected,*restored,born[i]);
    rejects([&]{(void)restored->entity(born[3]);});
    check(restored->save_snapshot(content)==bytes,"Fresh-process lifecycle restore changed canonical snapshot bytes.");
    expected.step(7,{});restored->step(7,{});
    for(std::size_t i=0;i<3;++i)equal_entity(expected,*restored,born[i],1e-4);
    const auto next=expected.change_structure(2,{{d.templates[3].id,{}}},{}).spawned;
    check(restored->change_structure(2,{{d.templates[3].id,{}}},{}).spawned==next && next.front()>born.back(),"Fresh-process lifecycle restore reused a retired identity.");
    std::cout<<"Read fresh-process version 3 fixture: exact canonical bytes, live prop state, future motion and retired identity continuity passed.\n";
}

void lifecycle_malformed() {
    const auto d=lifecycle_definition();Runtime source(d);
    const auto born=source.change_structure(0,{{d.templates[1].id,{}},{d.templates[3].id,{}}},{}).spawned;
    const auto bytes=source.save_snapshot(content);const auto original=Json::parse(bytes);
    auto invalid=[&](auto change) {auto j=original;change(j);rejects([&]{(void)Runtime::from_snapshot(d,content,sealed(j));});check(source.save_snapshot(content)==bytes,"Rejected lifecycle save mutated source.");};
    invalid([](auto& j){j["payload"]["structure"]["revision"]=0;});
    invalid([](auto& j){j["payload"]["structure"]["revision"]=1.0;});
    invalid([](auto& j){j["payload"]["structure"]["next_entity_id"]=std::string(32,'0');});
    invalid([&](auto& j){j["payload"]["structure"]["next_entity_id"]=born.back();});
    invalid([](auto& j){j["payload"]["structure"]["next_entity_id"]=std::string(32,'A');});
    invalid([](auto& j){j["payload"]["structure"]["exhausted"]=true;});
    invalid([](auto& j){j["payload"]["structure"]["exhausted"]=0;});
    invalid([](auto& j){j["payload"]["structure"]["extra"]=true;});
    invalid([](auto& j){j["payload"]["structure"]["spawned"][0]["template_id"]=std::string(32,'f');});
    invalid([](auto& j){j["payload"]["structure"]["spawned"][0]["id"]="floor";});
    invalid([](auto& j){j["payload"]["structure"]["spawned"][0]["id"]=std::string(31,'0')+"1";});
    invalid([](auto& j){auto& a=j["payload"]["structure"]["spawned"];std::swap(a[0],a[1]);});
    invalid([](auto& j){auto& a=j["payload"]["structure"]["spawned"];a.push_back(a[0]);});
    invalid([](auto& j){j["payload"]["structure"]["spawned"].erase(0);});
    invalid([](auto& j){j["payload"]["structure"]["spawned"][0]["initial_transform"]["scale"][0]=0;});
    invalid([](auto& j){j["payload"]["structure"]["spawned"][0]["initial_transform"]["position"][0]=900;});
    invalid([&](auto& j){row(j,born[0])["parent"]="floor";});
    invalid([](auto& j){auto& a=j["payload"]["entities"];for(auto it=a.begin();it!=a.end();++it)if(it->at("id")=="floor") {a.erase(it);break;}});
    invalid([](auto& j){j["payload"]["components"]["types"].push_back(Json::object());});
    invalid([](auto& j){j["payload"].erase("structure");});
    invalid([](auto& j){j["version"]=1;});
    auto restored=Runtime::from_snapshot(d,content,bytes);
    check(restored->change_structure(1,{{d.templates[3].id,{}}},{}).spawned==source.change_structure(1,{{d.templates[3].id,{}}},{}).spawned,"Malformed load attempts consumed generated IDs.");
}

void malformed() {
    const auto d=definition();Runtime source(d);source.step(12,{},{{"door",{3,1.5,-3},{0,0,0,1},90}});
    const auto bytes=source.save_snapshot(content);const auto original=Json::parse(bytes);
    auto invalid=[&](auto change) {
        auto document=original;change(document);const auto candidate=sealed(std::move(document));
        rejects([&]{(void)Runtime::from_snapshot(d,content,candidate);});
        check(source.save_snapshot(content)==bytes,"Rejected staged load changed source state.");
    };
    rejects([&]{(void)source.save_snapshot("bad");});
    rejects([&]{(void)Runtime::from_snapshot(d,std::string(64,'b'),bytes);});
    rejects([&]{(void)Runtime::from_snapshot(d,std::string(64,'A'),bytes);});
    auto changed=d;changed.world_id="another-world";rejects([&]{(void)Runtime::from_snapshot(changed,content,bytes);});
    changed=d;++changed.authored_revision;rejects([&]{(void)Runtime::from_snapshot(changed,content,bytes);});
    rejects([&]{(void)Runtime::from_snapshot(d,content,bytes.substr(0,bytes.size()/2));});
    auto wrong_hash=original;wrong_hash["sha256"]=std::string(64,'0');rejects([&]{(void)Runtime::from_snapshot(d,content,wrong_hash.dump());});
    const auto duplicate="{\"format\":\"poima.runtime-snapshot\","+bytes.substr(1);
    rejects([&]{(void)Runtime::from_snapshot(d,content,duplicate);});
    rejects([&]{(void)Runtime::from_snapshot(d,content,std::string(64U*1024U*1024U+1,' '));});
    invalid([](auto& j){j["version"]=2;});invalid([](auto& j){j["version"]=1.0;});invalid([](auto& j){j["extra"]=true;});
    invalid([](auto& j){j["payload"]["extra"]=true;});
    invalid([](auto& j){j["payload"]["world_id"]="different";});
    invalid([](auto& j){j["payload"]["content_sha256"]=std::string(64,'b');});
    invalid([](auto& j){j["payload"]["authored_revision"]=17.0;});
    invalid([](auto& j){j["payload"]["tick"]=-1;});invalid([](auto& j){j["payload"]["tick"]=1.5;});
    invalid([](auto& j){j["payload"]["tick"]=12.0;});
    invalid([](auto& j){j["payload"]["tick"]=9007199254740992ULL;});
    invalid([](auto& j){j["payload"]["gameplay_revision"]=-1;});
    invalid([](auto& j){j["payload"]["entities"].erase(0);});
    invalid([](auto& j){j["payload"]["entities"].push_back(j["payload"]["entities"][0]);});
    invalid([](auto& j){row(j,"door")["id"]="absent";});
    invalid([](auto& j){row(j,"handle")["parent"]="floor";});
    invalid([](auto& j){row(j,"door")["local"]["scale"][0]=-1;});
    invalid([](auto& j){row(j,"door")["local"]["scale"][0]=4;});
    invalid([](auto& j){row(j,"door")["body"]=nullptr;});
    invalid([](auto& j){row(j,"door")["body"]["motion"]="dynamic";});
    invalid([](auto& j){row(j,"door")["body"]["rotation"]={0,0,0,0};});
    invalid([](auto& j){row(j,"door")["body"]["linear_velocity"][0]=1000001;});
    invalid([](auto& j){row(j,"door")["body"]["angular_velocity"][0]=1000001;});
    invalid([](auto& j){row(j,"door")["body"]["position"][0]=1000001;});
    invalid([](auto& j){row(j,"door")["body"]["active"]="true";});
    invalid([](auto& j){row(j,"door")["body"]["motion_state"]["duration_ticks"]=0;});
    invalid([](auto& j){row(j,"door")["body"]["motion_state"]["duration_ticks"]=36001;});
    invalid([](auto& j){row(j,"door")["body"]["motion_state"]["elapsed_ticks"]=90;});
    invalid([](auto& j){row(j,"door")["body"]["position"][0]=100;});
    invalid([](auto& j){row(j,"player")["controller"]=nullptr;});
    invalid([](auto& j){row(j,"player")["controller"]["yaw"]=1e300;});
    invalid([](auto& j){row(j,"player")["controller"]["pitch"]=100;});
    invalid([](auto& j){j["payload"]["gameplay"]=Json::object();});
    invalid([](auto& j){Json deep=0;for(int i=0;i<40;++i)deep=Json::array({deep});j["payload"]["deep"]=std::move(deep);});
    check(source.save_snapshot(content)==bytes,"Binding/parser rejection modified source runtime.");
    auto valid=Runtime::from_snapshot(d,content,bytes);valid->step(1,{});
    check(valid->inspect().tick==13,"Valid staging failed after malformed snapshot attempts.");
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=1) {
            check(argc==3,"Expected a fixture writer/reader mode and PATH.");const std::string mode=argv[1];
            if(mode=="--write-lifecycle-fixture" || mode=="--read-lifecycle-fixture")lifecycle_fixture_file(mode,std::filesystem::path(argv[2]));
            else fixture_file(mode,std::filesystem::path(argv[2]));
            return 0;
        }
        roundtrip_motion();animation_and_sound();angular_and_sleep();export_boundaries();malformed();lifecycle_roundtrip();lifecycle_components();lifecycle_malformed();
        std::cout<<"Runtime snapshot moving door/child, airborne/angular dynamics and sleep, reconstructed character jump, future contact tolerance, outgoing/frozen animation fades, logical sound continuation, export bounds/yaw/IDs, repeated staging, lifecycle provenance/frontier/reference restoration and malformed/binding rejection passed.\n";
        return 0;
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
