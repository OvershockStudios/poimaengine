// SPDX-License-Identifier: Apache-2.0
// Portable graph/selection and real Runtime immutable presentation lifetimes.
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
namespace {
using Json=nlohmann::json;
const std::string hash(64,'a');
std::string id(unsigned n) {auto value=std::to_string(n);return std::string(32-value.size(),'0')+value;}
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F&& f) {bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"Invalid camera draw operation succeeded.");}
const SceneObject& object(const std::vector<SceneObject>& objects,const std::string& entity) {
    const auto found=std::find_if(objects.begin(),objects.end(),[&](const auto& value){return value.entity_id==entity;});
    check(found!=objects.end(),"Expected presentation object is absent.");return *found;
}
bool contains(const std::vector<SceneObject>& objects,const std::string& entity) {
    return std::any_of(objects.begin(),objects.end(),[&](const auto& value){return value.entity_id==entity;});
}
RuntimeSpawnTemplate recipe();
RuntimeDefinition definition() {
    RuntimeDefinition d;d.world_id="camera-draw-native";d.authored_revision=1;
    RuntimeEntityDefinition first;first.id=id(1);first.camera=RuntimeCamera{60,.1,100,SceneLens{40,.01,10}};
    RuntimeEntityDefinition second;second.id=id(2);second.camera=RuntimeCamera{75,.2,200,SceneLens{50,.02,20}};
    RuntimeEntityDefinition arms;arms.id=id(3);arms.parent=first.id;arms.mesh=RuntimeMesh{};arms.mesh->material=PbrMaterial{};
    arms.camera_draw=CameraDrawBinding{first.id,CameraDrawMode::Viewmodel};
    RuntimeEntityDefinition weapon=arms;weapon.id=id(4);weapon.parent=second.id;weapon.camera_draw->camera=second.id;
    RuntimeEntityDefinition body;body.id=id(5);body.mesh=RuntimeMesh{};body.camera_draw=CameraDrawBinding{first.id,CameraDrawMode::Hidden};
    RuntimeEntityDefinition ordinary;ordinary.id=id(6);ordinary.mesh=RuntimeMesh{};
    RuntimeEntityDefinition invisible=arms;invisible.id=id(7);invisible.mesh->visible=false;
    d.entities={first,second,arms,weapon,body,ordinary,invisible};return d;
}
SceneSnapshot generic(const RuntimeDefinition& d) {
    SceneSnapshot s;s.world_id=d.world_id;s.revision=d.authored_revision;s.camera_world=identity_matrix();s.presentation_source_id="source";
    for(const auto& e:d.entities)if(e.mesh && e.mesh->visible) {
        const auto& m=*e.mesh;
        s.objects.push_back({e.id,identity_matrix(),m.albedo,m.mesh,m.material,m.textures,{},17,m.cast_shadows,m.receive_shadows,true,e.camera_draw});
        if(e.camera_draw && e.camera_draw->mode==CameraDrawMode::Viewmodel)s.objects.back().cast_shadows=false;
    }
    return s;
}
void omission_and_lens() {
    RuntimeDefinition legacy;RuntimeEntityDefinition e;e.id="legacy";e.parent="existing-validator-authority";legacy.entities={e};
    validate_runtime_camera_draw(legacy); // No new graph rules without feature.
    SceneSnapshot s;s.camera_id="legacy";s.camera_world=identity_matrix();s.objects.push_back({"object",identity_matrix(),{1,1,1},{},{},{}});
    const auto previous=s.objects.data();select_scene_camera_draw(s,{});
    check(s.objects.data()==previous && s.objects.size()==1 && s.objects[0].camera_visible && !s.viewmodel,"Omission changed legacy snapshot.");
    auto d=definition();validate_runtime_camera_draw(d);
    for(unsigned kind=0;kind<6;++kind) {auto bad=d;auto& v=*bad.entities[0].camera->viewmodel;
        if(kind==0)v.vertical_fov=4;if(kind==1)v.vertical_fov=151;if(kind==2)v.near_plane=.0009;
        if(kind==3)v.far_plane=v.near_plane;if(kind==4)v.far_plane=1e7+1;if(kind==5)v.vertical_fov=std::numeric_limits<double>::quiet_NaN();
        rejects([&]{validate_runtime_camera_draw(bad);});}
    d.entities[0].camera->viewmodel=SceneLens{5,.001,1e7};validate_runtime_camera_draw(d);
    d.entities[0].camera->viewmodel->vertical_fov=150;validate_runtime_camera_draw(d);
}
void graph_rejection() {
    const auto original=definition();
    for(unsigned kind=0;kind<9;++kind) {auto d=original;
        if(kind==0)d.entities[2].camera_draw->camera=id(99);
        if(kind==1)d.entities[2].camera_draw->camera=id(6);
        if(kind==2)d.entities[0].camera->viewmodel.reset();
        if(kind==3)d.entities[2].parent=id(2);
        if(kind==4)d.entities[2].mesh.reset();
        if(kind==5)d.entities[2].camera_draw->mode=static_cast<CameraDrawMode>(99);
        if(kind==6)d.entities[2].camera_draw->camera=std::string(32,'0');
        if(kind==7)d.entities[0].parent=id(3);
        if(kind==8)d.entities[5].parent=id(99);
        rejects([&]{validate_runtime_camera_draw(d);});}
    auto duplicate=original;duplicate.entities.push_back(duplicate.entities.back());rejects([&]{validate_runtime_camera_draw(duplicate);});
    auto self=original;self.entities[0].mesh=RuntimeMesh{};self.entities[0].camera_draw=CameraDrawBinding{id(1),CameraDrawMode::Viewmodel};
    rejects([&]{validate_runtime_camera_draw(self);});
    RuntimeDefinition templates;templates.templates={recipe()};validate_runtime_templates(templates);
    auto external=templates;external.templates[0].entities[1].camera_draw->camera=id(99);
    rejects([&]{validate_runtime_templates(external);});
    const std::map<std::string,std::string> ids={{id(1),id(201)},{id(3),id(203)},{id(5),id(205)}};
    RuntimeDefinition expanded;expanded.entities=runtime_template_entities(templates.templates[0],ids,RuntimeTransform{},{});
    validate_runtime_camera_draw(expanded);
    check(expanded.entities[1].camera_draw->camera==id(201) && expanded.entities[2].camera_draw->camera==id(201),
        "Portable template binding camera references did not remap.");
}
void invisible_bounds() {
    RuntimeDefinition d;d.world_id="bounds";RuntimeEntityDefinition camera;camera.id=id(1);camera.camera=RuntimeCamera{60,.1,100,SceneLens{}};d.entities={camera};
    for(unsigned mode=0;mode<2;++mode)for(unsigned i=0;i<32;++i) {RuntimeEntityDefinition e;e.id=id(100+mode*32+i);e.parent=camera.id;e.mesh=RuntimeMesh{};e.mesh->visible=false;
        e.camera_draw=CameraDrawBinding{camera.id,mode ? CameraDrawMode::Hidden : CameraDrawMode::Viewmodel};d.entities.push_back(e);}
    validate_runtime_camera_draw(d);
    for(unsigned mode=0;mode<2;++mode) {auto bad=d;auto extra=d.entities[1+mode*32];extra.id=id(999);bad.entities.push_back(extra);rejects([&]{validate_runtime_camera_draw(bad);});}
    auto other=camera;other.id=id(2);d.entities.push_back(other);
    for(unsigned i=0;i<32;++i) {auto e=d.entities[1];e.id=id(500+i);e.parent=other.id;e.camera_draw->camera=other.id;d.entities.push_back(e);}
    validate_runtime_camera_draw(d); // Per camera, not a global 32 limit.
}
void selection_matrix() {
    const auto d=definition();const auto editor=generic(d);check(editor.objects.size()==4 && !editor.viewmodel,"Inspection omitted visible arms/body.");
    check(!object(editor.objects,id(3)).cast_shadows && object(editor.objects,id(3)).receive_shadows && object(editor.objects,id(5)).cast_shadows,"Generic inspection shadow policy changed.");
    auto target=editor;target.camera_id=id(1);select_scene_camera_draw(target,d.entities[0].camera->viewmodel);
    check(target.viewmodel && target.viewmodel->lens==*d.entities[0].camera->viewmodel && target.viewmodel->objects.size()==1 && target.objects.size()==2,"Target lists/lens differ.");
    check(!contains(target.objects,id(3)) && !contains(target.objects,id(4)) && !contains(target.viewmodel->objects,id(4)),"A viewmodel leaked into a Game world/other foreground.");
    const auto& arms=object(target.viewmodel->objects,id(3));check(!arms.cast_shadows && arms.receive_shadows && arms.incarnation==17,"Foreground policy/incarnation lost.");
    const auto& body=object(target.objects,id(5));check(!body.camera_visible && body.cast_shadows && body.receive_shadows,"Hidden body lost shadow eligibility.");
    auto other=editor;other.camera_id=id(2);select_scene_camera_draw(other,d.entities[1].camera->viewmodel);
    check(other.viewmodel->objects.size()==1 && object(other.objects,id(5)).camera_visible && !contains(other.objects,id(3)),"Other Game camera policy differs.");
    auto no_lens=editor;no_lens.camera_id=id(8);select_scene_camera_draw(no_lens,{});
    check(!no_lens.viewmodel && no_lens.objects.size()==2 && object(no_lens.objects,id(5)).camera_visible,"Unbound Game view did not omit global viewmodels.");
    check(editor.objects.size()==4 && object(editor.objects,id(5)).camera_visible,"Detached selection mutated inspection cache.");
    auto empty=SceneSnapshot{};empty.camera_id=id(1);select_scene_camera_draw(empty,SceneLens{});check(empty.viewmodel && empty.viewmodel->objects.empty(),"Empty configured lens was collapsed into omission.");
}
void selection_atomicity_and_owners() {
    auto d=definition();auto mesh=std::make_shared<MeshAsset>();auto textures=std::make_shared<MaterialTextures>();auto skin=std::make_shared<SkinPose>();skin->palette={identity_matrix()};
    auto s=generic(d);s.camera_id=id(1);s.objects[0].mesh=mesh;s.objects[0].textures=textures;s.objects[0].skin=skin;const auto original=s;
    s.objects[2].camera_draw->mode=static_cast<CameraDrawMode>(99);rejects([&]{select_scene_camera_draw(s,SceneLens{});});
    check(!s.viewmodel && s.objects.size()==original.objects.size() && s.objects[0].mesh==mesh && s.objects[0].camera_visible,"Rejected selection partially published.");
    s=original;select_scene_camera_draw(s,SceneLens{});const auto& retained=object(s.viewmodel->objects,id(3));
    check(retained.mesh==mesh && retained.textures==textures && retained.skin==skin && retained.world==original.objects[0].world,"Selection replaced immutable owners/pose.");
    rejects([&]{select_scene_camera_draw(s,SceneLens{});});
    auto missing=original;rejects([&]{select_scene_camera_draw(missing,{});});check(!missing.viewmodel && missing.objects.size()==4,"Missing lens failure changed caller.");
    for(const auto mode:{CameraDrawMode::Viewmodel,CameraDrawMode::Hidden}) {
        SceneSnapshot oversized;oversized.camera_id=id(1);
        for(unsigned i=0;i<33;++i) {SceneObject e;e.entity_id=id(100+i);e.world=identity_matrix();e.camera_draw=CameraDrawBinding{id(1),mode};oversized.objects.push_back(e);}
        rejects([&]{select_scene_camera_draw(oversized,SceneLens{});});
        check(!oversized.viewmodel && oversized.objects.size()==33 && oversized.objects.back().camera_visible,"Oversized selection partially published.");
    }
}
void live_snapshots_and_adoption() {
    auto d=definition();auto mesh=std::make_shared<MeshAsset>();MeshVertex v;v.normal={0,1,0};mesh->vertices={v,v,v};mesh->indices={0,1,2};
    auto textures=std::make_shared<MaterialTextures>();d.entities[2].mesh->mesh=mesh;d.entities[2].mesh->textures=textures;
    Runtime live(d);live.step(2,{});const auto original=live.save_snapshot(hash);const auto old=live.snapshot(id(1));const auto camera=live.camera_state(id(1));
    check(camera.viewmodel==d.entities[0].camera->viewmodel && camera.vertical_fov==60 && old.viewmodel->lens.vertical_fov==40,"Camera metadata collapsed world/foreground lenses.");
    check(live.snapshot().objects.size()==4 && !object(live.snapshot().objects,id(3)).cast_shadows,"Live inspection policy differs.");
    PbrMaterial material;material.base_color={.2f,.4f,.6f};material.roughness=.3f;
    auto stage=live.prepare_materials(0,2,{{id(3),material}});check(live.commit_materials(std::move(stage)),"Material adoption failed.");
    auto texture_stage=live.prepare_textures(1,3,{{id(3),std::make_shared<MaterialTextures>()}});check(live.commit_textures(std::move(texture_stage)),"Texture adoption failed.");
    const auto now=live.snapshot(id(1));check(now.viewmodel->lens==old.viewmodel->lens && now.viewmodel->objects.size()==1 && !object(now.objects,id(5)).camera_visible,"Adoption lost frozen selection.");
    check(object(old.viewmodel->objects,id(3)).material->base_color==PbrMaterial{}.base_color && object(old.viewmodel->objects,id(3)).mesh==mesh && object(old.viewmodel->objects,id(3)).textures==textures,"Adoption mutated prior resource snapshot.");
    auto before=Json::parse(original).at("payload"),after=Json::parse(live.save_snapshot(hash)).at("payload");after["authored_revision"]=before.at("authored_revision");check(before==after,"Adoption changed complete non-authored runtime payload.");
    const auto target=live.live_definition();const auto saved=live.save_snapshot(hash);auto fresh=Runtime::from_snapshot(target,hash,saved);
    check(fresh->save_snapshot(hash)==saved && fresh->snapshot(id(1)).viewmodel->lens==now.viewmodel->lens && !object(fresh->snapshot(id(1)).objects,id(5)).camera_visible,"Fresh restore lost frozen camera policy.");
    SceneSnapshot retired;std::weak_ptr<const MeshAsset> retained_mesh;
    {
        auto scoped=definition();auto owned=std::make_shared<MeshAsset>(*mesh);retained_mesh=owned;
        scoped.entities[2].mesh->mesh=owned;Runtime temporary(scoped);retired=temporary.snapshot(id(1));
    }
    check(!retained_mesh.expired() && object(retired.viewmodel->objects,id(3)).mesh && retired.viewmodel->lens.vertical_fov==40,
        "Runtime teardown released prior foreground snapshot resources.");
}
void weighted_pose() {
    auto d=definition();auto m=std::make_shared<ModelAsset>();m->nodes.resize(1);m->nodes[0].name="Hand";m->nodes[0].skin=0;m->nodes[0].primitives={0};m->roots={0};
    auto mesh=std::make_shared<MeshAsset>();MeshVertex v;v.normal={0,0,1};mesh->vertices={v,v,v};mesh->indices={0,1,2};mesh->influences.assign(3,SkinWeight{{0,0,0,0},{1,0,0,0}});m->primitives={mesh};
    m->skins.push_back({"Hand",0,{0},{identity_matrix()}});m->animations.push_back({"Reach",1,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0,1},{{0,0,0,0},{1,0,0,0}}}}});
    RuntimeEntityDefinition rig;rig.id=id(8);rig.parent=id(1);rig.animation_rig=RuntimeAnimationRig{m,0,0,1,true,true};d.entities.push_back(rig);
    RuntimeEntityDefinition joint;joint.id=id(9);joint.parent=rig.id;joint.rig_node=RuntimeRigNode{rig.id,0};d.entities.push_back(joint);
    auto& arms=d.entities[2];arms.parent=joint.id;arms.skinned_mesh=RuntimeSkinnedMesh{rig.id,0};arms.mesh->mesh=mesh;
    Runtime live(d);const auto old=live.snapshot(id(1));live.step(30,{});const auto next=live.snapshot(id(1));
    const auto& a=object(next.viewmodel->objects,id(3));check(a.skin && a.skin->palette.size()==1 && a.mesh==mesh,"Foreground lost weighted mesh palette.");
    check(std::abs(a.world[12]-.5)<1e-8 && object(old.viewmodel->objects,id(3)).world[12]==0,"Foreground did not use current committed animation or mutated old pose.");
    const auto state=live.save_snapshot(hash);(void)live.snapshot(id(1));(void)live.snapshot(id(2));check(live.save_snapshot(hash)==state,"Camera extraction advanced animation/simulation.");
}
RuntimeSpawnTemplate recipe() {
    const auto d=definition();RuntimeSpawnTemplate p;p.id=id(90);p.name="Camera arms body";p.root=id(1);
    p.entities={d.entities[0],d.entities[2],d.entities[4]};p.entities[2].parent=p.root;return p;
}
void templates_and_restore() {
    RuntimeDefinition d;d.world_id="camera-template";d.authored_revision=1;d.templates={recipe()};Runtime live(d);
    const auto birth=live.change_structure(0,{{id(90),{}},{id(90),{}}},{});check(birth.spawned.size()==2,"Two camera instances did not spawn.");
    const auto first=live.instance(birth.spawned[0]),second=live.instance(birth.spawned[1]);const auto camera1=first.nodes.at(id(1)),camera2=second.nodes.at(id(1));
    auto snapshot=live.snapshot(camera1);check(snapshot.viewmodel->objects.size()==1 && contains(snapshot.viewmodel->objects,first.nodes.at(id(3))) && !contains(snapshot.objects,second.nodes.at(id(3))),"Template cameras/foreground references did not remap.");
    check(!object(snapshot.objects,first.nodes.at(id(5))).camera_visible && object(snapshot.objects,second.nodes.at(id(5))).camera_visible,"Template body leaked between cameras.");
    const auto saved=live.save_snapshot(hash);auto fresh=Runtime::from_snapshot(d,hash,saved);check(fresh->save_snapshot(hash)==saved && fresh->snapshot(camera1).viewmodel->objects.size()==1,"Spawned camera save restore differs.");
    live.change_structure(1,{}, {birth.spawned[0]});rejects([&]{live.snapshot(camera1);});check(live.snapshot(camera2).viewmodel->objects.size()==1,"Despawn removed surviving foreground.");
    const auto reborn=live.change_structure(2,{{id(90),{}}},{}).spawned[0];check(reborn!=birth.spawned[0] && live.snapshot(reborn).viewmodel->objects.size()==1,"Rebirth reused retired camera identity.");
    check(snapshot.viewmodel->objects.size()==1 && snapshot.viewmodel->objects[0].entity_id==first.nodes.at(id(3)),"Retirement mutated old snapshot.");
}
void failed_membership_rollback() {
    RuntimeDefinition d;d.world_id="camera-candidate";d.authored_revision=1;d.templates={recipe()};Runtime live(d);
    std::vector<RuntimeSpawnRequest> requests(33,{id(90),{}});
    // Each recipe has a separate camera, so this is valid despite >32 global.
    const auto born=live.change_structure(0,requests,{});check(born.spawned.size()==33,"Bindings were incorrectly bounded globally.");
    const auto before=live.save_snapshot(hash);RuntimeStructureTick structure;structure.offset=0;structure.expected_revision=1;structure.despawns={born.spawned[0]};
    auto stale=structure;stale.offset=1;stale.despawns={born.spawned[1]};
    rejects([&]{live.step(2,{},{},{},{},{structure,stale});});
    check(live.save_snapshot(hash)==before && live.snapshot(born.spawned[0]).viewmodel->objects.size()==1,"Failed structural Tick changed selection/membership snapshot.");
}
}
int main() {
    try {
        omission_and_lens();graph_rejection();invisible_bounds();selection_matrix();selection_atomicity_and_owners();
        if(!Runtime::available()) {std::cout<<"5 camera draw portable checks passed\n";return 0;}
        live_snapshots_and_adoption();weighted_pose();templates_and_restore();failed_membership_rollback();
        std::cout<<"9 camera draw checks passed\n";return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
