// SPDX-License-Identifier: Apache-2.0
#include "poima/audio.hpp"
#include "audio_hrtf.hpp"
#include "audio_room.hpp"
#include "audio_water.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <stdexcept>
#if POIMA_AUDIO
#include <phonon.h>
#include "audio_reflections.hpp"
#endif
namespace poima {
namespace {
void check(bool ok,const char* text) { if(!ok)throw std::runtime_error(text); }
[[maybe_unused]] bool same_paths(const AudioSnapshot& a,const AudioSnapshot& b) {
    if(a.listener!=b.listener || a.early_reflections!=b.early_reflections ||
       a.geometry.size()!=b.geometry.size() || a.sources.size()!=b.sources.size())return false;
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
    // At most one 512-frame block is pending at a tick of 800 frames. It
    // may span the previous and current committed listener observations.
    std::optional<audio_detail::WaterFilter> water_filter;
    std::uint64_t water_mix_boundary=0;
    double previous_water_blend=0, water_blend=0;
    std::unique_ptr<audio_detail::RoomReverb> room_reverb;
    std::optional<AudioRoomSelection> previous_room, room;
    std::uint64_t room_boundary=0;
    void sample_room(const std::optional<AudioRoomSelection>& next) {
        if (!room && !next && !room_reverb) return;
        previous_room=room;
        room=next;
        room_boundary=committed;
        if (bool(room)!=bool(previous_room) || (room && room->entity!=previous_room->entity))
            ++stats.room_transitions;
        if (room) stats.room_reverb_used=true;
    }
    void filter_room(std::span<float> samples) {
        if (!room_reverb && !previous_room && !room) return;
        const auto previous_frames=room_boundary>cursor ?
            std::min<std::uint64_t>(room_boundary-cursor,samples.size()/2) : 0;
        const auto apply=[&](std::span<float> part,const std::optional<AudioRoomSelection>& selection) {
            if (part.empty()) return;
            if (!room_reverb && selection && selection->settings.wet_gain>0)
                room_reverb=std::make_unique<audio_detail::RoomReverb>(selection->settings,true);
            if (room_reverb) {
                room_reverb->target(selection ? &selection->settings : nullptr);
                room_reverb->process(part);
            }
        };
        const auto split=static_cast<std::size_t>(2*previous_frames);
        apply(samples.first(split),previous_room);
        apply(samples.subspan(split),room);
    }
    struct FlowEnvelope {
        // A rendering block can contain the end of the previous simulation tick.
        std::array<float,3> targets;
        std::uint64_t tick;

        float gain(std::uint64_t sample) const {
            const auto end=tick*audio_tick_frames;
            if (sample>=end) return targets[2];
            const auto distance=end-sample-1;
            const bool previous=distance>=audio_tick_frames;
            const auto offset=previous ? distance-audio_tick_frames : distance;
            const float weight=1.f-static_cast<float>(offset)/static_cast<float>(audio_tick_frames);
            return previous ? std::lerp(targets[0],targets[1],weight) :
                              std::lerp(targets[1],targets[2],weight);
        }
    };
    std::map<std::string,FlowEnvelope> flow_envelopes;
    void sample_flow(const AudioSnapshot& next,std::uint64_t tick) {
        for (const auto& source:next.sources) {
            if (!source.water_flow) continue;
            const auto gain=source.flow_gain();
            auto [entry,inserted]=flow_envelopes.try_emplace(source.entity,FlowEnvelope{{gain,gain,gain},tick});
            if (!inserted) {
                auto& envelope=entry->second;
                envelope.targets={envelope.targets[1],envelope.targets[2],gain};
                envelope.tick=tick;
            }
        }
        std::erase_if(flow_envelopes,[tick](const auto& entry) { return entry.second.tick!=tick; });
    }
    IPLContext context=nullptr;IPLHRTF hrtf=nullptr;
    struct DspPath {
        std::uint64_t delay=0;
        IPLDirectEffect direct=nullptr;IPLBinauralEffect spatial=nullptr;
        bool direct_done=false,done=false;
        ~DspPath() { if(spatial)iplBinauralEffectRelease(&spatial);if(direct)iplDirectEffectRelease(&direct); }
    };
    struct ReflectedVoice {
        AudioReflection path;
        DspPath dsp;
        float audibility=1;
    };
    struct Voice {
        SoundVoice state;
        DspPath direct;
        std::unique_ptr<ReflectedVoice> reflected;
    };
    std::map<std::uint64_t,std::unique_ptr<Voice>> voices;
    ~Impl() { voices.clear();if(hrtf)iplHRTFRelease(&hrtf);if(context)iplContextRelease(&context); }
    static void status(IPLerror e) { check(e==IPL_STATUS_SUCCESS,"Audio stream DSP initialization failed."); }
    void setup(DspPath& dsp,std::uint32_t delay) const {
        dsp.delay=delay;
        IPLAudioSettings audio{audio_rate,audio_block};
        IPLDirectEffectSettings direct_settings{1};status(iplDirectEffectCreate(context,&audio,&direct_settings,&dsp.direct));
        IPLBinauralEffectSettings spatial_settings{hrtf};status(iplBinauralEffectCreate(context,&audio,&spatial_settings,&dsp.spatial));
    }
    std::unique_ptr<ReflectedVoice> make_reflection(const AudioReflection& path,float gain) const {
        auto reflected=std::make_unique<ReflectedVoice>();
        reflected->path=path;
        reflected->audibility=gain;
        setup(reflected->dsp,path.propagation_delay_samples);
        return reflected;
    }
    const AudioPath& path(const std::string& entity) const {
        const auto p=std::find_if(paths.paths.begin(),paths.paths.end(),[&](const auto& v){return v.entity==entity;});check(p!=paths.paths.end(),"Playing voice lost its emitter path.");return *p;
    }
    void synchronize(const AudioSnapshot& next,const std::vector<SoundVoice>& states) {
        if(!same_paths(snapshot,next)) { paths=observe_audio(next);snapshot=next;++stats.path_updates; }
        // A committed instance removal retires emitter membership. Its delayed
        // and tail DSP voices cannot keep borrowing a now absent propagation
        // path; other emitters and their presentation state remain untouched.
        std::erase_if(voices,[&](const auto& item) {
            return std::none_of(paths.paths.begin(),paths.paths.end(),[&](const AudioPath& value) {
                return value.entity==item.second->state.emitter;
            });
        });
        for(const auto& state:states) {
            if(auto found=voices.find(state.id);found!=voices.end()) { found->second->state=state;continue; }
            if(state.id<=seen)continue;
            const auto& current=path(state.emitter);
            const auto delay=current.reflection ? std::max(current.propagation_delay_samples,current.reflection->propagation_delay_samples) :
                current.propagation_delay_samples;
            if(state.start_tick*audio_tick_frames<start && state.end_sample()!=UINT64_MAX && state.end_sample()+delay<=start)continue;
            check(voices.size()<256,"Audio presentation exceeded 256 active/delayed/tail voices.");
            auto voice=std::make_unique<Voice>();voice->state=state;
            setup(voice->direct,current.propagation_delay_samples);
            if(current.reflection)voice->reflected=make_reflection(*current.reflection,1);
            voices.emplace(state.id,std::move(voice));++stats.voices_started;
        }
        for(const auto& state:states)seen=std::max(seen,state.id);
    }
    void render_reflection(std::span<float> mix,const SoundVoice& state,ReflectedVoice& reflection,
                           const FlowEnvelope* flow,float target) {
        render_path(mix,state,reflection.dsp,reflection.path.direction,
                    audio_detail::reflection_filter(reflection.path),flow,reflection.audibility,target);
        reflection.audibility=target;
    }
    void update_reflection(std::span<float> mix,Voice& voice,const std::optional<AudioReflection>& selected,
                           const FlowEnvelope* flow) {
        auto& current=voice.reflected;
        const auto same_face=current && selected && selected->entity==current->path.entity &&
            selected->face==current->path.face;
        if(same_face) {
            current->path=*selected;
            render_reflection(mix,voice.state,*current,flow,1);
            return;
        }
        // An audible replacement needs source samples at its own delayed
        // cursor. A new branch cannot restart the clip or revive an ended voice.
        const auto end=voice.state.end_sample();
        if(selected && (end==UINT64_MAX || cursor<end+selected->propagation_delay_samples)) {
            auto replacement=make_reflection(*selected,0);
            if(current && current->audibility>0)render_reflection(mix,voice.state,*current,flow,0);
            current=std::move(replacement);
            render_reflection(mix,voice.state,*current,flow,1);
            ++stats.reflection_handoffs;
        } else if(current) {
            // Advance the muted branch in case this face becomes visible
            // again. This also permits its delayed tail to retire naturally.
            render_reflection(mix,voice.state,*current,flow,0);
        }
    }
    void render_path(std::span<float> mix,const SoundVoice& state,DspPath& dsp,const std::array<double,3>& direction,
                     IPLDirectEffectParams params,const FlowEnvelope* flow,float start_audibility=1,float end_audibility=1) {
        if(dsp.done)return;
        const auto begin=state.start_tick*audio_tick_frames+dsp.delay;
        const auto emission_end=state.end_sample();
        const auto end=emission_end==UINT64_MAX ? UINT64_MAX : emission_end+dsp.delay;
        if(end<=begin) { dsp.done=true;return; }
        if(cursor+audio_block<=begin)return;
        std::array<float,audio_block> mono{},filtered{},left{},right{};
        float* in_channels[]{mono.data()};float* filtered_channels[]{filtered.data()};float* out_channels[]{left.data(),right.data()};
        IPLAudioBuffer in{1,audio_block,in_channels},middle{1,audio_block,filtered_channels},out{2,audio_block,out_channels};
        IPLBinauralEffectParams spatial{};
        spatial.direction={float(direction[0]),float(direction[1]),float(direction[2])};
        spatial.interpolation=IPL_HRTFINTERPOLATION_BILINEAR;spatial.spatialBlend=1;spatial.hrtf=hrtf;
        if(dsp.direct_done)dsp.done=iplBinauralEffectGetTail(dsp.spatial,&out)==IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
        else {
            if(cursor<end) {
                const auto& clip=state.sound.clip->samples;
                for(std::uint64_t k=0;k<audio_block;++k)if(cursor+k>=begin && cursor+k<end)mono[k]=clip[(cursor+k-begin)%clip.size()];
                iplDirectEffectApply(dsp.direct,&params,&in,&middle);
            } else dsp.direct_done=iplDirectEffectGetTail(dsp.direct,&middle)==IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
            iplBinauralEffectApply(dsp.spatial,&spatial,&middle,&out);
        }
        const float bus_gain=state.sound.bus_gain();
        // Keep filter/tail advancement and retirement identical for muted
        // voices, without touching the output assigned to other sources.
        if(bus_gain==0)return;
        for(std::size_t k=0;k<audio_block;++k)if(cursor+k>=begin) {
            float audibility=std::lerp(start_audibility,end_audibility,float(k+1)/audio_block);
            if(flow) {
                const float gain=flow->gain(cursor+k);
                if(gain==0)continue;
                audibility*=gain;
            }
            if(bus_gain==1) {
                mix[2*k]+=left[k]*state.sound.gain*state.gain*audibility;
                mix[2*k+1]+=right[k]*state.sound.gain*state.gain*audibility;
            } else {
                mix[2*k]+=left[k]*state.sound.gain*state.gain*audibility*bus_gain;
                mix[2*k+1]+=right[k]*state.sound.gain*state.gain*audibility*bus_gain;
            }
        }
    }
    void block(std::span<float> mix) {
        std::fill(mix.begin(),mix.end(),0);
        for(auto& [id,owned]:voices) {
            (void)id;auto& voice=*owned;
            const auto& current=path(voice.state.emitter);
            IPLDirectEffectParams params{};
            params.flags=static_cast<IPLDirectEffectFlags>(IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION|IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION|IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION|IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
            params.transmissionType=IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
            params.distanceAttenuation=current.distance_gain;params.occlusion=current.occlusion;
            std::copy(current.air.begin(),current.air.end(),params.airAbsorption);
            std::copy(current.transmission.begin(),current.transmission.end(),params.transmission);
            const FlowEnvelope* flow=nullptr;
            if (voice.state.sound.water_flow)
                flow=&flow_envelopes.at(voice.state.emitter);
            render_path(mix,voice.state,voice.direct,current.direction,params,flow);
            update_reflection(mix,voice,current.reflection,flow);
        }
        std::erase_if(voices,[](const auto& item){
            const auto& voice=*item.second;
            return voice.direct.done && (!voice.reflected || voice.reflected->dsp.done);
        });
        ++stats.blocks;
    }
    void filter_water(std::span<float> samples) {
        if (!water_filter) return;
        const auto previous_frames = water_mix_boundary > cursor ?
            std::min<std::uint64_t>(water_mix_boundary - cursor, samples.size() / 2) : 0;
        const auto split = static_cast<std::size_t>(previous_frames * 2);
        water_filter->process(samples.first(split), previous_water_blend);
        water_filter->process(samples.subspan(split), water_blend);
    }
#endif
};
AudioStream::AudioStream(std::uint64_t tick,const AudioSnapshot& initial):impl_(std::make_unique<Impl>()) {
    check(audio_available(),"Audio stream is not built.");auto& s=*impl_;s.start=s.cursor=s.committed=tick*audio_tick_frames;s.snapshot=initial;s.paths=observe_audio(initial);s.stats.path_updates=1;
#if POIMA_AUDIO
    s.sample_flow(initial,tick);
    s.room=initial.room;
    if (s.room) {
        s.stats.room_reverb_used=true;
        if (s.room->settings.wet_gain>0)
            s.room_reverb=std::make_unique<audio_detail::RoomReverb>(s.room->settings);
    }
    if (initial.water_medium) {
        s.water_filter.emplace(*initial.water_medium);
        s.previous_water_blend=s.water_blend=initial.water_medium->blend;
    }
    IPLContextSettings settings{};settings.version=STEAMAUDIO_VERSION;settings.simdLevel=IPL_SIMDLEVEL_SSE2;Impl::status(iplContextCreate(&settings,&s.context));
    IPLAudioSettings audio{audio_rate,audio_block};IPLHRTFSettings hrtf{};hrtf.type=IPL_HRTFTYPE_DEFAULT;hrtf.volume=1;Impl::status(audio_detail::create_hrtf(s.context,&audio,&hrtf,&s.hrtf));
#endif
}
AudioStream::~AudioStream()=default;
AudioStreamStats AudioStream::stats() const { return impl_->stats; }
std::vector<float> AudioStream::advance(std::uint64_t tick,const AudioSnapshot& snapshot,const std::vector<SoundVoice>& voices,bool finish) {
    auto& s=*impl_;check(!s.finished && tick*audio_tick_frames>=s.committed,"Audio stream cannot rewind or resume after finish.");
    check(tick*audio_tick_frames-s.committed<=audio_tick_frames,"Feed committed audio one simulation tick at a time.");
#if POIMA_AUDIO
    if (tick * audio_tick_frames > s.committed) {
        s.sample_flow(snapshot,tick);
        s.sample_room(snapshot.room);
    }
    if (s.water_filter && tick * audio_tick_frames > s.committed) {
        s.water_mix_boundary=s.committed;
        s.previous_water_blend=s.water_blend;
        s.water_blend=snapshot.water_medium ? snapshot.water_medium->blend : 0;
    }
#endif
    s.committed=tick*audio_tick_frames;
#if POIMA_AUDIO
    const auto start=std::chrono::steady_clock::now();s.synchronize(snapshot,voices);
    std::vector<float> result;result.reserve(4*audio_block);std::array<float,2*audio_block> block{};
    while(s.cursor+audio_block<=s.committed || (finish && s.cursor<s.committed)) {
        s.block(block);
        const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(audio_block,s.committed-s.cursor));
        const auto committed_samples = std::span(block).first(count * 2);
        s.filter_room(committed_samples);
        s.filter_water(committed_samples);
        result.insert(result.end(),committed_samples.begin(),committed_samples.end());
        s.cursor+=count;
    }
    for(float v:result) { check(std::isfinite(v),"Nonfinite streamed audio.");s.stats.peak=std::max(s.stats.peak,std::abs(double(v)));if(std::abs(v)>1)++s.stats.over_range_samples; }
    s.stats.frames+=result.size()/2;s.stats.dsp_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();s.finished=finish;return result;
#else
    (void)snapshot;(void)voices;(void)finish;throw std::runtime_error("Audio stream is not built.");
#endif
}
}
