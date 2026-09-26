// SPDX-License-Identifier: Apache-2.0
#include "poima/audio.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace poima {
std::uint64_t SoundVoice::end_sample() const {
    const auto natural=sound.loop ? std::numeric_limits<std::uint64_t>::max() : start_tick*audio_tick_frames+sound.clip->samples.size();
    return stop_sample ? std::min(natural,*stop_sample) : natural;
}
bool SoundVoice::emitting(std::uint64_t sample) const { return sample>=start_tick*audio_tick_frames && sample<end_sample(); }
std::uint64_t SoundState::play(const std::string& emitter,const AudioEmitter& sound,std::uint64_t tick,float gain) {
    if(!sound.enabled || !sound.clip || sound.clip->samples.empty())throw std::runtime_error("Sound requires an enabled, loaded AudioEmitter.");
    if(!std::isfinite(gain) || gain<0 || gain>4)throw std::runtime_error("Sound event gain must be in [0,4].");
    if(next_>=9007199254740991ULL)throw std::runtime_error("Sound voice identity limit reached.");
    const auto sample=tick*audio_tick_frames;
    if(std::count_if(voices_.begin(),voices_.end(),[&](const auto& v){return v.emitting(sample);})>=64)throw std::runtime_error("At most 64 emitting sound voices.");
    if(voices_.size()==256) {
        const auto old=std::find_if(voices_.begin(),voices_.end(),[&](const auto& v){return !v.emitting(sample);});
        if(old==voices_.end())throw std::runtime_error("Sound history capacity reached.");
        voices_.erase(old);
    }
    const auto id=next_;voices_.push_back({id,tick,{},emitter,sound,gain});++next_;return id;
}
void SoundState::stop(std::uint64_t voice,std::uint64_t tick) {
    const auto found=std::find_if(voices_.begin(),voices_.end(),[&](const auto& v){return v.id==voice;});
    if(found==voices_.end())throw std::runtime_error("Sound voice is unknown or its finished record expired.");
    if(!found->stop_sample)found->stop_sample=tick*audio_tick_frames;
}
}
