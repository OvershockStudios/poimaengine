// SPDX-License-Identifier: Apache-2.0
// Independent row-major geometry oracle. Expected results never use Poima's
// matrix/quaternion/FK/curve helpers; the sampler is used only for actual output.
// The fixture declares known bone gauges; expected target poses preserve target
// reference positions/scales and never claim source affine/contact equivalence.
#include "../src/model_import.hpp"
#include "poima/animation.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

using namespace poima;
namespace {
using V=std::array<double,3>;using Q=std::array<double,4>;using M=std::array<double,16>;
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void near(double value,double expected,double epsilon=3e-5) {
    check(std::isfinite(value) && std::abs(value-expected)<=epsilon,"Independent rotation-retarget oracle disagrees.");
}
Q unit(Q q) {double n=0;for(double x:q)n+=x*x;check(n>1e-20,"Invalid oracle quaternion.");for(auto& x:q)x/=std::sqrt(n);return q;}
Q conjugate(Q q) {return {-q[0],-q[1],-q[2],q[3]};}
Q product(Q a,Q b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
Q axis(int which,double degrees) {Q q{};q[which]=std::sin(degrees*std::numbers::pi/360);q[3]=std::cos(degrees*std::numbers::pi/360);return q;}
M affine(V p,Q q,V scale={1,1,1}) {
    q=unit(q);const auto [x,y,z,w]=q;
    return {(1-2*y*y-2*z*z)*scale[0],(2*x*y-2*z*w)*scale[1],(2*x*z+2*y*w)*scale[2],p[0],
        (2*x*y+2*z*w)*scale[0],(1-2*x*x-2*z*z)*scale[1],(2*y*z-2*x*w)*scale[2],p[1],
        (2*x*z-2*y*w)*scale[0],(2*y*z+2*x*w)*scale[1],(1-2*x*x-2*y*y)*scale[2],p[2],0,0,0,1};
}
M times(const M& a,const M& b) {
    M out{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)for(unsigned k=0;k<4;++k)out[r*4+c]+=a[r*4+k]*b[k*4+c];return out;
}
M inverse(M a) {
    // General pivoted elimination also supports blended skin matrices; this
    // does not share the production affine inverse or its implementation.
    M result=affine({0,0,0},{0,0,0,1});
    for(unsigned c=0;c<4;++c) {
        unsigned pivot=c;for(unsigned r=c+1;r<4;++r)if(std::abs(a[r*4+c])>std::abs(a[pivot*4+c]))pivot=r;
        check(std::abs(a[pivot*4+c])>1e-12,"Oracle matrix is singular.");
        for(unsigned k=0;k<4;++k) {std::swap(a[c*4+k],a[pivot*4+k]);std::swap(result[c*4+k],result[pivot*4+k]);}
        const double divisor=a[c*4+c];for(unsigned k=0;k<4;++k){a[c*4+k]/=divisor;result[c*4+k]/=divisor;}
        for(unsigned r=0;r<4;++r)if(r!=c) {const double factor=a[r*4+c];for(unsigned k=0;k<4;++k){a[r*4+k]-=factor*a[c*4+k];result[r*4+k]-=factor*result[c*4+k];}}
    }
    return result;
}
V point(const M& m,V p) {V out{};for(unsigned r=0;r<3;++r){out[r]=m[r*4+3];for(unsigned c=0;c<3;++c)out[r]+=m[r*4+c]*p[c];}return out;}
Matrix4 api(const M& m) {Matrix4 out{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)out[c*4+r]=m[r*4+c];return out;}
M oracle(const Matrix4& m) {M out{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)out[r*4+c]=m[c*4+r];return out;}
std::vector<M> worlds(const ModelAsset& model,const std::vector<NodePose>& locals) {
    check(locals.size()==model.nodes.size(),"Oracle local count differs.");std::vector<M> result(locals.size());std::vector<bool> ready(locals.size());
    auto visit=[&](auto&& self,std::size_t i)->void {if(ready[i])return;const auto& p=locals[i];auto m=affine(p.position,p.rotation,p.scale);
        if(model.nodes[i].parent>=0){const auto parent=std::size_t(model.nodes[i].parent);self(self,parent);m=times(result[parent],m);}result[i]=m;ready[i]=true;};
    for(std::size_t i=0;i<locals.size();++i)visit(visit,i);return result;
}
std::vector<NodePose> defaults(const ModelAsset& m) {std::vector<NodePose> out;for(const auto& n:m.nodes)out.push_back({n.position,n.scale,n.rotation});return out;}
ModelPose reference(const ModelAsset& m,std::vector<NodePose> local) {ModelPose out;out.local=std::move(local);for(const auto& w:worlds(m,out.local))out.world.push_back(api(w));return out;}
std::array<float,4> floats(Q q) {return {float(q[0]),float(q[1]),float(q[2]),float(q[3])};}
Q raw(const std::array<float,4>& q) {return {q[0],q[1],q[2],q[3]};}
std::array<float,4> vec(V v) {return {float(v[0]),float(v[1]),float(v[2]),0};}
V plus(V a,V b) {for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
template<std::size_t N> std::array<double,N> bezier(std::array<double,N> a,std::array<double,N> b,
        std::array<double,N> out_tangent,std::array<double,N> in_tangent,double duration,double t) {
    // Hermite controls evaluated by de Casteljau, not the runtime polynomial.
    std::array<std::array<double,N>,4> controls{a,a,b,b};
    for(unsigned k=0;k<N;++k){controls[1][k]+=duration*out_tangent[k]/3;controls[2][k]-=duration*in_tangent[k]/3;}
    for(unsigned count=3;count>0;--count)for(unsigned i=0;i<count;++i)for(unsigned k=0;k<N;++k)
        controls[i][k]=(1-t)*controls[i][k]+t*controls[i+1][k];
    return controls[0];
}
struct Fixture {
    ModelAsset base,source;ModelPose source_ref,target_ref;
    std::array<unsigned,4> ids{2,0,3,1};
    std::array<Q,4> gauge{axis(1,90),axis(2,180),axis(0,180),axis(2,-90)};
    AnimationRotationRetarget policy;
};
Fixture fixture() {
    Fixture f;f.policy.alignment_rotation=axis(1,90);
    f.base.package_version=4;f.base.nodes.resize(5);f.base.roots={0,4};
    f.base.nodes[0].name="frame";f.base.nodes[0].position={2,0,1};f.base.nodes[0].scale={2,1,3};
    f.base.nodes[1].name="pivot";f.base.nodes[1].parent=0;f.base.nodes[1].position={0,1,0};f.base.nodes[1].rotation=axis(2,90);f.base.nodes[1].scale={1,2,1};
    f.base.nodes[2].name="shoulder";f.base.nodes[2].parent=1;f.base.nodes[2].position={1,0,0};f.base.nodes[2].rotation=axis(0,90);f.base.nodes[2].scale={1.5,.75,2};
    f.base.nodes[3].name="wrist";f.base.nodes[3].parent=2;f.base.nodes[3].position={0,2,0};f.base.nodes[3].rotation=axis(1,180);
    f.base.nodes[4].name="mesh";f.base.nodes[4].position={.5,-.25,.75};f.base.nodes[4].rotation=axis(1,90);f.base.nodes[4].skin=0;f.base.nodes[4].primitives={0};
    const auto original=reference(f.base,defaults(f.base));
    f.base.skins.push_back({"original-target-bind",0,{3,2},{api(times(inverse(oracle(original.world[3])),oracle(original.world[4]))),
        api(times(inverse(oracle(original.world[2])),oracle(original.world[4])))}});
    auto mesh=std::make_shared<MeshAsset>();mesh->has_uv=true;
    for(auto p:std::array<V,3>{{{1,0,1},{0,1,2},{-1,.5,0}}}){MeshVertex v;v.position={float(p[0]),float(p[1]),float(p[2])};v.normal={0,1,0};v.tangent={1,0,0,1};mesh->vertices.push_back(v);}
    mesh->vertices[1].uv={1,0};mesh->vertices[2].uv={0,1};mesh->indices={0,1,2};
    mesh->influences={{{0,1,0,0},{.25f,.75f,0,0}},{{1,0,0,0},{1,0,0,0}},{{0,1,0,0},{.75f,.25f,0,0}}};f.base.primitives={mesh};
    auto target_local=defaults(f.base);
    target_local[0].position={3,.5,-2};target_local[0].rotation=axis(0,45);target_local[0].scale={1.25,.8,1.5};
    target_local[1].position={.2,1.5,-.1};target_local[1].rotation=product(axis(2,90),axis(1,30));target_local[1].scale={.9,1.3,1.1};
    target_local[2].position={1.4,.2,0};target_local[2].rotation=product(axis(0,90),axis(2,20));target_local[2].scale={1.2,1,.7};
    target_local[3].position={0,2.7,.1};target_local[3].scale={1,2,1};
    f.target_ref=reference(f.base,target_local);
    f.source.package_version=4;f.source.nodes.resize(4);f.source.roots={f.ids[0]};
    for(unsigned i=0;i<4;++i){auto& n=f.source.nodes[f.ids[i]];n.name=f.base.nodes[i].name;n.parent=i ? int(f.ids[i-1]):-1;
        // Known independent gauges, without extracting orientation from any
        // sheared affine matrix. Source proportions and translations differ.
        n.rotation=i ? product(product(f.gauge[i-1],target_local[i].rotation),conjugate(f.gauge[i])):
            product(product(conjugate(f.policy.alignment_rotation),target_local[i].rotation),conjugate(f.gauge[i]));
        n.rotation=unit(n.rotation);n.position=i ? V{.1*i,.35*i,-.2*i}:V{-4,2,6};n.scale={1+.1*i,.8+.1*i,1.3+.1*i};
    }
    f.source_ref=reference(f.source,defaults(f.source));
    // Sparse motion inherits original defaults, not the selected reference.
    auto& leaf=f.source.nodes[f.ids[3]];leaf.position[0]+=.25;leaf.rotation=product(leaf.rotation,axis(2,30));leaf.scale={1.25,2,.75};
    const auto& root=f.source_ref.local[f.ids[0]];const auto& parent=f.source_ref.local[f.ids[1]];const auto& joint=f.source_ref.local[f.ids[2]];
    auto end=product(root.rotation,axis(0,90));for(auto& x:end)x=-x;
    AnimationClip mixed{"mixed",2,{
        {f.ids[0],AnimationPath::rotation,AnimationInterpolation::linear,{0,2},{floats(root.rotation),floats(end)}},
        {f.ids[0],AnimationPath::translation,AnimationInterpolation::linear,{0,2},{vec(root.position),vec(plus(root.position,{.5,0,-.25}))}},
        {f.ids[1],AnimationPath::translation,AnimationInterpolation::step,{0,1,2},{vec(parent.position),vec(plus(parent.position,{0,.5,0})),vec(plus(parent.position,{0,.5,0}))}},
        {f.ids[2],AnimationPath::rotation,AnimationInterpolation::cubic,{0,2},{floats({0,0,0,0}),floats(joint.rotation),floats(product(joint.rotation,{0,.2,0,0})),
            floats(product(joint.rotation,{0,-.1,0,.05})),floats(product(joint.rotation,axis(1,90))),floats({0,0,0,0})}},
        {f.ids[2],AnimationPath::translation,AnimationInterpolation::cubic,{0,2},{vec({0,0,0}),vec(joint.position),vec({0,1,0}),vec({-.5,.25,0}),vec(plus(joint.position,{.5,0,0})),vec({0,0,0})}},
        {f.ids[2],AnimationPath::scale,AnimationInterpolation::linear,{0,2},{{1,.5f,2,0},{2,3,1,0}}}}};
    f.source.animations={mixed,
        {"step",2,{{f.ids[2],AnimationPath::rotation,AnimationInterpolation::step,{0,1,2},{floats(joint.rotation),floats(product(joint.rotation,axis(1,180))),floats(product(joint.rotation,axis(1,180)))}}}},
        {"discarded-only",2,{{f.ids[0],AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{8,1,4,0},{10,3,7,0}}}}}};
    AnimationClip hold;hold.name="explicit-reference";hold.duration=.5;
    for(unsigned i=0;i<4;++i)hold.channels.push_back({i,AnimationPath::rotation,AnimationInterpolation::linear,{0,.5f},{floats(f.source_ref.local[i].rotation),floats(f.source_ref.local[i].rotation)}});
    f.source.animations.push_back(hold);validate_animation_data(f.base);validate_animation_data(f.source);return f;
}
V rotated(Q q,V v) {return point(affine({0,0,0},q),v);}
std::vector<NodePose> original_motion(const Fixture& f,unsigned clip,double time) {
    auto local=defaults(f.source);const double t=std::clamp(time,0.,clip==3 ? .5:2.);
    const auto& root=f.source_ref.local[f.ids[0]];const auto& parent=f.source_ref.local[f.ids[1]];const auto& joint=f.source_ref.local[f.ids[2]];
    if(clip==0){local[f.ids[0]].rotation=product(root.rotation,axis(0,45*t));local[f.ids[0]].position=plus(root.position,{t*.25,0,-t*.125});
        local[f.ids[1]].position=plus(parent.position,{0,t>=1 ? .5:0,0});
        const auto& c=f.source.animations[0].channels[3];local[f.ids[2]].rotation=unit(bezier<4>(raw(c.values[1]),raw(c.values[4]),raw(c.values[2]),raw(c.values[3]),2,t/2));
        local[f.ids[2]].position=bezier<3>(joint.position,plus(joint.position,{.5,0,0}),{0,1,0},{-.5,.25,0},2,t/2);
    }else if(clip==1)local[f.ids[2]].rotation=t>=1 ? product(joint.rotation,axis(1,180)):joint.rotation;
    else if(clip==2)local[f.ids[0]].position={8+t,1+t,4+t*1.5};
    else for(unsigned i=0;i<4;++i)local[i].rotation=f.source_ref.local[i].rotation;
    return local;
}
std::vector<NodePose> expected_target(const Fixture& f,unsigned clip,double time) {
    auto target=f.target_ref.local;const auto source=original_motion(f,clip,time);
    for(unsigned i=0;i<4;++i){const auto id=f.ids[i];
        // The known gauge conjugates the original authored LOCAL delta. This
        // oracle never recomputes production reference corrections/chains.
        const auto delta=product(conjugate(unit(f.source_ref.local[id].rotation)),unit(source[id].rotation));
        target[i].rotation=product(product(product(unit(f.target_ref.local[i].rotation),conjugate(f.gauge[i])),delta),f.gauge[i]);
        if(f.policy.position_mode==AnimationRetargetPositionMode::reference_delta &&
            std::find(f.policy.translation_nodes.begin(),f.policy.translation_nodes.end(),id)!=f.policy.translation_nodes.end()){
            V delta_position{};for(unsigned k=0;k<3;++k)delta_position[k]=source[id].position[k]-f.source_ref.local[id].position[k];
            delta_position=rotated(i ? conjugate(f.gauge[i-1]):f.policy.alignment_rotation,delta_position);
            for(unsigned k=0;k<3;++k)target[i].position[k]+=f.policy.translation_scale*delta_position[k];
        }
    }
    return target;
}
std::shared_ptr<const ModelAsset> converted(const Fixture& f) {return transfer_animation_rotations(f.base,f.source,f.source_ref,f.target_ref,f.policy);}
void verify_pose(const Fixture& f,const ModelAsset& model,unsigned clip,double time,double epsilon=3e-5) {
    const auto actual=sample_model(model,clip,time,false);const auto wanted_local=expected_target(f,clip,time);
    const auto wanted=worlds(f.base,wanted_local);
    for(unsigned i=0;i<5;++i){for(unsigned k=0;k<3;++k){near(actual.local[i].position[k],wanted_local[i].position[k],epsilon);near(actual.local[i].scale[k],wanted_local[i].scale[k],epsilon);}
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)near(actual.world[i][c*4+r],wanted[i][r*4+c],epsilon);}
    const auto mesh=wanted[4];std::array<M,2> palette{times(times(inverse(mesh),wanted[3]),oracle(f.base.skins[0].inverse_bind[0])),
        times(times(inverse(mesh),wanted[2]),oracle(f.base.skins[0].inverse_bind[1]))};
    const auto actual_palette=skin_palette(model,actual,4);for(unsigned i=0;i<2;++i)for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)near(actual_palette[i][c*4+r],palette[i][r*4+c],epsilon);
    const auto output=deform_mesh(*model.primitives[0],actual_palette);
    for(unsigned i=0;i<3;++i){const auto& p=f.base.primitives[0]->vertices[i].position;const auto& w=f.base.primitives[0]->influences[i];V expected{};
        for(unsigned j=0;j<4;++j){const auto v=point(palette[w.joints[j]],{p[0],p[1],p[2]});for(unsigned k=0;k<3;++k)expected[k]+=w.weights[j]*v[k];}
        for(unsigned k=0;k<3;++k)near(output->vertices[i].position[k],expected[k],epsilon);}
}
void orientation_and_anisotropic_target() {
    const auto f=fixture();const auto before=encode_model(f.base),source_before=encode_model(f.source);const auto donor=converted(f);
    const auto combined=compose_model_animations(f.base,{donor});
    check(encode_model(f.base)==before && encode_model(f.source)==source_before,"Retarget modified original inputs.");
    check(combined->primitives[0]==f.base.primitives[0] && combined->skins[0].inverse_bind==f.base.skins[0].inverse_bind,"Retarget replaced original target geometry/binds.");
    for(unsigned i=0;i<5;++i)check(combined->nodes[i].position==f.base.nodes[i].position && combined->nodes[i].rotation==f.base.nodes[i].rotation && combined->nodes[i].scale==f.base.nodes[i].scale,"Retarget modified imported target defaults.");
    for(unsigned clip=0;clip<4;++clip)for(double t:{0.,.125,.5,.999999,1.,1.000001,1.5,2.,3.})verify_pose(f,*combined,clip,t);
    const auto decoded=decode_model(encode_model(*combined));verify_pose(f,*decoded,0,.375);
    // At the deliberately selected complete reference, all target TRS must
    // equal its sampled reference, even though original skin binds differ.
    const auto ref=sample_model(*combined,3,0,false);for(unsigned i=0;i<5;++i)for(unsigned k=0;k<16;++k)near(ref.world[i][k],f.target_ref.world[i][k]);
    auto antipodal=f;for(auto& local:antipodal.source_ref.local)for(auto& x:local.rotation)x=-x;
    for(auto& local:antipodal.target_ref.local)for(auto& x:local.rotation)x=-x;
    const auto signed_result=compose_model_animations(antipodal.base,{converted(antipodal)});verify_pose(antipodal,*signed_result,0,.625);
}
void reference_delta_positions() {
    auto f=fixture();f.policy.position_mode=AnimationRetargetPositionMode::reference_delta;
    f.policy.translation_nodes={f.ids[0],f.ids[1],f.ids[2],f.ids[3]};f.policy.translation_scale=1.75;
    check(f.ids[1]==0,"Source-ID-zero coverage fixture changed.");const auto combined=compose_model_animations(f.base,{converted(f)});
    for(unsigned clip=0;clip<3;++clip)for(double t:{0.,.125,.5,.999999,1.,1.000001,1.5,2.})verify_pose(f,*combined,clip,t);
    // Root delta is a visual transform. There is no character/controller
    // creation or callback here, and fixed target-reference mode has no drift.
    const auto initial=sample_model(*combined,0,0,false),later=sample_model(*combined,0,1,false);check(initial.local[0].position!=later.local[0].position,"Explicit root translation delta was lost.");
    f.policy.translation_nodes={f.ids[2]};const auto partial=compose_model_animations(f.base,{converted(f)});verify_pose(f,*partial,0,.75);
    const auto pose=sample_model(*partial,0,.75,false);for(unsigned k=0;k<3;++k)near(pose.local[0].position[k],f.target_ref.local[0].position[k]);
}
void near_unit_cubic_covariance() {
    auto f=fixture();auto& c=f.source.animations[0].channels[3];for(auto& x:c.values[1])x*=.99996f;for(auto& x:c.values[4])x*=1.00003f;
    validate_animation_data(f.source);const auto combined=compose_model_animations(f.base,{converted(f)});
    for(double t:{0.,.25,.5,.75,1.,1.25,1.5,1.75,2.})verify_pose(f,*combined,0,t,2e-6);
    f.policy.position_mode=AnimationRetargetPositionMode::reference_delta;f.policy.translation_nodes={f.ids[2]};f.policy.translation_scale=.5;
    const auto translated=compose_model_animations(f.base,{converted(f)});for(double t:{.25,.75,1.25,1.75})verify_pose(f,*translated,0,t,2e-6);
}
void duration_sparse_and_determinism() {
    const auto f=fixture();const auto first=converted(f),second=converted(f);check(encode_model(*first)==encode_model(*second),"Repeated retarget changed cooked bytes.");
    check(first->animations[2].duration==2 && !first->animations[2].channels.empty(),"Discarding the only source translation lost duration/playable clip.");
    const auto combined=compose_model_animations(f.base,{first});verify_pose(f,*combined,2,0);verify_pose(f,*combined,2,2);
    const auto pose=sample_model(*combined,0,0,false);check(!std::equal(pose.local[3].rotation.begin(),pose.local[3].rotation.end(),f.target_ref.local[3].rotation.begin()),"Sparse original source default rotation was replaced by reference.");
    for(const auto& clip:first->animations)for(const auto& channel:clip.channels)if(channel.path==AnimationPath::scale)
        for(std::size_t i=0;i<channel.values.size();++i){if(channel.interpolation==AnimationInterpolation::cubic && i%3!=1)continue;
            const auto target=std::find(f.ids.begin(),f.ids.end(),channel.node);check(target!=f.ids.end(),"Unexpected scale target.");const auto index=std::size_t(target-f.ids.begin());
            for(unsigned k=0;k<3;++k)near(channel.values[i][k],f.target_ref.local[index].scale[k],1e-6);}
}
void identity_policy() {
    auto f=fixture();for(unsigned i=0;i<4;++i)f.gauge[i]={0,0,0,1};f.policy.alignment_rotation={0,0,0,1};
    for(unsigned i=0;i<4;++i){auto& n=f.source.nodes[f.ids[i]];n.rotation=f.target_ref.local[i].rotation;}
    f.source_ref=reference(f.source,defaults(f.source));f.source.animations={{"identity",2,{{f.ids[0],AnimationPath::rotation,AnimationInterpolation::linear,{0,2},
        {floats(f.source_ref.local[f.ids[0]].rotation),floats(product(f.source_ref.local[f.ids[0]].rotation,axis(1,90)))}}}}};
    const auto result=converted(f),repeat=converted(f);check(encode_model(*result)==encode_model(*repeat),"Identity retarget was nondeterministic.");
    const auto& output=result->animations[0].channels;const auto found=std::find_if(output.begin(),output.end(),[&](const auto& c){return c.node==f.ids[0] && c.path==AnimationPath::rotation;});
    check(found!=output.end() && found->times==f.source.animations[0].channels[0].times && found->interpolation==AnimationInterpolation::linear,"Identity retarget changed rotation timing/interpolation.");
    const auto combined=compose_model_animations(f.base,{result});const auto pose=sample_model(*combined,0,0,false);
    for(unsigned i=0;i<4;++i)for(unsigned k=0;k<3;++k){near(pose.local[i].position[k],f.target_ref.local[i].position[k]);near(pose.local[i].scale[k],f.target_ref.local[i].scale[k]);}
    for(double t:{0.,.25,1.,1.75,2.}){auto local=f.target_ref.local;local[0].rotation=product(local[0].rotation,axis(1,45*t));
        const auto wanted=worlds(f.base,local);const auto actual=sample_model(*combined,0,t,false);
        for(unsigned i=0;i<5;++i)for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)near(actual.world[i][c*4+r],wanted[i][r*4+c]);}
}
template<class F> void rejected(F change) {
    auto f=fixture();change(f);const auto base=encode_model(f.base),source=encode_model(f.source);bool failed=false;
    auto published=std::make_shared<const ModelAsset>(f.source);const auto sentinel=published;
    try{published=converted(f);}catch(const std::exception&){failed=true;}
    check(failed && published==sentinel,"Invalid retarget published output.");check(encode_model(f.base)==base && encode_model(f.source)==source,"Rejected retarget modified input models.");
}
void invalid_policy_and_topology() {
    rejected([](auto& f){f.policy.position_mode=static_cast<AnimationRetargetPositionMode>(99);});
    rejected([](auto& f){f.policy.position_mode=AnimationRetargetPositionMode::reference_delta;});
    rejected([](auto& f){f.policy.translation_nodes={f.ids[0]};});
    rejected([](auto& f){f.policy.position_mode=AnimationRetargetPositionMode::reference_delta;f.policy.translation_nodes={9999};});
    rejected([](auto& f){f.policy.position_mode=AnimationRetargetPositionMode::reference_delta;f.policy.translation_nodes={0,0};});
    rejected([](auto& f){f.policy.position_mode=AnimationRetargetPositionMode::reference_delta;f.policy.translation_nodes.assign(65,0);});
    rejected([](auto& f){ModelNode extra;extra.name="unmapped unanimated root";f.source.nodes.push_back(extra);f.source.roots.push_back(4);
        f.source_ref.local.push_back({});f.source_ref=reference(f.source,f.source_ref.local);f.policy.position_mode=AnimationRetargetPositionMode::reference_delta;f.policy.translation_nodes={4};});
    for(double scale:{0.,-1.,100.001,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        rejected([&](auto& f){f.policy.position_mode=AnimationRetargetPositionMode::reference_delta;f.policy.translation_nodes={0};f.policy.translation_scale=scale;});
    rejected([](auto& f){f.policy.translation_scale=2;});rejected([](auto& f){f.policy.alignment_rotation={0,0,0,2};});
    rejected([](auto& f){f.source.nodes[f.ids[3]].name="missing joint";});
    rejected([](auto& f){f.source.nodes[f.ids[3]].parent=int(f.ids[0]);f.source_ref=reference(f.source,f.source_ref.local);});
    rejected([](auto& f){ModelNode duplicate;duplicate.name="pivot";duplicate.parent=int(f.ids[0]);f.source.nodes.push_back(duplicate);f.source_ref.local.push_back({});f.source_ref=reference(f.source,f.source_ref.local);});
}
void invalid_reference_poses() {
    rejected([](auto& f){f.source_ref.local.pop_back();});rejected([](auto& f){f.target_ref.world.pop_back();});
    rejected([](auto& f){f.source_ref.time=std::numeric_limits<double>::quiet_NaN();});
    rejected([](auto& f){f.target_ref.world[0][0]=std::numeric_limits<double>::infinity();});
    rejected([](auto& f){f.source_ref.world[f.ids[3]][12]+=.1;});
    rejected([](auto& f){f.source_ref.local[f.ids[3]].rotation={0,0,0,0};});
    rejected([](auto& f){f.target_ref.local[3].scale={-1,1,1};});
    rejected([](auto& f){f.target_ref.local[3].position[0]=std::numeric_limits<double>::infinity();});
}
void synthesized_budget() {
    ModelAsset base;base.package_version=4;base.nodes.resize(2801);base.nodes[0].name="root";base.roots={0};
    for(unsigned i=1;i<=2800;++i){base.nodes[i].name="joint"+std::to_string(i);base.nodes[i].parent=0;
        if((i-1)%256==0)base.skins.push_back({"skin"+std::to_string(base.skins.size()),0,{},{}});
        base.skins.back().joints.push_back(i);base.skins.back().inverse_bind.push_back(api(affine({0,0,0},{0,0,0,1})));}
    auto source=base;source.skins.clear();const auto source_ref=reference(source,defaults(source));auto target=defaults(base);
    for(unsigned i=1;i<=2800;++i){source.nodes[i].rotation=axis(0,90);target[i].position={.125,0,0};target[i].scale={2,3,1};}
    const auto target_ref=reference(base,target);source.animations={{"budget",1,{{0,AnimationPath::rotation,AnimationInterpolation::linear,{0,1},{{0,0,0,1},{0,0,0,1}}}}}};
    validate_animation_data(base);validate_animation_data(source);const auto old_base=encode_model(base),old_source=encode_model(source);bool caught=false;
    try{static_cast<void>(transfer_animation_rotations(base,source,source_ref,target_ref));}catch(const std::exception&){caught=true;}
    check(caught,"Retarget exceeded8192synthesized channels without rejection.");check(encode_model(base)==old_base && encode_model(source)==old_source,"Budget rejection modified models.");
}
}
int main() {
    try{orientation_and_anisotropic_target();reference_delta_positions();near_unit_cubic_covariance();duration_sparse_and_determinism();
        identity_policy();invalid_policy_and_topology();invalid_reference_poses();synthesized_budget();
        std::cout<<"8 independent animation rotation-retarget check groups passed.\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
