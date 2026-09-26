// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime_animation.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace poima;
namespace {
void check(bool value,const char* text) { if(!value)throw std::runtime_error(text); }
void near(double a,double b,double epsilon=1e-6) { check(std::abs(a-b)<=epsilon,"Runtime animation analytic result differs."); }
template<class F> void rejects(F f) { bool failed=false;try { f(); }catch(const std::exception&) { failed=true; }check(failed,"Invalid rig or animation update succeeded."); }
std::shared_ptr<ModelAsset> model() {
    auto m=std::make_shared<ModelAsset>();m->nodes.resize(3);m->nodes[0].name="mesh";m->nodes[0].skin=0;m->nodes[0].position={7,0,0};m->nodes[0].primitives={0};
    m->nodes[1].name="tip";m->nodes[1].parent=2;m->nodes[1].position={0,2,0};m->nodes[2].name="root";m->roots={0,2};
    auto mesh=std::make_shared<MeshAsset>();MeshVertex v;v.position={1,2,0};v.normal={0,0,1};v.tangent={1,0,0,1};mesh->vertices={v,v,v};mesh->indices={0,1,2};
    mesh->influences.assign(3,SkinWeight{{0,1,0,0},{.5f,.5f,0,0}});m->primitives={mesh};
    auto bind=identity_matrix();bind[13]=-2;m->skins.push_back({"skin",2,{2,1},{identity_matrix(),bind}});
    m->animations.push_back({"Move",2,{{1,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,2,0,0},{4,2,0,0}}}}});
    // Endpoint scales are valid; Hermite tangents make the interior invalid.
    m->animations.push_back({"Invalid interior",2,{{1,AnimationPath::scale,AnimationInterpolation::cubic,{0,2},{{0,0,0,0},{1,1,1,0},{-8,0,0,0},{8,0,0,0},{1,1,1,0},{0,0,0,0}}}}});
    return m;
}
RuntimeDefinition definition() {
    RuntimeDefinition d;d.world_id="animation-native";d.authored_revision=4;auto m=model();
    RuntimeEntityDefinition rig;rig.id="rig";rig.transform.position={10,0,0};rig.animation_rig=RuntimeAnimationRig{m};d.entities.push_back(rig);
    const std::string ids[]{"meshnode","tip","bone"};
    for(std::size_t i=0;i<3;++i) {
        RuntimeEntityDefinition n;n.id=ids[i];n.parent=i==1 ? "bone" : "rig";
        n.transform.position=m->nodes[i].position;n.rig_node=RuntimeRigNode{"rig",static_cast<std::uint32_t>(i)};d.entities.push_back(n);
    }
    RuntimeEntityDefinition skin;skin.id="render";skin.parent="meshnode";skin.mesh=RuntimeMesh{};skin.mesh->mesh=m->primitives[0];skin.skinned_mesh=RuntimeSkinnedMesh{"rig",0};d.entities.push_back(skin);
    RuntimeEntityDefinition camera;camera.id="camera";camera.camera=RuntimeCamera{};camera.transform.position={10,2,8};d.entities.push_back(camera);
    RuntimeEntityDefinition body;body.id="body";body.transform.position={0,10,0};body.collider=BoxCollider{};body.collider->motion=BodyMotion::Dynamic;d.entities.push_back(body);return d;
}
AnimationCommand command(std::optional<std::uint32_t> clip=0,double time=0,bool playing=true,bool loop=true,double speed=1) { return {"rig",clip,time,speed,loop,playing}; }
void playback() {
    auto d=definition();Runtime r(d);check(!r.animation("body"),"Nonrig unexpectedly has animation.");check(!r.animation("rig")->clip,"Default rig should retain authored rest pose.");
    const auto before=r.snapshot("camera");check(before.objects.size()==1 && bool(before.objects[0].skin),"Skinned runtime snapshot is missing.");const auto old_palette=before.objects[0].skin->palette;
    r.step(60,{}, {}, {}, {command()});near(r.entity("tip").local.position[0],2);near(r.entity("tip").world[12],12);near(r.animation("rig")->time,1);
    auto live=r.snapshot("camera");auto deformed=deform_mesh(*live.objects[0].mesh,live.objects[0].skin->palette);
    near(deformed->vertices[0].position[0]+live.objects[0].world[12],12);check(before.objects[0].skin->palette==old_palette,"Snapshot palette changed after stepping.");
    r.step(10,{}, {}, {}, {command(0,.5,false)});near(r.entity("tip").local.position[0],1);near(r.animation("rig")->time,.5);
    r.step(60,{}, {}, {}, {command(0,1.5,true,true,2)});near(r.animation("rig")->time,1.5);near(r.entity("tip").local.position[0],3);
    r.step(120,{}, {}, {}, {command(0,1,true,false)});near(r.animation("rig")->time,2);check(!r.animation("rig")->playing,"Nonlooping clip did not stop at end.");
    r.step(1,{}, {}, {}, {command(std::nullopt)});near(r.entity("tip").local.position[0],0);near(r.animation("rig")->time,0);check(!r.animation("rig")->playing,"Rest rig must not play.");
}
void partition_and_instances() {
    auto d=definition();const auto original=d.entities;
    for(auto e:original)if(e.id!="camera" && e.id!="body") {
        e.id="second-"+e.id;if(!e.parent.empty())e.parent="second-"+e.parent;
        if(e.rig_node)e.rig_node->rig="second-rig";if(e.skinned_mesh)e.skinned_mesh->rig="second-rig";
        if(e.animation_rig)e.transform.position={-10,0,0};d.entities.push_back(e);
    }
    Runtime a(d),b(d);a.step(100,{}, {}, {}, {command()});b.step(1,{}, {}, {}, {command()});b.step(49,{});b.step(50,{});
    check(a.entity("tip").world==b.entity("tip").world,"Animation depends on batch partitioning.");check(a.entity("body").world==b.entity("body").world,"Physics depends on animation batch partitioning.");near(a.entity("second-tip").local.position[0],0);near(a.animation("second-rig")->time,0);
}
void authored_baseline_and_rollback() {
    auto d=definition();d.entities[2].transform.position={9,2,0};Runtime r(d);near(r.entity("tip").local.position[0],9);
    const auto body=r.entity("body"),tip=r.entity("tip");const auto snapshot=r.snapshot("camera");
    rejects([&] { r.step(60,{}, {}, {}, {command(1)}); });check(r.inspect().tick==0,"Failed animation advanced tick.");
    check(r.entity("body").world==body.world && r.entity("body").velocity==body.velocity,"Failed animation advanced physics.");
    check(r.entity("tip").world==tip.world && !r.animation("rig")->clip,"Failed animation changed pose/control.");
    check(r.snapshot("camera").objects[0].skin->palette==snapshot.objects[0].skin->palette,"Failed animation changed skin palette.");
    Runtime fresh(d);r.step(60,{}, {}, {}, {command()});fresh.step(60,{}, {}, {}, {command()});check(r.entity("body").world==fresh.entity("body").world,"Rollback left physics different on retry.");
    r.step(1,{}, {}, {}, {command(std::nullopt)});near(r.entity("tip").local.position[0],9);
    const auto tick=r.inspect().tick;rejects([&] { r.step(1,{}, {}, {}, {command(99)}); });rejects([&] { r.step(1,{}, {}, {}, {command(),command()}); });check(r.inspect().tick==tick,"Invalid commands advanced tick.");
}
void invalid_bindings() {
    const auto base=definition();validate_runtime_animation(base);
    auto bad=[&](auto mutate) { auto d=base;mutate(d);rejects([&] { validate_runtime_animation(d); }); };
    bad([](auto& d) { d.entities[2].rig_node->node=0; });
    bad([](auto& d) { d.entities[2].rig_node.reset(); });
    bad([](auto& d) { d.entities[2].parent="camera"; });
    bad([](auto& d) { d.entities[2].collider=BoxCollider{}; });
    bad([](auto& d) { d.entities[2].camera=RuntimeCamera{};d.entities.back().collider.reset();d.entities.back().character=CharacterController{};d.entities.back().character->camera="tip"; });
    bad([](auto& d) { d.entities[4].transform.position={1,0,0}; });
    bad([](auto& d) { d.entities[4].skinned_mesh->node=1; });
    bad([](auto& d) { d.entities[4].acoustics=AcousticMaterial{}; });
    bad([](auto& d) { d.entities[2].parent="tip"; });
}
void edited_hierarchy_overflow() {
    auto d=definition();auto m=model();m->animations[0].channels[0]={1,AnimationPath::scale,AnimationInterpolation::linear,{0,2},{{1,1,1,0},{100000000,1,1,0}}};
    d.entities[0].animation_rig->model=m;d.entities[4].mesh->mesh=m->primitives[0];
    // The source hierarchy stays bounded, but an allowed authored reparent
    // multiplies two large scales when the clip starts. Check the live tree.
    d.entities[1].parent="tip";d.entities[1].transform.scale={100000000,1,1};
    Runtime r(d);const auto before=r.entity("meshnode").world;
    rejects([&] { r.step(1,{}, {}, {}, {command()}); });
    check(r.inspect().tick==0 && r.entity("meshnode").world==before && !r.animation("rig")->clip,"Edited hierarchy overflow escaped rollback.");
}
}
int main() {
    try { playback();partition_and_instances();authored_baseline_and_rollback();invalid_bindings();edited_hierarchy_overflow();std::cout<<"Runtime animation analytic poses, immutable palettes, clocks, instances, ownership and physics rollback passed.\n"; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
