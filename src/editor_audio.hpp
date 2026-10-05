// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/audio.hpp"
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace poima {
struct EditorAudioConfig { bool enabled=false,muted=false;float volume=1; };
struct EditorAudioFrame {
    std::string session,listener;
    std::uint64_t tick=0;
    AudioSnapshot snapshot;
    std::vector<SoundVoice> voices;
};
struct EditorAudioStatus {
    EditorAudioConfig config;
    std::string state="disabled",session,listener,error,driver;
    bool active=false,worker_started=false,worker_busy=false,device_open=false,closing=false,closed=false;
    std::uint64_t epoch=0,last_submitted_tick=0,processed_tick=0;
    std::size_t jobs_queued=0,pcm_frames_queued=0,device_frames_queued=0;
    std::uint64_t submitted_frames=0,dropped_frames=0,dropped_ticks=0,discontinuities=0,stale_results=0,resets=0;
    AudioStreamStats stream;
};
// Internal seams also used for deterministic scheduling/output qualification.
// Processor construction, reset, advance and destruction happen on the worker.
class EditorAudioProcessor {
public:
    virtual ~EditorAudioProcessor()=default;
    virtual void reset(const EditorAudioFrame&)=0;
    virtual std::vector<float> advance(const EditorAudioFrame&)=0;
    virtual AudioStreamStats stats() const=0;
};
// Every sink method is called only by the presenter's creating thread. It must
// not run game DSP or wait for output to drain. close() is an idempotent teardown.
class EditorAudioSink {
public:
    virtual ~EditorAudioSink()=default;
    virtual std::string open()=0;
    virtual void clear_and_pause()=0;
    virtual void volume(float)=0;
    virtual std::size_t queued_frames()=0;
    virtual void put(std::span<const float>)=0;
    virtual void resume()=0;
    virtual void close() noexcept=0;
};
class EditorAudioPresenter {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    static constexpr std::size_t max_jobs=8,max_pcm_frames=4800,max_metadata_bytes=16*1024*1024;
    using ProcessorFactory=std::function<std::unique_ptr<EditorAudioProcessor>()>;
    static bool available();
    EditorAudioPresenter();
    EditorAudioPresenter(ProcessorFactory,std::unique_ptr<EditorAudioSink>);
    // Synchronous fallback; GUI owners should begin_shutdown(), poll until
    // closed(), then destroy. A non-cancellable SDK call has no join deadline.
    ~EditorAudioPresenter();
    EditorAudioPresenter(const EditorAudioPresenter&)=delete;
    EditorAudioPresenter& operator=(const EditorAudioPresenter&)=delete;
    void configure(EditorAudioConfig);
    void submit(EditorAudioFrame);
    void suspend();
    void poll();
    void retry();
    void begin_shutdown();
    bool closed() const;
    EditorAudioStatus status() const;
};
}
