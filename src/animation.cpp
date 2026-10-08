// SPDX-License-Identifier: Apache-2.0
#include "poima/animation.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <limits>
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
std::array<double,4> derivative(const AnimationChannel& c,double time) {
    std::array<double,4> out{};
    const auto upper=std::upper_bound(c.times.begin(),c.times.end(),time);
    if(upper==c.times.begin() || upper==c.times.end() || c.interpolation==AnimationInterpolation::step)return out;
    const auto right=static_cast<std::size_t>(upper-c.times.begin()),left=right-1;
    const bool cubic=c.interpolation==AnimationInterpolation::cubic;
    const std::size_t stride=cubic ? 3 : 1,offset=cubic ? 1 : 0;
    std::array<double,4> a{},b{},value{};
    std::copy(c.values[left*stride+offset].begin(),c.values[left*stride+offset].end(),a.begin());
    std::copy(c.values[right*stride+offset].begin(),c.values[right*stride+offset].end(),b.begin());
    const double dt=double(c.times[right])-c.times[left],t=(time-c.times[left])/dt;
    if(cubic) {
        const double t2=t*t,t3=t2*t;
        for(std::size_t k=0;k<4;++k) {
            const double from=c.values[left*3+2][k],to=c.values[right*3][k];
            value[k]=(2*t3-3*t2+1)*a[k]+(t3-2*t2+t)*dt*from+(-2*t3+3*t2)*b[k]+(t3-t2)*dt*to;
            out[k]=(6*t2-6*t)*a[k]/dt+(3*t2-4*t+1)*from+(-6*t2+6*t)*b[k]/dt+(3*t2-2*t)*to;
        }
    }else if(c.path==AnimationPath::rotation) {
        double dot=0;for(std::size_t k=0;k<4;++k)dot+=a[k]*b[k];
        if(dot<0) {for(auto& x:b)x=-x;dot=-dot;}
        double wa=1-t,wb=t,dwa=-1/dt,dwb=1/dt;
        if(dot<.9995) {
            const double angle=std::acos(std::clamp(dot,0.0,1.0)),den=std::sin(angle);
            wa=std::sin((1-t)*angle)/den;wb=std::sin(t*angle)/den;
            dwa=-angle*std::cos((1-t)*angle)/(den*dt);dwb=angle*std::cos(t*angle)/(den*dt);
        }
        for(std::size_t k=0;k<4;++k) {value[k]=wa*a[k]+wb*b[k];out[k]=dwa*a[k]+dwb*b[k];}
    }else for(std::size_t k=0;k<4;++k)out[k]=(b[k]-a[k])/dt;
    if(c.path==AnimationPath::rotation) {
        double norm2=0;for(double x:value)norm2+=x*x;
        check(std::isfinite(norm2) && norm2>1e-24,"Animation quaternion evaluates to zero or nonfinite.");
        const double norm=std::sqrt(norm2);double projection=0;
        for(std::size_t k=0;k<4;++k) {value[k]/=norm;projection+=value[k]*out[k];}
        for(std::size_t k=0;k<4;++k)out[k]=(out[k]-value[k]*projection)/norm;
    }
    for(double x:out)check(std::isfinite(x),"Animation channel derivative is nonfinite.");
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
struct CompiledAnimation::Data {
    std::vector<NodePose> rest;
    std::vector<int> parents;
    std::vector<std::size_t> traversal;
    std::vector<AnimationClip> clips;
};
CompiledAnimation::CompiledAnimation(const ModelAsset& model) {
    validate_animation_data(model);
    auto data=std::make_shared<Data>();const auto count=model.nodes.size();
    data->rest.reserve(count);data->parents.reserve(count);data->traversal.reserve(count);
    std::vector<std::vector<std::size_t>> children(count);std::vector<std::size_t> pending;
    for(std::size_t i=0;i<count;++i) {
        const auto& node=model.nodes[i];data->rest.push_back({node.position,node.scale,node.rotation});data->parents.push_back(node.parent);
        if(node.parent<0)pending.push_back(i);else children[std::size_t(node.parent)].push_back(i);
    }
    while(!pending.empty()) {
        const auto index=pending.back();pending.pop_back();data->traversal.push_back(index);
        for(auto child:children[index])pending.push_back(child);
    }
    check(data->traversal.size()==count,"Animation hierarchy contains a cycle.");
    // AnimationClip/Channel own their names, times and values by value. Do not
    // retain the ModelAsset or shared geometry/material handles in this cache.
    data->clips=model.animations;data_=std::move(data);
}
ModelPose CompiledAnimation::sample(std::optional<std::uint32_t> clip,double time,bool loop,std::span<const NodePose> baseline) const {
    check(std::isfinite(time) && time>=0 && time<=1e9,"Sample time must be finite within 0..1e9 seconds.");
    check(bool(data_),"Compiled animation has no data.");const auto& data=*data_;
    check(baseline.empty() || baseline.size()==data.rest.size(),"Animation baseline must contain one local pose per model node.");
    for(const auto& node:baseline) {
        for(double x:node.position)check(std::isfinite(x) && std::abs(x)<=1e9,"Invalid animation baseline translation.");
        for(double x:node.scale)check(std::isfinite(x) && x>0 && x<=1e9,"Invalid animation baseline scale.");
        double norm=0;for(double x:node.rotation)norm+=x*x;
        check(std::isfinite(norm) && std::abs(norm-1)<1e-6,"Invalid animation baseline quaternion.");
    }
    ModelPose result;result.time=time;result.world.resize(data.rest.size());
    if(baseline.empty())result.local=data.rest;else result.local.assign(baseline.begin(),baseline.end());
    if(clip) {
        check(*clip<data.clips.size(),"Animation clip index is invalid.");const auto& animation=data.clips[*clip];result.time=loop && animation.duration>0 ? std::fmod(time,animation.duration) : std::min(time,animation.duration);
        for(const auto& c:animation.channels) { const auto v=evaluate(c,result.time);auto& node=result.local[c.node];if(c.path==AnimationPath::rotation) { node.rotation=v;normalize(node.rotation); }else std::copy_n(v.begin(),3,c.path==AnimationPath::translation ? node.position.begin() : node.scale.begin()); }
    }
    for(auto index:data.traversal) {
        const auto& node=result.local[index];for(double x:node.scale)check(std::isfinite(x) && x>0 && x<=1e9,"Animation scale evaluates outside the positive scale range.");for(double x:node.position)check(std::isfinite(x) && std::abs(x)<=1e9,"Animation position evaluates outside the supported range.");
        auto matrix=local_matrix(node.position,node.rotation,node.scale);const auto parent=data.parents[index];if(parent>=0)matrix=multiply(result.world[std::size_t(parent)],matrix);
        for(double x:matrix)check(std::isfinite(x) && std::abs(x)<=1e12,"Evaluated hierarchy matrix exceeds the supported range.");
        result.world[index]=matrix;
    }
    return result;
}
ModelMotion CompiledAnimation::sample_motion(std::optional<std::uint32_t> clip,double time,std::span<const NodePose> baseline) const {
    // Reuse the ordinary pose path exactly, including its complete baseline,
    // pose and hierarchy validation; no neighbouring time is sampled.
    ModelMotion result;result.pose=sample(clip,time,false,baseline);result.velocities.resize(result.pose.local.size());
    if(!clip)return result;
    for(const auto& channel:data_->clips[*clip].channels) {
        const auto d=derivative(channel,result.pose.time);auto& motion=result.velocities[channel.node];
        if(channel.path==AnimationPath::translation)std::copy_n(d.begin(),3,motion.translation_velocity.begin());
        else if(channel.path==AnimationPath::scale) {
            const auto& scale=result.pose.local[channel.node].scale;
            for(std::size_t k=0;k<3;++k)motion.log_scale_velocity[k]=d[k]/scale[k];
        }else {
            const auto& q=result.pose.local[channel.node].rotation;
            // Quaternion layout is xyzw; qdot * conjugate(q) is spatial
            // angular velocity, expressed before this node's local rotation.
            for(std::size_t k=0;k<3;++k) {
                const auto next=(k+1)%3,last=(k+2)%3;
                motion.angular_velocity[k]=2*(q[3]*d[k]-d[3]*q[k]-d[next]*q[last]+d[last]*q[next]);
            }
        }
        for(const auto& velocity:{motion.translation_velocity,motion.angular_velocity,motion.log_scale_velocity})
            for(double x:velocity)check(std::isfinite(x),"Animation local motion is nonfinite.");
    }
    return result;
}
ModelPose sample_model(const ModelAsset& model,std::optional<std::uint32_t> clip,double time,bool loop) {
    check(std::isfinite(time) && time>=0 && time<=1e9,"Sample time must be finite within 0..1e9 seconds.");
    return CompiledAnimation(model).sample(clip,time,loop);
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
SkinBounds skin_bounds(const MeshAsset& mesh) {
    check(!mesh.vertices.empty() && mesh.influences.size()==mesh.vertices.size(),"Skin bounds require vertex influences.");
    SkinBounds result;
    for(std::size_t i=0;i<mesh.vertices.size();++i) {
        const auto& influence=mesh.influences[i];double sum=0;
        for(std::size_t k=0;k<4;++k) {
            const auto joint=influence.joints[k];const double weight=influence.weights[k];
            check(joint<max_skin_joints && std::isfinite(weight) && weight>=0 && weight<=1,"Invalid bounded skin influence.");
            sum+=weight;if(result.joints.size()<=joint)result.joints.resize(std::size_t(joint)+1);
            if(weight==0)continue;
            auto& bound=result.joints[joint];const auto& p=mesh.vertices[i].position;
            for(float value:p)check(std::isfinite(value) && std::abs(value)<=1e9f,"Invalid bounded skin vertex.");
            if(!bound)bound=Bounds{{p[0],p[1],p[2]},{p[0],p[1],p[2]}};
            else for(std::size_t axis=0;axis<3;++axis) {
                bound->minimum[axis]=std::min(bound->minimum[axis],double(p[axis]));
                bound->maximum[axis]=std::max(bound->maximum[axis],double(p[axis]));
            }
        }
        check(std::abs(sum-1)<=1e-5,"Bounded skin weights must sum to one.");
        result.weight_sum_error=std::max(result.weight_sum_error,std::abs(sum-1));
    }
    return result;
}
Bounds posed_bounds(const SkinBounds& source,std::span<const Matrix4> palette) {
    check(!source.joints.empty() && source.joints.size()<=palette.size() && palette.size()<=max_skin_joints,"Skin bounds palette mismatch.");
    std::optional<Bounds> result;
    for(std::size_t i=0;i<source.joints.size();++i)if(source.joints[i]) {
        const auto bound=transform_bounds(*source.joints[i],palette[i]);
        if(!result)result=bound;
        else for(std::size_t axis=0;axis<3;++axis) {
            result->minimum[axis]=std::min(result->minimum[axis],bound.minimum[axis]);
            result->maximum[axis]=std::max(result->maximum[axis],bound.maximum[axis]);
        }
    }
    check(bool(result),"Skin bounds have no positive influences.");
    // Nonnegative normalized blends lie inside the union of joint bounds.
    // Account for accepted weight-sum error and rounded compute arithmetic.
    for(std::size_t axis=0;axis<3;++axis) {
        const double magnitude=std::max(std::abs(result->minimum[axis]),std::abs(result->maximum[axis]))+1;
        const double margin=(source.weight_sum_error+256*std::numeric_limits<float>::epsilon())*magnitude;
        result->minimum[axis]-=margin;result->maximum[axis]+=margin;
    }
    return *result;
}

}
