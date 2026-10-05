// SPDX-License-Identifier: Apache-2.0
#include "editor_audio.hpp"
#include "poima/assets.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace poima;
using Clock=std::chrono::steady_clock;
namespace {
void check(bool ok,const char* why) { if(!ok)throw std::runtime_error(why); }
std::string id(unsigned n) { const char* hex="0123456789abcdef";std::string out(32,'0');for(int i=31;n;--i,n>>=4)out[static_cast<std::size_t>(i)]=hex[n&15];return out; }
EditorAudioFrame frame(std::uint64_t tick,unsigned session=1) { EditorAudioFrame f;f.session=id(session);f.listener=id(100);f.tick=tick;return f; }
struct SinkState {
    const std::thread::id owner=std::this_thread::get_id();
    std::vector<float> samples;std::size_t queued=0,max_queued=0;
    unsigned opens=0,closes=0,clears=0,resumes=0;bool opened=false,fail_put=false;float gain=1;
};
class TestSink:public EditorAudioSink {
    std::shared_ptr<SinkState> s;
    void owner() { check(s->owner==std::this_thread::get_id(),"Sink called off owner thread"); }
public:
    explicit TestSink(std::shared_ptr<SinkState> state):s(std::move(state)) {}
    std::string open() override { owner();check(!s->opened,"Sink opened twice");s->opened=true;++s->opens;return "test-owned"; }
    void clear_and_pause() override { owner();s->queued=0;++s->clears; }
    void volume(float value) override { owner();s->gain=value; }
    std::size_t queued_frames() override { owner();return s->queued; }
    void put(std::span<const float> pcm) override { owner();check(s->opened,"Put to closed sink");if(s->fail_put)throw std::runtime_error("Injected sink failure");s->samples.insert(s->samples.end(),pcm.begin(),pcm.end());s->queued+=pcm.size()/2;s->max_queued=std::max(s->max_queued,s->queued); }
    void resume() override { owner();++s->resumes; }
    void close() noexcept override { if(s->opened) { ++s->closes;s->opened=false;s->queued=0; } }
};
struct Gate {
    std::mutex mutex;std::condition_variable wake;bool released=false;
    std::atomic<std::uint64_t> block{UINT64_MAX},fail{UINT64_MAX};std::atomic<bool> entered=false;
    std::atomic<unsigned> resets=0;
    void release() { { const std::lock_guard lock(mutex);released=true; }wake.notify_all(); }
};
struct Unblock { std::shared_ptr<Gate> gate;~Unblock() { gate->release(); } };
class TestProcessor:public EditorAudioProcessor {
    std::shared_ptr<Gate> gate;std::thread::id owner;AudioStreamStats report;EditorAudioFrame retained;
public:
    TestProcessor(std::shared_ptr<Gate> g,std::thread::id ui):gate(std::move(g)),owner(ui) {}
    void reset(const EditorAudioFrame& f) override { check(std::this_thread::get_id()!=owner,"DSP reset on UI thread");retained=f;report={};++report.path_updates;++gate->resets; }
    std::vector<float> advance(const EditorAudioFrame& f) override {
        check(std::this_thread::get_id()!=owner,"DSP advance on UI thread");
        if(gate->block==f.tick) { std::unique_lock lock(gate->mutex);gate->entered=true;gate->wake.wait(lock,[&]{return gate->released;}); }
        if(gate->fail==f.tick)throw std::runtime_error("Injected DSP failure");
        retained=f;report.frames+=audio_tick_frames;++report.blocks;
        return std::vector<float>(2*audio_tick_frames,static_cast<float>(f.tick)/100);
    }
    AudioStreamStats stats() const override { return report; }
};
EditorAudioPresenter::ProcessorFactory factory(std::shared_ptr<Gate> gate) { const auto owner=std::this_thread::get_id();return [gate,owner]{return std::make_unique<TestProcessor>(gate,owner);}; }
template<class Predicate> void until(EditorAudioPresenter& p,Predicate predicate,SinkState* sink=nullptr) {
    const auto deadline=Clock::now()+std::chrono::seconds(5);
    while(!predicate()) { if(sink)sink->queued=0;p.poll();check(Clock::now()<deadline,"Timed out awaiting audio worker");std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
void close(EditorAudioPresenter& p) { p.begin_shutdown();until(p,[&]{return p.closed();}); }
void baseline(EditorAudioPresenter& p,const std::shared_ptr<Gate>& gate,EditorAudioFrame f=frame(0)) {
    const auto resets=gate->resets.load();p.submit(std::move(f));until(p,[&]{return gate->resets>resets && !p.status().worker_busy && p.status().jobs_queued==0;});
}
void normal() {
    auto gate=std::make_shared<Gate>();auto sink=std::make_shared<SinkState>();
    EditorAudioPresenter p(factory(gate),std::make_unique<TestSink>(sink));Unblock release{gate};
    p.poll();check(!p.status().worker_started && !sink->opened,"Disabled presenter eagerly initialized");
    bool invalid=false;try { p.configure({true,false,std::numeric_limits<float>::quiet_NaN()}); }catch(const std::invalid_argument&) { invalid=true; }
    check(invalid && !p.status().config.enabled,"Invalid configuration mutated state");
    p.configure({true,false,.5f});baseline(p,gate);
    for(std::uint64_t tick=1;tick<=6;++tick) { p.submit(frame(tick));until(p,[&]{return p.status().processed_tick==tick;});p.poll(); }
    check(sink->samples.size()==6*audio_tick_frames*2 && sink->max_queued<=4800 && sink->resumes==1,"PCM submission/order/cap mismatch");
    for(std::size_t tick=1;tick<=6;++tick)for(std::size_t i=0;i<2*audio_tick_frames;++i)check(sink->samples[(tick-1)*2*audio_tick_frames+i]==static_cast<float>(tick)/100,"PCM order changed");
    const auto presented=sink->samples.size();p.configure({true,true,.25f});check(sink->queued==0,"Mute retained queued PCM");
    for(std::uint64_t tick=7;tick<=9;++tick) { p.submit(frame(tick));until(p,[&]{return p.status().processed_tick==tick;}); }
    p.poll();check(sink->samples.size()==presented && p.status().stream.frames==9*audio_tick_frames,"Mute stopped DSP or submitted samples");
    p.configure({true,false,.25f});p.submit(frame(10));until(p,[&]{return p.status().processed_tick==10;});p.poll();
    check(sink->samples.size()==presented+2*audio_tick_frames && sink->samples.back()==.1f,"Unmute replayed suppressed PCM");
    p.suspend();const auto epoch=p.status().epoch;p.suspend();check(p.status().epoch==epoch && sink->queued==0,"Suspend was not idempotent");
    check(p.status().stream.frames==10*audio_tick_frames,"Epoch reset erased lifetime DSP stats");
    p.configure({false,false,1});check(!sink->opened && sink->closes==1,"Disable retained audio device");close(p);
}
void blocked_and_retention() {
    auto gate=std::make_shared<Gate>();auto sink=std::make_shared<SinkState>();
    EditorAudioPresenter p(factory(gate),std::make_unique<TestSink>(sink));Unblock release{gate};p.configure({true,false,1});
    auto initial=frame(0);auto clip=std::make_shared<AudioClip>();clip->samples.resize(16,.1f);std::weak_ptr<const AudioClip> oldest=clip;
    initial.snapshot.sources.push_back({id(2),{std::string(64,'a'),clip,1,true,true},identity_matrix()});clip.reset();baseline(p,gate,initial);
    gate->block=1;initial.tick=1;p.submit(std::move(initial));until(p,[&]{return gate->entered.load();});
    std::vector<std::weak_ptr<const AudioClip>> retired;
    const auto start=Clock::now();
    for(unsigned i=2;i<40;++i) {
        auto next=frame(10,i);auto sample=std::make_shared<AudioClip>();sample->samples.resize(16,.2f);retired.push_back(sample);
        next.snapshot.sources.push_back({id(2),{std::string(64,'b'),sample,1,true,true},identity_matrix()});sample.reset();p.submit(std::move(next));
    }
    check(Clock::now()-start<std::chrono::seconds(1),"Owner waited for blocked DSP during replacement");
    for(std::size_t i=0;i+1<retired.size();++i)check(retired[i].expired(),"Repeated replacement retained obsolete asset closure");
    check(!oldest.expired() && !retired.back().expired(),"In-flight/current assets released early");
    p.suspend();check(sink->queued==0 && !p.status().active,"Suspend did not synchronously clear output");
    check(retired.back().expired(),"Suspended queued assets retained");
    p.begin_shutdown();p.poll();check(!p.closed(),"Closed reported before blocked worker released");
    gate->release();until(p,[&]{return p.closed();});check(oldest.expired() && sink->samples.empty(),"Stale epoch resurrected samples/assets");
}
void bounds_and_faults() {
    auto gate=std::make_shared<Gate>();auto sink=std::make_shared<SinkState>();
    EditorAudioPresenter p(factory(gate),std::make_unique<TestSink>(sink));Unblock release{gate};p.configure({true,false,1});baseline(p,gate);
    gate->block=1;p.submit(frame(1));until(p,[&]{return gate->entered.load();});
    for(std::uint64_t tick=2;tick<=18;++tick)p.submit(frame(tick));
    auto status=p.status();check(status.jobs_queued<=8 && status.pcm_frames_queued<=4800 && status.discontinuities>=1 && status.dropped_ticks>=8,"Queue overflow was unbounded or silent");
    p.suspend();gate->release();baseline(p,gate,frame(100));
    gate->fail=101;p.submit(frame(101));until(p,[&]{return !p.status().error.empty();});check(!p.status().active && !sink->opened,"DSP error did not disable presentation");
    gate->fail=UINT64_MAX;p.retry();baseline(p,gate,frame(200));sink->fail_put=true;p.submit(frame(201));until(p,[&]{return !p.status().error.empty();});
    check(p.status().error=="Injected sink failure" && !sink->opened,"Sink failure escaped or retained device");
    sink->fail_put=false;p.retry();baseline(p,gate,frame(300));p.submit(frame(301));until(p,[&]{return p.status().processed_tick==301;});p.poll();check(p.status().error.empty(),"Retry failed to recover");
    close(p);
}
void immutable_assets() {
    auto gate=std::make_shared<Gate>();auto sink=std::make_shared<SinkState>();EditorAudioPresenter p(factory(gate),std::make_unique<TestSink>(sink));Unblock release{gate};p.configure({true,true,1});baseline(p,gate);
    auto changed=frame(1);auto clip=std::make_shared<AudioClip>();clip->samples.resize(10,.1f);changed.snapshot.sources.push_back({id(2),{std::string(64,'a'),clip,1,false,true},identity_matrix()});p.submit(std::move(changed));
    check(p.status().error.find("assets changed")!=std::string::npos,"New per-tick asset closure accepted silently");
    p.retry();auto oversized=frame(0);auto mesh=std::make_shared<MeshAsset>();auto texture=std::make_shared<TextureImage>();texture->mips.reserve(65);mesh->textures[0].image=texture;oversized.snapshot.geometry.push_back({id(3),identity_matrix(),mesh,{}});p.submit(std::move(oversized));
    check(p.status().error.find("too many mips")!=std::string::npos,"Transitive texture retention budget ignored");close(p);
}
class RealProcessor:public EditorAudioProcessor {
    std::unique_ptr<AudioStream> stream;
public:
    void reset(const EditorAudioFrame& f) override { stream=std::make_unique<AudioStream>(f.tick,f.snapshot);(void)stream->advance(f.tick,f.snapshot,f.voices); }
    std::vector<float> advance(const EditorAudioFrame& f) override { return stream->advance(f.tick,f.snapshot,f.voices); }
    AudioStreamStats stats() const override { return stream->stats(); }
};
void actual_dsp() {
    if(!audio_available())return;
    auto initial=frame(0);auto clip=std::make_shared<AudioClip>();clip->samples.resize(audio_rate);for(std::size_t i=0;i<clip->samples.size();++i)clip->samples[i]=i%97==0 ? .2f : 0;
    auto pose=identity_matrix();pose[12]=2;AudioEmitter emitter{std::string(64,'a'),clip,1,true,true};initial.snapshot.sources.push_back({id(2),emitter,pose});
    initial.voices.push_back({1,0,{},id(2),emitter,1});AudioStream reference(0,initial.snapshot);(void)reference.advance(0,initial.snapshot,initial.voices);std::vector<float> expected;
    auto sink=std::make_shared<SinkState>();EditorAudioPresenter p([]{return std::make_unique<RealProcessor>();},std::make_unique<TestSink>(sink));p.configure({true,false,1});p.submit(initial);
    std::exception_ptr observer_error;std::thread observer([&]{try { for(int i=0;i<12;++i)(void)observe_audio(initial.snapshot,audio_block); }catch(...) { observer_error=std::current_exception(); }});
    try {
        for(std::uint64_t tick=1;tick<=20;++tick) {
            auto next=initial;next.tick=tick;next.snapshot.listener[12]=static_cast<double>(tick)*.01;
            auto pcm=reference.advance(tick,next.snapshot,next.voices);expected.insert(expected.end(),pcm.begin(),pcm.end());p.submit(std::move(next));
            until(p,[&]{return p.status().processed_tick==tick;},sink.get());sink->queued=0;p.poll();
        }
        observer.join();if(observer_error)std::rethrow_exception(observer_error);
        check(sink->samples==expected && !expected.empty(),"Actual asynchronous Steam Audio differs from synchronous PCM");
        check(p.status().stream.voices_started==1 && p.status().stream.peak>0,"Actual DSP voice/peak qualification absent");close(p);
    }catch(...) { if(observer.joinable())observer.join();throw; }
}
}
int main() {
    try { normal();blocked_and_retention();bounds_and_faults();immutable_assets();actual_dsp();
        std::cout<<"{\"passed\":true,\"groups\":"<<(audio_available()?5:4)<<",\"real_steam_audio\":"<<(audio_available()?"true":"false")<<",\"checks\":[\"owner-only sink and worker-only DSP\",\"mute continuation and disable teardown\",\"blocked worker epoch/shutdown and asset lifetime\",\"bounded overflow and presentation faults\",\"immutable transitive asset closure\""<<(audio_available()?",\"exact Steam Audio PCM and concurrent HRTF creation\"":"")<<"]}\n";
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
