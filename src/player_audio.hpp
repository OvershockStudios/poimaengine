// SPDX-License-Identifier: Apache-2.0
#pragma once
// Initial main-thread presentation adapter. Gameplay owns only logical voices;
// SDL's device thread receives copied PCM and never calls the world or DSP.
#include "poima/player.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <stdexcept>
namespace poima {
class PlayerAudio {
    AudioStream mixer_;
    SDL_AudioStream* device_=nullptr;
    PlayerAudioReport report_;
    bool paused_=true,started_=false;
    static void check(bool value) { if(!value)throw std::runtime_error(SDL_GetError()); }
    int queued() const { const int bytes=SDL_GetAudioStreamQueued(device_);check(bytes>=0);return bytes; }
    int available() const { const int bytes=SDL_GetAudioStreamAvailable(device_);check(bytes>=0);return bytes; }
    void submit(const std::vector<float>& pcm) {
        if(pcm.empty())return;
        auto bytes=queued();if(started_ && !paused_ && bytes==0)++report_.empty_queue_observations;
        const auto before=SDL_GetTicksNS();
        // Replay may produce ticks faster than wall time. Bounded backpressure
        // keeps queued audio under 120 ms instead of retaining the whole replay.
        while(bytes>4800*8) {
            if(SDL_GetTicksNS()-before>500000000)throw std::runtime_error("Audio output queue did not drain within 500 ms.");
            SDL_Delay(1);bytes=queued();
        }
        report_.backpressure_ms+=double(SDL_GetTicksNS()-before)/1e6;
        check(SDL_PutAudioStreamData(device_,pcm.data(),static_cast<int>(pcm.size()*sizeof(float))));
        report_.submitted_frames+=pcm.size()/2;
        report_.max_queued_frames=std::max(report_.max_queued_frames,static_cast<std::uint64_t>(queued()/8));
        if(!started_ && queued()>=2048*8) { check(SDL_ResumeAudioStreamDevice(device_));started_=true;paused_=false; }
    }
public:
    PlayerAudio(Runtime& runtime,const std::string& listener):mixer_(runtime.inspect().tick,runtime.audio_snapshot(listener)) {
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
    void advance(Runtime& runtime,const std::string& listener) {
        submit(mixer_.advance(runtime.inspect().tick,runtime.audio_snapshot(listener),runtime.sound_state().voices()));
    }
    void finish(Runtime& runtime,const std::string& listener) {
        active(true);
        submit(mixer_.advance(runtime.inspect().tick,runtime.audio_snapshot(listener),runtime.sound_state().voices(),true));
        check(SDL_FlushAudioStream(device_));check(SDL_ResumeAudioStreamDevice(device_));paused_=false;
        const auto start=SDL_GetTicksNS();
        while(queued()>0 || available()>0) {
            if(SDL_GetTicksNS()-start>2000000000)throw std::runtime_error("Audio stream did not drain within two seconds.");
            SDL_Delay(2);
        }
        report_.stream_drained=true;
    }
    PlayerAudioReport report() const { auto result=report_;result.stream=mixer_.stats();return result; }
};
}
