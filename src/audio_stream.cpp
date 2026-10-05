// SPDX-License-Identifier: Apache-2.0
#include "poima/audio.hpp"
#include "audio_hrtf.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <stdexcept>
#if POIMA_AUDIO
#include <phonon.h>
#endif
namespace poima {
namespace {
void check(bool ok,const char* text) { if(!ok)throw std::runtime_error(text); }
[[maybe_unused]] bool same_paths(const AudioSnapshot& a,const AudioSnapshot& b) {
    if(a.listener!=b.listener || a.geometry.size()!=b.geometry.size() || a.sources.size()!=b.sources.size())return false;
    for(std::size_t i=0;i<a.geometry.size();++i) {
        const auto& x=a.geometry[i];const auto& y=b.geometry[i];
        if(x.entity!=y.entity || x.world!=y.world || x.mesh!=y.mesh || x.material.enabled!=y.material.enabled || x.material.absorption!=y.material.absorption || x.material.transmission!=y.material.transmission || x.material.scattering!=y.material.scattering)return false;
    }
    for(std::size_t i=0;i<a.sources.size();++i)if(a.sources[i].entity!=b.sources[i].entity || a.sources[i].world!=b.sources[i].world)return false;
    return true;
}
}
struct AudioStream::Impl {
    AudioStreamStats stats;
    std::uint64_t start=0,cursor=0,committed=0,seen=0;
    bool finished=false;
    AudioSnapshot snapshot;AudioReport paths;
#if POIMA_AUDIO
    IPLContext context=nullptr;IPLHRTF hrtf=nullptr;
    struct Voice {
        SoundVoice state;std::uint64_t delay=0;
        IPLDirectEffect direct=nullptr;IPLBinauralEffect spatial=nullptr;
        bool direct_done=false,done=false;
        ~Voice() { if(spatial)iplBinauralEffectRelease(&spatial);if(direct)iplDirectEffectRelease(&direct); }
    };
    std::map<std::uint64_t,std::unique_ptr<Voice>> voices;
    ~Impl() { voices.clear();if(hrtf)iplHRTFRelease(&hrtf);if(context)iplContextRelease(&context); }
    static void status(IPLerror e) { check(e==IPL_STATUS_SUCCESS,"Audio stream DSP initialization failed."); }
    const AudioPath& path(const std::string& entity) const {
        const auto p=std::find_if(paths.paths.begin(),paths.paths.end(),[&](const auto& v){return v.entity==entity;});check(p!=paths.paths.end(),"Playing voice lost its emitter path.");return *p;
    }
    void synchronize(const AudioSnapshot& next,const std::vector<SoundVoice>& states) {
        if(!same_paths(snapshot,next)) { paths=observe_audio(next);snapshot=next;++stats.path_updates; }
        for(const auto& state:states) {
            if(auto found=voices.find(state.id);found!=voices.end()) { found->second->state=state;continue; }
            if(state.id<=seen)continue;
            const auto delay=path(state.emitter).propagation_delay_samples;
            if(state.start_tick*audio_tick_frames<start && state.end_sample()!=UINT64_MAX && state.end_sample()+delay<=start)continue;
            check(voices.size()<256,"Audio presentation exceeded 256 active/delayed/tail voices.");
            auto voice=std::make_unique<Voice>();voice->state=state;voice->delay=path(state.emitter).propagation_delay_samples;
            IPLAudioSettings audio{audio_rate,audio_block};IPLDirectEffectSettings direct_settings{1};status(iplDirectEffectCreate(context,&audio,&direct_settings,&voice->direct));
            IPLBinauralEffectSettings spatial_settings{hrtf};status(iplBinauralEffectCreate(context,&audio,&spatial_settings,&voice->spatial));
            voices.emplace(state.id,std::move(voice));++stats.voices_started;
        }
        for(const auto& state:states)seen=std::max(seen,state.id);
    }
    void block(std::span<float> mix) {
        std::fill(mix.begin(),mix.end(),0);
        for(auto& [id,owned]:voices) {
            (void)id;auto& voice=*owned;const auto& state=voice.state;
            const auto begin=state.start_tick*audio_tick_frames+voice.delay;
            const auto emission_end=state.end_sample();const auto end=emission_end==UINT64_MAX ? UINT64_MAX : emission_end+voice.delay;
            if(end<=begin) { voice.done=true;continue; }
            if(cursor+audio_block<=begin)continue;
            std::array<float,audio_block> mono{},filtered{},left{},right{};float* in_channels[]{mono.data()};float* filtered_channels[]{filtered.data()};float* out_channels[]{left.data(),right.data()};
            IPLAudioBuffer in{1,audio_block,in_channels},middle{1,audio_block,filtered_channels},out{2,audio_block,out_channels};
            const auto& p=path(state.emitter);IPLDirectEffectParams params{};
            params.flags=static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION|IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION|IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION|IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
            params.transmissionType=IPL_TRANSMISSIONTYPE_FREQDEPENDENT;params.distanceAttenuation=p.distance_gain;params.occlusion=p.occlusion;std::copy(p.air.begin(),p.air.end(),params.airAbsorption);std::copy(p.transmission.begin(),p.transmission.end(),params.transmission);
            IPLBinauralEffectParams spatial{};spatial.direction={float(p.direction[0]),float(p.direction[1]),float(p.direction[2])};spatial.interpolation=IPL_HRTFINTERPOLATION_BILINEAR;spatial.spatialBlend=1;spatial.hrtf=hrtf;
            if(voice.direct_done)voice.done=iplBinauralEffectGetTail(voice.spatial,&out)==IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
            else {
                if(cursor<end) {
                    const auto& clip=state.sound.clip->samples;
                    for(std::uint64_t k=0;k<audio_block;++k)if(cursor+k>=begin && cursor+k<end)mono[k]=clip[(cursor+k-begin)%clip.size()];
                    iplDirectEffectApply(voice.direct,&params,&in,&middle);
                } else voice.direct_done=iplDirectEffectGetTail(voice.direct,&middle)==IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
                iplBinauralEffectApply(voice.spatial,&spatial,&middle,&out);
            }
            for(std::size_t k=0;k<audio_block;++k)if(cursor+k>=begin) { mix[2*k]+=left[k]*state.sound.gain*state.gain;mix[2*k+1]+=right[k]*state.sound.gain*state.gain; }
        }
        std::erase_if(voices,[](const auto& item){return item.second->done;});++stats.blocks;
    }
#endif
};
AudioStream::AudioStream(std::uint64_t tick,const AudioSnapshot& initial):impl_(std::make_unique<Impl>()) {
    check(audio_available(),"Audio stream is not built.");auto& s=*impl_;s.start=s.cursor=s.committed=tick*audio_tick_frames;s.snapshot=initial;s.paths=observe_audio(initial);s.stats.path_updates=1;
#if POIMA_AUDIO
    IPLContextSettings settings{};settings.version=STEAMAUDIO_VERSION;settings.simdLevel=IPL_SIMDLEVEL_SSE2;Impl::status(iplContextCreate(&settings,&s.context));
    IPLAudioSettings audio{audio_rate,audio_block};IPLHRTFSettings hrtf{};hrtf.type=IPL_HRTFTYPE_DEFAULT;hrtf.volume=1;Impl::status(audio_detail::create_hrtf(s.context,&audio,&hrtf,&s.hrtf));
#endif
}
AudioStream::~AudioStream()=default;
AudioStreamStats AudioStream::stats() const { return impl_->stats; }
std::vector<float> AudioStream::advance(std::uint64_t tick,const AudioSnapshot& snapshot,const std::vector<SoundVoice>& voices,bool finish) {
    auto& s=*impl_;check(!s.finished && tick*audio_tick_frames>=s.committed,"Audio stream cannot rewind or resume after finish.");
    check(tick*audio_tick_frames-s.committed<=audio_tick_frames,"Feed committed audio one simulation tick at a time.");
    s.committed=tick*audio_tick_frames;
#if POIMA_AUDIO
    const auto start=std::chrono::steady_clock::now();s.synchronize(snapshot,voices);
    std::vector<float> result;result.reserve(4*audio_block);std::array<float,2*audio_block> block{};
    while(s.cursor+audio_block<=s.committed || (finish && s.cursor<s.committed)) {
        s.block(block);const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(audio_block,s.committed-s.cursor));result.insert(result.end(),block.begin(),block.begin()+static_cast<std::ptrdiff_t>(2*count));s.cursor+=count;
    }
    for(float v:result) { check(std::isfinite(v),"Nonfinite streamed audio.");s.stats.peak=std::max(s.stats.peak,std::abs(double(v)));if(std::abs(v)>1)++s.stats.over_range_samples; }
    s.stats.frames+=result.size()/2;s.stats.dsp_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();s.finished=finish;return result;
#else
    (void)snapshot;(void)voices;(void)finish;throw std::runtime_error("Audio stream is not built.");
#endif
}
}
