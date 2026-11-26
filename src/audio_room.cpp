// SPDX-License-Identifier: Apache-2.0
#include "audio_room.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace poima {
void validate_audio_room(const AudioRoom& room) {
    for (const auto extent:room.half_extents)
        if (!(extent>=min_room_extent_m && extent<=max_room_extent_m))
            throw std::invalid_argument("AudioRoom half extents must be in [0.05,4096] metres.");
    if (!(room.decay_seconds>=min_room_decay_s && room.decay_seconds<=max_room_decay_s &&
        room.high_frequency_decay_seconds>=min_room_decay_s &&
        room.high_frequency_decay_seconds<=room.decay_seconds &&
        room.wet_gain>=0 && room.wet_gain<=1 && room.portal_send>=0 && room.portal_send<=1))
        throw std::invalid_argument("AudioRoom requires decay in [0.1,8] seconds, high-frequency decay no longer than the main decay, and wet gain and portal send in [0,1].");
}

namespace audio_detail {
void select_room(std::optional<AudioRoomSelection>& selected, const Matrix4& listener,
                 std::string_view entity, const Matrix4& world, const AudioRoom& room) {
    if (selected && (room.priority<selected->settings.priority ||
        (room.priority==selected->settings.priority && entity>=selected->entity))) return;
    const auto local=multiply(inverse_affine(world),listener);
    for (std::size_t axis=0;axis<3;++axis)
        if (!(std::abs(local[12+axis])<=room.half_extents[axis])) return;
    selected=AudioRoomSelection{std::string(entity),room};
}

RoomReverb::RoomReverb(const AudioRoom& initial, bool fade_in) {
    constexpr std::array lengths{743,929,1153,1423,1667,1999,2333,2711};
    for (std::size_t i=0;i<delays_.size();++i) delays_[i].samples.resize(lengths[i]);
    target(&initial);
    send_=fade_in ? 0.f : send_target_;
    for (auto& delay:delays_) {
        delay.low=delay.low_target;
        delay.high=delay.high_target;
    }
}

void RoomReverb::target(const AudioRoom* room) {
    if (room) validate_audio_room(*room);
    send_target_=room ? static_cast<float>(room->wet_gain) : 0.f;
    if (!room || (decay_==room->decay_seconds && high_decay_==room->high_frequency_decay_seconds)) return;
    decay_=room->decay_seconds;
    high_decay_=room->high_frequency_decay_seconds;
    for (auto& delay:delays_) {
        const auto seconds=static_cast<double>(delay.samples.size())/audio_rate;
        delay.low_target=static_cast<float>(std::pow(.001,seconds/decay_));
        delay.high_target=static_cast<float>(std::pow(.001,seconds/high_decay_));
    }
}

void RoomReverb::process(std::span<float> stereo,bool wet_only) noexcept {
    constexpr std::array<float,8> left{1,-1,1,-1,1,-1,1,-1}, right{1,1,-1,-1,1,1,-1,-1};
    constexpr float normalization=.3535533905932738f;
    constexpr float slew=1.f/transition_frames;
    for (std::size_t sample=0;sample<stereo.size();sample+=2) {
        send_+=(send_target_-send_)*slew;
        const float input_left=stereo[sample]*send_*normalization;
        const float input_right=stereo[sample+1]*send_*normalization;
        float sum=0, out_left=0, out_right=0;
        std::array<float,8> taps;
        for (std::size_t i=0;i<delays_.size();++i) {
            const auto& delay=delays_[i];
            taps[i]=delay.samples[delay.cursor];
            sum+=taps[i];
            out_left+=left[i]*taps[i];
            out_right+=right[i]*taps[i];
        }
        for (std::size_t i=0;i<delays_.size();++i) {
            auto& delay=delays_[i];
            delay.low+=(delay.low_target-delay.low)*slew;
            delay.high+=(delay.high_target-delay.high)*slew;
            // Orthogonal Householder feedback. The two-tap average gives
            // unity gain at DC and zero at Nyquist before shelving.
            const float feedback=.25f*sum-taps[i];
            const float low_pass=.5f*(feedback+delay.previous);
            delay.previous=feedback;
            const float value=input_left*left[i]+input_right*right[i]+
                delay.high*feedback+(delay.low-delay.high)*low_pass;
            delay.samples[delay.cursor]=std::abs(value)<1e-18f ? 0.f : value;
            if (++delay.cursor==delay.samples.size()) delay.cursor=0;
        }
        if(wet_only) {
            stereo[sample]=out_left*normalization;
            stereo[sample+1]=out_right*normalization;
        } else {
            stereo[sample]+=out_left*normalization;
            stereo[sample+1]+=out_right*normalization;
        }
    }
}

RoomReturnMixer::RoomReturnMixer(const AudioRoomReturn& initial)
    : reverb_(initial.settings),history_(100*audio_rate/343+3) {
    configure(initial);
    gains_=target_;
    fade_remaining_=0;
}

bool RoomReturnMixer::configure(const AudioRoomReturn& target) {
    reverb_.target(&target.settings);
    Bands next{};
    if(const auto& route=target.route) {
        const auto lateral=std::clamp(route->direction[0],-1.,1.);
        const std::array pan{std::sqrt(1-.85*lateral),std::sqrt(1+.85*lateral)};
        for(std::size_t ear=0;ear<pan.size();++ear)
            for(std::size_t band=0;band<next[ear].size();++band)
                next[ear][band]=static_cast<float>(target.settings.portal_send*route->distance_gain*
                    route->bands[band]*pan[ear]);
        target_delay_=std::min<std::size_t>(route->propagation_delay_samples,history_.size()-2);
        if(!seen_route_)delay_=target_delay_;
        seen_route_=true;
    }
    if(next!=target_) {
        target_=next;
        fade_remaining_=audio_block;
    }
    const auto changed=has_route_!=target.route.has_value();
    has_route_=target.route.has_value();
    return changed;
}

void RoomReturnMixer::mix(std::span<const float> excitation,std::span<float> destination) {
    std::array<float,2*audio_block> buffered;
    for(std::size_t frame=0;frame<excitation.size();++frame) {
        buffered[2*frame]=excitation[frame];
        buffered[2*frame+1]=excitation[frame];
    }
    reverb_.process(std::span(buffered).first(excitation.size()*2),true);
    constexpr float low_step=.05101270f, mid_step=.27909858f; // 400 Hz and 2.5 kHz poles at 48 kHz.
    for(std::size_t frame=0;frame<excitation.size();++frame) {
        history_[cursor_]={buffered[2*frame],buffered[2*frame+1]};
        delay_+=std::clamp(target_delay_-delay_,-.25,.25);
        const auto offset=static_cast<std::size_t>(delay_);
        const auto fraction=static_cast<float>(delay_-static_cast<double>(offset));
        const auto near=(cursor_+history_.size()-offset)%history_.size();
        const auto far=(near+history_.size()-1)%history_.size();
        const auto fade=fade_remaining_==0 ? 1.f : 1.f/static_cast<float>(fade_remaining_);
        for(std::size_t ear=0;ear<low_.size();++ear) {
            for(std::size_t band=0;band<gains_[ear].size();++band)
                gains_[ear][band]=std::lerp(gains_[ear][band],target_[ear][band],fade);
            const auto delayed=std::lerp(history_[near][ear],history_[far][ear],fraction);
            low_[ear]+=low_step*(delayed-low_[ear]);
            mid_[ear]+=mid_step*(delayed-mid_[ear]);
            destination[2*frame+ear]+=low_[ear]*gains_[ear][0]+(mid_[ear]-low_[ear])*gains_[ear][1]+
                (delayed-mid_[ear])*gains_[ear][2];
        }
        if(fade_remaining_>0)--fade_remaining_;
        if(++cursor_==history_.size())cursor_=0;
    }
}
}
}
