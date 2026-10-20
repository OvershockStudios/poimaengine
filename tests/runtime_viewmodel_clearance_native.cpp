// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
namespace {
const std::string digest(64,'a');
std::string id(unsigned n){auto s=std::to_string(n);return std::string(32-s.size(),'0')+s;}
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void near(double a,double b,double tolerance=5e-5){check(std::isfinite(a) && std::abs(a-b)<=tolerance,"Clearance analytic value differs.");}
void near(const Matrix4& a,const Matrix4& b){for(unsigned i=0;i<16;++i)near(a[i],b[i],1e-9);}
template<class F>void rejects(F&& f){bool yes=false;try{f();}catch(const std::exception&){yes=true;}check(yes,"Invalid clearance profile accepted.");}
RuntimeEntityDefinition node(unsigned n,unsigned parent=0){RuntimeEntityDefinition e;e.id=id(n);if(parent)e.parent=id(parent);return e;}
ViewmodelClearanceProfile profile(){ViewmodelClearanceProfile p;p.position={.18,-.17,-.4};p.rotation={std::sqrt(.5),0,0,std::sqrt(.5)};p.radius=.075;p.half_height=.1;p.max_retraction=.65;p.skin=.01;return p;}
RuntimeDefinition definition(bool with_wall=true){
    RuntimeDefinition d;d.world_id="viewmodel-clearance-native";d.authored_revision=1;
    auto camera=node(1);camera.camera=RuntimeCamera{60,.01,100,SceneLens{75,.01,10}};camera.camera->viewmodel_clearance=profile();
    auto weapon=node(2,1);weapon.transform.position=profile().position;weapon.transform.scale={.06,.06,.18};weapon.mesh=RuntimeMesh{};weapon.camera_draw=CameraDrawBinding{camera.id,CameraDrawMode::Viewmodel};d.entities={camera,weapon};
    if(with_wall){auto wall=node(3);wall.transform.position={.18,-.17,-.5};wall.collider=BoxCollider{{1,1,.01f},BodyMotion::Static};wall.mesh=RuntimeMesh{};d.entities.push_back(wall);}
    return d;
}
const ViewmodelClearanceResult& result(const SceneSnapshot& s){check(s.viewmodel_clearance.has_value(),"Authored clearance produced no inspectable outcome.");return *s.viewmodel_clearance;}
void pure_default_and_clear(){
    auto d=definition(false);d.entities[0].camera->viewmodel_clearance.reset();Runtime plain(d);const auto old=plain.snapshot(id(1));check(!old.viewmodel_clearance && !old.camera_pose_receipt,"Absent profile altered default receipt.");
    d=definition(false);Runtime r(d);const auto saved=r.save_snapshot(digest);const auto s=r.snapshot(id(1));const auto& c=result(s);
    check(c.status==ViewmodelClearanceStatus::Clear && c.resolved && !c.suppressed && c.retraction==0 && !c.anchor_hit && !c.sweep_hit && !c.final_hit && s.camera_pose_receipt,"Clear space did not retain a resolved no-op.");
    check(s.viewmodel && s.viewmodel->objects.size()==1,"Clear policy removed foreground geometry.");near(s.viewmodel->objects[0].world,r.entity(id(2)).world);
    for(unsigned i=0;i<5;++i)check(result(r.snapshot(id(1)))==c,"Repeated pure policy extraction changed its result.");
    check(r.save_snapshot(digest)==saved && !r.snapshot().viewmodel_clearance,"Clearance changed complete saved state or generic editor inspection.");
    auto scaled=definition();auto parent=node(4);parent.transform.scale={2,.5,1};scaled.entities[0].parent=parent.id;scaled.entities[0].transform.scale={.5,2,1};scaled.entities.push_back(parent);
    Runtime nested(scaled),unscaled(definition());const auto nested_before=nested.save_snapshot(digest);const auto nested_scene=nested.snapshot(id(1)),unscaled_scene=unscaled.snapshot(id(1));
    check(result(nested_scene)==result(unscaled_scene),"Canceled nonuniform camera ancestors changed the safety interval.");near(nested_scene.viewmodel->objects[0].world,unscaled_scene.viewmodel->objects[0].world);
    check(nested.save_snapshot(digest)==nested_before,"Nonuniform-ancestor policy extraction changed complete state.");
}
void wall_retraction_and_owned_lifetime(){
    const auto d=definition();Runtime r(d);const auto saved=r.save_snapshot(digest);const auto logical=r.entity(id(2)).world;const auto base=r.camera_pose(id(1))->base_world;
    const auto s=r.snapshot(id(1));const auto& c=result(s);
    check(c.status==ViewmodelClearanceStatus::Retracted && c.resolved && !c.suppressed && c.sweep_hit && !c.anchor_hit && !c.final_hit && c.sweep_hit->entity==id(3),"Wall did not produce an explicit safe retraction.");
    // Face z=-.49; capsule projected half extent=.175, desired center=-.4.
    near(c.retraction,.095);near(s.viewmodel->objects[0].world[14],-.4+c.retraction,1e-9);
    check(c.ignored_entities.empty() && c.cast.position[2]>.2 && c.cast.direction[2]==-1 && c.cast.distance==profile().max_retraction,"Policy sweep was not anchored behind the desired weapon.");
    auto final=c.cast;final.distance=0;for(unsigned k=0;k<3;++k)final.position[k]+=c.cast.direction[k]*(c.cast.distance-c.retraction);
    check(!r.capsule_cast(final),"Reported resolved final capsule still overlaps.");
    const auto& q=final.rotation;const auto axis_z=2*(q[1]*q[2]+q[0]*q[3]);
    check(final.position[2]-final.radius-final.half_height*std::abs(axis_z)>=-.49-2e-5,"Independent plane support bound still crosses the wall.");
    const auto old_world=s.viewmodel->objects[0].world;const auto again=r.snapshot(id(1));check(result(again)==c && again.viewmodel->objects[0].world==old_world,"Retraction accumulated across queries.");
    check(r.entity(id(2)).world==logical && r.camera_pose(id(1))->base_world==base && r.save_snapshot(digest)==saved,"Policy changed logical weapon, aim base or full archive.");
    auto cold=Runtime::from_snapshot(d,digest,saved);const auto restored=cold->snapshot(id(1));near(restored.viewmodel->objects[0].world,old_world);check(cold->save_snapshot(digest)==saved,"Cold policy evaluation changed saved state.");
}
void presented_pose_and_current_geometry(){
    auto d=definition();Runtime r(d);const auto saved=r.save_snapshot(digest);RuntimeCameraPose world,foreground;
    world.position={.02,.01,0};world.rotation={0,std::sin(.04),0,std::cos(.04)};foreground.position={.01,-.01,-.015};foreground.rotation={0,0,std::sin(.025),std::cos(.025)};
    const auto s=r.snapshot(id(1),world,foreground);const auto& c=result(s);check(c.resolved && c.retraction>0 && !c.suppressed,"Interpolated desired effects bypassed wall safety.");
    const auto camera=local_matrix(world.position,world.rotation,{1,1,1});near(s.camera_world,camera);
    const auto expected=multiply(multiply(multiply(camera,local_matrix({0,0,c.retraction},{0,0,0,1},{1,1,1})),local_matrix(foreground.position,foreground.rotation,{1,1,1})),local_matrix(d.entities[1].transform.position,d.entities[1].transform.rotation,d.entities[1].transform.scale));
    near(s.viewmodel->objects[0].world,expected);
    const auto desired=multiply(multiply(camera,local_matrix(foreground.position,foreground.rotation,{1,1,1})),local_matrix(profile().position,profile().rotation,{1,1,1}));
    for(unsigned k=0;k<3;++k)near(c.cast.position[k],desired[12+k]+camera[8+k]*profile().max_retraction,1e-9);
    const auto extension=c.cast.distance-c.retraction;const auto center_z=c.cast.position[2]+c.cast.direction[2]*extension;const auto& q=c.cast.rotation;
    check(center_z-c.cast.radius-c.cast.half_height*std::abs(2*(q[1]*q[2]+q[0]*q[3]))>=-.49-2e-5,"Rendered recoil/rotation envelope crosses the independent wall plane.");
    check(r.camera_pose(id(1))->world==RuntimeCameraPose{} && r.save_snapshot(digest)==saved,"Presented safety wrote back into committed camera effects.");
}
void blocked_anchor_and_avatar_ignore(){
    auto d=definition(false);auto blocker=node(3);blocker.transform.position={.18,-.17,.25};blocker.collider=BoxCollider{{.1f,.1f,.02f},BodyMotion::Static};blocker.mesh=RuntimeMesh{};d.entities.push_back(blocker);Runtime blocked(d);
    const auto saved=blocked.save_snapshot(digest);const auto s=blocked.snapshot(id(1));const auto& c=result(s);
    check(c.status==ViewmodelClearanceStatus::BlockedAnchor && c.suppressed && !c.resolved && c.anchor_hit && c.anchor_hit->initial_overlap && c.suppressed_entities==std::vector<std::string>{id(2)},"Blocked anchor was hidden as a resolved clear interval.");
    check(s.viewmodel && s.viewmodel->objects.empty() && s.objects.size()==1 && blocked.save_snapshot(digest)==saved,"Blocked policy changed world geometry, lens presence or saved state.");
    d=definition(false);auto actor=node(10);actor.character=CharacterController{};actor.character->camera=id(1);d.entities[0].parent=actor.id;d.entities[0].transform.position={0,1.6,0};d.entities.push_back(actor);Runtime own(d);
    const auto own_before=own.save_snapshot(digest);const auto actual=own.snapshot(id(1));const auto& ignored=result(actual);
    check(ignored.resolved && !ignored.suppressed && ignored.status==ViewmodelClearanceStatus::Clear && ignored.ignored_entities==std::vector<std::string>{id(10)},"Native policy did not ignore its physical camera ancestor.");
    auto raw=ignored.cast;raw.ignore.clear();raw.distance=0;check(own.capsule_cast(raw).has_value(),"Self-ignore fixture was not actually inside its native avatar volume.");
    check(own.save_snapshot(digest)==own_before,"Ancestor-ignore query changed complete controller state.");
}
void rigid_and_skinned_foreground(){
    auto d=definition();auto model=std::make_shared<ModelAsset>();model->nodes.resize(1);model->roots={0};model->nodes[0].name="weapon";model->nodes[0].skin=0;model->nodes[0].primitives={0};model->nodes[0].position=profile().position;
    auto mesh=std::make_shared<MeshAsset>();MeshVertex vertex;vertex.normal={0,0,1};vertex.tangent={1,0,0,1};mesh->vertices={vertex,vertex,vertex};mesh->indices={0,1,2};mesh->influences.assign(3,SkinWeight{{0,0,0,0},{1,0,0,0}});model->primitives={mesh};model->skins.push_back({"weapon",0,{0},{identity_matrix()}});
    auto rig=node(20,1);rig.animation_rig=RuntimeAnimationRig{model};auto joint=node(21,20);joint.rig_node=RuntimeRigNode{rig.id,0};joint.transform.position=model->nodes[0].position;
    auto weighted=node(22,21);weighted.mesh=RuntimeMesh{};weighted.mesh->mesh=mesh;weighted.skinned_mesh=RuntimeSkinnedMesh{rig.id,0};weighted.camera_draw=CameraDrawBinding{id(1),CameraDrawMode::Viewmodel};d.entities.insert(d.entities.end(),{rig,joint,weighted});Runtime r(d);
    const auto s=r.snapshot(id(1));const auto& c=result(s);check(c.resolved && s.viewmodel->objects.size()==2,"Mixed weighted/rigid foreground was lost.");
    const auto found=std::find_if(s.viewmodel->objects.begin(),s.viewmodel->objects.end(),[](const auto& object){return object.entity_id==id(22);});check(found!=s.viewmodel->objects.end() && found->skin && found->skin->palette.size()==1,"Real weighted foreground palette is absent.");
    near(found->world[14],profile().position[2]+c.retraction,1e-9);near(found->skin->palette[0],identity_matrix());
    check(r.entity(id(22)).world[14]==profile().position[2],"Clearance moved the committed weighted rig.");
}
void invalid_profiles_and_unused_templates(){
    for(unsigned kind=0;kind<10;++kind){auto d=definition(false);auto& p=*d.entities[0].camera->viewmodel_clearance;
        if(kind==0)p.radius=0;if(kind==1)p.half_height=-.01;if(kind==2)p.max_retraction=0;if(kind==3)p.skin=0;if(kind==4)p.skin=p.radius+.01;
        if(kind==5)p.rotation={0,0,0,0};if(kind==6)p.position[0]=10.01;if(kind==7)p.radius=std::numeric_limits<double>::quiet_NaN();
        if(kind==8)d.entities[0].camera->viewmodel.reset();if(kind==9)p.max_retraction=10.01;
        rejects([&]{Runtime rejected(d);});
    }
    auto d=definition(false);RuntimeSpawnTemplate t;t.id=id(50);t.name="Invalid unused clearance";t.root=id(51);auto camera=node(51);camera.camera=*d.entities[0].camera;camera.camera->viewmodel_clearance->skin=0;t.entities={camera};d.templates={t};rejects([&]{Runtime rejected(d);});
}
}
int main(){try{
    if(!Runtime::available()){std::cout<<"Native viewmodel clearance unavailable; simulation groups skipped.\n";return 0;}
    pure_default_and_clear();wall_retraction_and_owned_lifetime();presented_pose_and_current_geometry();blocked_anchor_and_avatar_ignore();rigid_and_skinned_foreground();invalid_profiles_and_unused_templates();
    std::cout<<"6 native viewmodel clearance groups passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
