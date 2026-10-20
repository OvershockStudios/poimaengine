// SPDX-License-Identifier: Apache-2.0
// Owned native extraction only; no renderer or synthetic character state.
#include "poima/runtime.hpp"
#include "poima/animation.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
namespace {
const std::string digest(64,'a');
std::string id(unsigned n){const auto text=std::to_string(n);return std::string(32-text.size(),'0')+text;}
void check(bool yes,const char* message){if(!yes)throw std::runtime_error(message);}
template<class F>void rejects(F&& action,const char* message="Invalid presentation override accepted."){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}check(rejected,message);}
void near(double a,double b){check(std::abs(a-b)<1e-9,"Presentation scalar differs from analytic result.");}
void near(const Matrix4& a,const Matrix4& b){for(unsigned i=0;i<16;++i)near(a[i],b[i]);}
Matrix4 pose_matrix(const RuntimeCameraPose& p){return local_matrix(p.position,p.rotation,{1,1,1});}
RuntimeCameraPose pose(double x,double y=0,double yaw=0){RuntimeCameraPose result;result.position={x,y,0};result.rotation={0,std::sin(yaw/2),0,std::cos(yaw/2)};return result;}
Matrix4 local(const RuntimeTransform& t){return local_matrix(t.position,t.rotation,t.scale);}
RuntimeEntityDefinition node(unsigned n,unsigned parent=0){RuntimeEntityDefinition e;e.id=id(n);if(parent)e.parent=id(parent);return e;}
const SceneObject& object(const std::vector<SceneObject>& objects,unsigned n){
    const auto found=std::find_if(objects.begin(),objects.end(),[&](const auto& o){return o.entity_id==id(n);});
    check(found!=objects.end(),"Expected presentation drawable absent.");return *found;
}
void same_objects(const std::vector<SceneObject>& a,const std::vector<SceneObject>& b){
    check(a.size()==b.size(),"Legacy drawable count changed.");
    for(std::size_t i=0;i<a.size();++i){const auto& x=a[i];const auto& y=b[i];
        check(x.entity_id==y.entity_id && x.world==y.world && x.albedo==y.albedo && x.mesh==y.mesh && x.textures==y.textures &&
            x.incarnation==y.incarnation && x.cast_shadows==y.cast_shadows && x.receive_shadows==y.receive_shadows && x.camera_visible==y.camera_visible &&
            x.camera_draw.has_value()==y.camera_draw.has_value() && x.material.has_value()==y.material.has_value() && bool(x.skin)==bool(y.skin),"Legacy drawable changed.");
        if(x.camera_draw)check(x.camera_draw->camera==y.camera_draw->camera && x.camera_draw->mode==y.camera_draw->mode,"Legacy draw binding changed.");
        if(x.material)check(x.material->base_color==y.material->base_color && x.material->emissive==y.material->emissive && x.material->metallic==y.material->metallic && x.material->roughness==y.material->roughness && x.material->double_sided==y.material->double_sided,"Legacy material changed.");
        if(x.skin)check(x.skin->palette==y.skin->palette,"Legacy owned palette changed.");
    }
}
void same_scene(const SceneSnapshot& a,const SceneSnapshot& b){
    check(a.world_id==b.world_id && a.revision==b.revision && a.camera_id==b.camera_id && a.camera_world==b.camera_world &&
        a.vertical_fov==b.vertical_fov && a.near_plane==b.near_plane && a.far_plane==b.far_plane && a.authored_vertical_fov==b.authored_vertical_fov &&
        a.presentation_source_id==b.presentation_source_id && a.presentation_generation==b.presentation_generation && a.view_cut_generation==b.view_cut_generation &&
        a.material_revision==b.material_revision && a.viewmodel.has_value()==b.viewmodel.has_value(),"Legacy snapshot metadata changed.");
    same_objects(a.objects,b.objects);if(a.viewmodel){check(a.viewmodel->lens==b.viewmodel->lens,"Legacy foreground lens changed.");same_objects(a.viewmodel->objects,b.viewmodel->objects);}
}
void step(Runtime& r,const std::vector<RuntimeCameraPoseCommand>& commands){r.step(1,{},{},{},{},{},{},{},{},{},{},{},commands);}
RuntimeDefinition definition(){
    RuntimeDefinition d;d.world_id="camera-pose-presentation";d.authored_revision=1;
    auto camera=node(1);camera.transform.position={2,3,4};camera.camera=RuntimeCamera{60,.1,100,SceneLens{75,.01,10}};
    auto foreground=node(2,1);foreground.transform.position={.1,-.2,-.6};foreground.mesh=RuntimeMesh{};foreground.camera_draw=CameraDrawBinding{id(1),CameraDrawMode::Viewmodel};
    auto detail=node(3,1);detail.transform.position={.4,.3,-2};detail.mesh=RuntimeMesh{};
    auto unrelated=node(4);unrelated.transform.position={-4,2,-6};unrelated.mesh=RuntimeMesh{};
    d.entities={camera,foreground,detail,unrelated};return d;
}
void legacy_and_purity(){
    const auto d=definition();Runtime r(d);const auto saved=r.save_snapshot(digest);const auto old=r.snapshot(id(1));
    same_scene(old,r.snapshot(id(1),RuntimeCameraPose{},RuntimeCameraPose{}));
    for(unsigned i=0;i<8;++i){const auto shown=r.snapshot(id(1),pose(.02*i,.01,-.04*i),pose(.01,.03*i,.02));
        near(shown.camera_world,multiply(old.camera_world,pose_matrix(pose(.02*i,.01,-.04*i))));
        check(r.save_snapshot(digest)==saved && r.entity(id(1)).world==old.camera_world,"Pure extraction changed complete authoritative state.");
    }
    same_scene(old,r.snapshot(id(1)));check(r.inspect().tick==0,"Pure extraction advanced the tick.");
    step(r,{{id(1),pose(.2,.1,.3),pose(.3,-.1,.2)}});const auto current=*r.camera_pose(id(1));
    same_scene(r.snapshot(id(1)),r.snapshot(id(1),current.world,current.viewmodel));
    const auto active=r.save_snapshot(digest);const auto retained=r.snapshot(id(1),pose(-.1,.04,-.2),pose(.08,.02,.12));const auto retained_foreground=retained.viewmodel->objects[0].world;
    (void)r.snapshot(id(1),RuntimeCameraPose{},RuntimeCameraPose{});
    check(retained.viewmodel->objects[0].world==retained_foreground && r.save_snapshot(digest)==active,"A later pure query mutated owned output or active saved state.");
}
void descendants_nonuniform_and_other_camera(){
    auto d=definition();auto scaled=node(10);scaled.transform.scale={2,3,4};auto inverse=node(11,10);inverse.transform.scale={.5,1./3,.25};
    d.entities[0].parent=id(11);d.entities[0].transform.rotation={0,std::sin(.17),0,std::cos(.17)};
    auto intermediate=node(12,1);intermediate.transform.position={.3,.2,-.4};intermediate.transform.scale={1.7,.4,2.3};d.entities[2].parent=id(12);
    auto other=node(5,1);other.transform.position={.7,.1,0};other.camera=RuntimeCamera{};
    auto other_child=node(6,5);other_child.transform.position={-.3,0,-1};other_child.mesh=RuntimeMesh{};
    d.entities.insert(d.entities.end(),{scaled,inverse,intermediate,other,other_child});Runtime r(d);
    const auto committed_other=pose(.07,.03,-.12);step(r,{{id(5),committed_other,{}}});const auto saved=r.save_snapshot(digest);
    const auto effect=pose(.11,.02,.24);const auto shown=r.snapshot(id(1),effect,RuntimeCameraPose{});
    const auto camera=multiply(multiply(multiply(local(scaled.transform),local(inverse.transform)),local(d.entities[0].transform)),pose_matrix(effect));
    near(shown.camera_world,camera);check(rigid_transform(camera),"Compensated camera fixture is not rigid.");
    near(object(shown.objects,3).world,multiply(multiply(camera,local(intermediate.transform)),local(d.entities[2].transform)));
    near(object(shown.objects,6).world,multiply(multiply(multiply(camera,local(other.transform)),pose_matrix(committed_other)),local(other_child.transform)));
    check(object(shown.objects,4).world==r.entity(id(4)).world,"Unrelated geometry changed under presentation override.");
    check(r.save_snapshot(digest)==saved,"Derived scaled hierarchy changed complete runtime archive.");
}
void attached_lights_and_foreground_once(){
    auto d=definition();auto light=node(7,1);light.transform.position={.3,.4,-.2};light.light=Light{};light.light->kind=LightKind::spot;
    auto static_light=node(8);static_light.transform.position={-3,4,5};static_light.light=Light{};d.entities.insert(d.entities.end(),{light,static_light});
    Runtime r(d);const auto world=pose(.2,.1,.3),foreground=pose(.07,-.02,-.15);const auto base=r.entity(id(1)).world;
    const auto shown=r.snapshot(id(1),world,foreground);const auto expected_camera=multiply(base,pose_matrix(world));
    const auto expected_foreground=multiply(multiply(expected_camera,pose_matrix(foreground)),local(d.entities[1].transform));
    near(shown.viewmodel->objects.at(0).world,expected_foreground);
    const auto matrix=multiply(expected_camera,local(light.transform));check(shown.lighting.lights.size()==2,"Attached light count differs.");
    const auto a=std::find_if(shown.lighting.lights.begin(),shown.lighting.lights.end(),[&](const auto& l){return l.entity_id==light.id;});check(a!=shown.lighting.lights.end(),"Attached light absent.");
    const auto length=std::hypot(matrix[8],matrix[9],matrix[10]);for(unsigned i=0;i<3;++i){near(a->position[i],matrix[12+i]);near(a->direction[i],-matrix[8+i]/length);}
    const auto b=std::find_if(shown.lighting.lights.begin(),shown.lighting.lights.end(),[&](const auto& l){return l.entity_id==static_light.id;});
    check(b!=shown.lighting.lights.end() && b->position==static_light.transform.position,"Unrelated light changed.");
    check(r.entity(id(2)).world==multiply(base,local(d.entities[1].transform)),"Foreground presentation moved its logical entity.");
}
RuntimeDefinition rig_definition(bool camera_above_mesh){
    RuntimeDefinition d;d.world_id="presentation-cross-subtree-skin";d.authored_revision=1;
    auto model=std::make_shared<ModelAsset>();model->nodes.resize(2);model->roots={0,1};model->nodes[0].name="mesh";model->nodes[0].skin=0;model->nodes[0].primitives={0};model->nodes[0].position={.4,0,0};model->nodes[1].name="joint";model->nodes[1].position={0,.2,0};
    auto mesh=std::make_shared<MeshAsset>();MeshVertex vertex;vertex.position={.1f,.2f,0};vertex.normal={0,0,1};vertex.tangent={1,0,0,1};mesh->vertices={vertex,vertex,vertex};mesh->indices={0,1,2};mesh->influences.assign(3,SkinWeight{{0,0,0,0},{1,0,0,0}});model->primitives={mesh};
    model->skins.push_back({"one-joint",1,{1},{identity_matrix()}});model->animations.push_back({"move",1,{{1,AnimationPath::translation,AnimationInterpolation::linear,{0,1},{{0,.2,0,0},{.4,.2,0,0}}}}});
    auto rig=node(30);rig.transform.position={1,2,-3};rig.animation_rig=RuntimeAnimationRig{model,0,0,1,true,true};
    auto camera=node(1,30);camera.camera=RuntimeCamera{};camera.transform.position={.15,0,0};
    auto mesh_node=node(32,camera_above_mesh ? 1 : 30);mesh_node.rig_node=RuntimeRigNode{rig.id,0};mesh_node.transform.position=model->nodes[0].position;
    auto joint=node(33,camera_above_mesh ? 30 : 1);joint.rig_node=RuntimeRigNode{rig.id,1};joint.transform.position=model->nodes[1].position;
    auto drawable=node(34,32);drawable.mesh=RuntimeMesh{};drawable.mesh->mesh=mesh;drawable.skinned_mesh=RuntimeSkinnedMesh{rig.id,0};
    d.entities={rig,camera,mesh_node,joint,drawable};return d;
}
void cross_subtree_skin(bool camera_above_mesh){
    const auto d=rig_definition(camera_above_mesh);Runtime r(d);r.step(12,{});const auto saved=r.save_snapshot(digest);
    const auto old=r.snapshot(id(1));const auto old_palette=object(old.objects,34).skin->palette;
    const auto effect=pose(.12,.03,.25);const auto shown=r.snapshot(id(1),effect,{});const auto& mesh=object(shown.objects,34);
    check(mesh.skin && mesh.skin->palette.size()==1,"Real weighted rig palette absent.");
    const auto wrapper=r.entity(id(30)).world;const auto camera=multiply(multiply(wrapper,local(d.entities[1].transform)),pose_matrix(effect));
    const auto mesh_world=multiply(camera_above_mesh ? camera : wrapper,local(r.entity(id(32)).local));
    const auto joint_world=multiply(camera_above_mesh ? wrapper : camera,local(r.entity(id(33)).local));
    near(mesh.world,mesh_world);near(mesh.skin->palette.at(0),multiply(inverse_affine(mesh_world),joint_world));
    // This is an actual mapped-joint/weighted-vertex oracle. A camera-only
    // matrix warp or unchanged palette fails one of these two subtree layouts.
    const auto deformed=deform_mesh(*mesh.mesh,mesh.skin->palette);const auto& v=deformed->vertices[0].position;
    const auto& original=mesh.mesh->vertices[0].position;
    // Deformed mesh vertices are float32, unlike the double matrix oracle.
    for(unsigned i=0;i<3;++i)check(std::abs((mesh.world[i]*v[0]+mesh.world[4+i]*v[1]+mesh.world[8+i]*v[2]+mesh.world[12+i])-
        (joint_world[i]*original[0]+joint_world[4+i]*original[1]+joint_world[8+i]*original[2]+joint_world[12+i]))<2e-6,"Weighted world vertex differs from mapped joint.");
    check(mesh.skin!=object(old.objects,34).skin && object(old.objects,34).skin->palette==old_palette,"New extraction borrowed/mutated an old palette.");
    check(mesh.skin->palette!=old_palette,"Cross-subtree fixture did not require palette re-derivation.");
    check(r.save_snapshot(digest)==saved,"Skin extraction advanced animation or changed complete saved state.");
}
void current_native_look_stance_and_motion(){
    RuntimeDefinition d;d.world_id="presentation-current-controller";d.authored_revision=1;
    auto floor=node(10);floor.transform.position={0,-.5,0};floor.collider=BoxCollider{{10,.5f,10},BodyMotion::Static};
    auto actor=node(11);actor.transform.position={0,.1,0};actor.character=CharacterController{};actor.character->camera=id(1);actor.character->stance=RuntimeCharacterStanceProfile{};
    auto camera=node(1,11);camera.camera=RuntimeCamera{};camera.transform.position={0,1.6,0};d.entities={floor,actor,camera};
    Runtime r(d);RuntimeInput input;input.entity=actor.id;input.move={.2f,.7f};input.look={23,11};
    r.step(6,{input},{},{},{},{},{},{},{},{{actor.id,true}},{},{},{{camera.id,pose(.05,.03,.12),{}}});
    const auto saved=r.save_snapshot(digest);const auto state=*r.camera_pose(camera.id);const auto character=r.entity(actor.id);const auto committed=r.entity(camera.id);
    check(r.character_stance(actor.id)->crouched && character.world[14]!=0 && character.pitch!=0,"Native controller fixture did not achieve crouch, motion and look.");
    const auto rendered=pose(-.03,.01,-.08);const auto shown=r.snapshot(camera.id,rendered,{});
    near(shown.camera_world,multiply(state.base_world,pose_matrix(rendered)));
    check(r.entity(actor.id).world==character.world && r.entity(camera.id).world==committed.world && r.camera_pose(camera.id)->base_world==state.base_world && r.save_snapshot(digest)==saved,"Presentation delayed or mutated native controller state.");
}
void numerical_membership_and_ownership_rejection(){
    Runtime r(definition());const auto saved=r.save_snapshot(digest);
    for(unsigned k=0;k<8;++k){auto world=pose(.1);auto foreground=std::optional(RuntimeCameraPose{});auto target=id(1);
        if(k==0)world.position[0]=10.01;if(k==1)world.position[1]=std::numeric_limits<double>::quiet_NaN();
        if(k==2)world.rotation={0,0,0,0};if(k==3)world.rotation[0]=std::numeric_limits<double>::infinity();
        if(k==4)foreground.reset();if(k==5)foreground->position[2]=11;if(k==6)target=id(4);if(k==7)target=id(99);
        rejects([&]{r.snapshot(target,world,foreground);});check(r.save_snapshot(digest)==saved,"Rejected extraction changed complete runtime.");
    }
    auto d=definition();auto body=node(8,1);body.transform.position={0,-4,0};body.collider=BoxCollider{};d.entities.push_back(body);Runtime physical_descendant(d);
    same_scene(physical_descendant.snapshot(id(1)),physical_descendant.snapshot(id(1),RuntimeCameraPose{},RuntimeCameraPose{}));
    rejects([&]{physical_descendant.snapshot(id(1),pose(.1),RuntimeCameraPose{});});
    const auto before=physical_descendant.save_snapshot(digest);(void)physical_descendant.snapshot(id(1),RuntimeCameraPose{},pose(.1));check(physical_descendant.save_snapshot(digest)==before,"Foreground-only extraction changed a physical subtree.");
    d=definition();d.entities[0].collider=BoxCollider{};Runtime physical_camera(d);same_scene(physical_camera.snapshot(id(1)),physical_camera.snapshot(id(1),RuntimeCameraPose{},RuntimeCameraPose{}));
    rejects([&]{physical_camera.snapshot(id(1),pose(.1),RuntimeCameraPose{});});rejects([&]{physical_camera.snapshot(id(1),RuntimeCameraPose{},pose(.1));});
    d=rig_definition(true);d.entities[2].camera=RuntimeCamera{};Runtime animated_camera(d);
    same_scene(animated_camera.snapshot(id(32)),animated_camera.snapshot(id(32),RuntimeCameraPose{},{}));
    const auto animated_before=animated_camera.save_snapshot(digest);
    rejects([&]{animated_camera.snapshot(id(32),pose(.1),{});},"Animation-owned camera presentation override accepted.");
    rejects([&]{step(animated_camera,{{id(32),pose(.1),{}}});},"Animation-owned camera committed world write accepted.");
    check(animated_camera.save_snapshot(digest)==animated_before,"Rejected animation-owned camera changes altered the complete runtime archive.");
    RuntimeDefinition plain_definition;auto plain_entity=node(1);plain_entity.camera=RuntimeCamera{};plain_definition.entities={plain_entity};Runtime plain(plain_definition);
    rejects([&]{plain.snapshot(id(1),RuntimeCameraPose{},RuntimeCameraPose{});});
}
}
int main(){try{
    if(!Runtime::available()){std::cout<<"Native camera pose presentation unavailable; simulation groups skipped.\n";return 0;}
    legacy_and_purity();descendants_nonuniform_and_other_camera();attached_lights_and_foreground_once();cross_subtree_skin(true);cross_subtree_skin(false);
    current_native_look_stance_and_motion();numerical_membership_and_ownership_rejection();
    std::cout<<"7 native camera pose presentation groups passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
