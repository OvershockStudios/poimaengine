// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace poima;
namespace {
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
RuntimeDefinition fixture() {
    RuntimeDefinition d; d.world_id="fixture"; d.authored_revision=7;
    RuntimeEntityDefinition floor; floor.id="floor"; floor.transform.position={0,-0.5,0}; floor.transform.scale={20,1,20}; floor.collider=BoxCollider{};
    RuntimeEntityDefinition wall; wall.id="wall"; wall.transform.position={0,1.5,-3}; wall.transform.scale={8,3,0.5}; wall.collider=BoxCollider{};
    RuntimeEntityDefinition box; box.id="box"; box.transform.position={2,3,0}; box.collider=BoxCollider{}; box.collider->motion=BodyMotion::Dynamic;
    RuntimeEntityDefinition player; player.id="player"; player.transform.position={0,1,2}; player.character=CharacterController{}; player.character->camera="camera";
    RuntimeEntityDefinition camera; camera.id="camera"; camera.parent="player"; camera.transform.position={0,1.6,0}; camera.camera=RuntimeCamera{};
    d.entities={player,box,camera,wall,floor}; return d;
}
void lifecycle() {
    const std::string tid(32,'a'),refid(32,'b'),field(32,'c');
    auto d=fixture();
    const auto canonical=[](const std::string& name) {
        if(name.empty())return name;
        return std::string(31,'e')+std::to_string(name=="floor" ? 1 : name=="wall" ? 2 : name=="box" ? 3 : name=="player" ? 4 : 5);
    };
    for(auto& e:d.entities) {
        e.id=canonical(e.id);e.parent=canonical(e.parent);
        if(e.character)e.character->camera=canonical(e.character->camera);
    }
    const auto schema=components::parse_schema("{\"id\":\""+refid+"\",\"name\":\"Target\",\"version\":1,\"fields\":[{\"id\":\""+field+"\",\"name\":\"Entity\",\"kind\":\"entity\",\"default\":\"00000000000000000000000000000000\"}]}");
    d.component_schemas.push_back(schema);
    RuntimeSpawnTemplate prop;prop.id=tid;prop.name="Crate";prop.collider=BoxCollider{};
    prop.collider->motion=BodyMotion::Dynamic;prop.transform.position={4,4,0};
    prop.components[refid]=components::defaults(schema);d.templates.push_back(prop);
    auto invalid=prop;invalid.id=std::string(32,'d');
    invalid.components[refid]=components::parse_values(schema,"{\""+field+"\":\"ffffffffffffffffffffffffffffffff\"}");
    d.templates.push_back(invalid);
    Runtime a(d),control(d);bool rejected=false;
    try {a.change_structure(0,{{tid,{}},{invalid.id,{}}},{});}catch(const std::exception&) {rejected=true;}
    require(rejected && a.structure_revision()==0 && a.component_revision()==0 && a.inspect().entities==5 && a.inspect().bodies==4,"Failed spawn changed membership.");
    const auto born=a.change_structure(0,{{tid,{}}},{});
    const auto same=control.change_structure(0,{{tid,{}}},{});
    require(born.spawned==same.spawned && born.revision==1,"Failed spawn consumed public IDs.");
    require(a.inspect().entities==6 && a.inspect().bodies==5 && a.inspect().tick==0,"Spawn did not publish native membership.");
    const auto id=born.spawned.front();require(a.entity(id).has_body,"Spawned prop has no body.");
    require(a.component_query(refid,"",100)==std::vector<std::string>{id},"Spawned custom component not queryable.");
    a.step(60,{});control.step(60,{});
    require(a.entity(id).world==control.entity(id).world && a.entity(id).velocity==control.entity(id).velocity,"Failed preparation changed subsequent physics.");
    RuntimeTransform other;other.position={6,3,0};const auto second=a.change_structure(1,{{tid,other}},{}).spawned.front();
    a.component_edit(refid,id,components::parse_values(schema,"{\""+field+"\":\""+second+"\"}"));
    const auto component_revision=a.component_revision();
    rejected=false;try {a.change_structure(2,{}, {second});}catch(const std::exception&) {rejected=true;}
    require(rejected && a.structure_revision()==2 && a.component_revision()==component_revision && a.inspect().entities==7,"Incoming reference did not protect removal.");
    a.component_edit(refid,id,components::defaults(schema));
    a.change_structure(2,{}, {second});
    require(a.inspect().entities==6 && a.inspect().bodies==5,"Removal retained live physics membership.");
    const auto next=a.change_structure(3,{{tid,other}}, {id});
    require(next.spawned.front()!=id && next.spawned.front()!=second && a.inspect().entities==6,"Retired public ID was reused.");
    a.change_structure(4,{},next.spawned);
    require(a.inspect().entities==5 && a.inspect().bodies==4 && a.component_query(refid,"",100).empty(),"Spawn/remove did not restore baseline membership.");
    rejected=false;try {a.change_structure(5,{}, {canonical("floor")});}catch(const std::exception&) {rejected=true;}
    require(rejected,"Unsupported authored removal was accepted.");
    const auto saved=a.save_snapshot(std::string(64,'a'));
    auto loaded=Runtime::from_snapshot(d,std::string(64,'a'),saved);
    require(loaded->structure_revision()==a.structure_revision(),"Save lost lifecycle revision with no spawned survivors.");
    for(unsigned i=0;i<32;++i) {
        const auto item=a.change_structure(a.structure_revision(),{{tid,{}}},{});
        a.change_structure(a.structure_revision(),{},item.spawned);
    }
    require(a.inspect().entities==5 && a.inspect().bodies==4,"Repeated lifecycle leaked live bodies.");
    auto platform_definition=d;
    platform_definition.templates[0].collider->motion=BodyMotion::Static;
    for(auto& e:platform_definition.entities)if(e.character)e.transform.position={0,4,2};
    Runtime platform(platform_definition);
    RuntimeTransform support;support.position={0,2,2};support.scale={4,1,4};
    const auto support_id=platform.change_structure(0,{{tid,support}},{}).spawned.front();
    platform.step(120,{});
    require(platform.entity(canonical("player")).ground=="on_ground" && platform.entity(canonical("player")).local.position[1]>2,
        "Platform fixture did not support the character.");
    platform.change_structure(1,{}, {support_id});
    require(platform.entity(canonical("player")).ground!="on_ground","Deleted support left character grounded.");
    RuntimeInput jump;jump.entity=canonical("player");jump.jump=true;platform.step(1,{jump});
    require(platform.entity(canonical("player")).velocity[1]<=0,"Character jumped from deleted support.");
    auto kinematic_definition=d;kinematic_definition.templates[0].collider->motion=BodyMotion::Kinematic;
    Runtime kinematic(kinematic_definition);
    const auto mover=kinematic.change_structure(0,{{tid,{}}},{}).spawned.front();
    KinematicTarget motion;motion.entity=mover;motion.position={5,4,0};motion.duration_ticks=2;
    kinematic.step(2,{}, {motion});
    require(std::abs(kinematic.entity(mover).local.position[0]-5)<1e-6,"Spawned kinematic body was not stepped.");
    kinematic.change_structure(1,{}, {mover});kinematic.step(2,{});
    auto logical_definition=d;logical_definition.templates[0].collider.reset();
    Runtime logical(logical_definition);
    const auto logical_id=logical.change_structure(0,{{tid,{}}},{}).spawned.front();
    require(logical.inspect().entities==6 && logical.inspect().bodies==4,"Bodyless spawn created a physical body.");
    rejected=false;try {logical.change_structure(0,{}, {logical_id});}catch(const std::exception&) {rejected=true;}
    require(rejected && logical.inspect().entities==6,"Stale structural request changed membership.");
    rejected=false;try {logical.change_structure(1,{}, {logical_id,logical_id});}catch(const std::exception&) {rejected=true;}
    require(rejected && logical.inspect().entities==6,"Duplicate removal partially committed.");
    logical.change_structure(1,{}, {logical_id});
}
void scheduled_lifecycle() {
    const std::string tid(32,'a'),logical_tid(32,'d'),kinematic_tid(32,'e'),type(32,'b'),field(32,'c'),hash(64,'a');
    const auto id=[](unsigned n){return std::string(31,'0')+std::to_string(n);};
    RuntimeDefinition d;d.world_id="scheduled-lifecycle";
    const auto schema=components::parse_schema("{\"id\":\""+type+"\",\"name\":\"Target\",\"version\":1,\"fields\":[{\"id\":\""+field+"\",\"name\":\"Entity\",\"kind\":\"entity\",\"default\":\"00000000000000000000000000000000\"}]}");
    d.component_schemas={schema};
    RuntimeSpawnTemplate prop;prop.id=tid;prop.name="Scheduled prop";prop.transform.position={0,10,0};
    prop.collider=BoxCollider{};prop.collider->motion=BodyMotion::Dynamic;prop.components[type]=components::defaults(schema);
    auto logical=prop;logical.id=logical_tid;logical.collider.reset();
    auto kinematic=prop;kinematic.id=kinematic_tid;kinematic.collider->motion=BodyMotion::Kinematic;
    d.templates={prop,logical,kinematic};
    Runtime a(d),control(d);
    const auto results=a.step(5,{}, {}, {}, {},{{0,0,{{tid,{}}},{}},{2,1,{{logical_tid,{}}},{id(1)}}});
    require(results.size()==2 && results[0].revision==1 && results[0].spawned==std::vector<std::string>{id(1)} && results[1].revision==2 && results[1].spawned==std::vector<std::string>{id(2)},"Scheduled lifecycle result order differs.");
    control.change_structure(0,{{tid,{}}},{});control.step(2,{});
    control.change_structure(1,{{logical_tid,{}}},{id(1)});control.step(3,{});
    require(a.save_snapshot(hash)==control.save_snapshot(hash),"Scheduled lifecycle differs from equivalent tick-boundary edits.");
    require(a.inspect().entities==1 && a.inspect().bodies==0 && a.component_query(type,"",64)==std::vector<std::string>{id(2)},"Committed born-then-retired prop retained ownership or components.");
    a.change_structure(2,{}, {id(2)});require(a.inspect().entities==0 && a.inspect().bodies==0,"Successful scheduled cleanup retained live entities.");
    // A later revision failure must unwind births already published and retired
    // within this same batch, including their custom-cell and body owners.
    Runtime failed(d),untouched(d);const auto before=failed.save_snapshot(hash);
    bool rejected=false;
    try {(void)failed.step(5,{}, {}, {}, {},{{0,0,{{tid,{}}},{}},{2,1,{}, {id(1)}},{4,0,{{logical_tid,{}}},{}}});}
    catch(const std::exception&) {rejected=true;}
    require(rejected && failed.save_snapshot(hash)==before,"Later structural failure did not restore complete original snapshot.");
    require(failed.structure_revision()==0 && failed.component_revision()==0 && failed.inspect().entities==0 && failed.inspect().bodies==0,"Failed born-then-retired batch retained state.");
    require(failed.change_structure(0,{{tid,{}}},{}).spawned==untouched.change_structure(0,{{tid,{}}},{}).spawned,"Failed schedule consumed generated IDs.");
    failed.step(3,{});untouched.step(3,{});
    require(failed.save_snapshot(hash)==untouched.save_snapshot(hash),"Failed schedule changed subsequent physics or component state.");
    // Schedule guards must reject before committing any partial tick sequence.
    const auto guarded=failed.save_snapshot(hash);
    for(const auto& schedule:std::vector<std::vector<RuntimeStructureTick>>{
        {{0,1,{{logical_tid,{}}},{}},{0,2,{{logical_tid,{}}},{}}},
        {{1,1,{{logical_tid,{}}},{}},{0,2,{{logical_tid,{}}},{}}},
        {{2,1,{{logical_tid,{}}},{}}}}) {
        rejected=false;try {(void)failed.step(2,{}, {}, {}, {},schedule);}catch(const std::exception&) {rejected=true;}
        require(rejected && failed.save_snapshot(hash)==guarded,"Invalid structural schedule partially committed.");
    }
    // Retirement of an existing moving kinematic must preserve its original
    // target and elapsed clock when a subsequent scheduled edit fails.
    Runtime moving(d),moving_control(d);
    const auto mover=moving.change_structure(0,{{kinematic_tid,{}}},{}).spawned.front();
    require(moving_control.change_structure(0,{{kinematic_tid,{}}},{}).spawned.front()==mover,"Kinematic fixture IDs differ.");
    KinematicTarget motion;motion.entity=mover;motion.position={2,10,0};motion.duration_ticks=20;
    moving.step(2,{}, {motion});moving_control.step(2,{}, {motion});
    const auto moving_before=moving.save_snapshot(hash);
    rejected=false;try {(void)moving.step(4,{}, {}, {}, {},{{1,1,{}, {mover}},{3,1,{{logical_tid,{}}},{}}});}catch(const std::exception&) {rejected=true;}
    require(rejected && moving.save_snapshot(hash)==moving_before,"Failed removal lost existing kinematic target/state.");
    moving.step(2,{});moving_control.step(2,{});
    require(moving.save_snapshot(hash)==moving_control.save_snapshot(hash),"Restored kinematic failed to continue its original motion.");
    const auto conflict_before=moving.save_snapshot(hash);
    rejected=false;try {(void)moving.step(1,{}, {motion}, {}, {},{{0,1,{}, {mover}}});}catch(const std::exception&) {rejected=true;}
    require(rejected && moving.save_snapshot(hash)==conflict_before,"Same-tick explicit motion and removal did not reject atomically.");
    const auto removed=moving.step(2,{}, {}, {}, {},{{1,1,{}, {mover}}});
    require(removed.size()==1 && moving.inspect().entities==0,"Later retirement of ongoing motion was rejected or retained membership.");
    // Trigger the real Jolt pair/contact overflow after a structural publication,
    // rather than failing a request validator before physics runs.
    Runtime overflow(d),overflow_control(d);std::vector<RuntimeSpawnRequest> crowded(150,RuntimeSpawnRequest{tid,{}});
    const auto overflow_before=overflow.save_snapshot(hash);std::string error;
    try {(void)overflow.step(3,{}, {}, {}, {},{{1,0,crowded,{}}});}catch(const std::exception& e) {error=e.what();}
    require(error.find("Jolt physics capacity/update error")!=std::string::npos,"Scheduled crowd did not reach real Jolt update failure.");
    require(overflow.save_snapshot(hash)==overflow_before && overflow.inspect().bodies==0,"Physics failure retained published structural state.");
    require(overflow.change_structure(0,{{tid,{}}},{}).spawned==overflow_control.change_structure(0,{{tid,{}}},{}).spawned,"Physics failure consumed generated IDs.");
    overflow.step(3,{});overflow_control.step(3,{});
    require(overflow.save_snapshot(hash)==overflow_control.save_snapshot(hash),"Physics failure left stale native body allocation state.");
}

void scheduled_character_support() {
    auto d=fixture();for(auto& e:d.entities)if(e.character)e.transform.position={0,4,2};
    RuntimeSpawnTemplate platform;platform.id=std::string(32,'a');platform.name="Support";
    platform.collider=BoxCollider{};platform.transform.position={0,2,2};platform.transform.scale={4,1,4};d.templates.push_back(platform);
    Runtime source(d),control(d);const auto support=source.change_structure(0,{{platform.id,{}}},{}).spawned.front();
    control.change_structure(0,{{platform.id,{}}},{});source.step(120,{});control.step(120,{});
    require(source.entity("player").ground=="on_ground" && source.entity("player").local.position[1]>2,"Scheduled support fixture failed to settle.");
    const std::string hash(64,'a');const auto before=source.save_snapshot(hash);bool rejected=false;
    try {(void)source.step(4,{}, {}, {}, {},{{0,1,{}, {support}},{3,1,{{platform.id,{}}},{}}});}
    catch(const std::exception&) {rejected=true;}
    require(rejected && source.save_snapshot(hash)==before && source.entity("player").ground=="on_ground",
        "Later failure did not restore character support and platform membership.");
    RuntimeInput jump;jump.entity="player";jump.jump=true;source.step(5,{jump});control.step(5,{jump});
    require(source.save_snapshot(hash)==control.save_snapshot(hash),"Restored support changed later character jump or contact state.");
}
void equal(const RuntimeEntityState& a,const RuntimeEntityState& b) {
    if(a.world!=b.world || a.velocity!=b.velocity || a.ground!=b.ground)
        std::cerr<<"Mismatch for "<<a.id<<": positions "<<a.world[12]<<','<<a.world[13]<<','<<a.world[14]
            <<" / "<<b.world[12]<<','<<b.world[13]<<','<<b.world[14]<<" ground "<<a.ground<<" / "<<b.ground<<'\n';
    require(a.world==b.world && a.velocity==b.velocity && a.ground==b.ground && a.yaw==b.yaw && a.pitch==b.pitch,"Same-build deterministic state differs.");
}
void camera_free_characters() {
    auto definition=fixture();
    for(auto& entity:definition.entities)if(entity.character)entity.character->camera.clear();
    Runtime free(definition),camera_bound(fixture());
    free.step(120,{});camera_bound.step(120,{});equal(free.entity("player"),camera_bound.entity("player"));
    RuntimeInput walk;walk.entity="player";walk.move={0,1};
    free.step(90,{walk});for(unsigned i=0;i<90;++i)camera_bound.step(1,{walk});
    equal(free.entity("player"),camera_bound.entity("player"));
    require(free.entity("player").world[14]>-2.5 && free.entity("player").world[14]<-2.3,"Camera-free actor passed through wall.");
    RuntimeInput jump;jump.entity="player";jump.jump=true;
    free.step(10,{jump});camera_bound.step(1,{jump});camera_bound.step(9,{});
    equal(free.entity("player"),camera_bound.entity("player"));require(free.entity("player").world[13]>.5,"Camera-free actor did not jump.");
    free.step(120,{});camera_bound.step(120,{});
    RuntimeInput look;look.entity="player";look.look={90,180};
    free.step(4,{look});camera_bound.step(1,{look});camera_bound.step(3,{});
    equal(free.entity("player"),camera_bound.entity("player"));
    require(free.entity("player").yaw==90 && free.entity("player").pitch==85,"Camera-free actor lost look or clamp semantics.");
    free.step(6,{walk});camera_bound.step(6,{walk});equal(free.entity("player"),camera_bound.entity("player"));
    require(std::abs(free.entity("player").velocity[0])>1,"Camera-free actor did not move using heading.");
    free.step(1,{});camera_bound.step(1,{});equal(free.entity("player"),camera_bound.entity("player"));
    require(std::abs(free.entity("player").velocity[0])<1e-6 && std::abs(free.entity("player").velocity[2])<1e-6,"Neutral actor retained horizontal input.");
    const std::string hash(64,'b');const auto saved=free.save_snapshot(hash);
    auto restored=Runtime::from_snapshot(definition,hash,saved);
    equal(free.entity("player"),restored->entity("player"));
    RuntimeInput resume;resume.entity="player";resume.move={.25f,.75f};resume.look={-15,-10};
    free.step(12,{resume});restored->step(12,{resume});
    require(free.save_snapshot(hash)==restored->save_snapshot(hash),"Camera-free fresh restore continuation differs.");
    const auto before=free.save_snapshot(hash);bool rejected=false;
    try {free.step(4,{look},{},{},{},{{2,1,{}, {"no-spawn"}}});}catch(const std::exception&) {rejected=true;}
    require(rejected && free.save_snapshot(hash)==before,"Camera-free later failure did not restore yaw/physics/support.");
    // Two autonomous actors share no camera, while a separate player retains
    // its bound camera. Neutral actors must not inherit caller movement.
    auto multi=fixture();RuntimeEntityDefinition npc;npc.id="npc";npc.transform.position={6,1,2};npc.character=CharacterController{};
    multi.entities.push_back(npc);auto another=npc;another.id="npc2";another.transform.position={-6,1,2};multi.entities.push_back(another);
    Runtime independent(multi);independent.step(120,{});
    const auto npc_pose=independent.entity("npc").world,npc2_pose=independent.entity("npc2").world;
    independent.step(10,{walk});
    require(independent.entity("npc").world==npc_pose && independent.entity("npc2").world==npc2_pose,"Camera-free actors inherited player input.");
    auto malformed=fixture();for(auto& entity:malformed.entities)if(entity.id=="camera")entity.parent.clear();
    rejected=false;try {Runtime invalid(malformed);}catch(const std::exception&) {rejected=true;}
    require(rejected,"Supplied non-child camera accepted after optional camera change.");
    malformed=fixture();for(auto& entity:malformed.entities)if(entity.id=="camera")entity.camera.reset();
    rejected=false;try {Runtime invalid(malformed);}catch(const std::exception&) {rejected=true;}
    require(rejected,"Supplied non-camera entity accepted.");
}
void camera_state() {
    auto definition=fixture();
    for(auto& entity:definition.entities)if(entity.camera)entity.camera=RuntimeCamera{81,.2,250};
    RuntimeEntityDefinition other;other.id="other-camera";other.camera=RuntimeCamera{35,.05,600};other.transform.position={3,4,5};
    RuntimeEntityDefinition light;light.id="camera-light";light.parent="camera";light.transform.position={.5,0,-1};
    light.light=Light{};light.light->kind=LightKind::spot;light.light->range=12;light.light->intensity=3;
    RuntimeEntityDefinition environment;environment.id="environment";environment.environment=LightingEnvironment{};
    environment.environment->ambient={.2f,.3f,.4f};environment.environment->exposure=1.3f;
    definition.entities.insert(definition.entities.end(),{other,light,environment});
    Runtime source(definition);
    const auto check=[&](Runtime& runtime,const char* id) {
        const auto view=runtime.camera_state(id);const auto full=runtime.snapshot(id);
        require(view.camera_world==full.camera_world && view.camera_world==runtime.entity(id).world &&
            view.vertical_fov==full.vertical_fov && view.near_plane==full.near_plane && view.far_plane==full.far_plane,
            "Lightweight camera projection differs from live/full snapshot.");
        require(view.lighting.preview==full.lighting.preview && !view.lighting.preview &&
            view.lighting.environment.ambient==full.lighting.environment.ambient &&
            view.lighting.environment.exposure==full.lighting.environment.exposure &&
            view.lighting.lights.size()==1 && full.lighting.lights.size()==1,
            "Lightweight camera lost the current lighting environment.");
        const auto& value=view.lighting.lights.front();const auto& expected=full.lighting.lights.front();
        require(value.entity_id==light.id && value.entity_id==expected.entity_id &&
            value.position==expected.position && value.direction==expected.direction &&
            value.light.kind==expected.light.kind && value.light.color==expected.light.color &&
            value.light.intensity==expected.light.intensity && value.light.range==expected.light.range,
            "Lightweight camera light values differ from the full snapshot.");
        const auto world=runtime.entity(light.id).world;
        require(value.position==std::array<double,3>{world[12],world[13],world[14]},
            "Lightweight camera used an old light transform.");
        return view;
    };
    const auto initial=check(source,"camera"),alternate=check(source,"other-camera");
    require(initial.vertical_fov==81 && initial.near_plane==.2 && initial.far_plane==250 &&
        alternate.vertical_fov==35 && alternate.near_plane==.05 && alternate.far_plane==600 &&
        alternate.camera_world!=initial.camera_world,"Camera selection reused another camera's lens or pose.");
    auto changed_definition=definition;
    for(auto& entity:changed_definition.entities) {
        if(entity.id=="camera")entity.camera=RuntimeCamera{50,.3,400};
        if(entity.light)entity.light->intensity=8;
    }
    Runtime changed(changed_definition);const auto changed_view=check(changed,"camera");
    require(changed_view.vertical_fov==50 && changed_view.near_plane==.3 && changed_view.far_plane==400 &&
        changed_view.lighting.lights[0].light.intensity==8 && check(source,"camera").vertical_fov==81 &&
        check(source,"camera").lighting.lights[0].light.intensity==3,
        "A different runtime retained old lens/light values or changed its neighbor's state.");
    source.step(120,{});
    RuntimeInput look;look.entity="player";look.move={.25f,.5f};look.look={35,-10};source.step(6,{look});
    const auto moved=check(source,"camera");
    require(moved.camera_world!=initial.camera_world && moved.lighting.lights[0].position!=initial.lighting.lights[0].position &&
        moved.lighting.lights[0].direction!=initial.lighting.lights[0].direction,
        "Camera fixture did not exercise actual current controller/light motion.");
    require(initial.camera_world!=source.entity("camera").world && initial.vertical_fov==81,
        "Later simulation mutated an earlier owned camera state.");
    const std::string hash(64,'c');const auto checkpoint=source.save_snapshot(hash);
    auto restored=Runtime::from_snapshot(definition,hash,checkpoint);
    const auto loaded=check(*restored,"camera");
    require(loaded.camera_world==moved.camera_world && loaded.vertical_fov==moved.vertical_fov &&
        loaded.lighting.lights[0].position==moved.lighting.lights[0].position &&
        loaded.lighting.lights[0].direction==moved.lighting.lights[0].direction,
        "Fresh checkpoint restore changed lightweight camera/light state.");
    require(source.presentation_source_id()!=restored->presentation_source_id(),"Restore did not create a fresh presentation source.");
    look.look={-15,5};source.step(3,{look});restored->step(3,{look});
    require(check(source,"camera").camera_world==check(*restored,"camera").camera_world,
        "Restored camera projection continuation differs.");
    const auto reject_both=[](Runtime& runtime,const std::string& id,const std::string& expected) {
        std::string lightweight,full;
        try {(void)runtime.camera_state(id);}catch(const std::runtime_error& error){lightweight=error.what();}
        try {(void)runtime.snapshot(id);}catch(const std::runtime_error& error){full=error.what();}
        require(lightweight==expected && full==expected,"Lightweight/full camera validation diverged.");
    };
    const auto before=source.save_snapshot(hash);
    reject_both(source,"missing","Runtime entity does not exist.");
    reject_both(source,"floor","Runtime entity has no Camera component.");
    require(source.save_snapshot(hash)==before,"Rejected camera read mutated native state.");
    RuntimeDefinition scaled;scaled.world_id="scaled-camera";
    RuntimeEntityDefinition parent;parent.id="parent";parent.transform.scale={2,1,1};
    RuntimeEntityDefinition child;child.id="camera";child.parent=parent.id;child.camera=RuntimeCamera{};
    scaled.entities={parent,child};Runtime nonrigid(scaled);
    reject_both(nonrigid,child.id,"Runtime camera hierarchy must not scale or shear the camera.");
}
}
int main() {
    try {
        lifecycle();scheduled_lifecycle();scheduled_character_support();camera_free_characters();camera_state();
        const auto definition=fixture(); Runtime a(definition), b(definition);
        {
            RuntimeDefinition source;source.world_id="template-only";
            RuntimeSpawnTemplate recipe;recipe.id=std::string(32,'1');recipe.name="Frozen crate";recipe.collider=BoxCollider{};recipe.mesh=RuntimeMesh{};
            source.templates.push_back(recipe);Runtime frozen(source);
            source.templates.front().name="Edited authored recipe";source.templates.front().transform.position[0]=9;
            require(frozen.inspect().entities==0 && frozen.inspect().bodies==0,"Standalone template created a live entity or body.");
            require(frozen.spawn_templates().front().name=="Frozen crate" && frozen.spawn_templates().front().transform.position[0]==0,"Runtime recipe changed with its source definition.");
            require(frozen.snapshot().objects.empty(),"Empty live world rendered a template instance.");
            const auto created=frozen.change_structure(0,{{recipe.id,{}}},{}).spawned.front();
            const auto visible=frozen.snapshot();
            require(visible.objects.size()==1 && visible.objects.front().entity_id==created,"Camera-independent live snapshot omitted a spawned prop.");
            frozen.change_structure(1,{}, {created});
            require(frozen.snapshot().objects.empty(),"Live snapshot retained a removed prop.");
            require(visible.objects.size()==1 && visible.objects.front().entity_id==created,"Structural removal invalidated an earlier render snapshot.");
        }
        const auto membership=a.inspect();
        require(membership.entities==5 && membership.bodies==4 && membership.characters==1,"Live membership counts omit a native owner or controller body.");
        a.step(120,{}); b.step(60,{}); b.step(60,{});
        equal(a.entity("player"),b.entity("player")); equal(a.entity("box"),b.entity("box"));
        const auto landed=a.entity("player");
        require(landed.ground=="on_ground" && std::abs(landed.world[13])<0.05,"Player did not land.");
        require(std::abs(a.entity("box").world[13]-0.48)<0.04,"Box did not settle.");
        RuntimeInput walk; walk.entity="player"; walk.move={0,1};
        a.step(180,{walk}); b.step(90,{walk}); b.step(90,{walk});
        equal(a.entity("player"),b.entity("player"));
        require(a.entity("player").world[14]>-2.5 && a.entity("player").world[14]<-2.3,"Wall collision failed.");
        RuntimeInput jump; jump.entity="player"; jump.jump=true;
        a.step(10,{jump}); b.step(1,{jump}); b.step(9,{});
        equal(a.entity("player"),b.entity("player")); require(a.entity("player").world[13]>0.5,"Jump failed.");
        a.step(120,{}); b.step(120,{}); equal(a.entity("player"),b.entity("player"));
        RuntimeInput look; look.entity="player"; look.look={45,20};
        a.step(1,{look}); b.step(1,{look}); equal(a.entity("camera"),b.entity("camera"));
        require(a.entity("player").yaw==45 && a.entity("player").pitch==20,"Look input failed.");
        const auto snapshot=a.snapshot("camera");
        require(snapshot.revision==7 && snapshot.camera_world==a.entity("camera").world,"Snapshot lost source identity or camera pose.");
        const auto before=a.entity("player"); const auto tick=a.inspect().tick;
        bool rejected=false;
        try { a.step(60,{walk,walk}); } catch(const std::runtime_error&) { rejected=true; }
        require(rejected && a.inspect().tick==tick,"Duplicate inputs were not rejected before advancing."); equal(before,a.entity("player"));
        auto invalid=definition; invalid.entities.front().character->camera="absent";
        rejected=false; try { Runtime bad(invalid); } catch(const std::runtime_error&) { rejected=true; }
        require(rejected,"Invalid camera reference accepted.");
        // Fail after both rigid bodies and a Character have been installed.
        // Teardown must use the complete ownership inventory, not a partially
        // constructed presentation/body lookup table.
        auto late_failure=definition;RuntimeEntityDefinition bad_tail;
        bad_tail.id="zz-invalid-body";bad_tail.collider=BoxCollider{};bad_tail.collider->mass=0;
        late_failure.entities.push_back(bad_tail);
        for(unsigned attempt=0;attempt<3;++attempt) {
            rejected=false;try {Runtime bad(late_failure);}catch(const std::runtime_error&) {rejected=true;}
            require(rejected,"Late invalid body material was accepted.");
        }
        // Exercise the rollback path after Jolt has started a real update:
        // overlapping bodies exceed the configured contact/pair capacities.
        RuntimeDefinition crowded; crowded.world_id="capacity-fixture";
        for(int i=0;i<150;++i) { RuntimeEntityDefinition e; e.id="body-"+std::to_string(i); e.collider=BoxCollider{}; e.collider->motion=BodyMotion::Dynamic; crowded.entities.push_back(e); }
        Runtime overflow(crowded);
        std::vector<RuntimeEntityState> original;
        for(const auto& e:crowded.entities) original.push_back(overflow.entity(e.id));
        rejected=false; try { overflow.step(2,{}); } catch(const std::runtime_error&) { rejected=true; }
        require(rejected,"Capacity fixture did not reach the expected Jolt update failure.");
        require(overflow.inspect().tick==0,"Failed physics batch advanced the clock.");
        for(const auto& state:original) equal(state,overflow.entity(state.id));
        // A new runtime still works after failed construction and update.
        Runtime c(definition); c.step(120,{}); equal(landed,c.entity("player"));
        std::cout << "Physics landing, wall collision, jump, look, chunked deterministic replay, snapshot identity, input guards, scheduled lifecycle ownership/revision rollback and real capacity rollback passed.\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
