// SPDX-License-Identifier: Apache-2.0
#include "poima/player_pose.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace poima {
namespace {
using Quaternion=std::array<double,4>;
void validate_pose(const RuntimeCameraPose& pose) {
    double squared_norm=0;
    for(double value:pose.position)
        if(!std::isfinite(value) || std::abs(value)>10)
            throw std::invalid_argument("Player pose requires finite local position within10 metres per axis.");
    for(double value:pose.rotation) {
        if(!std::isfinite(value) || std::abs(value)>1.00001)
            throw std::invalid_argument("Player pose requires finite normalized quaternion components.");
        squared_norm+=value*value;
    }
    if(std::abs(squared_norm-1)>1e-5)
        throw std::invalid_argument("Player pose quaternion squared norm must be within0.00001 of one.");
}
void validate(const PlayerPoseSample& sample) {
    validate_pose(sample.world);if(sample.viewmodel)validate_pose(*sample.viewmodel);
    for(double value:sample.base_world)
        if(!std::isfinite(value) || std::abs(value)>1e12)
            throw std::invalid_argument("Player pose base matrix requires finite components within1e12.");
}
std::string_view mismatch(const PlayerPoseSample& old,const PlayerPoseSample& fresh) {
    if(old.session_id!=fresh.session_id)return "session_changed";
    if(old.source_id!=fresh.source_id)return "source_changed";
    if(old.camera_id!=fresh.camera_id)return "camera_changed";
    if(old.material_revision!=fresh.material_revision)return "material_revision_changed";
    if(old.viewmodel.has_value()!=fresh.viewmodel.has_value())return "viewmodel_presence_changed";
    return {};
}
bool consecutive(std::uint64_t old,std::uint64_t fresh) {
    return old!=std::numeric_limits<std::uint64_t>::max() && fresh==old+1;
}
Quaternion unit(Quaternion value) {
    const auto length=std::hypot(std::hypot(value[0],value[1]),std::hypot(value[2],value[3]));
    for(auto& component:value)component/=length;
    return value;
}
Quaternion product(const Quaternion& a,const Quaternion& b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
        a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
        a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
Quaternion slerp(const Quaternion& from,const Quaternion& to,double alpha) {
    const auto a=unit(from);auto b=unit(to);
    double dot=0;for(std::size_t i=0;i<4;++i)dot+=a[i]*b[i];
    if(dot<0) {for(auto& value:b)value=-value;dot=-dot;}
    // Relative-rotation atan2 retains very small arcs when dot rounds to one.
    // No arbitrary near-angle nlerp threshold changes the requested arc speed.
    const auto relative=product(Quaternion{-a[0],-a[1],-a[2],a[3]},b);
    const double sine=std::hypot(relative[0],relative[1],relative[2]);
    if(sine==0)return a;
    const double angle=std::atan2(sine,std::clamp(dot,0.,1.));
    const double fraction_sine=std::sin(alpha*angle);
    return unit(product(a,Quaternion{relative[0]/sine*fraction_sine,
        relative[1]/sine*fraction_sine,relative[2]/sine*fraction_sine,std::cos(alpha*angle)}));
}
RuntimeCameraPose interpolate(const RuntimeCameraPose& previous,const RuntimeCameraPose& target,double alpha) {
    if(alpha==0)return previous;
    if(alpha==1)return target;
    if(previous==target)return previous;
    RuntimeCameraPose result;
    for(std::size_t i=0;i<3;++i)result.position[i]=previous.position[i]+(target.position[i]-previous.position[i])*alpha;
    result.rotation=slerp(previous.rotation,target.rotation,alpha);
    return result;
}
}
void PlayerPoseHistory::reset(const PlayerPoseSample& sample,const std::string& reason) {
    validate(sample);
    std::optional<PlayerPoseSample> previous=sample,target=sample;
    std::string why=reason;
    previous_=std::move(previous);target_=std::move(target);reset_reason_=std::move(why);
}
void PlayerPoseHistory::commit(const PlayerPoseSample& sample) {
    validate(sample);
    if(!target_) {reset(sample,"initial_sample");return;}
    const auto reason=mismatch(*target_,sample);
    if(!reason.empty()) {reset(sample,std::string(reason));return;}
    if(!consecutive(target_->tick,sample.tick)) {reset(sample,"discontinuous_tick");return;}
    std::optional<PlayerPoseSample> previous=target_,target=sample;
    previous_=std::move(previous);target_=std::move(target);reset_reason_.clear();
}
void PlayerPoseHistory::reconcile(const PlayerPoseSample& sample) {
    validate(sample);
    if(!target_) {reset(sample,"initial_sample");return;}
    if(sample==*target_)return;
    const auto reason=mismatch(*target_,sample);
    reset(sample,reason.empty() ? "external_change" : std::string(reason));
}
PlayerPosePresentation PlayerPoseHistory::presentation(double alpha,bool enabled) const {
    if(!std::isfinite(alpha) || alpha<0 || alpha>1)
        throw std::invalid_argument("Player pose alpha must be finite within0..1.");
    if(!target_)throw std::logic_error("Player pose presentation requires a committed sample.");
    const auto& before=*previous_;const auto& target=*target_;
    PlayerPosePresentation result;
    result.enabled=enabled;result.previous_tick=before.tick;result.target_tick=target.tick;
    result.alpha=enabled && consecutive(before.tick,target.tick) ? alpha : 1;
    result.reset_reason=reset_reason_;
    result.previous_world=before.world;result.target_world=target.world;
    result.previous_viewmodel=before.viewmodel;result.target_viewmodel=target.viewmodel;
    result.rendered_world=interpolate(before.world,target.world,result.alpha);
    result.rendered_viewmodel=target.viewmodel;
    if(result.rendered_viewmodel)result.rendered_viewmodel=interpolate(*before.viewmodel,*target.viewmodel,result.alpha);
    result.target_base_world=target.base_world;
    result.interpolating=enabled && consecutive(before.tick,target.tick) && result.alpha<1 &&
        (before.world!=target.world || before.viewmodel!=target.viewmodel);
    return result;
}
}
