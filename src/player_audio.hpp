// SPDX-License-Identifier: Apache-2.0
#pragma once
// Initial main-thread presentation adapter. Gameplay owns only logical voices;
// SDL's device thread receives copied PCM and never calls the world or DSP.
#include "poima/player.hpp"
#include "poima/profiler.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <stdexcept>
namespace poima {
class PlayerAudio {
    std::unique_ptr<AudioStream> mixer_;
    AudioStreamStats retired_;
    SDL_AudioStream* device_=nullptr;
    PlayerAudioReport report_;
    bool paused_=true,started_=false;
    static void check(bool value) { if(!value)throw std::runtime_error(SDL_GetError()); }
    int queued() const { const int bytes=SDL_GetAudioStreamQueued(device_);check(bytes>=0);return bytes; }
    int available() const { const int bytes=SDL_GetAudioStreamAvailable(device_);check(bytes>=0);return bytes; }
    void submit(const std::vector<float>& pcm) {
        if(pcm.empty())return;
        profiling::Scope submit_scope("audio.submit");
        auto bytes=queued();if(started_ && !paused_ && bytes==0)++report_.empty_queue_observations;
        const auto before=SDL_GetTicksNS();
        // Replay may produce ticks faster than wall time. Bounded backpressure
        // keeps queued audio under 120 ms instead of retaining the whole replay.
        {
        profiling::Scope queue_scope("audio.queue_wait");
        while(bytes>4800*8) {
            if(SDL_GetTicksNS()-before>500000000)throw std::runtime_error("Audio output queue did not drain within 500 ms.");
            SDL_Delay(1);bytes=queued();
        }
        }
        report_.backpressure_ms+=double(SDL_GetTicksNS()-before)/1e6;
        check(SDL_PutAudioStreamData(device_,pcm.data(),static_cast<int>(pcm.size()*sizeof(float))));
        report_.submitted_frames+=pcm.size()/2;
        report_.max_queued_frames=std::max(report_.max_queued_frames,static_cast<std::uint64_t>(queued()/8));
        profiling::counter("audio.submitted_frames",report_.submitted_frames);
        profiling::counter("audio.queued_bytes_before_submit",static_cast<std::uint64_t>(bytes));
        if(!started_ && queued()>=2048*8) { check(SDL_ResumeAudioStreamDevice(device_));started_=true;paused_=false; }
    }
public:
    explicit PlayerAudio(const PlayerAudioState& state):mixer_(std::make_unique<AudioStream>(state.tick,state.snapshot)) {
        check(SDL_InitSubSystem(SDL_INIT_AUDIO));SDL_AudioSpec spec{SDL_AUDIO_F32,2,static_cast<int>(audio_rate)};
        device_=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,nullptr,nullptr);
        if(!device_) { const std::string error=SDL_GetError();SDL_QuitSubSystem(SDL_INIT_AUDIO);throw std::runtime_error(error); }
        report_.enabled=true;const auto* driver=SDL_GetCurrentAudioDriver();report_.driver=driver ? driver : "unknown";
    }
    ~PlayerAudio() { if(device_)SDL_DestroyAudioStream(device_);SDL_QuitSubSystem(SDL_INIT_AUDIO); }
    void active(bool active) {
        if(!started_)return;
        if(active && paused_) { check(SDL_ResumeAudioStreamDevice(device_));paused_=false; }
        else if(!active && !paused_) { check(SDL_PauseAudioStreamDevice(device_));paused_=true; }
    }
    void discard_pending() {
        check(SDL_PauseAudioStreamDevice(device_));paused_=true;
        check(SDL_ClearAudioStream(device_));started_=false;
    }
    void reset(const PlayerAudioState& state) {
        profiling::Scope reset_scope("audio.timeline_reset",static_cast<std::int64_t>(state.tick));
        // Never play or drain old-world PCM after a load. The audio device and
        // Vulkan window remain alive; only the timeline's DSP is reconstructed.
        discard_pending();
        auto replacement=std::make_unique<AudioStream>(state.tick,state.snapshot);
        const auto previous=mixer_->stats();
        retired_.frames+=previous.frames;retired_.blocks+=previous.blocks;
        retired_.voices_started+=previous.voices_started;retired_.path_updates+=previous.path_updates;
        retired_.peak=std::max(retired_.peak,previous.peak);retired_.dsp_ms+=previous.dsp_ms;
        retired_.over_range_samples+=previous.over_range_samples;
        mixer_=std::move(replacement);++report_.timeline_resets;report_.stream_drained=false;
    }
    void advance(const PlayerAudioState& state) {
        profiling::Scope advance_scope("audio.advance",static_cast<std::int64_t>(state.tick));
        const auto pcm=[&] { profiling::Scope dsp_scope("audio.dsp");return mixer_->advance(state.tick,state.snapshot,state.voices); }();
        submit(pcm);
    }
    void finish(const PlayerAudioState& state) {
        profiling::Scope finish_scope("audio.finish",static_cast<std::int64_t>(state.tick));
        active(true);
        const auto pcm=[&] { profiling::Scope dsp_scope("audio.dsp");return mixer_->advance(state.tick,state.snapshot,state.voices,true); }();
        submit(pcm);
        check(SDL_FlushAudioStream(device_));check(SDL_ResumeAudioStreamDevice(device_));paused_=false;
        const auto start=SDL_GetTicksNS();
        profiling::Scope drain_scope("audio.drain_wait");
        while(queued()>0 || available()>0) {
            if(SDL_GetTicksNS()-start>2000000000)throw std::runtime_error("Audio stream did not drain within two seconds.");
            SDL_Delay(2);
        }
        report_.stream_drained=true;
    }
    PlayerAudioReport report() const {
        auto result=report_;result.stream=mixer_->stats();
        result.stream.frames+=retired_.frames;result.stream.blocks+=retired_.blocks;
        result.stream.voices_started+=retired_.voices_started;result.stream.path_updates+=retired_.path_updates;
        result.stream.peak=std::max(result.stream.peak,retired_.peak);result.stream.dsp_ms+=retired_.dsp_ms;
        result.stream.over_range_samples+=retired_.over_range_samples;return result;
    }
};
}
