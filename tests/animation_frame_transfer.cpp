// SPDX-License-Identifier: Apache-2.0
// Independent row-major geometry oracle. Expected results never use Poima's
// matrix/quaternion/FK/curve helpers; the sampler is used only for actual output.
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
    check(std::isfinite(value) && std::abs(value-expected)<=epsilon,"Independent frame-transfer oracle disagrees.");
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
    std::array<unsigned,4> ids{2,0,3,1}; // Target index -> independently ordered source index.
    std::array<Q,4> gauge{axis(1,90),axis(2,180),axis(0,180),axis(2,-90)};
    V align_position{4,-1,2};Q align_rotation=axis(1,90);
};
Fixture fixture() {
    Fixture f;f.base.package_version=4;f.base.nodes.resize(5);f.base.roots={0,4};
    f.base.nodes[0].name="frame";f.base.nodes[0].position={2,0,1};
    f.base.nodes[1].name="pivot";f.base.nodes[1].parent=0;f.base.nodes[1].position={0,1,0};f.base.nodes[1].rotation=axis(2,90);
    f.base.nodes[2].name="shoulder";f.base.nodes[2].parent=1;f.base.nodes[2].position={1,0,0};f.base.nodes[2].rotation=axis(0,90);
    f.base.nodes[3].name="wrist";f.base.nodes[3].parent=2;f.base.nodes[3].position={0,2,0};f.base.nodes[3].rotation=axis(1,180);
    f.base.nodes[4].name="mesh";f.base.nodes[4].position={.5,-.25,.75};f.base.nodes[4].rotation=axis(1,90);f.base.nodes[4].skin=0;f.base.nodes[4].primitives={0};
    f.target_ref=reference(f.base,defaults(f.base));
    // Hand-derived anchors catch errors in the independent generic FK itself.
    for(unsigned k=0;k<3;++k)near(f.target_ref.world[3][12+k],V{2,2,3}[k],1e-12);
    const auto anchor=point(oracle(f.target_ref.world[3]),{1,1,0});for(unsigned k=0;k<3;++k)near(anchor[k],V{2,1,4}[k],1e-12);
    f.base.skins.push_back({"original-target-bind",0,{3,2},{
        api(times(inverse(oracle(f.target_ref.world[3])),oracle(f.target_ref.world[4]))),
        api(times(inverse(oracle(f.target_ref.world[2])),oracle(f.target_ref.world[4])))}});
    auto mesh=std::make_shared<MeshAsset>();mesh->has_uv=true;
    for(auto p:std::array<V,3>{{{1,0,1},{0,1,2},{-1,.5,0}}}){MeshVertex v;v.position={float(p[0]),float(p[1]),float(p[2])};v.normal={0,1,0};v.tangent={1,0,0,1};mesh->vertices.push_back(v);}
    mesh->vertices[1].uv={1,0};mesh->vertices[2].uv={0,1};mesh->indices={0,1,2};
    mesh->influences={{{0,1,0,0},{.25f,.75f,0,0}},{{1,0,0,0},{1,0,0,0}},{{0,1,0,0},{.75f,.25f,0,0}}};f.base.primitives={mesh};
    f.source.package_version=4;f.source.nodes.resize(4);f.source.roots={f.ids[0]};
    std::array<Q,4> target_global_q{};std::array<Q,4> source_global_q{};std::array<M,4> source_global{};
    const auto hinv=inverse(affine(f.align_position,f.align_rotation));
    for(unsigned i=0;i<4;++i){target_global_q[i]=i ? product(target_global_q[i-1],f.base.nodes[i].rotation):f.base.nodes[i].rotation;
        source_global_q[i]=product(product(conjugate(f.align_rotation),target_global_q[i]),conjugate(f.gauge[i]));
        source_global[i]=times(times(hinv,oracle(f.target_ref.world[i])),affine({0,0,0},conjugate(f.gauge[i])));
        auto& n=f.source.nodes[f.ids[i]];n.name=f.base.nodes[i].name;n.parent=i ? int(f.ids[i-1]):-1;
        const auto local=i ? times(inverse(source_global[i-1]),source_global[i]):source_global[i];
        n.position={local[3],local[7],local[11]};n.rotation=i ? product(conjugate(source_global_q[i-1]),source_global_q[i]):source_global_q[i];n.rotation=unit(n.rotation);
    }
    f.source_ref=reference(f.source,defaults(f.source));
    // Sparse donor defaults intentionally differ from its explicit reference.
    auto& leaf=f.source.nodes[f.ids[3]];leaf.position[0]+=.25;leaf.rotation=product(leaf.rotation,axis(2,30));leaf.scale={1.25,1.25,1.25};
    const auto& root=f.source_ref.local[f.ids[0]];const auto& parent=f.source_ref.local[f.ids[1]];const auto& joint=f.source_ref.local[f.ids[2]];
    auto end_root=product(root.rotation,axis(0,90));for(auto& x:end_root)x=-x; // Antipodal endpoint must keep the short SLERP arc.
    const auto out_q=product(joint.rotation,Q{0,.2,0,0}),in_q=product(joint.rotation,Q{0,-.1,0,.05});
    AnimationClip mixed{"mixed",2,{
        {f.ids[0],AnimationPath::translation,AnimationInterpolation::cubic,{0,2},{vec({0,0,0}),vec(root.position),vec({.25,.125,0}),
            vec({.25,-.125,.2}),vec(plus(root.position,{.5,0,0})),vec({0,0,0})}},
        {f.ids[0],AnimationPath::rotation,AnimationInterpolation::linear,{0,2},{floats(root.rotation),floats(end_root)}},
        {f.ids[1],AnimationPath::translation,AnimationInterpolation::step,{0,1,2},{vec(parent.position),vec(plus(parent.position,{0,.5,0})),vec(plus(parent.position,{0,.5,0}))}},
        {f.ids[2],AnimationPath::translation,AnimationInterpolation::cubic,{0,2},{vec({0,0,0}),vec(joint.position),vec({0,1,0}),vec({-.5,.25,0}),vec(plus(joint.position,{.5,0,0})),vec({0,0,0})}},
        {f.ids[2],AnimationPath::rotation,AnimationInterpolation::cubic,{0,2},{floats({0,0,0,0}),floats(joint.rotation),floats(out_q),floats(in_q),floats(product(joint.rotation,axis(1,90))),floats({0,0,0,0})}}}};
    f.source.animations={mixed,
        {"step",2,{{f.ids[2],AnimationPath::rotation,AnimationInterpolation::step,{0,1,2},{floats(joint.rotation),floats(product(joint.rotation,axis(1,180))),floats(product(joint.rotation,axis(1,180)))}}}},
        {"uniform-scale",2,{{f.ids[2],AnimationPath::scale,AnimationInterpolation::linear,{0,2},{{1,1,1,0},{2,2,2,0}}},
            {f.ids[0],AnimationPath::translation,AnimationInterpolation::linear,{0,2},{vec(root.position),vec(plus(root.position,{.5,0,0}))}}}}};
    validate_animation_data(f.base);validate_animation_data(f.source);return f;
}
std::vector<NodePose> expected_source(const Fixture& f,unsigned clip,double time) {
    const double t=std::clamp(time,0.,2.);auto local=defaults(f.source);
    const auto& root=f.source_ref.local[f.ids[0]];const auto& parent=f.source_ref.local[f.ids[1]];const auto& joint=f.source_ref.local[f.ids[2]];
    if(clip==0){local[f.ids[0]].position=bezier<3>(root.position,plus(root.position,{.5,0,0}),{.25,.125,0},{.25,-.125,.2},2,t/2);
        local[f.ids[0]].rotation=product(root.rotation,axis(0,45*t));
        local[f.ids[1]].position=plus(parent.position,{0,t>=1 ? .5:0,0});
        local[f.ids[2]].position=bezier<3>(joint.position,plus(joint.position,{.5,0,0}),{0,1,0},{-.5,.25,0},2,t/2);
        // Preserve the original float key norms and tangent magnitudes. Only
        // the evaluated quaternion is normalized, never its stored controls.
        const auto& rotation=f.source.animations[0].channels[4];
        local[f.ids[2]].rotation=unit(bezier<4>(raw(rotation.values[1]),raw(rotation.values[4]),
            raw(rotation.values[2]),raw(rotation.values[3]),2,t/2));
    }else if(clip==1){local[f.ids[2]].rotation=t>=1 ? product(joint.rotation,axis(1,180)):joint.rotation;}
    else if(clip==2){local[f.ids[2]].scale={1+t/2,1+t/2,1+t/2};local[f.ids[0]].position=plus(root.position,{t*.25,0,0});}
    return local;
}
void verify_pose(const Fixture& f,const ModelAsset& combined,unsigned clip,double time,bool loop=false,double epsilon=3e-5) {
    const auto actual=sample_model(combined,clip,time,loop);const double source_time=loop ? std::fmod(time,2.):std::min(time,2.);
    const auto source_world=worlds(f.source,expected_source(f,clip,source_time));const auto h=affine(f.align_position,f.align_rotation);
    std::array<M,4> expected{};
    for(unsigned i=0;i<4;++i){expected[i]=times(times(h,source_world[f.ids[i]]),affine({0,0,0},f.gauge[i]));const auto a=oracle(actual.world[i]);for(unsigned k=0;k<16;++k)near(a[k],expected[i][k],epsilon);}
    const auto mesh_world=oracle(f.target_ref.world[4]);for(unsigned k=0;k<16;++k)near(oracle(actual.world[4])[k],mesh_world[k],1e-12);
    std::array<M,2> palette{times(times(inverse(mesh_world),expected[3]),oracle(f.base.skins[0].inverse_bind[0])),
        times(times(inverse(mesh_world),expected[2]),oracle(f.base.skins[0].inverse_bind[1]))};
    const auto actual_palette=skin_palette(combined,actual,4);for(unsigned j=0;j<2;++j)for(unsigned k=0;k<16;++k)near(oracle(actual_palette[j])[k],palette[j][k],epsilon);
    const auto deformed=deform_mesh(*combined.primitives[0],actual_palette);
    for(unsigned v=0;v<3;++v){const auto& raw=f.base.primitives[0]->vertices[v];const auto& weights=f.base.primitives[0]->influences[v];M blend{};
        for(unsigned j=0;j<4;++j)for(unsigned k=0;k<16;++k)blend[k]+=weights.weights[j]*palette[weights.joints[j]][k];
        const auto expected_position=point(blend,{raw.position[0],raw.position[1],raw.position[2]});const auto inv=inverse(blend);V normal{};
        for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)normal[r]+=inv[c*4+r]*raw.normal[c];double length=0;for(double x:normal)length+=x*x;
        for(unsigned k=0;k<3;++k){near(deformed->vertices[v].position[k],expected_position[k],epsilon);near(deformed->vertices[v].normal[k],normal[k]/std::sqrt(length),epsilon);}
    }
}
std::shared_ptr<const ModelAsset> converted(const Fixture& f) {
    return transfer_animation_frames(f.base,f.source,f.source_ref,f.target_ref,f.align_position,f.align_rotation);
}
void geometric_equivalence() {
    const auto f=fixture();const auto base_bytes=encode_model(f.base),source_bytes=encode_model(f.source);
    const auto normalized=converted(f),repeat=converted(f);check(encode_model(*normalized)==encode_model(*repeat),"Frame transfer was not deterministic.");
    const auto combined=compose_model_animations(f.base,{normalized});const auto repeated=compose_model_animations(f.base,{repeat});
    check(encode_model(*combined)==encode_model(*repeated),"Repeated transferred composition changed bytes.");
    check(encode_model(f.base)==base_bytes && encode_model(f.source)==source_bytes,"Transfer modified original inputs.");
    check(combined->skins[0].inverse_bind==f.base.skins[0].inverse_bind && combined->skins[0].joints==f.base.skins[0].joints &&
        combined->primitives[0]==f.base.primitives[0],"Transfer replaced original target binds/geometry.");
    for(unsigned i=0;i<f.base.nodes.size();++i)check(combined->nodes[i].position==f.base.nodes[i].position && combined->nodes[i].rotation==f.base.nodes[i].rotation &&
        combined->nodes[i].scale==f.base.nodes[i].scale && combined->nodes[i].parent==f.base.nodes[i].parent,"Transfer changed target defaults/hierarchy.");
    for(unsigned clip=0;clip<3;++clip){check(normalized->animations[clip].duration==2,"Transfer changed duration.");for(const auto& original:f.source.animations[clip].channels){
        auto match=std::find_if(normalized->animations[clip].channels.begin(),normalized->animations[clip].channels.end(),[&](const auto& c){return c.node==original.node && c.path==original.path;});
        check(match!=normalized->animations[clip].channels.end() && match->times==original.times && match->interpolation==original.interpolation,"Transfer changed original channel times/interpolation.");}
        for(double t:{0.,.125,.5,.999999,1.,1.000001,1.5,2.,3.})verify_pose(f,*combined,clip,t);verify_pose(f,*combined,clip,2,true);
    }
    const auto cooked=decode_model(encode_model(*combined));verify_pose(f,*cooked,0,.375);
    // Both norms are legal curve inputs but outside the rest-pose tolerance.
    // Normalizing values without identically scaling tangents changes the
    // cubic trajectory; the independent raw-control oracle must detect it.
    auto near_unit=fixture();auto& curve=near_unit.source.animations[0].channels[4];
    for(auto& x:curve.values[1])x*=.99996f;for(auto& x:curve.values[4])x*=1.00003f;
    validate_animation_data(near_unit.source);const auto raw_bytes=encode_model(near_unit.source);
    const auto near_unit_combined=compose_model_animations(near_unit.base,{converted(near_unit)});
    for(double t:{0.,.25,.5,.75,1.,1.25,1.5,1.75,2.})verify_pose(near_unit,*near_unit_combined,0,t,false,2e-6);
    check(encode_model(near_unit.source)==raw_bytes,"Transfer normalized original source curve values in place.");
    // A supplied pose can also carry the same legal near-unit representation;
    // its independent world array expresses the normalized orientation.
    auto supplied=near_unit;for(auto& x:supplied.source_ref.local[supplied.ids[2]].rotation)x*=.99996;
    const auto supplied_combined=compose_model_animations(supplied.base,{converted(supplied)});
    verify_pose(supplied,*supplied_combined,0,1,false,2e-6);
}
void sparse_defaults() {
    const auto f=fixture();const auto normalized=converted(f);const auto combined=compose_model_animations(f.base,{normalized});
    for(unsigned clip=0;clip<3;++clip)for(auto path:{AnimationPath::translation,AnimationPath::rotation,AnimationPath::scale}){
        const auto& channels=normalized->animations[clip].channels;auto found=std::find_if(channels.begin(),channels.end(),[&](const auto& c){return c.node==f.ids[3] && c.path==path;});
        check(found!=channels.end(),"Sparse source default differing from reference lost its constant channel.");
        for(const auto& value:found->values)check(value==found->values.front(),"Sparse constant channel fabricated motion.");
    }
    const auto actual=sample_model(*combined,0,0,false);bool differs=false;for(unsigned k=0;k<3;++k)differs=differs || std::abs(actual.local[3].position[k]-f.base.nodes[3].position[k])>.01;
    check(differs,"Sparse source default was silently replaced by target reference.");verify_pose(f,*combined,0,0);
}
void explicit_reference_before_filter() {
    auto f=fixture();auto full=f.source;AnimationClip ref;ref.name="authored-reference";ref.duration=1;
    for(unsigned i=0;i<4;++i){const auto& p=f.source_ref.local[i];ref.channels.push_back({i,AnimationPath::translation,AnimationInterpolation::linear,{0,1},{vec(p.position),vec(p.position)}});
        auto near_rotation=floats(p.rotation);for(auto& x:near_rotation)x*=.99996f;
        ref.channels.push_back({i,AnimationPath::rotation,AnimationInterpolation::linear,{0,1},{near_rotation,near_rotation}});
        ref.channels.push_back({i,AnimationPath::scale,AnimationInterpolation::linear,{0,1},{vec(p.scale),vec(p.scale)}});}
    full.animations.insert(full.animations.begin(),ref);const auto sampled=sample_model(full,0,0,false);
    for(unsigned i=0;i<4;++i)for(unsigned k=0;k<16;++k)near(sampled.world[i][k],f.source_ref.world[i][k]);
    f.source.animations={full.animations[1]}; // Reference take is no longer present in the selected donor.
    const auto normalized=transfer_animation_frames(f.base,f.source,sampled,f.target_ref,f.align_position,f.align_rotation);
    const auto combined=compose_model_animations(f.base,{normalized});check(combined->animations.size()==1 && combined->animations[0].name=="mixed","Filtering leaked reference take.");
    verify_pose(f,*combined,0,.375);verify_pose(f,*combined,0,1.5);
}
void antipodal_and_target_reference() {
    auto f=fixture();for(auto& p:f.source_ref.local)for(auto& x:p.rotation)x=-x;for(auto& p:f.target_ref.local)for(auto& x:p.rotation)x=-x;
    const auto normalized=converted(f);const auto combined=compose_model_animations(f.base,{normalized});verify_pose(f,*combined,0,.625);
    // Explicit target reference can differ from its imported defaults. Only
    // leaf axes change here, so joint origins stay exactly coincident.
    f=fixture();const auto change=axis(0,45);f.target_ref.local[3].rotation=product(f.target_ref.local[3].rotation,change);
    f.target_ref=reference(f.base,f.target_ref.local);f.gauge[3]=product(f.gauge[3],change);
    const auto base_before=encode_model(f.base);const auto changed=compose_model_animations(f.base,{converted(f)});
    check(encode_model(f.base)==base_before && changed->nodes[3].rotation==f.base.nodes[3].rotation,"Explicit target reference changed base rest.");verify_pose(f,*changed,0,.75);
}
void identity_conversion() {
    auto f=fixture();ModelAsset donor;donor.package_version=4;donor.nodes.assign(f.base.nodes.begin(),f.base.nodes.begin()+4);donor.roots={0};
    donor.animations={{"identity",2,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0,2},{{2,0,1,0},{3,0,1,0}}},
        {2,AnimationPath::rotation,AnimationInterpolation::linear,{0,2},{floats(donor.nodes[2].rotation),floats(product(donor.nodes[2].rotation,axis(1,180)))}}}}};
    const auto ref=reference(donor,defaults(donor));const auto before=encode_model(donor);
    const auto normalized=transfer_animation_frames(f.base,donor,ref,f.target_ref);
    check(encode_model(donor)==before && normalized->animations.size()==1 && normalized->animations[0].channels.size()==2,"Identity transfer changed input/channel count.");
    for(unsigned i=0;i<2;++i){const auto& a=normalized->animations[0].channels[i];const auto& b=donor.animations[0].channels[i];
        check(a.node==b.node && a.path==b.path && a.interpolation==b.interpolation && a.times==b.times && a.values==b.values,"Identity transfer changed authored curve data.");}
    const auto plain=compose_model_animations(f.base,{std::make_shared<const ModelAsset>(donor)}),actual=compose_model_animations(f.base,{normalized});
    for(double t:{0.,.25,1.,1.75,2.}){const auto a=sample_model(*actual,0,t,false),b=sample_model(*plain,0,t,false);for(unsigned i=0;i<5;++i)for(unsigned k=0;k<16;++k)near(a.world[i][k],b.world[i][k],1e-12);}
}
template<class F> void rejected(Fixture f,F change) {
    change(f);const auto base_before=encode_model(f.base),source_before=encode_model(f.source);auto published=std::make_shared<const ModelAsset>(f.source);const auto sentinel=published;bool caught=false;
    try {published=converted(f);}catch(const std::exception&){caught=true;}
    check(caught && published==sentinel,"Invalid transfer published a normalized donor.");
    check(encode_model(f.base)==base_before && encode_model(f.source)==source_before,"Rejected transfer modified source/base.");
}
void geometric_rejections() {
    rejected(fixture(),[](auto& f){f.target_ref.local[3].position[2]+=2e-5;f.target_ref=reference(f.base,f.target_ref.local);});
    rejected(fixture(),[](auto& f){f.source.nodes[f.ids[3]].scale={1,1+1e-12,1};});
    rejected(fixture(),[](auto& f){f.base.nodes[2].scale={1,1+1e-12,1};});
    rejected(fixture(),[](auto& f){f.source.animations[2].channels[0].values[1]={2,2+1e-5f,2,0};});
    rejected(fixture(),[](auto& f){auto& c=f.source.animations[2].channels[0];c.interpolation=AnimationInterpolation::cubic;
        c.values={{0,0,0,0},{1,1,1,0},{.1f,.2f,.1f,0},{0,0,0,0},{2,2,2,0},{0,0,0,0}};});
    rejected(fixture(),[](auto& f){f.source_ref.local[f.ids[3]].scale={1,1+1e-12,1};f.source_ref=reference(f.source,f.source_ref.local);});
    rejected(fixture(),[](auto& f){f.source.nodes[f.ids[3]].name="missing wrist";});
    rejected(fixture(),[](auto& f){f.source.nodes[f.ids[3]].parent=int(f.ids[0]);f.source_ref=reference(f.source,f.source_ref.local);});
    rejected(fixture(),[](auto& f){ModelNode duplicate;duplicate.name="pivot";duplicate.parent=int(f.ids[0]);f.source.nodes.push_back(duplicate);
        f.source_ref.local.push_back({});f.source_ref=reference(f.source,f.source_ref.local);});
    rejected(fixture(),[](auto& f){f.source.animations.clear();});
    rejected(fixture(),[](auto& f){
        f.base.nodes[0].scale={1000,1000,1000};f.source.nodes[f.ids[0]].scale={1000,1000,1000};
        f.target_ref.local[0].scale={1000,1000,1000};f.source_ref.local[f.ids[0]].scale={1000,1000,1000};
        // The local difference and C translation are only 2e-8, but the actual
        // coincident-origin policy is in global meters: 1000 * 2e-8 = 2e-5.
        f.target_ref.local[3].position[2]+=2e-8;f.target_ref=reference(f.base,f.target_ref.local);
        f.source_ref=reference(f.source,f.source_ref.local);const auto aligned=times(affine(f.align_position,f.align_rotation),oracle(f.source_ref.world[f.ids[3]]));
        double squared=0;for(unsigned k=0;k<3;++k){const double delta=aligned[k*4+3]-f.target_ref.world[3][12+k];squared+=delta*delta;}
        near(std::sqrt(squared),2e-5,1e-9);
    });
}
void invalid_references_and_alignment() {
    rejected(fixture(),[](auto& f){f.source_ref.local.pop_back();});
    rejected(fixture(),[](auto& f){f.target_ref.world.pop_back();});
    rejected(fixture(),[](auto& f){f.source_ref.time=std::numeric_limits<double>::quiet_NaN();});
    rejected(fixture(),[](auto& f){f.target_ref.world[0][0]=std::numeric_limits<double>::infinity();});
    rejected(fixture(),[](auto& f){f.source_ref.world[f.ids[3]][12]+=.1;});
    rejected(fixture(),[](auto& f){f.source_ref.local[f.ids[3]].rotation={0,0,0,0};});
    rejected(fixture(),[](auto& f){f.target_ref.local[3].scale={-1,1,1};});
    rejected(fixture(),[](auto& f){f.align_position[0]=std::numeric_limits<double>::infinity();});
    rejected(fixture(),[](auto& f){f.align_rotation={0,0,0,2};});
}
void synthesized_budget() {
    Fixture f;f.align_position={0,0,0};f.align_rotation={0,0,0,1};f.base.package_version=4;f.base.nodes.resize(2801);f.base.nodes[0].name="root";f.base.roots={0};
    for(unsigned i=1;i<=2800;++i){f.base.nodes[i].name="joint"+std::to_string(i);f.base.nodes[i].parent=0;
        if((i-1)%256==0)f.base.skins.push_back({"skin"+std::to_string(f.base.skins.size()),0,{},{}});
        f.base.skins.back().joints.push_back(i);f.base.skins.back().inverse_bind.push_back(api(affine({0,0,0},{0,0,0,1})));}
    f.source=f.base;f.source.skins.clear();f.target_ref=reference(f.base,defaults(f.base));f.source_ref=reference(f.source,defaults(f.source));
    for(unsigned i=1;i<=2800;++i){f.source.nodes[i].position={.125,0,0};f.source.nodes[i].rotation=axis(0,90);f.source.nodes[i].scale={2,2,2};}
    f.source.animations={{"bounded",1,{{0,AnimationPath::translation,AnimationInterpolation::linear,{0,1},{{0,0,0,0},{0,0,0,0}}}}}};
    // 2,800 required nodes each need three sparse constants: 8,401 channels
    // including the authored root track, beyond the existing 8,192 budget.
    validate_animation_data(f.base);validate_animation_data(f.source);rejected(std::move(f),[](auto&){});
}
}
int main() {
    try {geometric_equivalence();sparse_defaults();explicit_reference_before_filter();antipodal_and_target_reference();
        identity_conversion();geometric_rejections();invalid_references_and_alignment();synthesized_budget();
        std::cout<<"8 independent animation frame-transfer check groups passed.\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
