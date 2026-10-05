// SPDX-License-Identifier: Apache-2.0
#include "poima/audio.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
namespace poima {
namespace {
using Json=nlohmann::json;
constexpr std::uint64_t safe_integer=9007199254740991ULL;
constexpr std::size_t save_limit=512*1024;
void check(bool valid,const char* message) { if(!valid)throw std::runtime_error(message); }
void fields(const Json& object,std::initializer_list<const char*> names) {
    check(object.is_object() && object.size()==names.size(),"Invalid sound save object fields.");
    for(const auto* name:names)check(object.contains(name),"Missing sound save field.");
}
std::uint64_t integer(const Json& value,std::uint64_t maximum=safe_integer) {
    check(value.is_number_integer() && value>=0 && value<=maximum,"Invalid sound save integer.");return value.get<std::uint64_t>();
}
std::string text(const Json& value,bool empty=false) {
    check(value.is_string(),"Sound save identity must be text.");const auto result=value.get<std::string>();
    check((empty || !result.empty()) && result.size()<=128 && result.find('\0')==std::string::npos,"Invalid sound save identity.");return result;
}
float gain(const Json& value) {
    check(value.is_number() && std::isfinite(value.get<double>()) && value>=0 && value<=4,"Invalid sound save gain.");return value.get<float>();
}
std::uint64_t sample_index(const Json& value) {
    check(value.is_string(),"Sound sample indices must be decimal strings.");const auto value_text=value.get<std::string>();
    check(!value_text.empty() && value_text.size()<=20 && (value_text.size()==1 || value_text[0]!='0'),"Invalid sound sample index.");
    std::uint64_t result=0;const auto [end,error]=std::from_chars(value_text.data(),value_text.data()+value_text.size(),result);
    check(error==std::errc{} && end==value_text.data()+value_text.size(),"Invalid sound sample index.");return result;
}
using ClipHashes=std::map<const AudioClip*,std::string>;
const std::string& clip_hash(const AudioEmitter& emitter,ClipHashes& hashes) {
    check(emitter.enabled && emitter.clip && !emitter.clip->samples.empty() && emitter.clip->samples.size()<=max_audio_clip_frames,
          "Sound save needs an enabled, loaded bounded AudioEmitter.");
    check(std::isfinite(emitter.gain) && emitter.gain>=0 && emitter.gain<=4,"Trusted sound emitter gain is invalid.");
    if(auto found=hashes.find(emitter.clip.get());found!=hashes.end())return found->second;
    const auto encoded=encode_audio(*emitter.clip);
    return hashes.emplace(emitter.clip.get(),sha256(std::as_bytes(std::span(encoded.data(),encoded.size())))).first->second;
}
struct SavedSound { std::uint64_t next=1;std::vector<SoundVoice> voices; };
SavedSound decode_state(const Json& value,std::uint64_t tick,const std::map<std::string,AudioEmitter>& emitters,ClipHashes& hashes) {
    check(tick<=safe_integer,"Sound save tick exceeds the supported range.");
    fields(value,{"format","version","tick","next_voice_id","voices"});
    check(value.at("format")=="poima.sound-state" && integer(value.at("version"))==1,"Unsupported sound save format/version.");
    check(integer(value.at("tick"))==tick,"Sound save tick differs from the runtime checkpoint.");
    SavedSound result;result.next=integer(value.at("next_voice_id"));check(result.next>=1,"Invalid sound voice allocator.");
    const auto& voices=value.at("voices");check(voices.is_array() && voices.size()<=256,"Sound save exceeds 256 retained voices.");
    std::uint64_t previous=0,previous_start=0;
    std::vector<std::pair<std::uint64_t,int>> events;events.reserve(voices.size()*2);
    result.voices.reserve(voices.size());
    for(const auto& item:voices) {
        fields(item,{"id","emitter","asset","clip_sha256","clip_frames","emitter_gain","loop","start_tick","stop_sample","gain"});
        SoundVoice voice;voice.id=integer(item.at("id"));voice.start_tick=integer(item.at("start_tick"));voice.emitter=text(item.at("emitter"));
        check(voice.id>previous && voice.id<result.next,"Sound voice IDs must be unique, increasing and below the allocator.");
        check(voice.start_tick>=previous_start && voice.start_tick<=tick,"Invalid sound voice start chronology.");
        const auto found=emitters.find(voice.emitter);check(found!=emitters.end(),"Sound save references an unknown emitter.");voice.sound=found->second;
        check(text(item.at("asset"),true)==voice.sound.asset,"Sound save clip asset differs from its emitter.");
        check(item.at("clip_sha256").is_string() && item.at("clip_sha256")==clip_hash(voice.sound,hashes),"Sound save clip content differs from its emitter.");
        check(integer(item.at("clip_frames"),max_audio_clip_frames)==voice.sound.clip->samples.size(),"Sound save clip frame count differs.");
        (void)gain(item.at("emitter_gain"));
        check(item.at("emitter_gain").get<double>()==static_cast<double>(voice.sound.gain),"Sound save emitter gain differs.");
        check(item.at("loop").is_boolean() && item.at("loop").get<bool>()==voice.sound.loop,"Sound save loop setting differs.");
        voice.gain=gain(item.at("gain"));
        if(!item.at("stop_sample").is_null()) {
            const auto stopped=sample_index(item.at("stop_sample"));
            check(stopped>=voice.start_tick*audio_tick_frames && stopped<=tick*audio_tick_frames && stopped%audio_tick_frames==0,"Invalid sound voice stop chronology.");
            voice.stop_sample=stopped;
        }
        const auto start=voice.start_tick*audio_tick_frames,end=voice.end_sample();
        if(end>start) { events.emplace_back(start,1);events.emplace_back(end,-1); }
        result.voices.push_back(std::move(voice));previous=result.voices.back().id;previous_start=result.voices.back().start_tick;
    }
    check(result.voices.empty() ? result.next==1 : previous==result.next-1,"Sound allocator does not follow its retained newest voice.");
    if(result.voices.size()<256) {
        check(result.next==result.voices.size()+1,"Unpruned sound history has missing voice IDs.");
        for(std::size_t i=0;i<result.voices.size();++i)check(result.voices[i].id==i+1,"Unpruned sound history has missing voice IDs.");
    }
    // End before start at the same sample: a replacement may begin precisely
    // when an older voice stops, including zero-length play/stop commands.
    std::sort(events.begin(),events.end());int active=0;
    for(const auto& [sample,delta]:events) { (void)sample;active+=delta;check(active>=0 && active<=64,"Sound save exceeds 64 simultaneous voices."); }
    return result;
}
}
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
std::string SoundState::save_state(std::uint64_t tick) const {
    check(tick<=safe_integer,"Sound save tick exceeds the supported range.");
    Json value={{"format","poima.sound-state"},{"version",1},{"tick",tick},{"next_voice_id",next_},{"voices",Json::array()}};
    std::map<std::string,AudioEmitter> emitters;ClipHashes hashes;
    for(const auto& voice:voices_) {
        const auto& hash=clip_hash(voice.sound,hashes);emitters.emplace(voice.emitter,voice.sound);
        value["voices"].push_back({{"id",voice.id},{"emitter",voice.emitter},{"asset",voice.sound.asset},{"clip_sha256",hash},
            {"clip_frames",voice.sound.clip->samples.size()},{"emitter_gain",voice.sound.gain},{"loop",voice.sound.loop},
            {"start_tick",voice.start_tick},{"stop_sample",voice.stop_sample ? Json(std::to_string(*voice.stop_sample)) : Json(nullptr)},{"gain",voice.gain}});
    }
    (void)decode_state(value,tick,emitters,hashes);
    auto encoded=value.dump();check(encoded.size()<=save_limit,"Sound save exceeds 512 KiB.");return encoded;
}
void SoundState::load_state(const std::string& state,std::uint64_t tick,const std::map<std::string,AudioEmitter>& emitters) {
    check(state.size()<=save_limit,"Sound save exceeds 512 KiB.");std::vector<std::set<std::string>> keys;
    auto value=Json::parse(state,[&](int depth,Json::parse_event_t event,Json& item) {
        check(depth<=16,"Sound save JSON nesting exceeds 16 levels.");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key)check(keys.back().insert(item.get<std::string>()).second,"Duplicate sound save JSON field.");
        if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    });
    ClipHashes hashes;auto candidate=decode_state(value,tick,emitters,hashes);
    voices_.swap(candidate.voices);next_=candidate.next;
}
}
