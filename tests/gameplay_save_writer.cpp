// SPDX-License-Identifier: Apache-2.0
// Real bounded save-store operations; no renderer, gameplay host or child process.
#include "poima/gameplay_save_writer.hpp"
#include "poima/assets.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;
namespace fs=std::filesystem;
namespace jobs=poima::jobs;
namespace saves=poima::saves;
using poima::GameplaySaveState;
using poima::GameplaySaveWriter;
using poima::GameplaySaveWriteInput;
using poima::GameplaySaveWriteResult;
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class Error=std::logic_error,class F> void rejects(F&& action,const char* message) {
    bool rejected=false;try {action();}catch(const Error&) {rejected=true;}check(rejected,message);
}
std::string hash(const std::string& bytes) {
    return poima::sha256(std::as_bytes(std::span(bytes.data(),bytes.size())));
}
std::string id(unsigned number) {
    std::string result(32,'0');for(unsigned index=0;number;++index,number>>=4)
        result[31-index]="0123456789abcdef"[number&15];return result;
}
std::string read(const fs::path& path) {
    std::ifstream file(path,std::ios::binary);check(bool(file),"Cannot read test-owned save file.");
    return {std::istreambuf_iterator<char>(file),{}};
}
void replace(const fs::path& path,const std::string& bytes) {
    std::ofstream file(path,std::ios::binary|std::ios::trunc);
    check(bool(file),"Cannot open test-owned manifest.");
    file.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));file.close();
    check(file.good(),"Cannot replace test-owned manifest.");
}
struct Workspace {
    fs::path path;
    Workspace() {
        std::random_device random;
        for(unsigned attempt=0;attempt<32;++attempt) {
            auto candidate=fs::temp_directory_path()/("poima-save-writer-"+std::to_string(random())+"-"+std::to_string(random()));
            if(fs::create_directory(candidate)) {path=fs::absolute(candidate);break;}
        }
        check(!path.empty(),"Cannot create unique writer test workspace.");
    }
    ~Workspace(){std::error_code ignored;fs::remove_all(path,ignored);}
};
struct Gate {
    std::mutex mutex;std::condition_variable wake;
    bool entered=false,released=false;
    void block() {
        std::unique_lock lock(mutex);entered=true;wake.notify_all();
        check(wake.wait_for(lock,5s,[&]{return released;}),"Writer test gate was not released.");
    }
    void await_entry() {
        std::unique_lock lock(mutex);
        check(wake.wait_for(lock,5s,[&]{return entered;}),"Writer did not enter the requested native boundary.");
    }
    void release() {std::lock_guard lock(mutex);released=true;wake.notify_all();}
};
struct Release {Gate& gate;~Release(){gate.release();}};
std::shared_ptr<jobs::Executor> executor(unsigned workers=1) {
    jobs::Config config;config.workers=workers;config.frame_reserved_workers=0;
    return std::make_shared<jobs::Executor>(config);
}
GameplaySaveWriteInput input(const fs::path& path,unsigned sequence=1) {
    GameplaySaveWriteInput value;value.path=path;value.bytes=std::string("saved\0opaque\xff",13);
    value.content_hash=hash("immutable authored content");
    auto& request=value.request;request.ticket={{0x123456789abcdef0ULL,0xfedcba9876543210ULL},sequence};
    request.kind=poima::GameplaySaveKind::save;
    const std::string slot="writer-test";std::copy(slot.begin(),slot.end(),request.slot.begin());
    request.configuration_generation=7;request.requested_tick=41;request.committed_tick=42;request.committed=true;
    return value;
}
GameplaySaveWriteResult await(GameplaySaveWriter& writer) {
    const auto deadline=std::chrono::steady_clock::now()+5s;
    while(std::chrono::steady_clock::now()<deadline) {
        if(const auto* result=writer.poll())return *result;
        std::this_thread::sleep_for(1ms);
    }
    throw std::runtime_error("Writer did not settle within the bounded test deadline.");
}
void clean(const std::shared_ptr<jobs::Executor>& pool) {
    const auto status=pool->status();
    check(status.background.groups==0 && status.background.tasks==0 && status.executing==0,
          "Writer left native background work active.");
}
std::map<std::string,std::string> files(const fs::path& path) {
    std::map<std::string,std::string> result;
    for(const auto& item:fs::directory_iterator(path))if(item.is_regular_file())
        result.emplace(item.path().filename().string(),read(item.path()));
    return result;
}
void gated_capture(const fs::path& root) {
    Gate gate;auto pool=executor();
    GameplaySaveWriter writer(pool,[&](saves::Boundary boundary) {
        if(boundary==saves::Boundary::payload_flushed)gate.block();
    });
    Release release{gate};auto original=input(root/"captured");
    const auto bytes=original.bytes;const auto request=original.request;
    writer.submit(original);gate.await_entry();
    check(writer.busy(),"Queued writer did not retain admission.");
    const auto before=std::chrono::steady_clock::now();
    for(unsigned iteration=0;iteration<64;++iteration)check(writer.poll()==nullptr,"Running writer exposed a partial result.");
    check(std::chrono::steady_clock::now()-before<1s,"Memory-only poll waited on the gated store.");
    rejects([&]{writer.submit(input(root/"wrong-slot",2));},"Busy writer admitted another checkpoint.");
    rejects([&]{writer.clear();},"Running writer was released prematurely.");
    rejects([&]{writer.submit_resolution();},"Running writer admitted a resolution job.");
    original.bytes="caller mutation";original.path=root/"changed-path";
    original.content_hash=hash("changed content");original.request.ticket.sequence=999;
    gate.release();const auto result=await(writer);
    check(result.state==GameplaySaveState::succeeded && result.completion.succeeded &&
          result.completion.generation==1 && result.request==request && result.path==root/"captured" &&
          result.started && result.generation_chosen && result.expected_generation==0,
          "Writer did not publish the immutable captured request.");
    check(saves::Store(root/"captured").read().bytes==bytes && !fs::exists(original.path),
          "Caller mutation changed captured bytes or redirected storage.");
    const auto receipt=saves::Store(result.path).lookup(result.operation,result.hash);
    check(receipt && receipt->committed.generation==1,
          "Result identity does not identify its durable receipt.");
    check(writer.poll()==writer.poll() && writer.busy(),"Terminal result lost stable retained ownership.");
    writer.clear();check(!writer.busy() && !writer.poll(),"Terminal clear did not release memory.");clean(pool);
}
void errors_and_receipts(const fs::path& root) {
    auto pool=executor();
    {
        const auto path=root/"conflict";saves::Store(path).write(0,id(1),"original");
        GameplaySaveWriter writer(pool);auto value=input(path);
        value.request.has_expected_generation=true;value.request.expected_generation=0;
        writer.submit(value);const auto result=await(writer);
        check(result.state==GameplaySaveState::failed && result.completion.error_code==-32009 &&
              result.started && result.generation_chosen && result.expected_generation==0,
              "Generation conflict was not a definitive stale-save failure.");
        check(saves::Store(path).read().bytes=="original" && !saves::Store(path).lookup(result.operation,result.hash),
              "Generation conflict changed the checkpoint or fabricated a receipt.");writer.clear();
    }
    for(const auto boundary:{saves::Boundary::payload_flushed,saves::Boundary::manifest_replaced}) {
        const bool durable=boundary==saves::Boundary::manifest_replaced;
        const auto path=root/(durable?"after-publication":"before-publication");
        saves::Store(path).write(0,id(1),"original");std::atomic<unsigned> injected=0;
        GameplaySaveWriter writer(pool,[&](saves::Boundary point) {
            if(point==boundary) {++injected;throw std::runtime_error("controlled native store fault");}
        });
        const auto value=input(path);writer.submit(value);const auto result=await(writer);
        check(injected==1,"Real store did not reach the injected fault boundary.");
        const auto stored=saves::Store(path).read();
        check(result.state==(durable?GameplaySaveState::succeeded:GameplaySaveState::failed) &&
              result.completion.succeeded==durable && result.expected_generation==1,
              "Writer confused exception with durable publication.");
        check(stored.bytes==(durable?value.bytes:"original") && stored.status.generation==(durable?2u:1u),
              "Store exception exposed incorrect authoritative bytes.");
        const auto receipt=saves::Store(path).lookup(result.operation,result.hash);
        check(bool(receipt)==durable && (!durable || result.completion.generation==receipt->committed.generation),
              "Writer result differs from actual receipt history.");writer.clear();
    }
    clean(pool);
}
void retained_resolution(const fs::path& root) {
    const auto path=root/"resolution";saves::Store(path).write(0,id(1),"original");
    auto pool=executor();std::string committed_manifest;std::atomic<unsigned> writes=0;
    check(!pool->config().trace_always,"Attribution proof requires normally inactive tracing.");
    const auto native_thread=[]() -> std::uint64_t {
#ifdef _WIN32
        return GetCurrentThreadId();
#elif defined(__linux__)
        return static_cast<std::uint64_t>(syscall(SYS_gettid));
#else
        return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
    };
    const auto owner_thread=native_thread();std::uint64_t writing_thread=0;
    GameplaySaveWriter writer(pool,[&](saves::Boundary point) {
        if(point==saves::Boundary::manifest_replaced) {
            writing_thread=native_thread();
            ++writes;committed_manifest=read(path/"current.json");
            replace(path/"current.json","{unreadable current");
            replace(path/"previous.json","{unreadable previous");
            throw std::runtime_error("durable receipt temporarily inaccessible");
        }
    });
    auto value=input(path);
    const auto session=id(7);
    std::copy(session.begin(),session.end(),value.attribution.session.begin());
    value.attribution.tick=123;value.attribution.parent=991;
    value.attribution.recording_generation=7;value.attribution.source=17;
    const auto captured_attribution=value.attribution;
    writer.submit(value);
    // Caller changes must not change the worker's original attribution or the
    // attribution later used to resolve that same durable receipt.
    value.attribution={};
    const auto uncertain=await(writer);
    check(uncertain.state==GameplaySaveState::resolving && uncertain.started && uncertain.generation_chosen && writes==1,
          "Unknown receipt was incorrectly declared terminal.");
    rejects([&]{writer.clear();},"Unknown operation identity was discarded.");
    rejects([&]{writer.submit(input(root/"replacement",2));},"Unresolved writer recaptured another checkpoint.");
    replace(path/"current.json",committed_manifest);const auto before=files(path);
    writer.submit_resolution();const auto recovered=await(writer);
    check(recovered.state==GameplaySaveState::succeeded && recovered.completion.generation==2 &&
          recovered.request==uncertain.request && recovered.path==uncertain.path &&
          recovered.operation==uncertain.operation && recovered.hash==uncertain.hash &&
          recovered.expected_generation==uncertain.expected_generation && writes==1,
          "Resolution changed operation identity, generation or recaptured data.");
    check(files(path)==before && saves::Store(path).read().bytes==value.bytes,
          "Receipt-only resolution rewrote stored bytes.");
    const auto traces=pool->drain_traces();
    check(traces.dropped==0 && traces.records.size()==2,
          "Write and receipt resolution did not retain exactly two attributed worker traces.");
    bool saw_write=false,saw_resolution=false;std::uint64_t first_group=0;
    for(const auto& trace:traces.records) {
        const std::string name(trace.name.data());
        if(name=="save.write.durable") {check(!saw_write,"Duplicate write trace.");saw_write=true;}
        else if(name=="save.resolve.receipt") {check(!saw_resolution,"Duplicate resolution trace.");saw_resolution=true;}
        else check(false,"Unexpected traced task in isolated writer proof.");
        check(trace.group!=0 && (!first_group || trace.group!=first_group) &&
              trace.thread!=0 && trace.thread==writing_thread && trace.thread!=owner_thread && trace.task==0 &&
              trace.lane==jobs::Lane::background && trace.outcome==jobs::Outcome::succeeded,
              "Attributed work did not execute on distinct real background worker groups.");
        first_group=trace.group;
        check(trace.attribution.session==captured_attribution.session &&
              trace.attribution.tick==captured_attribution.tick &&
              trace.attribution.parent==captured_attribution.parent &&
              trace.attribution.recording_generation==captured_attribution.recording_generation &&
              trace.attribution.source==captured_attribution.source,
              "Write or receipt-resolution trace changed the captured checkpoint attribution.");
    }
    check(saw_write && saw_resolution,"Missing write or receipt-resolution trace.");
    writer.clear();clean(pool);
}
// The release helper observes actual shutdown admission before permitting a
// blocked native operation. Every path releases/joins; no detached watchdog.
struct ShutdownRelease {
    Gate& gate;std::shared_ptr<jobs::Executor> pool;
    std::atomic<bool> stopping_seen=false;std::thread thread;
    ShutdownRelease(Gate& value,std::shared_ptr<jobs::Executor> executor)
        :gate(value),pool(std::move(executor)),thread([this] {
            const auto deadline=std::chrono::steady_clock::now()+3s;
            while(std::chrono::steady_clock::now()<deadline) {
                if(pool->status().stopping) {stopping_seen=true;break;}
                std::this_thread::sleep_for(1ms);
            }
            gate.release();
        }) {}
    ~ShutdownRelease(){gate.release();if(thread.joinable())thread.join();}
};
void cancellation(const fs::path& root,bool durable) {
    Gate gate;auto pool=executor();
    jobs::Group blocker;
    if(!durable) {
        blocker=pool->submit({jobs::Task{[&](jobs::Context&){gate.block();},{},"save.test.blocker"}},jobs::Lane::background);
        gate.await_entry();
    }
    GameplaySaveWriter writer(pool,[&](saves::Boundary point) {
        if(durable && point==saves::Boundary::manifest_replaced)gate.block();
    });
    Release release{gate};const auto value=input(root/(durable?"cancel-durable":"cancel-queued"));
    writer.submit(value);if(durable)gate.await_entry();
    check(writer.poll()==nullptr,"Cancellation test did not retain outstanding work.");
    {ShutdownRelease cancel_release(gate,pool);pool->shutdown();
     check(cancel_release.stopping_seen,"Gate release preceded actual executor shutdown.");}
    const auto result=await(writer);
    if(durable) {
        check(result.state==GameplaySaveState::succeeded && result.started && result.completion.generation==1 &&
              saves::Store(value.path).read().bytes==value.bytes,
              "Generic cancellation hid an actually published checkpoint.");
    }else {
        check(result.state==GameplaySaveState::failed && !result.started && !result.generation_chosen &&
              result.completion.error_code==-32070 && !fs::exists(value.path),
              "Cancelled queued write entered storage or fabricated a generation.");
        check(blocker.poll().terminal(),"Controlled sibling did not retire during shutdown.");
    }
    writer.clear();clean(pool);
}
void destructor_group_boundary(const fs::path& root) {
    Gate gate;auto pool=executor();
    auto writer=std::make_unique<GameplaySaveWriter>(pool);
    writer->submit(input(root/"destructor-complete"));check(await(*writer).state==GameplaySaveState::succeeded,"Destructor fixture failed to save.");
    const auto sibling=pool->submit({jobs::Task{[&](jobs::Context&){gate.block();},{},"save.test.unrelated"}},jobs::Lane::background);
    Release release{gate};gate.await_entry();const auto started=std::chrono::steady_clock::now();
    writer.reset();
    check(std::chrono::steady_clock::now()-started<1s && sibling.poll().state==jobs::State::running,
          "Writer destruction waited for or cancelled an unrelated group.");
    gate.release();check(pool->wait(sibling).snapshot.state==jobs::State::succeeded,"Unrelated group failed after writer destruction.");clean(pool);
    Gate own;std::atomic<bool> hook_completed=false,destruction_entered=false;
    std::atomic<bool> destruction_finished=false,returned_before_release=false;
    auto unfinished=std::make_unique<GameplaySaveWriter>(pool,[&](saves::Boundary point) {
        if(point==saves::Boundary::payload_flushed) {own.block();hook_completed=true;}
    });
    Release release_own{own};const auto value=input(root/"destructor-outstanding");unfinished->submit(value);own.await_entry();
    std::thread permit([&] {
        const auto deadline=std::chrono::steady_clock::now()+3s;
        while(!destruction_entered && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
        // A deliberately outstanding own group distinguishes joining from
        // merely releasing a shared pointer. This is a gate, not a speed test.
        std::this_thread::sleep_for(30ms);
        returned_before_release=destruction_finished.load();
        own.release();
    });
    struct Join {Gate& gate;std::thread& thread;~Join(){gate.release();if(thread.joinable())thread.join();}} join{own,permit};
    destruction_entered=true;unfinished.reset();destruction_finished=true;
    if(permit.joinable())permit.join();
    check(!returned_before_release && hook_completed && saves::Store(value.path).read().bytes==value.bytes,
          "Writer destructor returned before its own native write completed.");clean(pool);
}
void rejected_admission(const fs::path& root) {
    auto pool=executor();GameplaySaveWriter writer(pool);
    for(unsigned kind=0;kind<5;++kind) {
        auto value=input(root/"invalid");
        if(kind==0)value.request.committed=false;
        if(kind==1)value.request.kind=poima::GameplaySaveKind::load;
        if(kind==2)value.content_hash="bad";
        if(kind==3)value.path="relative-slot";
        if(kind==4)value.request.ticket.sequence=0;
        rejects<std::invalid_argument>([&]{writer.submit(value);},"Invalid save input reached worker admission.");
        check(!writer.busy() && !writer.poll() && !fs::exists(root/"invalid"),"Rejected input consumed writer state or created storage.");
    }
    rejects([&]{writer.clear();},"Absent writer result could be cleared.");
    rejects([&]{writer.submit_resolution();},"Absent writer result admitted resolution.");clean(pool);
    GameplaySaveWriter serial(executor(0));const auto value=input(root/"serial");serial.submit(value);
    check(serial.poll() && serial.poll()->state==GameplaySaveState::succeeded && saves::Store(value.path).read().bytes==value.bytes,
          "Explicit serial executor changed save semantics.");serial.clear();
}
}
int main() {
    try {
        Workspace workspace;
        rejected_admission(workspace.path);gated_capture(workspace.path);errors_and_receipts(workspace.path);
        retained_resolution(workspace.path);cancellation(workspace.path,false);cancellation(workspace.path,true);
        destructor_group_boundary(workspace.path);
        std::cout<<"gameplay_save_writer: admission, capture, faults, receipts, cancellation and owner boundaries pass\n";
        return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
