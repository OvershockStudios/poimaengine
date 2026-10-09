// SPDX-License-Identifier: Apache-2.0
#include "../src/model_import.hpp"
#include "poima/animation.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <tuple>

using namespace poima;
namespace {
void check(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
void near(double got,double expected,double tolerance=1e-14) {
    check(std::isfinite(got) && std::abs(got-expected)<=tolerance,"Reported measurement differs from analytic expectation.");
}
ModelAsset base_model() {
    ModelAsset m;m.package_version=4;m.nodes.resize(6);m.roots={0};
    m.nodes[0].name="root";
    m.nodes[1].name="group";m.nodes[1].parent=0;
    m.nodes[2].name="joint:/";m.nodes[2].parent=1;
    m.nodes[3].name="optional";m.nodes[3].parent=0;
    m.nodes[4].name="target";m.nodes[4].parent=3;
    m.nodes[5].name="mesh";m.nodes[5].parent=0;m.nodes[5].skin=0;m.nodes[5].primitives={0};
    m.skins.push_back({"skin",0,{2},{identity_matrix()}});
    auto mesh=std::make_shared<MeshAsset>();mesh->has_uv=true;
    for(const auto& p:std::array<std::array<float,3>,3>{{{0,0,0},{1,0,0},{0,1,0}}}) {
        MeshVertex v;v.position=p;v.normal={0,0,1};v.uv={p[0],p[1]};v.tangent={1,0,0,1};mesh->vertices.push_back(v);
    }
    mesh->indices={0,1,2};mesh->influences.assign(3,SkinWeight{{0,0,0,0},{1,0,0,0}});m.primitives={mesh};
    auto image=std::make_shared<TextureImage>();image->mips.push_back({1,1,{255,255,255,255}});m.images={image};
    m.diagnostics={"Original analytic fixture."};validate_animation_data(m);return m;
}
AnimationChannel channel(std::uint32_t target) {
    return {target,AnimationPath::translation,AnimationInterpolation::linear,{0,1},{{0,0,0,0},{2,0,0,0}}};
}
std::shared_ptr<ModelAsset> donor_model(const ModelAsset& base,const std::string& name="move",std::uint32_t target=2) {
    auto d=std::make_shared<ModelAsset>(base);d->skins.clear();d->primitives.clear();d->images.clear();d->diagnostics.clear();
    for(auto& n:d->nodes) {n.skin=-1;n.primitives.clear();}
    d->animations={{name,1,{channel(target)}}};validate_animation_data(*d);return d;
}
AnimationCompositionReport failure(const ModelAsset& base,
        const std::vector<std::shared_ptr<const ModelAsset>>& donors,const char* message=nullptr) {
    // Failed assignment must not publish even the successful prefix of donors.
    auto published=std::make_shared<const ModelAsset>(base);const auto original=published;
    const auto before=encode_model(base);std::vector<std::string> donor_before;
    for(const auto& d:donors)donor_before.push_back(d ? encode_model(*d) : std::string{});
    try {published=compose_model_animations(base,donors);}
    catch(const AnimationCompositionError& e) {
        check(published==original,"Failed composition published a partial model.");
        check(encode_model(base)==before,"Failed composition modified the base.");
        for(std::size_t i=0;i<donors.size();++i)if(donors[i])
            check(encode_model(*donors[i])==donor_before[i],"Failed composition modified a donor.");
        if(message)check(std::string(e.what())==message,"Existing first-error precedence/message changed.");
        return e.report();
    }
    throw std::runtime_error("Incompatible composition did not produce a structured rejection.");
}
template<class F> void plain_failure(F f,const char* message) {
    bool caught=false;
    try {f();}catch(const AnimationCompositionError&) {throw std::runtime_error("Non-composition guard became a structured mismatch.");}
    catch(const std::exception& e) {caught=true;check(std::string(e.what())==message,"Existing validation guard changed.");}
    check(caught,"Invalid model/composition guard was accepted.");
}
const AnimationCompositionIssue& one_frame(const AnimationCompositionReport& r) {
    check(r.donor_index==0 && r.required_base_nodes==3 && r.checked_source_nodes==3 && r.matched_nodes==2 &&
        r.issue_count==1 && r.issues.size()==1 && !r.truncated,"Single-frame report counts differ.");
    const auto& i=r.issues.front();check(i.kind==AnimationCompositionIssueKind::rest_frame && i.base_node==2 &&
        i.donor_node==2 && i.base_name=="joint:/" && i.donor_name=="joint:/" && i.required_skeleton && i.animated_ancestry,
        "Single-frame issue identity/roles differ.");return i;
}
void boundaries() {
    const auto base=base_model();
    auto d=donor_model(base);d->nodes[2].position[0]=1e-5;
    check(bool(compose_model_animations(base,{d})),"Inclusive translation boundary rejected.");
    d->nodes[2].position[0]=std::nextafter(1e-5,std::numeric_limits<double>::infinity());
    auto r=failure(base,{d});auto i=one_frame(r);check(i.translation_mismatch && !i.scale_mismatch && !i.rotation_mismatch,"Translation boundary flags differ.");
    near(i.max_translation_meters,d->nodes[2].position[0],0);near(i.absolute_quaternion_dot,1,0);
    d=donor_model(base);d->nodes[2].scale[1]=1+1e-6;
    check(d->nodes[2].scale[1]-1<=1e-6,"Scale fixture is outside the binary64 boundary.");
    check(bool(compose_model_animations(base,{d})),"Inclusive scale boundary rejected.");
    d->nodes[2].scale[1]=std::nextafter(1+1e-6,std::numeric_limits<double>::infinity());
    r=failure(base,{d});i=one_frame(r);check(!i.translation_mismatch && i.scale_mismatch && !i.rotation_mismatch,"Scale boundary flags differ.");
    near(i.max_scale_absolute,d->nodes[2].scale[1]-1,0);
    d=donor_model(base);const double limit=1-1e-10;
    d->nodes[2].rotation={std::sqrt(1-limit*limit),0,0,limit};
    check(bool(compose_model_animations(base,{d})),"Inclusive quaternion-dot boundary rejected.");
    const double below=std::nextafter(limit,0.);d->nodes[2].rotation={std::sqrt(1-below*below),0,0,below};
    r=failure(base,{d});i=one_frame(r);check(!i.translation_mismatch && !i.scale_mismatch && i.rotation_mismatch,"Rotation boundary flags differ.");
    near(i.absolute_quaternion_dot,below,0);
    // Validation permits tiny quaternion norm residuals. The unchanged raw-dot
    // predicate still rejects this pair, but its orientation difference is zero.
    d=donor_model(base);d->nodes[2].rotation={0,0,0,1-2e-10};
    r=failure(base,{d});i=one_frame(r);check(i.rotation_mismatch,"Raw quaternion-dot admission was silently normalized.");
    near(i.absolute_quaternion_dot,1-2e-10,0);near(i.rotation_degrees,0,0);
}
void successful_identity() {
    auto base=base_model();const double s=std::sqrt(.5);base.nodes[2].rotation={0,0,s,s};
    auto d=donor_model(base);d->nodes[2].rotation={0,0,-s,-s};d->diagnostics={"Source take retained."};
    // Permute node IDs while preserving named hierarchy. IDs are source-local,
    // so exact hierarchy composition must remap the authored channel to node 2.
    std::swap(d->nodes[2],d->nodes[4]);d->animations[0].channels[0].node=4;
    const auto base_bytes=encode_model(base),donor_bytes=encode_model(*d);
    auto expected=base;auto clip=d->animations.front();clip.channels.front().node=2;expected.animations.push_back(clip);
    expected.diagnostics.push_back("Animation donor 0 node 0 'root' -> base node 0");
    expected.diagnostics.push_back("Animation donor 0 node 1 'group' -> base node 1");
    expected.diagnostics.push_back("Animation donor 0 node 4 'joint:/' -> base node 2");
    expected.diagnostics.push_back("Animation donor 0: Source take retained.");
    const auto combined=compose_model_animations(base,{d});
    check(encode_model(*combined)==encode_model(expected),"Successful cooked composition identity changed.");
    check(encode_model(base)==base_bytes && encode_model(*d)==donor_bytes,"Successful composition modified input.");
    check(combined->primitives[0]==base.primitives[0] && combined->images[0]==base.images[0],"Composition copied/replaced immutable content.");
    const auto pose=sample_model(*combined,0,.25,false);near(pose.local[2].position[0],.5);near(pose.local[2].rotation[2],s);
    const auto decoded=decode_model(encode_model(*combined));check(encode_model(*decoded)==encode_model(*combined),"Composed package round trip changed bytes.");
}
void multiple_frames() {
    const auto base=base_model();auto d=donor_model(base);
    d->animations[0].channels.push_back(channel(4));
    d->nodes[1].position[1]=-.75;
    d->nodes[2].position={.125,-.5,.25};d->nodes[2].scale={1.25,2,.5};
    d->nodes[2].rotation={0,0,std::sqrt(.75),.5}; // exactly 120 degrees around Z.
    d->nodes[4].scale[0]=1.125;
    const auto r=failure(base,{d},"Animation donor rest frame differs from the base; retargeting is required.");
    check(r.required_base_nodes==3 && r.checked_source_nodes==5 && r.matched_nodes==2 && r.issue_count==3 && !r.truncated,"Unique-node multi-frame counts differ.");
    check(r.issues[0].base_node==1 && r.issues[1].base_node==2 && r.issues[2].base_node==4,"Stable required/animated issue ordering differs.");
    for(const auto& i:r.issues)check(i.kind==AnimationCompositionIssueKind::rest_frame && i.base_node==i.donor_node && i.animated_ancestry,"Multi-frame identity/ancestry differs.");
    check(r.issues[0].required_skeleton && r.issues[1].required_skeleton && !r.issues[2].required_skeleton,"Required versus optional animated roles overlap incorrectly.");
    near(r.issues[0].max_translation_meters,.75,0);near(r.issues[1].max_translation_meters,.5,0);
    near(r.issues[1].max_scale_absolute,1,0);near(r.issues[1].absolute_quaternion_dot,.5,0);
    near(r.issues[1].rotation_degrees,120,1e-12);
    check(r.issues[1].translation_mismatch && r.issues[1].scale_mismatch && r.issues[1].rotation_mismatch,"Combined component flags lost an error.");
    near(r.issues[2].max_scale_absolute,.125,0);
}
void missing_nodes() {
    const auto base=base_model();auto d=donor_model(base,"move",4);d->nodes[2].name="different joint";d->nodes[4].name="different target";
    auto r=failure(base,{d},"Animation donor is missing a base skeleton joint/ancestor.");
    check(r.required_base_nodes==3 && r.checked_source_nodes==4 && r.matched_nodes==3 && r.issue_count==2,"Missing-node counts differ.");
    const auto& required=r.issues[0];const auto& animated=r.issues[1];
    check(required.kind==AnimationCompositionIssueKind::missing_joint_or_ancestor && required.base_node==2 && !required.donor_node &&
        required.base_name=="joint:/" && required.donor_name.empty() && required.required_skeleton && !required.animated_ancestry,"Missing required identity fabricated an absent source.");
    check(animated.kind==AnimationCompositionIssueKind::missing_animation_target && !animated.base_node && animated.donor_node==4 &&
        animated.base_name.empty() && animated.donor_name=="different target" && !animated.required_skeleton && animated.animated_ancestry,"Missing animated identity fabricated an absent base.");
    d=donor_model(base);d->nodes[1].name="missing ancestor";
    r=failure(base,{d});check(r.issue_count==4 && r.checked_source_nodes==3 && r.matched_nodes==1,"Missing ancestor descendant closure was not reported.");
    check(r.issues[0].base_node==1 && r.issues[1].base_node==2 && !r.issues[0].donor_node && !r.issues[1].donor_node,
        "Missing required ancestor/joint identities differ.");
    check(r.issues[2].donor_node==2 && r.issues[3].donor_node==1 && !r.issues[2].base_node && !r.issues[3].base_node,
        "Missing animation target/ancestor identities differ.");
}
void failed_donor_index() {
    const auto base=base_model();auto first=donor_model(base,"first");auto second=donor_model(base,"second");
    second->nodes[2].position[2]=.25;
    const auto r=failure(base,{first,second,nullptr});
    check(r.donor_index==1 && r.issue_count==1 && r.issues.front().base_node==2,"Report did not identify first failed donor.");
}
auto issue_key(const AnimationCompositionIssue& i) {
    return std::tie(i.kind,i.base_node,i.donor_node,i.base_name,i.donor_name,i.required_skeleton,i.animated_ancestry,
        i.max_translation_meters,i.max_scale_absolute,i.absolute_quaternion_dot,i.rotation_degrees,
        i.translation_mismatch,i.scale_mismatch,i.rotation_mismatch);
}
void bounded_complete_report() {
    ModelAsset base;base.package_version=4;base.nodes.resize(71);base.nodes[0].name="root";base.roots={0};
    ModelSkin skin;skin.name="crowd";skin.skeleton=0;
    for(std::uint32_t i=1;i<=70;++i) {base.nodes[i].name="bone"+std::to_string(i);base.nodes[i].parent=0;skin.joints.push_back(i);skin.inverse_bind.push_back(identity_matrix());}
    base.skins.push_back(skin);auto d=donor_model(base);d->animations.front().channels.clear();
    for(std::uint32_t i=1;i<=70;++i) {d->nodes[i].position[0]=double(i)/8;d->animations.front().channels.push_back(channel(i));}
    const auto a=failure(base,{d}),b=failure(base,{d});
    check(a.required_base_nodes==71 && a.checked_source_nodes==71 && a.matched_nodes==1 && a.issue_count==70 && a.issues.size()==64 && a.truncated,
        "Bounded report lost complete counts or exceeded capacity.");
    check(b.donor_index==a.donor_index && b.required_base_nodes==a.required_base_nodes && b.checked_source_nodes==a.checked_source_nodes &&
        b.matched_nodes==a.matched_nodes && b.issue_count==a.issue_count && b.truncated==a.truncated && b.issues.size()==a.issues.size(),"Repeated report summary changed.");
    for(std::uint32_t i=0;i<64;++i) {
        const auto& v=a.issues[i];check(v.base_node==i+1 && v.donor_node==i+1 && v.base_name=="bone"+std::to_string(i+1) &&
            v.donor_name==v.base_name && v.required_skeleton && v.animated_ancestry,"Bounded report identities/order changed.");
        near(v.max_translation_meters,double(i+1)/8,0);check(issue_key(v)==issue_key(b.issues[i]),"Repeated report fields changed.");
    }
}
void unmatched_unreferenced_nodes() {
    const auto base=base_model();auto d=donor_model(base);d->nodes[4].name="unreferenced renamed node";d->nodes[4].position[0]=500;
    check(bool(compose_model_animations(base,{d})),"Unreferenced nonskeletal donor node unexpectedly became required.");
    // Adding an authored channel makes the same node relevant and must reject.
    d->animations.front().channels.push_back(channel(4));const auto r=failure(base,{d});
    check(r.issue_count==1 && r.issues.front().kind==AnimationCompositionIssueKind::missing_animation_target &&
        r.issues.front().donor_node==4 && !r.issues.front().required_skeleton,"Animated optional node was ignored.");
}
void original_guards() {
    const auto base=base_model();auto d=donor_model(base);
    plain_failure([&]{compose_model_animations(base,{d,d});},"Duplicate/empty donor clip name; rename takes before combining them.");
    auto ambiguous=donor_model(base);ambiguous->nodes[3].name="group";
    plain_failure([&]{compose_model_animations(base,{ambiguous});},"Animation composition rejects duplicate sibling/root names; rename ambiguous nodes first.");
    plain_failure([&]{compose_model_animations(base,{});},"Animation composition requires 1..32 donors.");
    plain_failure([&]{compose_model_animations(base,{nullptr});},"Null animation donor.");
    plain_failure([&]{compose_model_animations(base,std::vector<std::shared_ptr<const ModelAsset>>(33,d));},"Animation composition requires 1..32 donors.");
    auto invalid=donor_model(base);invalid->nodes[2].scale[0]=0;
    plain_failure([&]{compose_model_animations(base,{invalid});},"Invalid rest scale.");
    auto duplicate=donor_model(base);duplicate->animations.front().channels.push_back(channel(2));
    plain_failure([&]{compose_model_animations(base,{duplicate});},"Invalid or duplicate animation channel target.");
    auto no_skin=base;no_skin.skins.clear();no_skin.nodes[5].skin=-1;no_skin.nodes[5].primitives.clear();no_skin.primitives.clear();
    plain_failure([&]{compose_model_animations(no_skin,{d});},"Animation composition requires a skinned base model.");
}
}
int main() {
    try {
        boundaries();successful_identity();multiple_frames();missing_nodes();failed_donor_index();
        bounded_complete_report();unmatched_unreferenced_nodes();original_guards();
        std::cout<<"8 animation composition diagnostic check groups passed.\n";return 0;
    }catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
