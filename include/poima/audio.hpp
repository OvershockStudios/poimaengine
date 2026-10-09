// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/scene.hpp"
#include <span>
#include <map>

namespace poima {
inline constexpr std::uint32_t audio_rate=48000, audio_block=512, max_audio_clip_frames=60*audio_rate;
inline constexpr std::size_t max_audio_sources=64, max_audio_triangles=131072;
struct AudioClip { std::vector<float> samples; }; // Mono 48 kHz, finite [-1,1].
std::shared_ptr<const AudioClip> decode_wave(std::span<const std::byte> bytes);
std::string encode_audio(const AudioClip& clip);
std::shared_ptr<const AudioClip> decode_audio(const std::string& bytes);
std::string audio_wave(std::span<const float> interleaved,std::uint16_t channels);
struct AcousticMaterial {
    std::array<float,3> absorption{.1f,.1f,.1f}, transmission{.1f,.05f,.01f};
    float scattering=.5f;
    bool enabled=true;
};
void validate_acoustic_material(const AcousticMaterial& material);
struct AudioEmitter {
    std::string asset;
    std::shared_ptr<const AudioClip> clip;
    float gain=1;
    bool loop=false,enabled=true;
};
struct AcousticGeometry {
    std::string entity;
    Matrix4 world=identity_matrix();
    std::shared_ptr<const MeshAsset> mesh; // Null selects a unit box.
    AcousticMaterial material;
};
struct AudioSource { std::string entity;AudioEmitter emitter;Matrix4 world; };
struct AudioSnapshot {
    Matrix4 listener=identity_matrix();
    std::vector<AcousticGeometry> geometry;
    std::vector<AudioSource> sources;
};
struct AudioPath {
    std::string entity,asset;
    std::array<double,3> source{},listener{},direction{};
    double distance=0;
    std::uint32_t propagation_delay_samples=0;
    float distance_gain=1,occlusion=1;
    std::array<float,3> air{1,1,1},transmission{1,1,1};
};
struct AudioReport {
    std::vector<AudioPath> paths;
    std::vector<float> samples; // Stereo 48 kHz, unclipped float mix.
    std::size_t triangles=0;
    double scene_ms=0,simulation_ms=0,dsp_ms=0,peak=0,rms=0;
    std::size_t over_range_samples=0;
};
bool audio_available();
// Synchronous frozen-snapshot observation. No device, simulation clock changes,
// source cursor changes or background work. frames=0 requests paths only.
AudioReport observe_audio(const AudioSnapshot& snapshot,std::uint32_t frames=0);
inline constexpr std::uint64_t audio_tick_frames=audio_rate/60;
struct SoundCommand { bool stop=false;std::string emitter;std::uint64_t voice=0;float gain=1; };
struct SoundVoice {
    std::uint64_t id=0,start_tick=0;
    std::optional<std::uint64_t> stop_sample;
    std::string emitter;
    AudioEmitter sound;
    float gain=1;
    std::uint64_t end_sample() const;
    bool emitting(std::uint64_t sample) const;
};
// Authoritative, copyable logical voice state. DSP handles never enter gameplay
// rollback storage. Recent finished records permit presentation to observe
// short events even when a whole simulation tick exceeds the clip duration.
class SoundState {
    std::uint64_t next_=1;
    std::vector<SoundVoice> voices_;
public:
    std::uint64_t play(const std::string& emitter,const AudioEmitter& sound,std::uint64_t tick,float gain);
    void stop(std::uint64_t voice,std::uint64_t tick);
    // Removing emitter membership retires its logical records immediately;
    // surviving voice identities and allocation history remain unchanged.
    void retire_emitters(std::span<const std::string> emitters) noexcept;
    const std::vector<SoundVoice>& voices() const { return voices_; }
    std::uint64_t next_id() const { return next_; }
    // Portable logical state only; presentation must reset its DSP after restore.
    std::string save_state(std::uint64_t tick) const;
    void load_state(const std::string& state,std::uint64_t tick,const std::map<std::string,AudioEmitter>& emitters);
};
struct AudioStreamStats {
    std::uint64_t frames=0,blocks=0,voices_started=0,path_updates=0;
    double peak=0,dsp_ms=0;
    std::uint64_t over_range_samples=0;
};
// Persistent native direct/HRTF DSP, fed only after committed simulation ticks.
// Full 512-frame blocks are rendered up to (never beyond) committed time.
// finish renders/trims the final partial block. No audio device is required.
class AudioStream {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    AudioStream(std::uint64_t start_tick,const AudioSnapshot& initial);
    ~AudioStream();
    AudioStream(const AudioStream&)=delete;AudioStream& operator=(const AudioStream&)=delete;
    std::vector<float> advance(std::uint64_t tick,const AudioSnapshot& snapshot,const std::vector<SoundVoice>& voices,bool finish=false);
    AudioStreamStats stats() const;
};
}
