// SPDX-License-Identifier: Apache-2.0
#include "editor_audio.hpp"
#include "poima/assets.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#if POIMA_AUDIO && POIMA_RENDER_SMOKE
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif
namespace poima {
namespace {
void require(bool ok,const char* message) { if(!ok)throw std::invalid_argument(message); }
class Processor final:public EditorAudioProcessor {
    std::unique_ptr<AudioStream> stream_;
public:
    void reset(const EditorAudioFrame& f) override {
        stream_=std::make_unique<AudioStream>(f.tick,f.snapshot);
        (void)stream_->advance(f.tick,f.snapshot,f.voices);
    }
    std::vector<float> advance(const EditorAudioFrame& f) override { return stream_->advance(f.tick,f.snapshot,f.voices); }
    AudioStreamStats stats() const override { return stream_ ? stream_->stats() : AudioStreamStats{}; }
};
class Sink final:public EditorAudioSink {
#if POIMA_AUDIO && POIMA_RENDER_SMOKE
    SDL_AudioStream* stream_=nullptr;bool initialized_=false;
    static void check(bool ok) { if(!ok)throw std::runtime_error(SDL_GetError()); }
public:
    std::string open() override {
        SDL_SetMainReady();check(SDL_InitSubSystem(SDL_INIT_AUDIO));initialized_=true;
        SDL_AudioSpec spec{SDL_AUDIO_F32,2,static_cast<int>(audio_rate)};
        stream_=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,nullptr,nullptr);
        if(!stream_) { const std::string error=SDL_GetError();close();throw std::runtime_error(error); }
        const auto* driver=SDL_GetCurrentAudioDriver();return driver ? driver : "unknown";
    }
    void clear_and_pause() override { if(stream_) { check(SDL_PauseAudioStreamDevice(stream_));check(SDL_ClearAudioStream(stream_)); } }
    void volume(float value) override { if(stream_)check(SDL_SetAudioStreamGain(stream_,value)); }
    std::size_t queued_frames() override { const int bytes=SDL_GetAudioStreamQueued(stream_);check(bytes>=0);return static_cast<std::size_t>(bytes)/8; }
    void put(std::span<const float> samples) override { check(SDL_PutAudioStreamData(stream_,samples.data(),static_cast<int>(samples.size_bytes()))); }
    void resume() override { check(SDL_ResumeAudioStreamDevice(stream_)); }
    void close() noexcept override { if(stream_) { SDL_DestroyAudioStream(stream_);stream_=nullptr; }if(initialized_) { SDL_QuitSubSystem(SDL_INIT_AUDIO);initialized_=false; } }
#else
public:
    std::string open() override { throw std::runtime_error("Editor audio output is not built."); }
    void clear_and_pause() override {}
    void volume(float) override {}
    std::size_t queued_frames() override { return 0; }
    void put(std::span<const float>) override {}
    void resume() override {}
    void close() noexcept override {}
#endif
};
bool id(const std::string& value) {
    return value.size()==32 && std::all_of(value.begin(),value.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');});
}
struct Assets {
    std::set<const void*> identities;
    std::vector<std::shared_ptr<const AudioClip>> clips;
    std::vector<std::shared_ptr<const MeshAsset>> meshes;
    std::vector<std::shared_ptr<const TextureImage>> textures;
};
struct FrameBudget { std::size_t metadata=0;std::shared_ptr<const Assets> assets; };
FrameBudget metadata(const EditorAudioFrame& frame) {
    require(id(frame.session) && id(frame.listener),"Editor audio frame requires session and listener IDs.");
    require(frame.tick<=9007199254740991ULL,"Editor audio tick exceeds runtime clock range.");
    require(frame.snapshot.geometry.size()<=10000 && frame.snapshot.sources.size()<=max_audio_sources && frame.voices.size()<=256,"Editor audio snapshot exceeds element budgets.");
    std::size_t result=sizeof(frame)+frame.session.capacity()+frame.listener.capacity();
    result+=frame.snapshot.geometry.capacity()*sizeof(AcousticGeometry)+frame.snapshot.sources.capacity()*sizeof(AudioSource)+frame.voices.capacity()*sizeof(SoundVoice);
    std::size_t clip_bytes=0,mesh_bytes=0;auto assets=std::make_shared<Assets>();
    auto text=[&](const std::string& value) { require(value.size()<=128 && value.capacity()<=1024,"Editor audio snapshot string exceeds budget.");result+=value.capacity(); };
    auto clip=[&](const AudioEmitter& emitter) {
        text(emitter.asset);
        if(emitter.clip && assets->identities.insert(emitter.clip.get()).second) {
            require(emitter.clip->samples.size()<=max_audio_clip_frames && emitter.clip->samples.capacity()<=64*1024*1024/sizeof(float),"Editor audio clip exceeds frame budget.");
            clip_bytes+=sizeof(AudioClip)+emitter.clip->samples.capacity()*sizeof(float);
            require(clip_bytes<=64*1024*1024,"Editor audio retained clips exceed 64 MiB.");assets->clips.push_back(emitter.clip);
        }
    };
    for(const auto& geometry:frame.snapshot.geometry) {
        text(geometry.entity);
        if(geometry.mesh && assets->identities.insert(geometry.mesh.get()).second) {
            const auto& mesh=*geometry.mesh;
            require(mesh.vertices.capacity()<=3*max_audio_triangles && mesh.indices.capacity()<=3*max_audio_triangles && mesh.influences.capacity()<=3*max_audio_triangles,"Editor audio mesh exceeds geometry budget.");
            mesh_bytes+=sizeof(MeshAsset)+mesh.vertices.capacity()*sizeof(MeshVertex)+mesh.indices.capacity()*sizeof(std::uint32_t)+mesh.influences.capacity()*sizeof(SkinWeight);
            assets->meshes.push_back(geometry.mesh);
            for(const auto& map:mesh.textures)if(map.image && assets->identities.insert(map.image.get()).second) {
                require(map.image->mips.capacity()<=64,"Editor audio retained texture has too many mips.");
                mesh_bytes+=sizeof(TextureImage)+map.image->mips.capacity()*sizeof(TextureMip);
                for(const auto& mip:map.image->mips) { require(mip.rgba.capacity()<=256*1024*1024,"Editor audio retained texture exceeds byte budget.");mesh_bytes+=mip.rgba.capacity(); }
                assets->textures.push_back(map.image);
            }
            require(mesh_bytes<=256*1024*1024,"Editor audio retained geometry/textures exceed 256 MiB.");
        }
    }
    for(const auto& source:frame.snapshot.sources) { text(source.entity);clip(source.emitter); }
    for(const auto& voice:frame.voices) { text(voice.emitter);clip(voice.sound); }
    require(result<=4*1024*1024,"Editor audio frame metadata exceeds 4 MiB.");return {result,std::move(assets)};
}
}
struct EditorAudioPresenter::Impl {
    struct Job { EditorAudioFrame frame;std::shared_ptr<const Assets> assets;std::uint64_t epoch=0,output=0;std::size_t bytes=0;bool baseline=false; };
    struct Result { std::vector<float> pcm;std::uint64_t epoch=0,output=0; };
    const std::thread::id owner=std::this_thread::get_id();
    ProcessorFactory factory;std::unique_ptr<EditorAudioSink> sink;
    EditorAudioConfig config;EditorAudioStatus report;std::shared_ptr<const Assets> current_assets;
    std::thread worker;mutable std::mutex mutex;std::condition_variable wake;
    std::array<std::optional<Job>,max_jobs> jobs;
    std::array<std::optional<Result>,max_jobs> results;
    std::size_t job_head=0,job_count=0,result_head=0,result_count=0,job_bytes=0,busy_bytes=0,pcm_frames=0;
    std::uint64_t epoch=0,output=0;
    bool stopping=false,done=false,busy=false,muted=false,worker_fault=false;
    std::string worker_error;
    AudioStreamStats stats,retired;std::uint64_t processed=0,discarded_frames=0,discarded_ticks=0,stale=0;
    bool active=false,device_open=false,resumed=false,closing=false,is_closed=false;
    Impl(ProcessorFactory f,std::unique_ptr<EditorAudioSink> s):factory(std::move(f)),sink(std::move(s)) { require(bool(factory) && bool(sink),"Editor audio requires processor and sink."); }
    void owner_check() const { require(std::this_thread::get_id()==owner,"Editor audio must be called on its creating thread."); }
    void clear_output() {
        if(device_open)sink->clear_and_pause();
        resumed=false;report.device_frames_queued=0;
    }
    void invalidate() {
        std::array<std::optional<Job>,max_jobs> dropped_jobs;
        std::array<std::optional<Result>,max_jobs> dropped_results;
        {
            const std::lock_guard lock(mutex);++epoch;++output;
            for(std::size_t i=0;i<max_jobs;++i) { dropped_jobs[i]=std::move(jobs[i]);jobs[i].reset();dropped_results[i]=std::move(results[i]);results[i].reset(); }
            for(const auto& job:dropped_jobs)if(job && !job->baseline)++discarded_ticks;
            discarded_frames+=pcm_frames;
            retired.frames+=stats.frames;retired.blocks+=stats.blocks;retired.voices_started+=stats.voices_started;retired.path_updates+=stats.path_updates;
            retired.peak=std::max(retired.peak,stats.peak);retired.dsp_ms+=stats.dsp_ms;retired.over_range_samples+=stats.over_range_samples;
            job_count=job_head=result_count=result_head=job_bytes=pcm_frames=0;
            worker_fault=false;worker_error.clear();processed=0;stats={};
        }
        current_assets.reset();active=false;++report.resets;wake.notify_one();clear_output();
    }
    void fault(const std::string& error) {
        try { invalidate(); }catch(...) { sink->close();device_open=false; }
        sink->close();device_open=false;resumed=false;report.device_frames_queued=0;
        report.error=error;active=false;
    }
    void run() noexcept {
        std::unique_ptr<EditorAudioProcessor> processor;std::shared_ptr<const Assets> processor_assets;std::uint64_t processor_epoch=0;
        try {
            for(;;) {
                std::optional<Job> job;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock,[&]{return stopping || processor_epoch!=epoch || (!worker_fault && job_count);});
                    if(stopping)break;
                    if(processor_epoch!=epoch) {
                        processor_epoch=epoch;busy=true;lock.unlock();processor.reset();processor_assets.reset();lock.lock();busy=false;
                        if(stopping)break;
                        if(processor_epoch!=epoch)continue;
                    }
                    if(worker_fault || !job_count)continue;
                    job=std::move(jobs[job_head]);jobs[job_head].reset();job_head=(job_head+1)%max_jobs;--job_count;
                    job_bytes-=job->bytes;busy_bytes=job->bytes;busy=true;
                }
                struct ReleaseJob {
                    Impl& owner;std::optional<Job>& value;
                    ~ReleaseJob() { value.reset();const std::lock_guard lock(owner.mutex);owner.busy=false;owner.busy_bytes=0; }
                } release{*this,job};
                try {
                    if(job->baseline) { processor=factory();require(bool(processor),"Audio processor factory returned null.");processor->reset(job->frame);processor_assets=job->assets; }
                    require(bool(processor),"Audio worker requires a baseline before advancement.");
                    auto pcm=job->baseline ? std::vector<float>{} : processor->advance(job->frame);
                    require(pcm.size()%2==0 && pcm.size()/2<=max_pcm_frames,"Audio processor returned invalid PCM size.");
                    for(float sample:pcm)require(std::isfinite(sample),"Audio processor returned nonfinite PCM.");
                    const auto latest=processor->stats();const auto frames=pcm.size()/2;
                    std::unique_lock lock(mutex);
                    if(job->epoch!=epoch) { discarded_frames+=frames;if(!job->baseline)++discarded_ticks;++stale;continue; }
                    processed=job->frame.tick;stats=latest;
                    if(job->output!=output || muted) { discarded_frames+=frames;continue; }
                    if(!frames)continue;
                    wake.wait(lock,[&]{return stopping || job->epoch!=epoch || job->output!=output || muted || (result_count<max_jobs && frames<=max_pcm_frames-pcm_frames);});
                    if(stopping)break;
                    if(job->epoch!=epoch || job->output!=output || muted) { discarded_frames+=frames;++stale;continue; }
                    const auto tail=(result_head+result_count)%max_jobs;
                    results[tail]=Result{std::move(pcm),job->epoch,job->output};++result_count;pcm_frames+=frames;
                }catch(const std::exception& error) {
                    const std::lock_guard lock(mutex);
                    if(job->epoch==epoch) { worker_error=std::string(error.what()).substr(0,1024);worker_fault=true; }
                }catch(...) {
                    const std::lock_guard lock(mutex);
                    if(job->epoch==epoch) { worker_error="Unknown audio worker failure.";worker_fault=true; }
                }
            }
        }catch(...) {
            // Covers allocation failure while reporting an SDK failure. The
            // owner detects an unexpectedly exited worker independently.
        }
        processor.reset();processor_assets.reset();
        const std::lock_guard lock(mutex);busy=false;busy_bytes=0;done=true;
    }
    void start_worker() { if(!worker.joinable())worker=std::thread([this]{run();}); }
};
bool EditorAudioPresenter::available() {
#if POIMA_AUDIO && POIMA_RENDER_SMOKE
    return true;
#else
    return false;
#endif
}
EditorAudioPresenter::EditorAudioPresenter():EditorAudioPresenter([]{return std::make_unique<Processor>();},std::make_unique<Sink>()) {}
EditorAudioPresenter::EditorAudioPresenter(ProcessorFactory factory,std::unique_ptr<EditorAudioSink> sink):impl_(std::make_unique<Impl>(std::move(factory),std::move(sink))) {}
EditorAudioPresenter::~EditorAudioPresenter() {
    if(!impl_)return;
    { const std::lock_guard lock(impl_->mutex);impl_->stopping=true; }
    impl_->wake.notify_one();if(impl_->worker.joinable())impl_->worker.join();impl_->sink->close();
}
void EditorAudioPresenter::configure(EditorAudioConfig config) {
    auto& p=*impl_;p.owner_check();require(!p.closing,"Audio presenter is closing.");
    require(std::isfinite(config.volume) && config.volume>=0 && config.volume<=1,"Editor audio volume must be finite in [0,1].");
    const auto previous=p.config;p.config=config;
    try {
        if(previous.enabled!=config.enabled) { p.invalidate();p.report.error.clear();if(!config.enabled) { p.sink->close();p.device_open=false; } }
        if(previous.muted!=config.muted) {
            { const std::lock_guard lock(p.mutex);p.muted=config.muted;++p.output; }
            p.wake.notify_one();p.clear_output();
        }
        if(p.device_open)p.sink->volume(config.volume);
    }catch(const std::exception& error) { p.fault(error.what()); }
}
void EditorAudioPresenter::submit(EditorAudioFrame frame) {
    auto& p=*impl_;p.owner_check();if(p.closing || !p.config.enabled || !p.report.error.empty())return;
    try {
        auto budget=metadata(frame);const auto bytes=budget.metadata;
        if(p.active && (frame.session!=p.report.session || frame.listener!=p.report.listener || frame.tick!=p.report.last_submitted_tick+1)) {
            ++p.report.discontinuities;p.invalidate();
        }
        bool full;
        { const std::lock_guard lock(p.mutex);full=p.job_count==max_jobs || bytes>max_metadata_bytes-p.job_bytes-p.busy_bytes; }
        if(full) { ++p.report.discontinuities;p.invalidate(); }
        const bool baseline=!p.active;
        if(baseline) { p.report.session=frame.session;p.report.listener=frame.listener;p.current_assets=std::move(budget.assets);p.start_worker(); }
        else for(const auto* asset:budget.assets->identities)require(p.current_assets->identities.contains(asset),"Editor audio assets changed within a frozen epoch; submit a new baseline.");
        const auto tick=frame.tick;
        { const std::lock_guard lock(p.mutex);
            p.jobs[(p.job_head+p.job_count)%max_jobs]=Impl::Job{std::move(frame),p.current_assets,p.epoch,p.output,bytes,baseline};++p.job_count;p.job_bytes+=bytes;
        }
        p.active=true;p.report.last_submitted_tick=tick;p.wake.notify_one();
    }catch(const std::exception& error) { p.fault(error.what()); }
}
void EditorAudioPresenter::suspend() {
    auto& p=*impl_;p.owner_check();if(!p.active)return;
    try { p.invalidate(); }catch(const std::exception& error) { p.fault(error.what()); }
}
void EditorAudioPresenter::retry() {
    auto& p=*impl_;p.owner_check();require(!p.closing,"Audio presenter is closing.");
    try {
        p.invalidate();p.report.error.clear();
        bool done;{ const std::lock_guard lock(p.mutex);done=p.done; }
        if(done && p.worker.joinable()) { p.worker.join();const std::lock_guard lock(p.mutex);p.done=false; }
    }catch(const std::exception& error) { p.fault(error.what()); }
}
void EditorAudioPresenter::poll() {
    auto& p=*impl_;p.owner_check();if(p.is_closed)return;
    if(p.closing) {
        bool done;{ const std::lock_guard lock(p.mutex);done=p.done || !p.worker.joinable(); }
        if(done) { if(p.worker.joinable())p.worker.join();p.sink->close();p.device_open=false;p.is_closed=true; }
        return;
    }
    if(!p.report.error.empty())return;
    try {
        std::string error;{ const std::lock_guard lock(p.mutex);error=p.worker_error;if(p.done && error.empty())error="Audio worker stopped unexpectedly; retry to restart."; }
        if(!error.empty()) { p.fault(error);return; }
        if(!p.active || !p.config.enabled || !p.report.error.empty())return;
        if(!p.device_open) { p.report.driver=p.sink->open();p.device_open=true;p.sink->volume(p.config.volume); }
        for(std::size_t count=0;count<max_jobs;++count) {
            std::optional<Impl::Result> result;
            const auto queued=p.device_open ? p.sink->queued_frames() : 0;p.report.device_frames_queued=queued;
            require(queued<=max_pcm_frames,"Audio output exceeded its queue cap.");
            { const std::lock_guard lock(p.mutex);
                if(!p.result_count)break;
                const auto& next=*p.results[p.result_head];
                const bool discard=p.config.muted || next.epoch!=p.epoch || next.output!=p.output;
                if(!discard && next.pcm.size()/2>max_pcm_frames-queued)break;
                result=std::move(p.results[p.result_head]);p.results[p.result_head].reset();p.result_head=(p.result_head+1)%max_jobs;--p.result_count;p.pcm_frames-=result->pcm.size()/2;
                if(discard) { p.discarded_frames+=result->pcm.size()/2;++p.stale;result.reset(); }
            }
            p.wake.notify_one();if(!result)continue;
            p.sink->put(result->pcm);p.report.submitted_frames+=result->pcm.size()/2;
            p.report.device_frames_queued=p.sink->queued_frames();
            if(!p.resumed && p.report.device_frames_queued>=2048) { p.sink->resume();p.resumed=true; }
        }
    }catch(const std::exception& error) { p.fault(error.what()); }
}
void EditorAudioPresenter::begin_shutdown() {
    auto& p=*impl_;p.owner_check();if(p.closing)return;p.closing=true;
    try { p.invalidate(); }catch(const std::exception& error) { p.report.error=error.what();p.sink->close();p.device_open=false; }
    { const std::lock_guard lock(p.mutex);p.stopping=true; }p.wake.notify_one();
}
bool EditorAudioPresenter::closed() const { impl_->owner_check();return impl_->is_closed; }
EditorAudioStatus EditorAudioPresenter::status() const {
    auto& p=*impl_;p.owner_check();auto result=p.report;result.config=p.config;result.active=p.active;
    result.worker_started=p.worker.joinable();result.device_open=p.device_open;result.closing=p.closing;result.closed=p.is_closed;
    { const std::lock_guard lock(p.mutex);result.epoch=p.epoch;result.worker_busy=p.busy;result.jobs_queued=p.job_count;result.pcm_frames_queued=p.pcm_frames;result.processed_tick=p.processed;result.stream=p.stats;result.stream.frames+=p.retired.frames;result.stream.blocks+=p.retired.blocks;result.stream.voices_started+=p.retired.voices_started;result.stream.path_updates+=p.retired.path_updates;result.stream.peak=std::max(result.stream.peak,p.retired.peak);result.stream.dsp_ms+=p.retired.dsp_ms;result.stream.over_range_samples+=p.retired.over_range_samples;result.dropped_ticks=p.discarded_ticks;result.dropped_frames=p.discarded_frames;result.stale_results=p.stale; }
    result.state=p.is_closed ? "closed" : p.closing ? "closing" : !result.error.empty() ? "fault" : !p.config.enabled ? "disabled" : !p.active ? "idle" : p.config.muted ? "muted" : p.device_open ? "active" : "starting";
    return result;
}
}
