// SPDX-License-Identifier: Apache-2.0
#include "poima/player_lens.hpp"
#include "poima/player.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace poima {
namespace {
bool valid_fov(double value) {
    return std::isfinite(value) && value>=5 && value<=150;
}
void validate(const PlayerLensSample& sample) {
    const auto lens=[](const SceneLens& value) {
        if(!valid_fov(value.vertical_fov) || !std::isfinite(value.near_plane) ||
           !std::isfinite(value.far_plane) || value.near_plane<.001 ||
           value.near_plane>=value.far_plane || value.far_plane>1e7)
            throw std::invalid_argument("Player lens requires finite FOV5..150 and .001<=near<far<=1e7.");
    };
    lens(sample.world);if(sample.viewmodel)lens(*sample.viewmodel);
    if(sample.authored_fov && !valid_fov(*sample.authored_fov))
        throw std::invalid_argument("Player authored FOV must be finite within5..150.");
}
std::string_view mismatch(const PlayerLensSample& old,const PlayerLensSample& fresh) {
    if(old.session_id!=fresh.session_id)return "session_changed";
    if(old.source_id!=fresh.source_id)return "source_changed";
    if(old.camera_id!=fresh.camera_id)return "camera_changed";
    if(old.material_revision!=fresh.material_revision)return "material_revision_changed";
    if(old.world.near_plane!=fresh.world.near_plane || old.world.far_plane!=fresh.world.far_plane)
        return "world_clip_changed";
    if(old.viewmodel.has_value()!=fresh.viewmodel.has_value())return "viewmodel_presence_changed";
    if(old.viewmodel && (old.viewmodel->near_plane!=fresh.viewmodel->near_plane ||
       old.viewmodel->far_plane!=fresh.viewmodel->far_plane))return "viewmodel_clip_changed";
    if(old.authored_fov!=fresh.authored_fov)return "authored_fov_changed";
    return {};
}
bool consecutive(std::uint64_t old,std::uint64_t fresh) {
    return old!=std::numeric_limits<std::uint64_t>::max() && fresh==old+1;
}
double interpolate(double previous,double target,double alpha) {
    if(alpha==0 || previous==target)return previous;
    if(alpha==1)return target;
    constexpr double half_radians=0.0087266462599716478846;
    const auto from=std::log(std::tan(previous*half_radians));
    const auto to=std::log(std::tan(target*half_radians));
    const auto result=std::atan(std::exp(from+(to-from)*alpha))/half_radians;
    return std::clamp(result,std::min(previous,target),std::max(previous,target));
}
}
void PlayerLensHistory::reset(const PlayerLensSample& sample,const std::string& reason) {
    validate(sample);
    // Prepare every potentially allocating copy before replacing committed data.
    std::optional<PlayerLensSample> previous=sample,target=sample;
    std::string why=reason;
    previous_=std::move(previous);target_=std::move(target);reset_reason_=std::move(why);
}
void PlayerLensHistory::commit(const PlayerLensSample& sample) {
    validate(sample);
    if(!target_) {reset(sample,"initial_sample");return;}
    const auto reason=mismatch(*target_,sample);
    if(!reason.empty()) {reset(sample,std::string(reason));return;}
    if(!consecutive(target_->tick,sample.tick)) {reset(sample,"discontinuous_tick");return;}
    std::optional<PlayerLensSample> previous=target_,target=sample;
    previous_=std::move(previous);target_=std::move(target);reset_reason_.clear();
}
void PlayerLensHistory::reconcile(const PlayerLensSample& sample) {
    validate(sample);
    if(!target_) {reset(sample,"initial_sample");return;}
    if(sample==*target_)return;
    const auto reason=mismatch(*target_,sample);
    reset(sample,reason.empty() ? "external_change" : std::string(reason));
}
PlayerLensPresentation PlayerLensHistory::presentation(double alpha,bool enabled,std::optional<double> pref) const {
    if(!std::isfinite(alpha) || alpha<0 || alpha>1)
        throw std::invalid_argument("Player lens alpha must be finite within0..1.");
    if(pref && !valid_fov(*pref))
        throw std::invalid_argument("Player preferred FOV must be finite within5..150.");
    if(!target_)throw std::logic_error("Player lens presentation requires a committed sample.");
    const auto& before=*previous_;const auto& target=*target_;
    const bool pair=consecutive(before.tick,target.tick);
    PlayerLensPresentation result;
    result.enabled=enabled;result.previous_tick=before.tick;result.target_tick=target.tick;
    result.alpha=enabled && pair ? alpha : 1;
    result.reset_reason=reset_reason_;result.previous_world=before.world;result.target_world=target.world;
    result.previous_viewmodel=before.viewmodel;result.target_viewmodel=target.viewmodel;
    result.authored_fov=target.authored_fov;result.preference_fov=pref;
    result.rendered_world=target.world;
    result.rendered_world.vertical_fov=interpolate(before.world.vertical_fov,target.world.vertical_fov,result.alpha);
    result.rendered_world.vertical_fov=player_effective_vertical_fov(result.rendered_world.vertical_fov,target.authored_fov,pref);
    result.rendered_viewmodel=target.viewmodel;
    if(result.rendered_viewmodel)
        result.rendered_viewmodel->vertical_fov=interpolate(before.viewmodel->vertical_fov,target.viewmodel->vertical_fov,result.alpha);
    result.interpolating=enabled && pair && result.alpha<1 &&
        (before.world.vertical_fov!=target.world.vertical_fov ||
         (before.viewmodel && before.viewmodel->vertical_fov!=target.viewmodel->vertical_fov));
    return result;
}
}
