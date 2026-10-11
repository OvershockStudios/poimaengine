// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include <cmath>
#include <algorithm>
#include <limits>
#include "poima/assets.hpp"
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
void material_adoption() {
    const auto id=[](const std::string& name) {
        const std::vector<std::string> names={"box","camera","floor","hidden","legacy","player","wall","missing"};
        const auto found=std::find(names.begin(),names.end(),name);
        require(found!=names.end(),"Unknown fixture identity.");
        return std::string(31,'e')+std::to_string(1+std::distance(names.begin(),found));
    };
    auto definition=fixture();
    for(auto& entity:definition.entities) {
        entity.id=id(entity.id);if(!entity.parent.empty())entity.parent=id(entity.parent);
        if(entity.character)entity.character->camera=id(entity.character->camera);
    }
    auto mesh=std::make_shared<MeshAsset>();mesh->has_uv=true;
    mesh->vertices={{{-1,0,0},{0,1,0},{0,0}},{{1,0,0},{0,1,0},{1,0}},{{0,0,1},{0,1,0},{0,1}}};
    mesh->indices={0,2,1};
    auto image=std::make_shared<TextureImage>();image->srgb=true;image->mips={{1,1,{255,255,255,255}}};
    auto textures=std::make_shared<MaterialTextures>();textures->maps[0].image=image;
    for(auto& entity:definition.entities)if(entity.id==id("floor") || entity.id==id("box")) {
        entity.mesh=RuntimeMesh{};entity.mesh->material=PbrMaterial{};
        entity.mesh->mesh=mesh;entity.mesh->textures=textures;
    }
    RuntimeEntityDefinition hidden;hidden.id=id("hidden");hidden.mesh=RuntimeMesh{};
    hidden.mesh->material=PbrMaterial{};hidden.mesh->visible=false;definition.entities.push_back(hidden);
    RuntimeEntityDefinition legacy;legacy.id=id("legacy");legacy.mesh=RuntimeMesh{};definition.entities.push_back(legacy);
    const auto schema=components::parse_schema("{\"id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"name\":\"Counter\",\"version\":1,\"fields\":[{\"id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"name\":\"Count\",\"kind\":\"int32\",\"default\":0}]}");
    definition.component_schemas={schema};definition.entities[0].components[schema.id]=components::defaults(schema);
    RuntimeSpawnTemplate prop;prop.id=std::string(32,'c');prop.name="PBR spawn";prop.mesh=RuntimeMesh{};
    prop.mesh->material=PbrMaterial{};definition.templates={prop};
    Runtime runtime(definition);runtime.step(2,{});
    const std::string hash(64,'a');const auto original=runtime.save_snapshot(hash);
    const auto old=runtime.snapshot(id("camera"));const auto original_source=runtime.presentation_source_id();
    const auto player=runtime.entity(id("player")),body=runtime.entity(id("box"));
    const auto old_component=runtime.component_read(schema.id,id("player"));
    const auto material=[](const SceneSnapshot& snapshot,const std::string& id)->const SceneObject& {
        for(const auto& value:snapshot.objects)if(value.entity_id==id)return value;
        throw std::runtime_error("Material snapshot omitted drawable.");
    };
    PbrMaterial changed;changed.base_color={.25f,.5f,.75f};changed.emissive={.1f,.2f,.3f};changed.metallic=.4f;changed.roughness=.6f;
    const std::vector<RuntimeMaterialEdit> edits={{id("box"),changed},{id("floor"),changed},{id("hidden"),changed}};
    auto caller_edits=edits;auto prepared=runtime.prepare_materials(0,8,caller_edits);
    caller_edits[0].material.base_color={0,0,0};
    require(runtime.save_snapshot(hash)==original && runtime.snapshot().revision==7 && runtime.material_revision()==0,
        "Material preparation changed live state.");
    auto moved=std::move(prepared);
    require(!runtime.commit_materials(std::move(prepared)),"Moved-from material stage committed.");
    require(runtime.commit_materials(std::move(moved)),"Valid material stage did not commit.");
    const auto now=runtime.snapshot(id("camera"));
    require(now.revision==8 && now.material_revision==1 && now.presentation_generation==1 &&
        now.presentation_source_id==original_source && runtime.inspect().tick==2 && runtime.structure_revision()==0 &&
        runtime.component_revision()==0 && runtime.gameplay_revision()==0 && runtime.control_sequence()==0,
        "Material adoption changed unrelated identity/revisions.");
    for(const auto& object_id:{id("box"),id("floor")}) {
        const auto& current=material(now,object_id);const auto& previous=material(old,object_id);
        require(current.material->base_color==changed.base_color && current.material->emissive==changed.emissive &&
            current.material->metallic==changed.metallic && current.material->roughness==changed.roughness,
            "Material scalar publication lost a field.");
        require(previous.material->base_color==PbrMaterial{}.base_color && previous.material->emissive==PbrMaterial{}.emissive &&
            previous.material->metallic==1 && previous.material->roughness==1 && old.revision==7 && old.material_revision==0 &&
            old.presentation_generation==0,"Published adoption mutated an earlier snapshot.");
        require(current.mesh==mesh && current.textures==textures && previous.mesh==mesh && previous.textures==textures &&
            current.world==previous.world && current.albedo==previous.albedo && current.incarnation==previous.incarnation &&
            current.cast_shadows==previous.cast_shadows && current.receive_shadows==previous.receive_shadows,
            "Material adoption replaced resources or unrelated drawable state.");
    }
    const auto hidden_live=std::find_if(runtime.live_definition().entities.begin(),runtime.live_definition().entities.end(),
        [&](const auto& entity){return entity.id==id("hidden");});
    require(hidden_live!=runtime.live_definition().entities.end() && hidden_live->mesh->material->roughness==changed.roughness &&
        !hidden_live->mesh->visible && now.objects.size()==old.objects.size(),"Hidden material adoption changed visibility.");
    const auto same_state=[](RuntimeEntityState a,RuntimeEntityState b) {
        return a.id==b.id && a.world==b.world && a.local.position==b.local.position && a.local.rotation==b.local.rotation &&
            a.local.scale==b.local.scale && a.velocity==b.velocity && a.ground==b.ground && a.yaw==b.yaw && a.pitch==b.pitch &&
            a.has_body==b.has_body && a.is_character==b.is_character && a.motion==b.motion &&
            a.motion_remaining_ticks==b.motion_remaining_ticks;
    };
    require(same_state(player,runtime.entity(id("player"))) && same_state(body,runtime.entity(id("box"))) &&
        old_component==runtime.component_read(schema.id,id("player")),"Material adoption changed controller/body/custom state.");
    // Complete logical checkpoint payload differs only in adopted authored revision.
    const auto payload=[](const std::string& bytes) {
        const auto begin=bytes.find("\"payload\":")+10;const auto end=bytes.find(",\"sha256\":",begin);
        require(begin>=10 && end!=std::string::npos,"Unexpected save envelope ordering.");return bytes.substr(begin,end-begin);
    };
    auto adopted=runtime.save_snapshot(hash);auto normalized=payload(adopted);
    const auto revision=normalized.find("\"authored_revision\":8");require(revision!=std::string::npos,"Save did not adopt material revision.");
    normalized.replace(revision,std::string("\"authored_revision\":8").size(),"\"authored_revision\":7");
    require(normalized==payload(original),"Material adoption changed another logical checkpoint field.");
    auto target=definition;target.authored_revision=8;
    for(auto& entity:target.entities)for(const auto& edit:edits)if(entity.id==edit.id)entity.mesh->material=edit.material;
    auto restored=Runtime::from_snapshot(target,hash,adopted);
    require(restored->save_snapshot(hash)==adopted && restored->material_revision()==0 &&
        material(restored->snapshot(),id("box")).material->roughness==changed.roughness,
        "Fresh restore lost adopted baseline or persisted an ephemeral epoch.");
    const auto reject=[&](std::uint64_t expected,std::uint64_t revision,const std::vector<RuntimeMaterialEdit>& bad) {
        const auto before=runtime.save_snapshot(hash);const auto epoch=runtime.material_revision();bool rejected=false;
        try { auto stage=runtime.prepare_materials(expected,revision,bad);(void)stage; }catch(const std::exception&) {rejected=true;}
        require(rejected && runtime.save_snapshot(hash)==before && runtime.material_revision()==epoch,
            "Rejected material preparation mutated native state.");
    };
    reject(0,9,edits);reject(1,8,edits);reject(1,9007199254740992ULL,edits);reject(1,9,{});
    reject(1,9,{{id("missing"),changed}});reject(1,9,{{id("camera"),changed}});reject(1,9,{{id("legacy"),changed}});
    reject(1,9,{{id("floor"),changed},{id("box"),changed}});reject(1,9,{{id("box"),changed},{id("box"),changed}});
    reject(1,9,std::vector<RuntimeMaterialEdit>(33,{id("box"),changed}));
    auto invalid=changed;invalid.double_sided=true;reject(1,9,{{id("box"),invalid}});
    for(const float bad:{-.01f,1.01f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        invalid=changed;invalid.base_color[2]=bad;reject(1,9,{{id("box"),changed},{id("floor"),invalid}});
        invalid=changed;invalid.emissive[1]=bad;reject(1,9,{{id("box"),invalid}});
        invalid=changed;invalid.metallic=bad;reject(1,9,{{id("box"),invalid}});
        invalid=changed;invalid.roughness=bad;reject(1,9,{{id("box"),invalid}});
    }
    Runtime other(target);auto wrong_owner=runtime.prepare_materials(1,9,edits);
    require(!other.commit_materials(std::move(wrong_owner)) && other.material_revision()==0,"Foreign material stage committed.");
    auto stale_tick=runtime.prepare_materials(1,9,edits);runtime.step(1,{});
    require(!runtime.commit_materials(std::move(stale_tick)) && runtime.material_revision()==1,"Stale-tick material stage committed.");
    auto stale_component=runtime.prepare_materials(1,9,edits);
    runtime.component_edit(schema.id,id("player"),components::parse_values(schema,"{\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\":5}"));
    require(!runtime.commit_materials(std::move(stale_component)) && runtime.material_revision()==1,"Stale-component stage committed.");
    auto stale_structure=runtime.prepare_materials(1,9,edits);
    const auto spawned=runtime.change_structure(0,{{prop.id,{}}},{}).spawned.front();
    require(!runtime.commit_materials(std::move(stale_structure)),"Changed membership accepted an old material stage.");
    reject(1,9,{{spawned,changed}});
    auto spawned_save=runtime.save_snapshot(hash);auto expanded=Runtime::from_snapshot(target,hash,spawned_save);
    bool reject_restored_spawn=false;
    try {auto stage=expanded->prepare_materials(0,9,{{spawned,changed}});(void)stage;}catch(const std::exception&) {reject_restored_spawn=true;}
    require(reject_restored_spawn,"Restore expansion misclassified spawned member as authored.");
    runtime.change_structure(1,{}, {spawned});reject(1,9,{{spawned,changed}});
    GameplaySaveLedger ledger;runtime.gameplay_save_host({1,2},&ledger);
    require(runtime.gameplay_saves().configure(true,1),"Cannot configure material save guard.");
    auto pending_stage=runtime.prepare_materials(1,9,edits);
    const auto queued=runtime.gameplay_saves().enqueue(GameplaySaveKind::save,"material",std::nullopt,false,runtime.inspect().tick);
    require(queued.rejection==GameplaySaveRejection::none && runtime.gameplay_saves().commit(runtime.inspect().tick) &&
        runtime.gameplay_saves().mark_dispatched(queued.ticket),"Cannot dispatch material Save guard.");
    require(!runtime.commit_materials(std::move(pending_stage)),"Pending dispatched Save accepted prepared adoption.");
    reject(1,9,edits);require(runtime.gameplay_saves().clear(queued.ticket),"Cannot clear test Save intent.");
    auto one=runtime.prepare_materials(1,9,edits),two=runtime.prepare_materials(1,9,edits);
    require(runtime.commit_materials(std::move(one)) && !runtime.commit_materials(std::move(two)) &&
        runtime.material_revision()==2,"Duplicate stages applied twice.");
    // Boundary-sized batch succeeds without reallocating or replacing owners at commit.
    RuntimeDefinition bounded;bounded.world_id="material-boundary";bounded.authored_revision=1;
    std::vector<RuntimeMaterialEdit> boundary;
    for(unsigned i=0;i<32;++i) {RuntimeEntityDefinition entity;entity.id="drawable-"+std::to_string(100+i);
        entity.mesh=RuntimeMesh{};entity.mesh->material=PbrMaterial{};bounded.entities.push_back(entity);boundary.push_back({entity.id,changed});}
    Runtime maximum(bounded);auto maximum_stage=maximum.prepare_materials(0,2,boundary);
    require(maximum.commit_materials(std::move(maximum_stage)) && maximum.snapshot().objects.size()==32 && maximum.material_revision()==1,
        "The 32-drawable material boundary did not commit.");
    // A real failed post-adoption Jolt update rolls back to the adopted root.
    RuntimeDefinition crowded;crowded.world_id="material-rollback";crowded.authored_revision=1;
    for(int i=0;i<150;++i) {RuntimeEntityDefinition e;e.id="body-"+std::to_string(i);e.collider=BoxCollider{};
        e.collider->motion=BodyMotion::Dynamic;if(i==0) {e.mesh=RuntimeMesh{};e.mesh->material=PbrMaterial{};}crowded.entities.push_back(e);}
    Runtime rollback(crowded);auto stage=rollback.prepare_materials(0,2,{{"body-0",changed}});
    require(rollback.commit_materials(std::move(stage)),"Cannot stage rollback fixture material.");
    const auto before_failure=rollback.save_snapshot(hash);bool failed=false;
    try {rollback.step(2,{});}catch(const std::exception&) {failed=true;}
    require(failed && rollback.save_snapshot(hash)==before_failure && rollback.material_revision()==1 &&
        rollback.snapshot().revision==2 && material(rollback.snapshot(),"body-0").material->base_color==changed.base_color,
        "Failed Tick restored pre-adoption material/topology.");
}
void renderer_shadow_controls() {
    // Defaults preserve existing aggregate users and ordinary visible geometry.
    const SceneObject legacy{"legacy",identity_matrix(),{1,1,1},{},{},{}};
    require(legacy.cast_shadows && legacy.receive_shadows,"Legacy SceneObject defaults changed.");
    RuntimeDefinition definition;definition.world_id="shadow-controls";
    RuntimeEntityDefinition camera;camera.id="camera";camera.camera=RuntimeCamera{};
    RuntimeEntityDefinition ordinary;ordinary.id="ordinary";ordinary.mesh=RuntimeMesh{};
    RuntimeEntityDefinition no_cast=ordinary;no_cast.id="no-cast";no_cast.mesh->cast_shadows=false;
    RuntimeEntityDefinition no_receive=ordinary;no_receive.id="no-receive";no_receive.mesh->receive_shadows=false;
    RuntimeEntityDefinition hidden=ordinary;hidden.id="hidden";hidden.mesh->visible=false;
    hidden.mesh->cast_shadows=false;hidden.mesh->receive_shadows=false;
    definition.entities={camera,ordinary,no_cast,no_receive,hidden};
    RuntimeSpawnTemplate prop;prop.id=std::string(32,'1');prop.name="No-cast prop";
    prop.mesh=RuntimeMesh{};prop.mesh->cast_shadows=false;definition.templates.push_back(prop);
    RuntimeSpawnTemplate hierarchy;hierarchy.id=std::string(32,'2');hierarchy.name="No-receive hierarchy";
    RuntimeEntityDefinition local;local.id=std::string(32,'3');local.mesh=RuntimeMesh{};local.mesh->receive_shadows=false;
    hierarchy.root=local.id;hierarchy.entities={local};definition.templates.push_back(hierarchy);
    const auto frozen_definition=definition;Runtime runtime(definition);
    definition.entities[2].mesh->cast_shadows=true;definition.templates[0].mesh->cast_shadows=true;
    definition.templates[1].entities[0].mesh->receive_shadows=true;
    const auto object=[](const SceneSnapshot& scene,const std::string& id)->const SceneObject& {
        for(const auto& item:scene.objects)if(item.entity_id==id)return item;
        throw std::runtime_error("Shadow-control snapshot object missing.");
    };
    const auto check=[&](const SceneSnapshot& scene) {
        require(object(scene,"ordinary").cast_shadows && object(scene,"ordinary").receive_shadows,
            "Default runtime shadow controls changed.");
        require(!object(scene,"no-cast").cast_shadows && object(scene,"no-cast").receive_shadows,
            "Runtime cast-only control lost or coupled to receive.");
        require(object(scene,"no-receive").cast_shadows && !object(scene,"no-receive").receive_shadows,
            "Runtime receive-only control lost or coupled to cast.");
        for(const auto& item:scene.objects)require(item.entity_id!="hidden","Invisible mesh entered render snapshot.");
    };
    const auto before=runtime.snapshot("camera");require(before.objects.size()==3,"Shadow flags suppressed visible geometry.");check(before);
    const auto legacy_id=runtime.change_structure(0,{{prop.id,{}}},{}).spawned.front();
    const auto hierarchy_id=runtime.change_structure(1,{{hierarchy.id,{}}},{}).spawned.front();
    auto live=runtime.snapshot();check(live);
    require(!object(live,legacy_id).cast_shadows && object(live,legacy_id).receive_shadows,
        "Legacy frozen spawn template lost independent shadow controls.");
    require(object(live,hierarchy_id).cast_shadows && !object(live,hierarchy_id).receive_shadows,
        "Hierarchical frozen spawn template lost independent shadow controls.");
    runtime.step(1,{});const std::string hash(64,'d');const auto saved=runtime.save_snapshot(hash);
    auto restored=Runtime::from_snapshot(frozen_definition,hash,saved);const auto after=restored->snapshot("camera");check(after);
    require(!object(after,legacy_id).cast_shadows && !object(after,hierarchy_id).receive_shadows,
        "Restore did not recover frozen renderer controls for both template forms.");
    require(saved==restored->save_snapshot(hash),"Shadow-control restore changed logical save payload.");
    runtime.change_structure(2,{}, {legacy_id,hierarchy_id});
    require(runtime.snapshot().objects.size()==3 && before.objects.size()==3 && live.objects.size()==5,
        "Shadow controls or lifecycle changed immutable snapshot membership.");
    check(before);check(live);
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
        material_adoption();renderer_shadow_controls();lifecycle();scheduled_lifecycle();scheduled_character_support();camera_free_characters();camera_state();
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
