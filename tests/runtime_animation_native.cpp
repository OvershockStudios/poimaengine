// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime_animation.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
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
RuntimeDefinition blend_definition() {
    auto d=definition();auto m=model();
    m->animations.push_back({"Target",2,{
        {1,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{10,4,0,0},{14,4,0,0}}},
        {1,AnimationPath::scale,AnimationInterpolation::linear,{0,2},{{3,3,3,0},{3,3,3,0}}}}});
    m->animations.push_back({"Interrupt",2,{{1,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{20,6,0,0},{20,6,0,0}}}}});
    const float y=static_cast<float>(std::sin(85*std::acos(-1.0)/180)),w=static_cast<float>(std::cos(85*std::acos(-1.0)/180));
    for(auto q:{std::array<float,4>{0,y,0,w},std::array<float,4>{0,-y,0,w},std::array<float,4>{0,-y,0,-w}})
        m->animations.push_back({"Rotation "+std::to_string(m->animations.size()),2,{{1,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},{q,q}}}});
    d.entities[0].animation_rig->model=m;d.entities[4].mesh->mesh=m->primitives[0];return d;
}
AnimationCommand fade(std::optional<std::uint32_t> clip,std::uint32_t ticks,double time=0,bool playing=true,double speed=1) {
    auto c=command(clip,time,playing,false,speed);c.blend_ticks=ticks;return c;
}
RuntimeTransform tip_pose(RuntimeAnimations& animations,std::uint64_t tick) {
    for(const auto& pose:animations.sample(tick))if(pose.entity=="tip")return pose.local;
    throw std::runtime_error("Blend fixture tip is absent.");
}
void blend_clocks_and_interruption() {
    RuntimeAnimations animations(blend_definition());animations.apply({fade(0,0)},0);
    near(tip_pose(animations,30).position[0],1);
    animations.apply({fade(2,60,0,true,2)},30);
    const auto initial=animations.state("rig",30);check(initial->transition.has_value(),"Fade missing at weight zero.");
    near(initial->transition->weight,0);near(initial->transition->source_time,.5);check(!initial->transition->source_frozen && initial->transition->source_clip==0,"First fade froze its advancing source.");
    near(tip_pose(animations,30).position[0],1);
    // Source advances to x=2; target advances at speed2 to x=12. The blend
    // weight is still 0.5, independent of either clip's playback speed.
    const auto half=tip_pose(animations,60);near(half.position[0],7);near(half.position[1],3);near(half.scale[0],2);
    const auto state=animations.state("rig",60);near(state->time,1);near(state->transition->source_time,1);near(state->transition->weight,.5);
    check(state->transition->source_speed==1 && !state->transition->source_loop && state->transition->source_playing,"Outgoing clock inspection differs.");
    animations.apply({fade(3,60)},60);const auto frozen=animations.checkpoint();
    check(frozen[0].transition && frozen[0].transition->frozen_source,"Interruption did not retain an immutable local pose.");
    const auto frozen_positions=(*frozen[0].transition->frozen_source)[1].position;
    const auto boundary=tip_pose(animations,60);check(boundary.position==half.position && boundary.scale==half.scale && boundary.rotation==half.rotation,"Interruption is discontinuous at its command tick.");
    near(tip_pose(animations,90).position[0],13.5);near(tip_pose(animations,90).scale[0],1.5);
    check(animations.state("rig",90)->transition->source_frozen,"Interrupted fade resumed an outgoing clip.");
    auto invalid=fade(0,3601);rejects([&]{ animations.apply({invalid},90); });
    check(animations.checkpoint()[0].transition->frozen_source==frozen[0].transition->frozen_source,"Rejected command replaced frozen source.");
    auto late_invalid=fade(2,0);late_invalid.entity="absent";
    rejects([&]{ animations.apply({fade(0,0),late_invalid},90); });
    near(tip_pose(animations,90).position[0],13.5);
    const auto completed=tip_pose(animations,120);near(completed.position[0],20);near(completed.scale[0],1);
    check(!animations.state("rig",120)->transition && !animations.checkpoint()[0].transition,"Completed fade retained active state or its buffer.");
    check((*frozen[0].transition->frozen_source)[1].position==frozen_positions,"Completed fade mutated checkpoint-owned source pose.");
    animations.apply({fade(0,60)},120);animations.apply({fade(3,0)},120);
    check(!animations.state("rig",120)->transition,"Immediate replacement did not cancel fade.");near(tip_pose(animations,120).position[0],20);
}
void blend_rest_and_rotations() {
    auto d=blend_definition();d.entities[2].transform.position={8,2,0};d.entities[2].transform.scale={2,2,2};
    RuntimeAnimations animations(d);animations.apply({fade(0,60,0,false)},0);
    const auto initial=animations.state("rig",0);check(initial->transition && !initial->transition->source_frozen && !initial->transition->source_clip && initial->transition->source_time==0,"Rest source metadata differs.");
    near(tip_pose(animations,30).position[0],4);near(tip_pose(animations,30).scale[0],2);
    near(tip_pose(animations,60).position[0],0);
    animations.apply({fade({},60)},60);near(tip_pose(animations,90).position[0],4);near(tip_pose(animations,120).position[0],8);
    check(!animations.state("rig",120)->playing && !animations.state("rig",120)->transition,"Transition to rest did not finish.");
    // Missing channels always use the authored baseline, never the previous
    // clip's scale. Rotation follows the short path 170 -> 190 degrees.
    animations.apply({fade(4,0,0,false)},120);animations.apply({fade(5,60,0,false)},120);
    const auto middle=tip_pose(animations,150);near(std::abs(middle.rotation[1]),1,1e-6);near(middle.rotation[3],0,1e-6);near(middle.scale[0],2);
    animations.apply({fade(6,0,0,false)},150);const auto source=tip_pose(animations,150);
    animations.apply({fade(4,60,0,false)},150);const auto equivalent=tip_pose(animations,180);
    double dot=0,norm=0;for(std::size_t k=0;k<4;++k) { dot+=source.rotation[k]*equivalent.rotation[k];norm+=equivalent.rotation[k]*equivalent.rotation[k]; }
    near(std::abs(dot),1);near(norm,1);
    const auto target=CompiledAnimation(*d.entities[0].animation_rig->model).sample(4,0,false);
    check(tip_pose(animations,210).rotation==target.local[1].rotation,"Completion did not return the exact destination quaternion.");
}
void blend_partition_and_rollback() {
    const auto d=blend_definition();Runtime a(d),b(d);
    a.step(30,{}, {}, {},{fade(0,0)});b.step(10,{}, {}, {},{fade(0,0)});b.step(20,{});
    a.step(45,{}, {}, {},{fade(2,120,0,true,2)});b.step(1,{}, {}, {},{fade(2,120,0,true,2)});b.step(44,{});
    a.step(15,{}, {}, {},{fade(3,90)});b.step(5,{}, {}, {},{fade(3,90)});b.step(10,{});
    check(a.entity("tip").world==b.entity("tip").world && a.entity("body").world==b.entity("body").world,"Interrupted blend depends on batch partitioning.");
    const auto before=a.animation("rig");const auto body=a.entity("body"),tip=a.entity("tip");const auto snapshot=a.snapshot("camera");const auto palette=snapshot.objects[0].skin->palette;
    rejects([&]{ a.step(60,{}, {}, {},{fade(1,60)}); });
    const auto after=a.animation("rig");check(after->clip==before->clip && after->time==before->time && after->transition->source_frozen && after->transition->start_tick==before->transition->start_tick && after->transition->weight==before->transition->weight,"Failed target replaced an active frozen transition.");
    check(a.inspect().tick==b.inspect().tick && a.entity("tip").world==tip.world && a.entity("body").world==body.world && a.entity("body").velocity==body.velocity,"Failed blend did not roll back physics/local poses.");
    check(a.snapshot("camera").objects[0].skin->palette==palette,"Failed blend changed skin palette.");
    a.step(100,{});b.step(20,{});b.step(80,{});
    check(a.entity("tip").world==b.entity("tip").world && a.entity("body").world==b.entity("body").world && !a.animation("rig")->transition,"Retried interrupted blend differs or did not complete.");
    check(snapshot.objects[0].skin->palette==palette,"Subsequent blend mutated an earlier snapshot.");
    // Active outgoing invalid curves reject before blending can conceal their
    // negative scale. Once a short transition ends, that source is irrelevant.
    Runtime source(d);source.step(1,{}, {}, {},{fade(1,0)});const auto valid=source.entity("tip");
    rejects([&]{ source.step(60,{}, {}, {},{fade(3,120)}); });
    check(source.inspect().tick==1 && source.entity("tip").world==valid.world && !source.animation("rig")->transition,"Invalid outgoing curve escaped rollback.");
    source.step(120,{}, {}, {},{fade(3,1)});check(!source.animation("rig")->transition,"Expired invalid source was evaluated after completion.");
    // A rejected direct native apply must not publish any replacement clocks.
    RuntimeAnimations direct(d);direct.apply({fade(2,60)},0);const auto control=direct.state("rig",0);
    rejects([&]{ direct.apply({fade(1,60,1,false)},0); });
    check(direct.state("rig",0)->clip==control->clip && direct.state("rig",0)->transition->start_tick==control->transition->start_tick,"Invalid command-time sample mutated direct native clocks.");
}

// Solve the six Hermite boundary conditions independently rather than copying
// the production decay coefficients. u is elapsed/duration, v is units/second.
double correction(double displacement,double velocity,double duration,double elapsed) {
    double rows[6][7]{};
    rows[0][0]=1;rows[0][6]=displacement;
    rows[1][1]=1;rows[1][6]=velocity*duration;
    rows[2][2]=2;
    for(std::size_t k=0;k<6;++k) {
        rows[3][k]=1;
        rows[4][k]=static_cast<double>(k);
        rows[5][k]=k>=2 ? static_cast<double>(k*(k-1)) : 0;
    }
    for(std::size_t column=0;column<6;++column) {
        auto pivot=column;
        for(std::size_t row=column+1;row<6;++row)if(std::abs(rows[row][column])>std::abs(rows[pivot][column]))pivot=row;
        check(std::abs(rows[pivot][column])>1e-12,"Independent polynomial system is singular.");
        for(std::size_t k=0;k<7;++k)std::swap(rows[pivot][k],rows[column][k]);
        const double divisor=rows[column][column];for(std::size_t k=column;k<7;++k)rows[column][k]/=divisor;
        for(std::size_t row=0;row<6;++row)if(row!=column) {
            const double factor=rows[row][column];
            for(std::size_t k=column;k<7;++k)rows[row][k]-=factor*rows[column][k];
        }
    }
    const double u=elapsed/duration;double value=rows[5][6];
    for(std::size_t k=5;k>0;--k)value=value*u+rows[k-1][6];
    return value;
}
AnimationCommand inertial(std::optional<std::uint32_t> clip,std::uint32_t ticks,double time=0,bool playing=true,double speed=1) {
    auto c=fade(clip,ticks,time,playing,speed);c.transition_mode=AnimationTransitionMode::Inertial;return c;
}
void warm(RuntimeAnimations& animations,std::uint64_t first,std::uint64_t last) {
    for(auto tick=first;tick<=last;++tick)(void)tip_pose(animations,tick);
}
void inertial_analytic_and_history() {
    const auto d=blend_definition();RuntimeAnimations animations(d),repeated(d);
    for(auto* value:{&animations,&repeated}) {value->apply({fade(0,0)},0);warm(*value,0,30);}
    const auto source=tip_pose(animations,30);
    // Repeated command-boundary reads must not replace tick29 with tick30.
    for(int i=0;i<5;++i)(void)tip_pose(repeated,30);
    for(auto* value:{&animations,&repeated})value->apply({inertial(2,60,0,true,2)},30);
    check(tip_pose(animations,30).position==source.position,"Inertial command boundary changed position.");
    check(tip_pose(animations,30).scale==source.scale,"Inertial command boundary changed scale.");
    check(animations.state("rig",30)->transition->mode==AnimationTransitionMode::Inertial,"Inertial mode is not inspectable.");
    for(std::uint64_t k=1;k<=60;++k) {
        const double t=static_cast<double>(k)/60;const auto actual=tip_pose(animations,30+k),other=tip_pose(repeated,30+k);
        near(actual.position[0],10+4*t+correction(-9,-2,1,t),1e-10);
        near(actual.position[1],4+correction(-2,0,1,t),1e-10);
        for(auto s:actual.scale)near(s,3*std::exp(correction(-std::log(3.0),0,1,t)),1e-10);
        check(actual.position==other.position && actual.rotation==other.rotation && actual.scale==other.scale,"Same-tick reads erased outgoing velocity.");
    }
    check(!animations.state("rig",90)->transition,"Inertial correction missed its endpoint.");
    check(tip_pose(animations,90).position==std::array<double,3>{14,4,0} && tip_pose(animations,90).scale==std::array<double,3>{3,3,3},"Completion is not the exact destination.");

    RuntimeAnimations startup(d);(void)tip_pose(startup,0);startup.apply({inertial(2,60,0,true,2)},0);
    const auto first=tip_pose(startup,1);near(first.position[0],10+4.0/60+correction(-10,-4,1,1.0/60),1e-10);
    // Equal positions do not imply equal velocity. Held targets have zero
    // incoming derivative even with a nonzero configured playback speed.
    RuntimeAnimations equal(d);equal.apply({fade(0,0)},0);warm(equal,0,30);
    equal.apply({inertial(0,60,.5,false,8)},30);
    near(tip_pose(equal,31).position[0],1+correction(0,2,1,1.0/60),1e-10);
    check(tip_pose(equal,31).position[0]>1,"Zero displacement discarded nonzero velocity.");
    near(tip_pose(equal,90).position[0],1);check(!equal.state("rig",90)->transition,"Held destination did not complete.");
    RuntimeAnimations sparse(d);sparse.apply({fade(0,0)},0);
    (void)tip_pose(sparse,5);const auto sparse_source=tip_pose(sparse,17);
    sparse.apply({inertial(3,60,0,false)},17);
    check(tip_pose(sparse,17).position==sparse_source.position,"Sparse-history command changed its source pose.");
    near(tip_pose(sparse,18).position[0],20+correction(17.0/30-20,2,1,1.0/60),1e-10);
    RuntimeAnimations overwritten(d);overwritten.apply({fade(0,0)},0);warm(overwritten,0,30);
    overwritten.apply({fade(0,0,.75,false)},30);near(tip_pose(overwritten,30).position[0],1.5);
    overwritten.apply({inertial(0,60,.75,false)},30);
    near(tip_pose(overwritten,31).position[0],1.5+correction(0,(1.5-29.0/30)*60,1,1.0/60),1e-10);
    auto scaled=d;auto scale_model=std::make_shared<ModelAsset>(*d.entities[0].animation_rig->model);
    scale_model->animations.push_back({"Independent scale axes",2,{{1,AnimationPath::scale,AnimationInterpolation::linear,{0,2},{{1,2,4,0},{3,6,8,0}}}}});
    scaled.entities[0].animation_rig->model=scale_model;RuntimeAnimations scaling(scaled);
    scaling.apply({fade(0,0)},0);warm(scaling,0,30);scaling.apply({inertial(7,60,0,true,1.5)},30);
    const double initial_scale[]{1,2,4},incoming_rate[]{1.5,3,3};
    for(std::uint64_t k=1;k<=60;++k) {
        const auto actual=tip_pose(scaling,30+k);const double t=static_cast<double>(k)/60;
        for(std::size_t axis=0;axis<3;++axis)near(actual.scale[axis],(initial_scale[axis]+incoming_rate[axis]*t)*
            std::exp(correction(-std::log(initial_scale[axis]),-incoming_rate[axis]/initial_scale[axis],1,t)),1e-9);
    }

    // Interrupt a composed inertial output: its last two output samples, not
    // either clip's derivative, determine the next correction's velocity.
    RuntimeAnimations interrupted(d);interrupted.apply({fade(0,0)},0);warm(interrupted,0,30);
    interrupted.apply({inertial(2,60,0,true,2)},30);warm(interrupted,30,45);
    const double x0=10+1+correction(-9,-2,1,.25);
    const double xprevious=10+4*(14.0/60)+correction(-9,-2,1,14.0/60);
    const double sx=3*std::exp(correction(-std::log(3.0),0,1,.25));
    const double sxprevious=3*std::exp(correction(-std::log(3.0),0,1,14.0/60));
    const auto boundary=tip_pose(interrupted,45);interrupted.apply({inertial(3,60,0,false)},45);
    check(tip_pose(interrupted,45).position==boundary.position && tip_pose(interrupted,45).scale==boundary.scale,"Second interruption lost its composed pose.");
    for(std::uint64_t k=1;k<=60;++k) {
        const auto actual=tip_pose(interrupted,45+k);const double t=static_cast<double>(k)/60;
        near(actual.position[0],20+correction(x0-20,(x0-xprevious)*60,1,t),1e-9);
        near(actual.scale[0],std::exp(correction(std::log(sx),(std::log(sx)-std::log(sxprevious))*60,1,t)),1e-9);
    }
    // An outgoing cubic clip may become invalid immediately after switching;
    // inertialization samples only the destination after capturing the source.
    RuntimeAnimations expired(d);expired.apply({fade(1,0)},0);warm(expired,0,1);expired.apply({inertial(3,60,0,false)},1);
    warm(expired,1,61);near(tip_pose(expired,61).position[0],20);
    auto invalid=inertial(0,60);invalid.transition_mode=static_cast<AnimationTransitionMode>(2);
    const auto before=expired.save_state(61);rejects([&]{expired.apply({invalid},61);});check(expired.save_state(61)==before,"Invalid mode mutated animation state.");
}
std::array<double,4> quaternion_product(const std::array<double,4>& a,const std::array<double,4>& b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
void inertial_rotations() {
    auto d=blend_definition();auto m=std::make_shared<ModelAsset>(*d.entities[0].animation_rig->model);
    const float half=static_cast<float>(std::sqrt(.5));
    m->animations.push_back({"Held X90",2,{{1,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},{{half,0,0,half},{half,0,0,half}}}}});
    m->animations.push_back({"Moving Y",2,{{1,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},{{0,0,0,1},{0,.5f,0,static_cast<float>(std::sqrt(.75))}}}}});
    d.entities[0].animation_rig->model=m;RuntimeAnimations rotation(d);
    rotation.apply({fade(7,0,0,false)},0);warm(rotation,0,30);const auto source=tip_pose(rotation,30);
    rotation.apply({inertial(8,3600)},30);const auto initial=tip_pose(rotation,30);
    check(initial.rotation==source.rotation,"Noncommuting rotation changed at command boundary.");
    // Four-point forward derivative of the independently observed quaternions.
    // The outgoing clip is constant, so spatial angular velocity must start at
    // zero even though the incoming clip rotates about a different axis.
    std::array<std::array<double,4>,4> q{initial.rotation};
    for(std::size_t i=1;i<q.size();++i) {
        q[i]=tip_pose(rotation,30+i).rotation;double dot=0;
        for(std::size_t j=0;j<4;++j)dot+=q[0][j]*q[i][j];
        if(dot<0)for(auto& value:q[i])value=-value;
    }
    std::array<double,4> derivative{};
    for(std::size_t j=0;j<4;++j)derivative[j]=10*(-11*q[0][j]+18*q[1][j]-9*q[2][j]+2*q[3][j]);
    const auto angular=quaternion_product(derivative,{-q[0][0],-q[0][1],-q[0][2],q[0][3]});
    for(std::size_t j=0;j<3;++j)near(2*angular[j],0,2e-4);
    // Antipodal representations and crossing +/-180 degrees must retain the
    // shortest orientation path, independently of the stored quaternion sign.
    RuntimeAnimations shortest(d);shortest.apply({fade(4,0,0,false)},0);warm(shortest,0,30);
    shortest.apply({inertial(5,60,0,false)},30);const auto middle=tip_pose(shortest,60);
    near(std::abs(middle.rotation[1]),1);near(middle.rotation[3],0);
    shortest.apply({fade(6,0,0,false)},60);warm(shortest,60,61);const auto equivalent=tip_pose(shortest,61);
    shortest.apply({inertial(4,60,0,false)},61);const auto same=tip_pose(shortest,91);
    double dot=0,norm=0;for(std::size_t j=0;j<4;++j){dot+=same.rotation[j]*equivalent.rotation[j];norm+=same.rotation[j]*same.rotation[j];}
    near(std::abs(dot),1);near(norm,1);
}
void inertial_partition_and_rollback() {
    const auto d=blend_definition();Runtime whole(d),split(d);
    whole.step(30,{}, {}, {},{fade(0,0)});split.step(1,{}, {}, {},{fade(0,0)});split.step(11,{});split.step(18,{});
    whole.step(15,{}, {}, {},{inertial(2,120,0,true,2)});split.step(1,{}, {}, {},{inertial(2,120,0,true,2)});split.step(3,{});split.step(11,{});
    whole.step(17,{}, {}, {},{inertial(3,90,0,false)});split.step(5,{}, {}, {},{inertial(3,90,0,false)});split.step(12,{});
    const std::string content(64,'a');check(whole.save_snapshot(content)==split.save_snapshot(content),"Inertial state/history depends on request partitioning.");
    const auto tick=whole.inspect().tick;const auto body=whole.entity("body"),tip=whole.entity("tip");
    const auto before=whole.save_snapshot(content);const auto snapshot=whole.snapshot("camera");
    const auto palette=snapshot.objects[0].skin->palette;
    rejects([&]{whole.step(60,{}, {}, {},{inertial(1,60)});});
    check(whole.inspect().tick==tick && whole.save_snapshot(content)==before,"Failed inertial batch did not restore correction/history.");
    check(whole.entity("body").world==body.world && whole.entity("body").velocity==body.velocity && whole.entity("tip").world==tip.world,"Failed inertial batch changed physics/pose.");
    check(whole.snapshot("camera").objects[0].skin->palette==palette,"Failed inertial batch changed palette.");
    // Immediately use the rolled-back history, before a successful tick could
    // overwrite it and hide a damaged checkpoint.
    whole.step(19,{}, {}, {},{inertial(0,120,.5,false)});split.step(1,{}, {}, {},{inertial(0,120,.5,false)});split.step(18,{});
    check(whole.save_snapshot(content)==split.save_snapshot(content),"Re-interruption after rollback used damaged velocity history.");
    whole.step(120,{});split.step(23,{});split.step(97,{});
    check(whole.save_snapshot(content)==split.save_snapshot(content) && !whole.animation("rig")->transition,"Partitioned inertial completion differs.");
    check(snapshot.objects[0].skin->palette==palette,"Later inertial samples mutated an owned palette.");
}
void inertial_whole_runtime_restore() {
    auto d=blend_definition();d.world_id="inertial-whole-save";d.authored_revision=55;
    const std::string content(64,'a');Runtime original(d);
    original.step(30,{}, {}, {},{fade(0,0)});
    original.step(15,{}, {}, {},{inertial(2,120,0,true,2)});
    original.step(17,{}, {}, {},{inertial(3,90,0,false)});
    const auto saved=original.save_snapshot(content);
    check(nlohmann::json::parse(saved).at("payload").at("animation").at("version")==2,"Whole snapshot did not embed version-2 animation state.");
    auto restored=Runtime::from_snapshot(d,content,saved);
    check(restored->save_snapshot(content)==saved,"Whole-runtime load changed inertial history, locals or physics bytes.");
    check(restored->entity("tip").world==original.entity("tip").world && restored->entity("body").world==original.entity("body").world &&
        restored->entity("body").velocity==original.entity("body").velocity,"Whole-runtime inertial restoration changed animation or physics.");
    const auto snapshot=original.snapshot("camera");const auto palette=snapshot.objects[0].skin->palette;
    // Re-interrupt immediately after restore, before advancing a successful tick
    // could reconstruct missing history and conceal a broken nested save/load.
    original.step(1,{}, {}, {},{inertial(0,120,.5,false)});
    restored->step(1,{}, {}, {},{inertial(0,120,.5,false)});
    check(restored->save_snapshot(content)==original.save_snapshot(content),"Whole-runtime restore lost velocity needed by immediate re-interruption.");
    original.step(19,{});restored->step(3,{});restored->step(16,{});
    check(restored->save_snapshot(content)==original.save_snapshot(content),"Restored inertial motion/physics differs after partitioned continuation.");
    original.step(101,{});restored->step(23,{});restored->step(78,{});
    check(restored->save_snapshot(content)==original.save_snapshot(content) && !restored->animation("rig")->transition,"Whole-runtime restored correction missed exact completion.");
    check(snapshot.objects[0].skin->palette==palette,"Whole-runtime restoration/continuation mutated an earlier palette.");
}

}
int main() {
    try { playback();partition_and_instances();authored_baseline_and_rollback();invalid_bindings();edited_hierarchy_overflow();blend_clocks_and_interruption();blend_rest_and_rotations();blend_partition_and_rollback();inertial_analytic_and_history();inertial_rotations();inertial_partition_and_rollback();inertial_whole_runtime_restore();std::cout<<"Runtime animation analytic poses, immutable palettes, clocks, instances, ownership, fixed-tick crossfades, independent inertial pose/velocity/log-scale references, noncommuting rotations, partitioning, physics rollback and whole-runtime save restoration passed.\n"; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
