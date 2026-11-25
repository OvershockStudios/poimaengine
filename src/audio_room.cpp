// SPDX-License-Identifier: Apache-2.0
#include "audio_room.hpp"
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
        room.wet_gain>=0 && room.wet_gain<=1))
        throw std::invalid_argument("AudioRoom requires decay in [0.1,8] seconds, high-frequency decay no longer than the main decay, and wet gain in [0,1].");
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

void RoomReverb::process(std::span<float> stereo) noexcept {
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
        stereo[sample]+=out_left*normalization;
        stereo[sample+1]+=out_right*normalization;
    }
}
}
}
