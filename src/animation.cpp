// SPDX-License-Identifier: Apache-2.0
#include "poima/animation.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
namespace poima {
namespace {
void check(bool ok,const char* text) { if(!ok)throw std::runtime_error(text); }
void normalize(std::array<double,4>& q) {
    double length=0;for(double x:q)length+=x*x;check(std::isfinite(length) && length>1e-24,"Animation quaternion evaluates to zero or nonfinite.");
    for(auto& x:q)x/=std::sqrt(length);
}
std::array<double,4> evaluate(const AnimationChannel& c,double time) {
    const bool cubic=c.interpolation==AnimationInterpolation::cubic;const std::size_t stride=cubic ? 3 : 1,offset=cubic ? 1 : 0;
    auto value=[&](std::size_t i) { std::array<double,4> out{};std::copy(c.values[i*stride+offset].begin(),c.values[i*stride+offset].end(),out.begin());return out; };
    const auto upper=std::upper_bound(c.times.begin(),c.times.end(),time);
    if(upper==c.times.begin())return value(0);
    if(upper==c.times.end())return value(c.times.size()-1);
    const auto right=static_cast<std::size_t>(upper-c.times.begin()),left=right-1;
    auto a=value(left),b=value(right);if(c.interpolation==AnimationInterpolation::step)return a;
    const double dt=double(c.times[right])-c.times[left],t=(time-c.times[left])/dt;std::array<double,4> out{};
    if(cubic) {
        const double t2=t*t,t3=t2*t;
        for(std::size_t k=0;k<4;++k)out[k]=(2*t3-3*t2+1)*a[k]+(t3-2*t2+t)*dt*c.values[left*3+2][k]+(-2*t3+3*t2)*b[k]+(t3-t2)*dt*c.values[right*3][k];
    } else if(c.path==AnimationPath::rotation) {
        double dot=0;for(std::size_t k=0;k<4;++k)dot+=a[k]*b[k];
        if(dot<0) { for(auto& x:b)x=-x;dot=-dot; }
        double wa=1-t,wb=t;
        if(dot<.9995) { const double angle=std::acos(std::clamp(dot,0.0,1.0)),den=std::sin(angle);wa=std::sin((1-t)*angle)/den;wb=std::sin(t*angle)/den; }
        for(std::size_t k=0;k<4;++k)out[k]=wa*a[k]+wb*b[k];
    } else for(std::size_t k=0;k<4;++k)out[k]=a[k]+t*(b[k]-a[k]);
    if(c.path==AnimationPath::rotation)normalize(out);
    return out;
}
void normalize3(std::array<float,3>& v) {
    double length=0;for(float x:v)length+=double(x)*x;check(std::isfinite(length) && length>1e-24,"Skinned direction collapses to zero.");for(auto& x:v)x=static_cast<float>(x/std::sqrt(length));
}
}
void validate_animation_data(const ModelAsset& model) {
    check(!model.nodes.empty() && model.nodes.size()<=10000,"Animation model has invalid node count.");
    check(model.skins.size()<=max_model_skins && model.animations.size()<=max_model_clips,"Skin/clip count limit exceeded.");
    // Preorder intervals give bounded ancestry checks without recursively
    // walking an externally authored hierarchy or depending on node order.
    const auto count=model.nodes.size();
    std::vector<std::vector<std::size_t>> children(count);
    std::vector<std::size_t> tree(count),enter(count),leave(count);
    std::vector<std::pair<std::size_t,bool>> stack;
    for(std::size_t i=0;i<count;++i) {
        const auto& node=model.nodes[i];const auto parent=node.parent;
        check(parent>=-1 && (parent<0 || std::size_t(parent)<count),"Animation parent index is invalid.");
        if(parent<0) { tree[i]=i;stack.emplace_back(i,false); }
        else children[std::size_t(parent)].push_back(i);
        for(double x:node.position)check(std::isfinite(x) && std::abs(x)<=1e9,"Invalid rest translation.");
        for(double x:node.scale)check(std::isfinite(x) && x>0 && x<=1e9,"Invalid rest scale.");
        double norm=0;for(double x:node.rotation)norm+=x*x;
        check(std::isfinite(norm) && std::abs(norm-1)<1e-6,"Invalid rest quaternion.");
    }
    std::size_t visited=0,clock=0;
    while(!stack.empty()) {
        const auto [index,exit]=stack.back();stack.pop_back();
        if(exit) { leave[index]=clock++;continue; }
        ++visited;enter[index]=clock++;stack.emplace_back(index,true);
        for(auto child:children[index]) { tree[child]=tree[index];stack.emplace_back(child,false); }
    }
    check(visited==count,"Animation hierarchy contains a cycle.");
    std::set<std::uint32_t> selected;
    for(auto root:model.roots)check(root<count && model.nodes[root].parent<0 && selected.insert(root).second,"Invalid/repeated model root.");
    check(!selected.empty(),"Model has no selected scene roots.");
    auto ancestor=[&](std::size_t a,std::size_t b) { return enter[a]<=enter[b] && leave[a]>=leave[b]; };
    std::size_t joints=0,channels=0,keys=0;
    for(const auto& skin:model.skins) {
        check(skin.name.size()<=256 && !skin.joints.empty() && skin.joints.size()<=max_skin_joints && skin.inverse_bind.size()==skin.joints.size(),"Invalid skin identity/joint/bind count.");
        joints+=skin.joints.size();check(joints<=4096,"Model joint-reference budget exceeded.");
        check(skin.skeleton>=-1 && (skin.skeleton<0 || std::size_t(skin.skeleton)<model.nodes.size()),"Skin skeleton node is invalid.");std::set<std::uint32_t> unique;
        for(std::size_t i=0;i<skin.joints.size();++i) {
            check(skin.joints[i]<model.nodes.size() && unique.insert(skin.joints[i]).second,"Skin joint is invalid or duplicated.");const auto joint=skin.joints[i];
            check(tree[joint]==tree[skin.joints.front()],"Skin joints have no common root.");
            check(skin.skeleton<0 || ancestor(std::size_t(skin.skeleton),joint),"Skin skeleton must be an ancestor of every joint.");
            const auto& m=skin.inverse_bind[i];
            for(double x:m)check(std::isfinite(x) && std::abs(x)<=1e9,"Invalid inverse bind matrix number.");
            check(m[3]==0 && m[7]==0 && m[11]==0 && m[15]==1,"Inverse bind matrix must be affine.");(void)inverse_affine(m);
        }
    }
    std::vector<std::uint16_t> max_joint(model.primitives.size());
    for(std::size_t primitive=0;primitive<model.primitives.size();++primitive) {
        const auto& mesh=model.primitives[primitive];
        check(bool(mesh),"Null model primitive.");
        check(mesh->influences.empty() || mesh->influences.size()==mesh->vertices.size(),"Skin influence count mismatch.");
        for(const auto& v:mesh->influences) {
            double sum=0;for(std::size_t k=0;k<4;++k) { check(v.joints[k]<max_skin_joints && std::isfinite(v.weights[k]) && v.weights[k]>=0 && v.weights[k]<=1,"Invalid skin joint/weight.");sum+=v.weights[k];max_joint[primitive]=std::max(max_joint[primitive],v.joints[k]); }
            check(std::abs(sum-1)<=1e-5,"Skin weights must sum to one.");
        }
    }
    for(std::size_t node_index=0;node_index<count;++node_index) {
        const auto& node=model.nodes[node_index];
        check(node.skin>=-1 && (node.skin<0 || std::size_t(node.skin)<model.skins.size()),"Invalid model skin binding.");
        if(node.skin>=0) {
            check(!node.primitives.empty(),"A skin binding requires mesh primitives.");
            const auto& skin=model.skins[std::size_t(node.skin)];
            if(selected.contains(static_cast<std::uint32_t>(tree[node_index])))
                check(selected.contains(static_cast<std::uint32_t>(tree[skin.joints.front()])),"Bound skin joints are outside the selected scene.");
        }
        for(auto p:node.primitives) {
            check(p<model.primitives.size(),"Invalid model primitive reference.");const auto& mesh=*model.primitives[p];
            if(node.skin<0) { check(mesh.influences.empty(),"A mesh with skin weights needs a skin binding.");continue; }
            check(!mesh.influences.empty(),"A skinned mesh needs JOINTS_0 and WEIGHTS_0.");
            check(max_joint[p]<model.skins[std::size_t(node.skin)].joints.size(),"Vertex joint is outside its bound skin.");
        }
    }
    for(const auto& clip:model.animations) {
        check(!clip.name.empty() && clip.name.size()<=256 && !clip.channels.empty(),"Invalid animation identity/channels.");
        channels+=clip.channels.size();check(channels<=max_animation_channels,"Animation channel budget exceeded.");std::set<std::pair<std::uint32_t,AnimationPath>> targets;double duration=0;
        for(const auto& c:clip.channels) {
            check(c.node<model.nodes.size() && std::uint32_t(c.path)<=2 && std::uint32_t(c.interpolation)<=2 && targets.emplace(c.node,c.path).second,"Invalid or duplicate animation channel target.");
            keys+=c.times.size();check(!c.times.empty() && keys<=max_animation_keys,"Animation key budget exceeded.");const bool cubic=c.interpolation==AnimationInterpolation::cubic;
            check(c.values.size()==c.times.size()*(cubic ? 3 : 1),"Animation key/value count mismatch.");float previous=-1;
            for(float t:c.times) { check(std::isfinite(t) && t>=0 && t<=3600 && t>previous,"Animation times must increase strictly within 0..3600 seconds.");previous=t; }
            duration=std::max(duration,double(c.times.back()));
            for(std::size_t i=0;i<c.values.size();++i) {
                const auto& v=c.values[i];for(float x:v)check(std::isfinite(x) && std::abs(x)<=1e9,"Invalid animation value/tangent.");
                if(c.path!=AnimationPath::rotation)check(v[3]==0,"Unused curve component must be zero.");
                if(cubic && i%3!=1)continue;
                if(c.path==AnimationPath::rotation) { double norm=0;for(float x:v)norm+=double(x)*x;check(std::abs(norm-1)<1e-4,"Animation rotation key must be normalized."); }
                if(c.path==AnimationPath::scale)for(std::size_t k=0;k<3;++k)check(v[k]>0,"Animation scale keys must be positive.");
            }
        }
        check(std::isfinite(clip.duration) && clip.duration==duration,"Animation duration disagrees with its keys.");
    }
}
ModelPose sample_model(const ModelAsset& model,std::optional<std::uint32_t> clip,double time,bool loop) {
    check(std::isfinite(time) && time>=0 && time<=1e9,"Sample time must be finite within 0..1e9 seconds.");validate_animation_data(model);ModelPose result;result.time=time;result.local.reserve(model.nodes.size());result.world.resize(model.nodes.size());
    for(const auto& node:model.nodes)result.local.push_back({node.position,node.scale,node.rotation});
    if(clip) {
        check(*clip<model.animations.size(),"Animation clip index is invalid.");const auto& animation=model.animations[*clip];result.time=loop && animation.duration>0 ? std::fmod(time,animation.duration) : std::min(time,animation.duration);
        for(const auto& c:animation.channels) { const auto v=evaluate(c,result.time);auto& node=result.local[c.node];if(c.path==AnimationPath::rotation) { node.rotation=v;normalize(node.rotation); }else std::copy_n(v.begin(),3,c.path==AnimationPath::translation ? node.position.begin() : node.scale.begin()); }
    }
    std::vector<unsigned char> marks(model.nodes.size());
    for(std::size_t i=0;i<model.nodes.size();++i) {
        if(marks[i]==2)continue;
        std::vector<std::size_t> chain;int current=static_cast<int>(i);
        while(current>=0 && marks[std::size_t(current)]!=2) {
            const auto index=static_cast<std::size_t>(current);check(marks[index]==0,"Animation hierarchy contains a cycle.");marks[index]=1;chain.push_back(index);current=model.nodes[index].parent;
            check(current>=-1 && (current<0 || std::size_t(current)<model.nodes.size()),"Animation parent index is invalid.");
        }
        for(auto it=chain.rbegin();it!=chain.rend();++it) {
            const auto& node=result.local[*it];for(double x:node.scale)check(std::isfinite(x) && x>0 && x<=1e9,"Animation scale evaluates outside the positive scale range.");for(double x:node.position)check(std::isfinite(x) && std::abs(x)<=1e9,"Animation position evaluates outside the supported range.");
            auto matrix=local_matrix(node.position,node.rotation,node.scale);const auto parent=model.nodes[*it].parent;if(parent>=0)matrix=multiply(result.world[std::size_t(parent)],matrix);
            for(double x:matrix)check(std::isfinite(x) && std::abs(x)<=1e12,"Evaluated hierarchy matrix exceeds the supported range.");
            result.world[*it]=matrix;marks[*it]=2;
        }
    }
    return result;
}
std::vector<Matrix4> skin_palette(const ModelAsset& model,const ModelPose& pose,std::uint32_t node) {
    check(node<model.nodes.size() && pose.world.size()==model.nodes.size(),"Skin pose/node does not match the model.");const auto skin_index=model.nodes[node].skin;check(skin_index>=0 && std::size_t(skin_index)<model.skins.size(),"Selected node has no skin.");const auto& skin=model.skins[std::size_t(skin_index)];const auto inverse=inverse_affine(pose.world[node]);std::vector<Matrix4> result;result.reserve(skin.joints.size());
    for(std::size_t i=0;i<skin.joints.size();++i)result.push_back(multiply(multiply(inverse,pose.world[skin.joints[i]]),skin.inverse_bind[i]));
    return result;
}
std::shared_ptr<const MeshAsset> deform_mesh(const MeshAsset& mesh,std::span<const Matrix4> palette) {
    check(mesh.influences.size()==mesh.vertices.size() && !mesh.influences.empty(),"CPU deformation requires skin influences.");auto result=std::make_shared<MeshAsset>(mesh);result->influences.clear();
    for(std::size_t i=0;i<mesh.vertices.size();++i) {
        Matrix4 blend{};const auto& skin=mesh.influences[i];for(std::size_t k=0;k<4;++k) { check(skin.joints[k]<palette.size(),"Skin palette is missing a joint.");for(std::size_t j=0;j<16;++j)blend[j]+=skin.weights[k]*palette[skin.joints[k]][j]; }
        const auto inverse=inverse_affine(blend);const auto& source=mesh.vertices[i];auto& vertex=result->vertices[i];
        for(std::size_t row=0;row<3;++row) {
            double position=blend[12+row],normal=0;for(std::size_t k=0;k<3;++k) { position+=blend[k*4+row]*source.position[k];normal+=inverse[row*4+k]*source.normal[k]; }
            check(std::isfinite(position) && std::abs(position)<=1e9,"Skinned vertex exceeds supported coordinates.");vertex.position[row]=static_cast<float>(position);vertex.normal[row]=static_cast<float>(normal);
        }
        normalize3(vertex.normal);
        if(source.tangent[3]!=0) {
            std::array<float,3> tangent{};for(std::size_t row=0;row<3;++row)for(std::size_t k=0;k<3;++k)tangent[row]+=static_cast<float>(blend[k*4+row])*source.tangent[k];
            double dot=0;for(std::size_t k=0;k<3;++k)dot+=double(tangent[k])*vertex.normal[k];for(std::size_t k=0;k<3;++k)tangent[k]-=static_cast<float>(dot*vertex.normal[k]);normalize3(tangent);
            const double determinant=blend[0]*(blend[5]*blend[10]-blend[9]*blend[6])-blend[4]*(blend[1]*blend[10]-blend[9]*blend[2])+blend[8]*(blend[1]*blend[6]-blend[5]*blend[2]);
            std::copy(tangent.begin(),tangent.end(),vertex.tangent.begin());vertex.tangent[3]=source.tangent[3]*(determinant<0 ? -1.0f : 1.0f);
        }
    }
    return result;
}
}
