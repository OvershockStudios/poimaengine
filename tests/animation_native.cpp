// SPDX-License-Identifier: Apache-2.0
#include "poima/animation.hpp"
#include "../third_party/nlohmann/json.hpp"
#include <bit>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
using namespace poima;
namespace {
void check(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
void near(double value,double expected,double tolerance=1e-5) { check(std::abs(value-expected)<tolerance,"Analytic animation result disagrees."); }
template<class F> void rejects(F f) { bool rejected=false;try { f(); }catch(const std::exception&) { rejected=true; }check(rejected,"Invalid animation was accepted."); }
ModelAsset model() {
    ModelAsset m;m.nodes.resize(3);m.nodes[0].name="mesh";m.nodes[0].primitives={0};m.nodes[0].skin=0;m.nodes[0].position={7,0,0};
    m.nodes[1].name="child";m.nodes[1].parent=2;m.nodes[1].position={0,2,0};m.nodes[2].name="root";m.roots={0,2};
    auto mesh=std::make_shared<MeshAsset>();MeshVertex v;v.position={1,2,0};v.normal={0,0,1};v.tangent={1,0,0,1};
    mesh->vertices={v,v,v};mesh->indices={0,1,2};mesh->has_uv=true;
    mesh->influences.assign(3,SkinWeight{{0,1,0,0},{.5f,.5f,0,0}});m.primitives={mesh};
    auto child_bind=identity_matrix();child_bind[13]=-2;
    // Palette indices deliberately differ from both hierarchy and node order.
    m.skins.push_back({"rig",2,{2,1},{identity_matrix(),child_bind}});return m;
}
AnimationChannel translation() { return {1,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{0,2,0,0},{4,2,0,0}}}; }
void curves() {
    auto m=model();m.animations.push_back({"move",2,{translation()}});
    auto pose=sample_model(m,0,1,false);near(pose.local[1].position[0],2);near(pose.world[1][13],2);
    near(sample_model(m,0,3,false).local[1].position[0],4);near(sample_model(m,0,3,true).local[1].position[0],2);
    near(sample_model(m,0,2,true).time,0);near(sample_model(m,std::nullopt,1,false).local[1].position[0],0);
    auto& c=m.animations[0].channels[0];c.interpolation=AnimationInterpolation::step;
    near(sample_model(m,0,1.99,false).local[1].position[0],0);near(sample_model(m,0,2,false).local[1].position[0],4);
    c.times={1,2};near(sample_model(m,0,0,false).local[1].position[1],2);
    c.times={0};c.values={{3,2,0,0}};m.animations[0].duration=0;
    near(sample_model(m,0,999,true).local[1].position[0],3);
    c={1,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},{{0,0,0,1},{0,0,1,0}}};m.animations[0].duration=2;
    pose=sample_model(m,0,1,false);near(pose.local[1].rotation[2],std::sqrt(.5));near(pose.world[1][0],0);near(pose.world[1][1],1);
    c.values={{0,0,0,1},{0,0,0,-1}};near(sample_model(m,0,1,false).local[1].rotation[3],1);
    // Hermite endpoint tangents are derivatives: the two-second interval
    // contributes dt * tangent. Expected midpoint = 2 + .25 * (2 - 0).
    c={1,AnimationPath::translation,AnimationInterpolation::cubic,{0,2},{{0,0,0,0},{0,2,0,0},{2,0,0,0},{0,0,0,0},{4,2,0,0},{0,0,0,0}}};
    near(sample_model(m,0,1,false).local[1].position[0],2.5);
    c={1,AnimationPath::rotation,AnimationInterpolation::cubic,{0,2},{{0,0,0,0},{0,0,0,1},{0,0,0,0},{0,0,0,0},{0,0,1,0},{0,0,0,0}}};
    near(sample_model(m,0,1,false).local[1].rotation[2],std::sqrt(.5));
    c.values[4]={0,0,0,-1};rejects([&] { sample_model(m,0,1,false); }); // Cubic must not flip quaternion signs.
}
void same_pose(const ModelPose& a,const ModelPose& b) {
    check(a.time==b.time && a.local.size()==b.local.size() && a.world==b.world,"Compiled/reference pose differs.");
    for(std::size_t i=0;i<a.local.size();++i)
        check(a.local[i].position==b.local[i].position && a.local[i].scale==b.local[i].scale &&
            a.local[i].rotation==b.local[i].rotation,"Compiled/reference local pose differs.");
}
void compiled_sampling() {
    auto source=model();source.animations.push_back({"linear",2,{translation()}});
    source.animations.push_back({"step",2,{{1,AnimationPath::rotation,AnimationInterpolation::step,{0,2},{{0,0,0,1},{0,0,1,0}}}}});
    source.animations.push_back({"cubic",2,{{1,AnimationPath::translation,AnimationInterpolation::cubic,{0,2},
        {{0,0,0,0},{0,2,0,0},{2,0,0,0},{0,0,0,0},{4,2,0,0},{0,0,0,0}}}}});
    const CompiledAnimation compiled(source);
    for(auto clip:std::vector<std::optional<std::uint32_t>>{std::nullopt,0,1,2})
        for(double time:{0.,.1,1.,1.99,2.,3.,1000000000.})for(bool loop:{false,true})
            same_pose(compiled.sample(clip,time,loop),sample_model(source,clip,time,loop));
    // Independent analytic checkpoints still qualify the cached path, rather
    // than relying only on parity with the convenience wrapper that uses it.
    near(compiled.sample(0,1,false).local[1].position[0],2);
    near(compiled.sample(2,1,false).local[1].position[0],2.5);
    near(compiled.sample(1,1.99,false).local[1].rotation[3],1);
    near(compiled.sample(1,2,false).local[1].rotation[2],1);
    near(compiled.sample(0,2,true).time,0);
    near(compiled.sample(0,3,false).time,2);
    near(compiled.sample(std::nullopt,1000000000.,true).time,1000000000.);

    const auto expected=compiled.sample(0,1,false);
    std::weak_ptr<const MeshAsset> geometry=source.primitives[0];
    source.nodes[1].position={999,999,999};source.nodes[1].parent=1;
    source.animations[0].channels[0].values[1][0]=-999;
    source.animations[0].channels[0].times.clear();source.animations[0].duration=999;
    source=ModelAsset{};
    check(geometry.expired(),"Compiled curves unnecessarily retained shared model geometry.");
    same_pose(compiled.sample(0,1,false),expected);
    auto modified=compiled.sample(0,1,false);modified.local[1].position[0]=99;modified.world[1][12]=99;
    same_pose(compiled.sample(0,1,false),expected);
    const auto copy=compiled;same_pose(copy.sample(0,1,false),expected);

    auto baseline=compiled.sample(std::nullopt,0,false).local;
    baseline[0].position={17,0,0};baseline[2].position={10,20,30};baseline[2].scale={2,3,4};
    baseline[1].position={100,200,300};baseline[1].scale={2,3,4};
    baseline[1].rotation={0,0,std::sqrt(.5),std::sqrt(.5)};
    const auto baseline_copy=baseline;
    const auto custom=compiled.sample(0,1,false,baseline);
    near(custom.local[1].position[0],2);near(custom.local[1].position[1],2);near(custom.local[1].position[2],0);
    check(custom.local[1].scale==baseline[1].scale && custom.local[1].rotation==baseline[1].rotation,
        "Animated translation overwrote unanimated authored channels.");
    near(custom.world[1][12],14);near(custom.world[1][13],26);near(custom.world[1][14],30);near(custom.world[0][12],17);
    const auto rest=compiled.sample(std::nullopt,0,false,baseline);
    near(rest.world[1][12],210);near(rest.world[1][13],620);near(rest.world[1][14],1230);
    for(std::size_t i=0;i<baseline.size();++i)
        check(baseline[i].position==baseline_copy[i].position && baseline[i].scale==baseline_copy[i].scale &&
            baseline[i].rotation==baseline_copy[i].rotation,"Sampling mutated caller baseline storage.");
    baseline[2].position[0]=20;near(compiled.sample(0,1,false,baseline).world[1][12],24);
    same_pose(compiled.sample(0,1,false),expected);

    rejects([&] { compiled.sample(99,0,false); });
    for(double time:{-1.,1000000001.,std::numeric_limits<double>::infinity(),std::nan("")})
        rejects([&] { compiled.sample(0,time,false); });
    rejects([&] { compiled.sample(0,0,false,std::span<const NodePose>(baseline.data(),2)); });
    auto reject_baseline=[&](auto change) { auto invalid=baseline;change(invalid);rejects([&] { compiled.sample(0,1,false,invalid); }); };
    // Invalid authored fields reject even when the animation would override them.
    reject_baseline([](auto& nodes) { nodes[1].position[0]=std::nan(""); });
    reject_baseline([](auto& nodes) { nodes[1].position[0]=1000000001.; });
    reject_baseline([](auto& nodes) { nodes[1].scale[1]=0; });
    reject_baseline([](auto& nodes) { nodes[0].scale[0]=std::numeric_limits<double>::infinity(); });
    reject_baseline([](auto& nodes) { nodes[1].rotation={0,0,0,2}; });
    reject_baseline([](auto& nodes) { nodes[0].rotation={0,0,0,std::nan("")}; });
    reject_baseline([](auto& nodes) { nodes[2].scale={1e9,1e9,1e9};nodes[1].scale={1e9,1e9,1e9}; });
    same_pose(compiled.sample(0,1,false),expected); // Failure leaves the compiled snapshot reusable.

    auto one=model();one.animations.push_back({"constant",0,{{1,AnimationPath::translation,AnimationInterpolation::linear,{0},{{3,2,0,0}}}}});
    CompiledAnimation constant(one);near(constant.sample(0,1000,true).local[1].position[0],3);near(constant.sample(0,1000,true).time,0);
    auto overshoot=model();overshoot.animations.push_back({"invalid-between-keys",1,{{1,AnimationPath::scale,AnimationInterpolation::cubic,{0,1},
        {{0,0,0,0},{1,1,1,0},{-8,0,0,0},{8,0,0,0},{1,1,1,0},{0,0,0,0}}}}});
    const CompiledAnimation between(overshoot);rejects([&] { between.sample(0,.5,false); });
    near(between.sample(0,0,false).local[1].scale[0],1);near(between.sample(0,1,false).local[1].scale[0],1);

    ModelAsset deep;deep.nodes.resize(10000);deep.roots={9999};
    for(std::size_t i=0;i<deep.nodes.size();++i) { deep.nodes[i].parent=i+1<deep.nodes.size() ? int(i+1) : -1;deep.nodes[i].position={.001,0,0}; }
    const CompiledAnimation hierarchy(deep);near(hierarchy.sample(std::nullopt,0,false).world[0][12],10,1e-9);
}
void analytic_local_motion() {
    auto make=[](AnimationChannel channel) {
        ModelAsset source;source.nodes.resize(2);source.nodes[1].parent=0;source.roots={0};
        source.animations.push_back({"Motion",double(channel.times.back()),{std::move(channel)}});
        return source;
    };
    const auto zero=[](const NodeMotion& motion) {
        for(const auto& vector:{motion.translation_velocity,motion.angular_velocity,motion.log_scale_velocity})
            for(double value:vector)check(value==0,"Constant/held animation channel has a nonzero derivative.");
    };
    auto linear=make({1,AnimationPath::translation,AnimationInterpolation::linear,{1,2,4},
        {{0,3,4,0},{4,5,8,0},{2,11,0,0}}});
    const CompiledAnimation positions(linear);
    for(double time:{0.,.5,1.,1.5,2.,3.,4.,8.}) {
        const auto motion=positions.sample_motion(0,time);
        same_pose(motion.pose,positions.sample(0,time,false));zero(motion.velocities[0]);
        const auto expected=time<1 || time>=4 ? std::array<double,3>{} :
            time<2 ? std::array<double,3>{4,2,4} : std::array<double,3>{-1,3,-4};
        check(motion.velocities[1].translation_velocity==expected,"Translation derivative did not use the right segment/held endpoint.");
    }
    auto step=linear;step.animations[0].channels[0].interpolation=AnimationInterpolation::step;
    const CompiledAnimation discrete(step);
    for(double time:{0.,1.,1.9,2.,3.,4.})zero(discrete.sample_motion(0,time).velocities[1]);
    const CompiledAnimation scales(make({1,AnimationPath::scale,AnimationInterpolation::linear,{0,2},
        {{1,2,4,0},{3,6,2,0}}}));
    const auto scaled=scales.sample_motion(0,1);
    near(scaled.velocities[1].log_scale_velocity[0],.5,1e-12);
    near(scaled.velocities[1].log_scale_velocity[1],.5,1e-12);
    near(scaled.velocities[1].log_scale_velocity[2],-1./3,1e-12);
    zero(scales.sample_motion(0,2).velocities[1]);

    // x(t)=1+2t+3t² on [0,2]; Hermite endpoint tangents reproduce
    // the polynomial exactly, so its independent derivative is 2+6t.
    const CompiledAnimation polynomial(make({1,AnimationPath::translation,AnimationInterpolation::cubic,{0,2},
        {{0,0,0,0},{1,0,0,0},{2,0,0,0},{14,0,0,0},{17,0,0,0},{0,0,0,0}}}));
    for(double time:{0.,.125,.5,1.,1.875}) {
        const auto result=polynomial.sample_motion(0,time);
        near(result.pose.local[1].position[0],1+2*time+3*time*time,1e-12);
        near(result.velocities[1].translation_velocity[0],2+6*time,1e-12);
    }
    zero(polynomial.sample_motion(0,2).velocities[1]);
    const CompiledAnimation growing(make({1,AnimationPath::scale,AnimationInterpolation::cubic,{0,2},
        {{0,0,0,0},{1,1,1,0},{2,0,0,0},{14,0,0,0},{17,1,1,0},{0,0,0,0}}}));
    near(growing.sample_motion(0,.5).velocities[1].log_scale_velocity[0],5./2.75,1e-12);

    const double pi=std::acos(-1.);
    const CompiledAnimation turn(make({1,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},
        {{0,0,0,1},{0,0,1,0}}}));
    for(double time:{0.,.25,1.,1.75}) {
        const auto result=turn.sample_motion(0,time);same_pose(result.pose,turn.sample(0,time,false));
        near(result.velocities[1].angular_velocity[0],0,1e-12);near(result.velocities[1].angular_velocity[1],0,1e-12);
        near(result.velocities[1].angular_velocity[2],pi/2,1e-12);
    }
    zero(turn.sample_motion(0,2).velocities[1]);
    const CompiledAnimation antipodal(make({1,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},
        {{0,0,0,1},{0,0,0,-1}}}));
    zero(antipodal.sample_motion(0,1).velocities[1]);
    const float half=std::sqrt(.5f);
    // q1=Rz(90°)*q0, q0=Rx(90°). Spatial velocity must be Z;
    // inverse(q)*qdot would incorrectly report the body's Y axis.
    const CompiledAnimation noncommuting(make({1,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},
        {{half,0,0,half},{.5f,.5f,.5f,.5f}}}));
    for(double time:{0.,.5,1.5}) {
        const auto result=noncommuting.sample_motion(0,time);
        near(result.velocities[1].angular_velocity[0],0,2e-7);
        near(result.velocities[1].angular_velocity[1],0,2e-7);
        near(result.velocities[1].angular_velocity[2],pi/4,2e-7);
    }
    const float sine=std::sin(.01f),cosine=std::cos(.01f);
    const CompiledAnimation small_turn(make({1,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},
        {{0,0,0,1},{0,0,sine,cosine}}}));
    for(double time:{0.,.5,1.,1.5}) {
        const double u=time/2,z=u*sine,w=1+u*(double(cosine)-1);
        near(small_turn.sample_motion(0,time).velocities[1].angular_velocity[2],double(sine)/(z*z+w*w),1e-12);
    }
    const CompiledAnimation cubic_turn(make({1,AnimationPath::rotation,AnimationInterpolation::cubic,{0,2},
        {{0,0,0,0},{0,0,0,1},{0,0,0,0},{0,0,0,0},{0,0,1,0},{0,0,0,0}}}));
    // Smoothstep raw quaternion [0,0,s,1-s], s=3u²-2u³.
    for(double time:{0.,.25,1.,1.5}) {
        const double u=time/2,s=3*u*u-2*u*u*u,ds=3*u-3*u*u;
        near(cubic_turn.sample_motion(0,time).velocities[1].angular_velocity[2],2*ds/(s*s+(1-s)*(1-s)),1e-12);
    }
    const CompiledAnimation cubic_spatial(make({1,AnimationPath::rotation,AnimationInterpolation::cubic,{0,2},
        {{0,0,0,0},{half,0,0,half},{0,half,half,0},{0,0,0,0},{half,0,0,half},{0,0,0,0}}}));
    const auto spatial=cubic_spatial.sample_motion(0,0).velocities[1].angular_velocity;
    near(spatial[0],0,1e-12);near(spatial[1],0,1e-12);near(spatial[2],2,1e-12);

    auto baseline=positions.sample({},0,false).local;baseline[0].rotation={0,0,1,0};baseline[1].scale={2,3,4};
    const auto saved_baseline=baseline;
    const auto rest=positions.sample_motion({},1,baseline);
    same_pose(rest.pose,positions.sample({},1,false,baseline));for(const auto& motion:rest.velocities)zero(motion);
    const auto custom=positions.sample_motion(0,1.5,baseline);same_pose(custom.pose,positions.sample(0,1.5,false,baseline));
    check(custom.velocities[1].translation_velocity==std::array<double,3>{4,2,4},"Parent rotation altered local derivative coordinates.");
    check(baseline[0].rotation==saved_baseline[0].rotation && baseline[1].scale==saved_baseline[1].scale,"Motion sampling mutated baseline.");
    rejects([&]{positions.sample_motion(99,0);});
    for(double time:{-1.,1000000001.,std::numeric_limits<double>::infinity(),std::nan("")})rejects([&]{positions.sample_motion(0,time);});
    auto invalid_baseline=baseline;invalid_baseline[0].rotation={0,0,0,0};rejects([&]{positions.sample_motion(0,1,invalid_baseline);});
    const CompiledAnimation collapsing(make({1,AnimationPath::rotation,AnimationInterpolation::cubic,{0,2},
        {{0,0,0,0},{0,0,0,1},{0,0,0,0},{0,0,0,0},{0,0,0,-1},{0,0,0,0}}}));
    rejects([&]{collapsing.sample_motion(0,1);});
    // A future invalid interior must not invalidate the valid current key.
    const CompiledAnimation future_invalid(make({1,AnimationPath::scale,AnimationInterpolation::cubic,{0,1},
        {{0,0,0,0},{1,1,1,0},{-8,0,0,0},{8,0,0,0},{1,1,1,0},{0,0,0,0}}}));
    near(future_invalid.sample_motion(0,0).velocities[1].log_scale_velocity[0],-8,1e-12);
    rejects([&]{future_invalid.sample_motion(0,.5);});
    same_pose(positions.sample_motion(0,1.5).pose,positions.sample(0,1.5,false));
}
void skinning() {
    auto m=model();m.animations.push_back({"move",2,{translation()}});auto pose=sample_model(m,0,1,false);
    auto palette=skin_palette(m,pose,0);auto deformed=deform_mesh(*m.primitives[0],palette);
    // Mesh node's authored +7 translation cancels; half the weights follow
    // the child translated by +2, so world-space x is 1 + 0.5*2 = 2.
    near(deformed->vertices[0].position[0]+pose.world[0][12],2);near(deformed->vertices[0].position[1],2);
    check(deformed->influences.empty() && m.primitives[0]->influences.size()==3,"Reference deformation mutated its source.");
    auto matrix=identity_matrix();matrix[0]=2;matrix[5]=3;matrix[10]=4;
    auto mesh=*m.primitives[0];for(auto& v:mesh.vertices) { const float h=std::sqrt(.5f);v.normal={h,h,0};v.tangent={h,-h,0,1}; }
    deformed=deform_mesh(mesh,std::vector<Matrix4>{matrix,matrix});
    near(deformed->vertices[0].normal[0],3/std::sqrt(13.));near(deformed->vertices[0].normal[1],2/std::sqrt(13.));check(valid_tangent(deformed->vertices[0]),"Deformed tangent frame is invalid.");
    matrix[0]=-2;deformed=deform_mesh(mesh,std::vector<Matrix4>{matrix,matrix});near(deformed->vertices[0].tangent[3],-1);
    matrix[0]=0;rejects([&] { deform_mesh(mesh,std::vector<Matrix4>{matrix,matrix}); });
}
void packages() {
    auto m=model();m.animations.push_back({"move",2,{translation()}});const auto bytes=encode_model(m);auto copy=decode_model(bytes);
    check(copy->package_version==4 && copy->skins[0].joints==m.skins[0].joints,"Skin package round trip failed.");
    check(encode_model(*copy)==bytes,"Cooked animation is not canonical on round trip.");near(sample_model(*copy,0,1,false).local[1].position[0],2);
    for(std::size_t size:{0u,15u,100u})rejects([&] { decode_model(bytes.substr(0,size)); });rejects([&] { decode_model(bytes.substr(0,bytes.size()-1)); });
    // Weighted primitive remains preserved even if no node uses it.
    m.animations.clear();m.skins.clear();m.nodes[0].skin=-1;m.nodes[0].primitives.clear();copy=decode_model(encode_model(m));
    check(copy->package_version==4 && copy->primitives[0]->influences.size()==3,"Unused skin weights were lost.");
    auto mesh=std::make_shared<MeshAsset>(*m.primitives[0]);mesh->influences.clear();m.primitives={mesh};m.nodes[0].primitives={0};
    check(encode_model(m).substr(0,8)=="POIMAM03","Static model changed package format.");
}
// Independent fixed cardinalities: changing another engine limit must not make
// these channel boundary cases silently exercise a different constraint.
static_assert(max_animation_channels==16384 && max_animation_keys==1000000);
static_assert(max_model_clips==256 && max_model_skins==64 && max_skin_joints==256);
template<class F> void rejects_exact(F&& operation,const char* expected) {
    bool caught=false;
    try { operation(); }
    catch(const std::exception& error) { caught=true;check(std::string(error.what())==expected,"Boundary rejected for a different constraint."); }
    check(caught,"Over-bound animation was accepted.");
}
void budget_word(std::string& bytes,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<char>((value>>(8*i))&255));
}
std::uint32_t budget_u32(const std::string& bytes,std::size_t offset) {
    check(offset+4<=bytes.size(),"Test package prefix is truncated.");std::uint32_t result=0;
    for(unsigned i=0;i<4;++i)result|=std::uint32_t(static_cast<unsigned char>(bytes[offset+i]))<<(8*i);return result;
}
void aggregate_channel_boundary() {
    auto m=model();m.nodes.resize(129);
    for(unsigned node=3;node<=128;++node) {m.nodes[node].name="budget"+std::to_string(node);m.nodes[node].parent=2;}
    for(unsigned clip=0;clip<128;++clip) {
        AnimationClip animation;animation.name="take"+std::to_string(clip);animation.duration=0;
        for(unsigned node=1;node<=128;++node)animation.channels.push_back({node,AnimationPath::translation,AnimationInterpolation::linear,{0},{{float(clip),float(node),0,0}}});
        m.animations.push_back(std::move(animation));
    }
    std::size_t channels=0;for(const auto& clip:m.animations)channels+=clip.channels.size();
    check(channels==16384 && m.animations.size()==128 && m.skins.size()==1 && m.skins[0].joints.size()==2,"Wrong independent aggregate fixture.");
    validate_animation_data(m);
    const auto bytes=encode_model(m);check(bytes.substr(0,8)=="POIMAM04","Channel extension changed cooked layout.");
    const auto decoded=decode_model(bytes);check(encode_model(*decoded)==bytes,"Exact-boundary cooked bytes changed on reopen.");
    const CompiledAnimation compiled(*decoded);
    for(unsigned clip:{0u,127u}) {
        const auto sampled=compiled.sample(clip,0,false);
        for(unsigned node=1;node<=128;++node) {near(sampled.local[node].position[0],clip);near(sampled.local[node].position[1],node);near(sampled.local[node].position[2],0);}
    }
    auto over=m;over.animations.back().channels.push_back({0,AnimationPath::translation,AnimationInterpolation::linear,{0},{{0,0,0,0}}});
    // The extra target is unique in this take; only the aggregate count is bad.
    rejects_exact([&]{validate_animation_data(over);},"Animation channel budget exceeded.");
    rejects_exact([&]{static_cast<void>(CompiledAnimation(over));},"Animation channel budget exceeded.");
    rejects_exact([&]{static_cast<void>(encode_model(over));},"Animation channel budget exceeded.");
    // Bypass encoder validation: add a complete independent channel to the
    // accepted v4 metadata AND its20-byte curve payload, updating both lengths.
    // A decoder regression cannot hide behind the encoder's shared validator.
    const auto header_size=budget_u32(bytes,8),binary_size=budget_u32(bytes,12);
    check(std::size_t(header_size)+binary_size+16==bytes.size(),"Original package length mismatch.");
    auto metadata=nlohmann::json::parse(bytes.substr(16,header_size));auto binary=bytes.substr(16+header_size);
    check(metadata.at("images").empty(),"Boundary mutation assumes no image payload.");
    metadata["animations"].back()["channels"].push_back({{"node",0},{"path",0},{"interpolation",1},{"keys",1}});
    metadata["animation_bytes"]=metadata.at("animation_bytes").get<std::size_t>()+20;
    for(unsigned word=0;word<5;++word)budget_word(binary,0);
    const auto text=metadata.dump();std::string malformed="POIMAM04";budget_word(malformed,static_cast<std::uint32_t>(text.size()));budget_word(malformed,static_cast<std::uint32_t>(binary.size()));malformed+=text;malformed+=binary;
    rejects_exact([&]{static_cast<void>(decode_model(malformed));},"Invalid channel count.");
    check(encode_model(m)==bytes,"Rejected boundary operations modified the original model.");
}
void unchanged_key_boundary() {
    auto m=model();AnimationChannel channel;channel.node=1;channel.path=AnimationPath::translation;
    channel.times.reserve(1000001);channel.values.reserve(1000001);
    // Binary-exact step1/1024 stays strictly increasing below3600seconds.
    for(unsigned i=0;i<1000000;++i) {channel.times.push_back(float(i)/1024);channel.values.push_back({2,2,0,0});}
    m.animations.push_back({"key-boundary",double(channel.times.back()),{std::move(channel)}});
    validate_animation_data(m);
    {const CompiledAnimation compiled(m);near(compiled.sample(0,500,false).local[1].position[0],2);}
    auto& curve=m.animations[0].channels[0];const auto duration=m.animations[0].duration;
    curve.times.push_back(1000000.f/1024);curve.values.push_back({2,2,0,0});m.animations[0].duration=curve.times.back();
    rejects_exact([&]{validate_animation_data(m);},"Animation key budget exceeded.");
    rejects_exact([&]{static_cast<void>(CompiledAnimation(m));},"Animation key budget exceeded.");
    rejects_exact([&]{static_cast<void>(encode_model(m));},"Animation key budget exceeded.");
    curve.times.pop_back();curve.values.pop_back();m.animations[0].duration=duration;validate_animation_data(m);
    check(curve.times.size()==1000000 && curve.values.size()==1000000 && curve.times.back()==999999.f/1024,"Failed key checks changed accepted fixture.");
}
struct BudgetTemp {
    std::filesystem::path directory;
    BudgetTemp() {
        for(unsigned attempt=0;attempt<32;++attempt) {
            directory=std::filesystem::temp_directory_path()/("poima-animation-budget-"+std::to_string(std::random_device{}())+"-"+std::to_string(attempt));
            if(std::filesystem::create_directory(directory))return;
        }
        throw std::runtime_error("Cannot create owned animation budget fixture.");
    }
    ~BudgetTemp() {std::error_code error;std::filesystem::remove_all(directory,error);}
};
void gltf_channel_boundary() {
    using Json=nlohmann::json;BudgetTemp temporary;
    // One ordinary triangle, one shared constant time/value sampler,128takes
    // and128distinct targets per take. No joints/images/extra dependencies.
    std::string binary;
    for(float value:{0.f,0.f,0.f,1.f,0.f,0.f,0.f,1.f,0.f,0.f,3.f,2.f,1.f})budget_word(binary,std::bit_cast<std::uint32_t>(value));
    Json children=Json::array(),nodes=Json::array({{{"name","root"},{"mesh",0}}});
    for(unsigned node=1;node<=128;++node) {children.push_back(node);nodes.push_back({{"name","target"+std::to_string(node)}});}
    nodes[0]["children"]=children;
    Json source={{"asset",{{"version","2.0"}}},{"nodes",nodes},{"scenes",Json::array({{{"nodes",{0}}}})},{"scene",0},
        {"buffers",Json::array({{{"byteLength",binary.size()}}})},
        {"bufferViews",Json::array({{{"buffer",0},{"byteOffset",0},{"byteLength",36}},{{"buffer",0},{"byteOffset",36},{"byteLength",4}},{{"buffer",0},{"byteOffset",40},{"byteLength",12}}})},
        {"accessors",Json::array({{{"bufferView",0},{"componentType",5126},{"count",3},{"type","VEC3"},{"min",{0,0,0}},{"max",{1,1,0}}},{{"bufferView",1},{"componentType",5126},{"count",1},{"type","SCALAR"},{"min",{0}},{"max",{0}}},{{"bufferView",2},{"componentType",5126},{"count",1},{"type","VEC3"}}})},
        {"meshes",Json::array({{{"primitives",Json::array({{{"attributes",{{"POSITION",0}}}}})}}})},{"animations",Json::array()}};
    for(unsigned clip=0;clip<128;++clip) {
        Json channels=Json::array();for(unsigned node=1;node<=128;++node)channels.push_back({{"sampler",0},{"target",{{"node",node},{"path","translation"}}}});
        source["animations"].push_back({{"name","take"+std::to_string(clip)},{"samplers",Json::array({{{"input",1},{"output",2},{"interpolation","LINEAR"}}})},{"channels",channels}});
    }
    auto write_glb=[&](const char* filename) {
        auto text=source.dump();while(text.size()%4)text.push_back(' ');
        std::string glb;budget_word(glb,0x46546c67);budget_word(glb,2);budget_word(glb,static_cast<std::uint32_t>(28+text.size()+binary.size()));
        budget_word(glb,static_cast<std::uint32_t>(text.size()));budget_word(glb,0x4e4f534a);glb+=text;
        budget_word(glb,static_cast<std::uint32_t>(binary.size()));budget_word(glb,0x004e4942);glb+=binary;
        check(glb.size()<2*1024*1024,"Synthetic aggregate source unexpectedly large.");
        const auto path=temporary.directory/filename;std::ofstream stream(path,std::ios::binary);stream.write(glb.data(),static_cast<std::streamsize>(glb.size()));stream.close();check(bool(stream),"Cannot write bounded original GLB.");return path;
    };
    const auto accepted=import_gltf(write_glb("exact.glb"));
    check(accepted->animations.size()==128 && accepted->nodes.size()==129 && accepted->skins.empty(),"Exact GLB changed its independent profile.");
    std::size_t count=0;for(const auto& clip:accepted->animations) {check(clip.channels.size()==128 && clip.duration==0,"Wrong imported boundary clip.");count+=clip.channels.size();}
    check(count==16384,"Exact GLB lost channels.");const auto cooked=encode_model(*accepted);const auto reopened=decode_model(cooked);
    check(encode_model(*reopened)==cooked,"Imported boundary package failed exact reopen.");
    const CompiledAnimation compiled(*reopened);
    for(unsigned clip:{0u,127u})for(unsigned node:{1u,128u})check(compiled.sample(clip,0,false).local[node].position==std::array<double,3>{3,2,1},"Independent original constant pose changed.");
    source["animations"].back()["channels"].push_back({{"sampler",0},{"target",{{"node",0},{"path","translation"}}}});
    rejects_exact([&]{static_cast<void>(import_gltf(write_glb("over.glb")));},"Animation channel budget exceeded.");
    check(encode_model(*accepted)==cooked,"Rejected GLB changed the accepted model.");
}

void invalid() {
    auto base=model();base.animations.push_back({"move",2,{translation()}});
    auto mutation=[&](auto change) { auto m=base;change(m);rejects([&] { validate_animation_data(m); });rejects([&] { CompiledAnimation invalid(m); }); };
    mutation([](auto& m) { m.nodes[2].parent=1; });mutation([](auto& m) { m.nodes[1].parent=999; });
    mutation([](auto& m) { m.nodes[1].parent=-1; });mutation([](auto& m) { m.skins[0].skeleton=0; });
    mutation([](auto& m) { m.skins[0].joints[0]=999; });mutation([](auto& m) { m.skins[0].joints[1]=2; });
    mutation([](auto& m) { m.roots={0}; });mutation([](auto& m) { m.nodes[0].primitives.clear(); });
    mutation([](auto& m) { m.skins[0].inverse_bind[0][3]=1; });mutation([](auto& m) { m.skins[0].inverse_bind[0][0]=0; });
    mutation([](auto& m) { m.animations[0].channels[0].times={0,0}; });mutation([](auto& m) { m.animations[0].duration=1; });
    mutation([](auto& m) { m.animations[0].channels.push_back(m.animations[0].channels[0]); });
    mutation([](auto& m) { m.animations[0].channels[0].values.pop_back(); });
    mutation([](auto& m) { m.animations[0].channels[0].values[0][0]=std::numeric_limits<float>::infinity(); });
    mutation([](auto& m) { auto& c=m.animations[0].channels[0];c.path=AnimationPath::scale;c.values[0][0]=0; });
    rejects([&] { sample_model(base,99,0,false); });rejects([&] { sample_model(base,0,-1,false); });rejects([&] { sample_model(base,0,std::nan(""),false); });
    mutation([](auto& m) { auto mesh=std::make_shared<MeshAsset>(*m.primitives[0]);mesh->influences[0].weights[0]=0;m.primitives[0]=mesh; });
    mutation([](auto& m) { auto mesh=std::make_shared<MeshAsset>(*m.primitives[0]);mesh->influences[0].joints[3]=9;m.primitives[0]=mesh; });
}
void conservative_skin_bounds() {
    auto m=model();auto mesh=*m.primitives[0];std::mt19937 rng(472);
    std::uniform_real_distribution<float> random(-10,10),weight(0,1);
    mesh.vertices.resize(500);mesh.influences.resize(500);
    for(std::size_t i=0;i<mesh.vertices.size();++i) {
        mesh.vertices[i].position={random(rng),random(rng),random(rng)};
        const float w=weight(rng);mesh.influences[i]={{0,1,0,0},{w,1-w,0,0}};
    }
    const auto source=skin_bounds(mesh);
    for(int sample=0;sample<200;++sample) {
        std::vector<Matrix4> palette(2,identity_matrix());
        for(auto& matrix:palette)for(std::size_t col=0;col<4;++col)for(std::size_t row=0;row<3;++row)matrix[col*4+row]=random(rng);
        const auto bounds=posed_bounds(source,palette);
        for(std::size_t i=0;i<mesh.vertices.size();++i)for(std::size_t row=0;row<3;++row) {
            float a[4]{};for(std::size_t k=0;k<4;++k)for(std::size_t col=0;col<4;++col)a[col]+=mesh.influences[i].weights[k]*float(palette[mesh.influences[i].joints[k]][col*4+row]);
            float value=a[3];for(std::size_t col=0;col<3;++col)value+=a[col]*mesh.vertices[i].position[col];
            check(value>=bounds.minimum[row] && value<=bounds.maximum[row],"GPU-like blended vertex escaped its joint bounds.");
        }
    }
    rejects([&] { posed_bounds(source,std::vector<Matrix4>{identity_matrix()}); });
    mesh.influences[0].weights={0,0,0,0};rejects([&] { skin_bounds(mesh); });
}
void cancellation_skin_bounds() {
    MeshAsset mesh;mesh.vertices.resize(1);
    mesh.vertices[0].position={100000000,0,0};
    mesh.influences={SkinWeight{{0,0,0,0},{1,0,0,0}}};
    auto matrix=identity_matrix();matrix[0]=1.00000001;matrix[12]=-100000000;
    const auto bounds=posed_bounds(skin_bounds(mesh),std::vector<Matrix4>{matrix});
    // The exact transformed x is 1, while conversion of the palette to float
    // makes it 0. Bounds must cover arithmetic magnitude before cancellation.
    near(matrix[0]*double(mesh.vertices[0].position[0])+matrix[12],1);
    const float actual=float(matrix[0])*mesh.vertices[0].position[0]+float(matrix[12]);
    check(actual==0 && bounds.minimum[0]<=actual && bounds.maximum[0]>=actual,
          "Palette float conversion escaped cancellation bounds.");

    std::mt19937 rng(98427);
    std::uniform_real_distribution<double> unit(-1,1);
    std::uniform_real_distribution<float> weight(.01f,1);
    for(int sample=0;sample<1200;++sample) {
        const double magnitude=std::pow(10.,sample%10);
        const double coefficient=std::pow(10.,(sample/10)%7-3);
        for(auto& p:mesh.vertices[0].position)p=float(unit(rng)*magnitude);
        auto& influence=mesh.influences[0];influence.joints={0,1,2,3};
        double sum=0;for(auto& w:influence.weights) { w=weight(rng);sum+=w; }
        for(auto& w:influence.weights)w=float(double(w)/sum);
        std::vector<Matrix4> palette(4,identity_matrix());
        for(auto& joint:palette)for(std::size_t row=0;row<3;++row) {
            double translation=unit(rng);
            for(std::size_t col=0;col<3;++col) {
                joint[col*4+row]=unit(rng)*coefficient;
                translation-=joint[col*4+row]*double(mesh.vertices[0].position[col]);
            }
            joint[12+row]=translation;
        }
        const auto limit=posed_bounds(skin_bounds(mesh),palette);
        // Cover both separate multiply/add and fused contraction in palette
        // blending and the final dot product; production compilers may fuse.
        for(bool fused:{false,true})for(std::size_t row=0;row<3;++row) {
            float blended[4]{};
            for(std::size_t k=0;k<4;++k)for(std::size_t col=0;col<4;++col) {
                const float a=influence.weights[k],b=float(palette[k][col*4+row]);
                const volatile float product=a*b;
                blended[col]=fused ? std::fma(a,b,blended[col]) : blended[col]+product;
            }
            float value=blended[3];
            for(std::size_t col=0;col<3;++col) {
                const float p=mesh.vertices[0].position[col];
                const volatile float product=blended[col]*p;
                value=fused ? std::fma(blended[col],p,value) : value+product;
            }
            check(std::isfinite(value) && value>=limit.minimum[row] && value<=limit.maximum[row],
                  "Extreme cancelling GPU-like blend escaped skin bounds.");
        }
    }
}
void tangent_weights() {
    MeshAsset mesh;mesh.has_uv=true;
    for(const auto& pos:std::vector<std::array<float,3>>{{0,0,0},{1,0,0},{0,1,0},{0,0,0},{1,0,0},{0,1,0}}) {
        MeshVertex v;v.position=pos;v.normal={0,0,1};v.uv={pos[0],pos[1]};mesh.vertices.push_back(v);
    }
    mesh.indices={0,1,2,3,4,5};mesh.influences.assign(6,SkinWeight{{0,0,0,0},{1,0,0,0}});
    for(std::size_t i=3;i<6;++i)mesh.influences[i].joints[0]=1;
    generate_tangents(mesh);check(mesh.vertices.size()==6,"Different skin influences were welded together.");
    for(std::size_t i=0;i<6;++i)check(mesh.influences[mesh.indices[i]].joints[0]==i/3,"Tangent reindexing lost weights.");
}
}
int main() {
    try { curves();compiled_sampling();analytic_local_motion();skinning();packages();aggregate_channel_boundary();unchanged_key_boundary();gltf_channel_boundary();invalid();tangent_weights();conservative_skin_bounds();cancellation_skin_bounds();std::cout<<"Animation immutable compiled sampling, analytic curves/local motion, authored baselines, hierarchy, CPU skinning, packages and invalid inputs passed.\n"; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
