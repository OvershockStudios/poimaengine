// SPDX-License-Identifier: Apache-2.0
#include "poima/world.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
using namespace poima;
using Json=nlohmann::json;
namespace fs=std::filesystem;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
std::string id(char value) { return std::string(32,value); }
std::string text(const fs::path& path) { const auto value=path.u8string();return {value.begin(),value.end()}; }
Json call(WorldSession& session,const char* method,Json params=Json::object()) {
    auto reply=Json::parse(session.request(Json{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",std::move(params)}}.dump()));
    if(reply.contains("error"))throw std::runtime_error(std::string(method)+": "+reply.at("error").dump());
    return reply.at("result");
}
template<class F> void rejects(F action) { bool failed=false;try { action(); }catch(const std::exception&) { failed=true; }check(failed,"Invalid audio observation succeeded."); }
std::map<std::string,std::string> tree(const fs::path& root) {
    std::map<std::string,std::string> result;
    for(const auto& entry:fs::recursive_directory_iterator(root))if(entry.is_regular_file()) {
        std::ifstream input(entry.path(),std::ios::binary);result[text(entry.path().lexically_relative(root))]={std::istreambuf_iterator<char>(input),{}};
    }
    return result;
}
Json transform(Json position,Json scale={1,1,1}) { return {{"position",position},{"rotation",{0,0,0,1}},{"scale",scale}}; }
void verify_retained(const WorldAudioState& state,const Matrix4& geometry) {
    check(state.session_id==id('a') && state.listener==id('1') && state.tick==1,"Captured audio boundary changed.");
    check(state.snapshot.geometry.size()==1 && state.snapshot.geometry.front().world==geometry,"Captured acoustic geometry mutated.");
    check(state.snapshot.sources.size()==1 && state.voices.size()==1,"Captured sources or voices changed.");
    const auto& voice=state.voices.front();
    check(voice.id==1 && voice.start_tick==0 && !voice.stop_sample && voice.emitting(audio_tick_frames),"Captured logical voice changed.");
    check(voice.sound.clip && voice.sound.clip==state.snapshot.sources.front().emitter.clip,"Voice/source clip ownership differs.");
    check(voice.sound.clip->samples.size()==1600 && voice.sound.clip->samples[0]==.25f && voice.sound.clip->samples[1]==-.125f,"Retained clip data expired or changed.");
}
}
int main() {
    fs::path directory;
    try {
        directory=fs::temp_directory_path()/fs::path("poima-world-audio-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        check(fs::create_directory(directory),"Create fixture directory.");
        WorldAudioState retained;Matrix4 initial_geometry{};
        {
            WorldSession world(text(directory/"world.json"));
            rejects([&]{(void)world.audio_state(id('a'),0,id('1'));});
            if(!Runtime::available()) {
                call(world,"session.close");rejects([&]{(void)world.audio_state(id('a'),0,id('1'));});
                std::cout<<"World audio closed/unavailable guards passed (simulation not built).\n";
            } else {
                std::vector<float> pcm(1600,.25f);pcm[1]=-.125f;
                { const auto wav=audio_wave(pcm,1);std::ofstream output(directory/"clip.wav",std::ios::binary);output.write(wav.data(),static_cast<std::streamsize>(wav.size())); }
                const auto asset=call(world,"asset.audio.import",{{"source",text(directory/"clip.wav")}}).at("asset");
                Json operations=Json::array();
                for(char entity:{'1','2','3','4'})operations.push_back({{"op","entity.create"},{"id",id(entity)},{"name",std::string("Audio ")+entity}});
                auto component=[&](char entity,const char* type,Json value) { operations.push_back({{"op","component.set"},{"id",id(entity)},{"type",type},{"value",std::move(value)}}); };
                component('1',"Camera",{{"vertical_fov",60},{"near",.1},{"far",1000}});
                component('2',"Transform",transform({0,0,-2}));
                component('2',"BoxCollider",{{"half_extents",{.5,.5,.5}},{"motion","kinematic"},{"mass",1},{"friction",.5},{"restitution",0}});
                component('2',"AcousticMaterial",{{"absorption",{.1,.1,.1}},{"transmission",{.1,.05,.01}},{"scattering",.5},{"enabled",true}});
                component('3',"Transform",transform({0,0,-4}));component('3',"AudioEmitter",{{"asset",asset},{"gain",.75},{"loop",true},{"enabled",true}});
                component('4',"Camera",{{"vertical_fov",60},{"near",.1},{"far",1000}});component('4',"Transform",transform({0,0,0},{2,1,1}));
                call(world,"world.transact",{{"request_id",id('5')},{"base_revision",0},{"ops",operations}});
                call(world,"runtime.start",{{"session_id",id('a')},{"revision",1}});
                const Json play={{"op","play"},{"emitter",id('3')},{"gain",.5}};
                call(world,"runtime.step",{{"session_id",id('a')},{"request_id",id('6')},{"expected_tick",0},{"ticks",1},{"sounds",Json::array({play})}});
                retained=world.audio_state(id('a'),1,id('1'));initial_geometry=retained.snapshot.geometry.at(0).world;verify_retained(retained,initial_geometry);
                const auto storage=tree(directory);
                const auto runtime=call(world,"runtime.inspect",{{"session_id",id('a')}});const auto history=call(world,"world.history");
                world.profiler().start(64);
                for(int n=0;n<3;++n)(void)world.audio_state(id('a'),1,id('1'));
                rejects([&]{(void)world.audio_state("bad",1,id('1'));});
                rejects([&]{(void)world.audio_state(id('b'),1,id('1'));});
                rejects([&]{(void)world.audio_state(id('a'),0,id('1'));});
                rejects([&]{(void)world.audio_state(id('a'),std::numeric_limits<std::uint64_t>::max(),id('1'));});
                rejects([&]{(void)world.audio_state(id('a'),1,"bad");});
                rejects([&]{(void)world.audio_state(id('a'),1,id('f'));});
                rejects([&]{(void)world.audio_state(id('a'),1,id('2'));});
                rejects([&]{(void)world.audio_state(id('a'),1,id('4'));});
                world.profiler().stop();bool traced=false;
                for(const auto& event:world.profiler().events())if(std::string_view(event.name.data())=="world.audio.snapshot" && !event.failed) {
                    check(event.tick==1 && std::string_view(event.session.data())==id('a'),"Audio snapshot profiler context differs.");traced=true;
                }
                check(traced,"Audio snapshot profiler scope missing.");
                check(tree(directory)==storage && call(world,"runtime.inspect",{{"session_id",id('a')}})==runtime && call(world,"world.history")==history,"Audio queries mutated storage/runtime/history.");
                fs::create_directory(directory/"saves");call(world,"save.configure",{{"request_id",id('7')},{"expected_generation",0},{"root",text(directory/"saves")}});
                call(world,"save.write",{{"request_id",id('8')},{"configuration_generation",1},{"slot","audio"},{"expected_generation",0},{"session_id",id('a')},{"expected_tick",1},{"expected_gameplay_revision",0}});
                call(world,"runtime.step",{{"session_id",id('a')},{"request_id",id('9')},{"expected_tick",1},{"ticks",1},{"sounds",Json::array({Json{{"op","stop"},{"voice",1}}})},
                    {"motions",Json::array({Json{{"entity",id('2')},{"position",{1,0,-2}},{"rotation",{0,0,0,1}},{"duration_ticks",1}}})}});
                const auto changed=world.audio_state(id('a'),2,id('1'));
                check(changed.voices.at(0).stop_sample.has_value() && changed.snapshot.geometry.at(0).world!=initial_geometry,"Fixture failed to alter live sound/geometry.");
                verify_retained(retained,initial_geometry);
                call(world,"save.load",{{"request_id",id('c')},{"configuration_generation",1},{"slot","audio"},{"expected_generation",1},{"revision",1},{"expected_session_id",id('a')},{"expected_tick",2},{"expected_gameplay_revision",0},{"new_session_id",id('b')}});
                rejects([&]{(void)world.audio_state(id('a'),1,id('1'));});
                const auto restored=world.audio_state(id('b'),1,id('1'));
                check(restored.snapshot.geometry.at(0).world==initial_geometry && !restored.voices.at(0).stop_sample,"Restored boundary did not restore logical audio state.");
                verify_retained(retained,initial_geometry);
                call(world,"runtime.stop",{{"session_id",id('b')}});rejects([&]{(void)world.audio_state(id('b'),1,id('1'));});verify_retained(retained,initial_geometry);
                call(world,"session.close");rejects([&]{(void)world.audio_state(id('b'),1,id('1'));});
            }
        }
        if(Runtime::available())verify_retained(retained,initial_geometry);
        fs::remove_all(directory);std::cout<<"World audio ownership, guards, immutable committed voices/geometry, save replacement and lifetime passed.\n";
    }catch(const std::exception& error) { std::cerr<<error.what()<<"\nFixture: "<<text(directory)<<'\n';return 1; }
}
