// SPDX-License-Identifier: Apache-2.0
#include "poima/audio.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
using namespace poima;
using Json=nlohmann::json;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
AudioEmitter emitter(bool loop=false) {
    auto clip=std::make_shared<AudioClip>();clip->samples.assign(1600,.25f);
    return {"test-audio",clip,.75f,loop,true};
}
void same(const SoundState& a,const SoundState& b,std::uint64_t tick) {
    check(a.save_state(tick)==b.save_state(tick),"Logical voice state did not round-trip exactly.");
    check(a.next_id()==b.next_id(),"Voice allocator changed.");
}
}
int main() {
    try {
        const auto short_sound=emitter(),loop_sound=emitter(true);
        const std::map<std::string,AudioEmitter> emitters{{"short",short_sound},{"loop",loop_sound}};
        SoundState empty,restored;restored.load_state(empty.save_state(0),0,{});same(empty,restored,0);
        SoundState source;
        check(source.play("short",short_sound,1,.5f)==1,"First voice ID changed.");
        source.play("loop",loop_sound,2,1.5f);source.play("short",short_sound,3,0);
        source.stop(2,4);source.play("loop",loop_sound,4,1);source.stop(3,4);
        const auto saved=source.save_state(5);restored.load_state(saved,5,emitters);same(source,restored,5);
        check(restored.voices()[0].sound.clip==short_sound.clip && restored.voices()[1].sound.clip==loop_sound.clip,
              "Restore did not resolve clips through trusted emitters.");
        check(!restored.voices()[0].emitting(3*audio_tick_frames) && restored.voices()[3].emitting(5*audio_tick_frames),
              "Terminal or looping cursor boundary changed.");
        source.stop(4,6);restored.stop(4,6);
        check(source.play("short",short_sound,7,1)==restored.play("short",short_sound,7,1),"Continuation allocated a different ID.");
        same(source,restored,8);

        // Emitter destruction retires records, never the allocator. Empty,
        // missing-newest and middle-hole histories must round-trip without
        // requiring a destroyed emitter or reusing any previous handle.
        const std::array<std::string,1> retire_short{"short"},retire_loop{"loop"},retire_middle{"middle"};
        SoundState sole,sole_restored;
        check(sole.play("short",short_sound,1,1)==1,"Sole retirement fixture ID differs.");
        sole.retire_emitters(retire_short);
        check(sole.voices().empty() && sole.next_id()==2,"Sole retirement recycled the voice allocator.");
        sole_restored.load_state(sole.save_state(2),2,{});same(sole,sole_restored,2);
        bool retired_unknown=false;try { sole_restored.stop(1,2); }catch(const std::exception&) { retired_unknown=true; }
        check(retired_unknown,"Restored state resurrected a retired sound handle.");
        check(sole.play("loop",loop_sound,3,1)==2 && sole_restored.play("loop",loop_sound,3,1)==2,
              "Empty retired history reused an old voice identity.");
        same(sole,sole_restored,3);

        SoundState newest,newest_restored;
        newest.play("short",short_sound,1,1);newest.play("loop",loop_sound,2,1);
        newest.retire_emitters(retire_loop);
        check(newest.voices().size()==1 && newest.voices()[0].id==1 && newest.next_id()==3,
              "Newest retirement changed surviving voice or allocator.");
        newest_restored.load_state(newest.save_state(2),2,{{"short",short_sound}});same(newest,newest_restored,2);
        check(newest.play("short",short_sound,3,1)==3 && newest_restored.play("short",short_sound,3,1)==3,
              "Newest retirement reused a retired identity on continuation.");
        same(newest,newest_restored,3);

        SoundState middle,middle_restored;
        middle.play("short",short_sound,1,1);middle.play("middle",loop_sound,2,1);middle.play("loop",loop_sound,3,1);
        middle.retire_emitters(retire_middle);
        check(middle.voices().size()==2 && middle.voices()[0].id==1 && middle.voices()[1].id==3 && middle.next_id()==4,
              "Middle retirement failed to preserve its history hole.");
        middle_restored.load_state(middle.save_state(3),3,emitters);same(middle,middle_restored,3);
        check(middle.play("short",short_sound,4,1)==4 && middle_restored.play("short",short_sound,4,1)==4,
              "Middle retirement reused an identity on continuation.");
        same(middle,middle_restored,4);

        // Every malformed load must leave the complete previous state usable.
        const auto baseline=restored.save_state(8);std::size_t rejected=0;
        auto reject_text=[&](const std::string& text,std::uint64_t tick,const auto& trusted) {
            bool failed=false;try { restored.load_state(text,tick,trusted); }catch(const std::exception&) { failed=true; }
            check(failed,"Malformed sound state was accepted.");
            check(restored.save_state(8)==baseline,"Rejected load changed voice state or allocator.");++rejected;
        };
        auto reject=[&](const auto& change) { auto value=Json::parse(saved);change(value);reject_text(value.dump(),5,emitters); };
        reject([](Json& j){j["unexpected"]=true;});
        reject([](Json& j){j["format"]="other";});
        reject([](Json& j){j["version"]=2;});
        reject([](Json& j){j["version"]=true;});
        reject([](Json& j){j["tick"]=5.0;});
        reject([](Json& j){j["tick"]=-1;});
        reject([](Json& j){j["next_voice_id"]=0;});
        reject([](Json& j){j["next_voice_id"]=-1;});
        reject([](Json& j){j["next_voice_id"]=true;});
        reject([](Json& j){j["next_voice_id"]="5";});
        reject([](Json& j){j["next_voice_id"]=5.0;});
        reject([](Json& j){j["next_voice_id"]=9007199254740992ULL;});
        reject([](Json& j){j["next_voice_id"]=4;});
        reject([](Json& j){j["voices"][0]["id"]=0;});
        reject([](Json& j){j["voices"][0]["id"]=-1;});
        reject([](Json& j){j["voices"][0]["id"]=true;});
        reject([](Json& j){j["voices"][0]["id"]=1.0;});
        reject([](Json& j){j["voices"][0]["id"]="1";});
        reject([](Json& j){j["voices"][3]["id"]=5;});
        reject([](Json& j){j["voices"][3]["id"]=9007199254740992ULL;});
        reject([](Json& j){j["voices"][1]["id"]=1;});
        reject([](Json& j){j["voices"][1]["id"]=3;j["voices"][2]["id"]=2;});
        reject([](Json& j){j["voices"]=nullptr;});
        reject([](Json& j){
            const auto voice=j["voices"][0];j["voices"]=Json::array();j["next_voice_id"]=258;
            for(unsigned i=1;i<=257;++i) { auto item=voice;item["id"]=i;j["voices"].push_back(std::move(item)); }
        });
        reject([](Json& j){j["voices"][3]["start_tick"]=6;});
        reject([](Json& j){j["voices"][2]["start_tick"]=0;});
        reject([](Json& j){j["voices"][1]["stop_sample"]="800";});
        reject([](Json& j){j["voices"][1]["stop_sample"]="4800";});
        reject([](Json& j){j["voices"][1]["stop_sample"]="3201";});
        reject([](Json& j){j["voices"][1]["stop_sample"]="03200";});
        reject([](Json& j){j["voices"][1]["stop_sample"]=3200;});
        reject([](Json& j){j["voices"][1]["stop_sample"]="18446744073709551616";});
        reject([](Json& j){j["voices"][0]["emitter"]="missing";});
        reject([](Json& j){j["voices"][0]["asset"]="changed";});
        reject([](Json& j){j["voices"][0]["clip_sha256"]=std::string(64,'0');});
        reject([](Json& j){j["voices"][0]["clip_frames"]=1;});
        reject([](Json& j){j["voices"][0]["emitter_gain"]=1;});
        reject([](Json& j){j["voices"][0]["loop"]=true;});
        reject([](Json& j){j["voices"][0]["gain"]=4.01;});
        reject([](Json& j){j["voices"][0]["gain"]=false;});
        reject([](Json& j){j["voices"][0]["gain"]=nullptr;});
        reject([](Json& j){j["voices"][0]["unknown"]=1;});
        reject_text(saved,6,emitters);
        reject_text(saved.substr(0,saved.size()-1),5,emitters);
        reject_text(std::string(512*1024+1,' '),5,emitters);
        reject_text("{\"format\":\"first\",\"format\":\"second\"}",5,emitters);
        reject_text(std::string(20,'[')+std::string(20,']'),5,emitters);
        for(int kind=0;kind<7;++kind) {
            auto trusted=emitters;auto& changed=trusted.at("short");
            if(kind==0)changed.enabled=false;
            if(kind==1)changed.clip.reset();
            if(kind==2)changed.gain=std::numeric_limits<float>::infinity();
            if(kind==3)changed.loop=true;
            if(kind>=4) {
                auto clip=std::make_shared<AudioClip>(*changed.clip);
                if(kind==4)clip->samples[0]=.5f; // Same length and asset, different immutable content.
                if(kind==5)clip->samples[0]=std::numeric_limits<float>::quiet_NaN();
                if(kind==6)clip->samples.clear();
                changed.clip=clip;
            }
            reject_text(saved,5,trusted);
        }
        restored.load_state(saved,5,emitters);check(restored.save_state(5)==saved,"Valid load failed after rejected loads.");

        SoundState crowded;
        for(unsigned i=0;i<64;++i)crowded.play("loop",loop_sound,0,1);
        bool capped=false;try { crowded.play("loop",loop_sound,0,1); }catch(const std::exception&) { capped=true; }
        check(capped,"Existing 64-voice limit changed.");
        auto too_many=Json::parse(crowded.save_state(1));auto extra=too_many["voices"][0];extra["id"]=65;
        too_many["voices"].push_back(extra);too_many["next_voice_id"]=66;
        bool bad=false;try { restored.load_state(too_many.dump(),1,emitters); }catch(const std::exception&) { bad=true; }
        check(bad,"Restore accepted 65 historically simultaneous voices.");
        crowded.stop(1,1);crowded.play("loop",loop_sound,1,1);
        restored.load_state(crowded.save_state(1),1,emitters);same(crowded,restored,1);
        SoundState immediate;for(unsigned i=0;i<65;++i)immediate.stop(immediate.play("loop",loop_sound,0,1),0);
        restored.load_state(immediate.save_state(0),0,emitters);same(immediate,restored,0);

        // Pruning must preserve holes caused by a still-active oldest voice.
        SoundState pruned;pruned.play("loop",loop_sound,0,1);
        for(unsigned i=1;i<=300;++i)pruned.play("short",short_sound,i*3,1);
        check(pruned.voices().size()==256 && pruned.voices()[0].id==1 && pruned.voices()[1].id>2,"Fixture did not prune terminal history.");
        restored.load_state(pruned.save_state(900),900,emitters);same(pruned,restored,900);
        check(pruned.play("short",short_sound,903,1)==restored.play("short",short_sound,903,1),"Pruned allocator continuation changed.");
        same(pruned,restored,903);
        // Retirement after capacity pruning drops the retained count below256
        // while older history holes remain. Count must not reset the allocator.
        pruned.retire_emitters(retire_loop);SoundState pruned_retired;
        check(pruned.voices().size()==255 && pruned.next_id()==303,"Pruned retirement fixture differs.");
        pruned_retired.load_state(pruned.save_state(903),903,{{"short",short_sound}});same(pruned,pruned_retired,903);
        check(pruned.play("short",short_sound,906,1)==303 && pruned_retired.play("short",short_sound,906,1)==303,
              "Pruned retired history reused an identity.");
        same(pruned,pruned_retired,906);

        // Sample cursors exceed JSON's exact numeric range before ticks do.
        constexpr std::uint64_t high_tick=9007199254740990ULL;
        SoundState late;late.play("loop",loop_sound,high_tick-1,1);late.stop(1,high_tick);
        const auto late_json=late.save_state(high_tick);
        check(Json::parse(late_json)["voices"][0]["stop_sample"].get<std::string>()==std::to_string(high_tick*audio_tick_frames),
              "Large sample cursor lost integer precision.");
        restored.load_state(late_json,high_tick,emitters);same(late,restored,high_tick);
        std::cout<<"Audio save contract passed: round-trip, trusted content, continuation, pruning, emitter retirement, chronology, limits; "<<rejected<<" atomic rejection cases.\n";
        return 0;
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
