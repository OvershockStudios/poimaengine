// SPDX-License-Identifier: Apache-2.0
#include "poima/world.hpp"
#include "poima/jobs.hpp"
#include "poima/save_store.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
using Json=nlohmann::json;
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::string text(const fs::path& path) {const auto u=path.u8string();return {u.begin(),u.end()};}
std::string id(unsigned value) {std::ostringstream out;out<<std::hex<<std::setw(32)<<std::setfill('0')<<value;return out.str();}
std::string bytes(const fs::path& path) {std::ifstream in(path,std::ios::binary);check(bool(in),"Cannot read authored fixture.");return {std::istreambuf_iterator<char>(in),{}};}
struct Policy {
    std::optional<std::string> previous;
    explicit Policy(const char* value) {
        if(const auto* p=std::getenv("POIMA_JOB_WORKERS"))previous=p;
        set(value);
    }
    static void set(const char* value) {
#ifdef _WIN32
        check(_putenv_s("POIMA_JOB_WORKERS",value ? value : "")==0,"Cannot set worker policy.");
#else
        check((value ? setenv("POIMA_JOB_WORKERS",value,1) : unsetenv("POIMA_JOB_WORKERS"))==0,"Cannot set worker policy.");
#endif
    }
    ~Policy() noexcept {try {set(previous ? previous->c_str() : nullptr);}catch(...) {std::abort();}}
};
struct Rpc {
    poima::WorldSession& world;unsigned next=1000;
    std::string receipt() {return id(next++);}
    Json response(const char* method,const Json& params=Json::object()) {
        const auto correlation=next++;
        auto r=Json::parse(world.request(Json{{"jsonrpc","2.0"},{"id",correlation},{"method",method},{"params",params}}.dump()));
        check(r.at("id")==correlation,"Native response correlation differs.");return r;
    }
    Json call(const char* method,const Json& params=Json::object()) {
        const auto r=response(method,params);if(r.contains("error"))throw std::runtime_error(r.at("error").dump());return r.at("result");
    }
    void reject(const char* method,const Json& params,int code,const char* message) {
        const auto r=response(method,params);
        check(r.contains("error") && r.at("error").at("code")==code,"Unexpected rejection code.");
        check(r.at("error").at("message").get<std::string>().find(message)!=std::string::npos,"Wrong native rejection boundary.");
    }
};
// Occupy the only background-eligible worker, leaving the reserved frame worker
// free. Release before World destruction on every failure path. The callback
// has its own finite deadline; this is a controlled scheduling oracle, not a
// filesystem latency benchmark or a promise to forcibly cancel storage IO.
class Gate {
    std::shared_ptr<poima::jobs::Executor> executor_;
    std::mutex mutex_;std::condition_variable cv_;bool entered_=false,released_=false;
    std::atomic<bool> expired_{false};poima::jobs::Group group_;
public:
    explicit Gate(std::shared_ptr<poima::jobs::Executor> executor):executor_(std::move(executor)) {
        group_=executor_->submit({{[this](poima::jobs::Context& context) {
            std::unique_lock lock(mutex_);entered_=true;cv_.notify_all();
            const auto end=Clock::now()+std::chrono::seconds(5);
            while(!released_ && !context.cancel_requested()) {
                if(cv_.wait_until(lock,end)==std::cv_status::timeout && !released_) {expired_=true;break;}
            }
        },{},"save.test.background_gate"}},poima::jobs::Lane::background);
        try {
            std::unique_lock lock(mutex_);
            check(cv_.wait_for(lock,std::chrono::seconds(2),[&]{return entered_;}),"Background gate never started.");
        }catch(...) {
            // A failed constructor has no destructor. Fence its submitted
            // callback before releasing the mutex/condition-variable storage.
            release();group_.cancel();executor_->wait(group_);throw;
        }
    }
    void release() noexcept { {std::lock_guard lock(mutex_);released_=true;}cv_.notify_all();}
    void healthy() const {check(!expired_ && !group_.poll().terminal(),"Background gate expired or retired prematurely.");}
    void finish() {release();executor_->wait(group_).rethrow();check(!expired_,"Background gate expired before release.");}
    ~Gate() noexcept {release();try {executor_->wait(group_);}catch(...) {std::abort();}}
};
Json configure(Rpc& rpc,const fs::path& root,std::uint64_t generation=0,const char* mode=nullptr) {
    Json p={{"request_id",rpc.receipt()},{"expected_generation",generation},{"root",text(root)}};
    if(mode)p["write_mode"]=mode;
    return p;
}
void author(Rpc& rpc) {
    rpc.call("world.transact",{{"request_id",rpc.receipt()},{"base_revision",0},{"ops",Json::array({
        {{"op","entity.create"},{"id",id(1)},{"name","Falling checkpoint body"}},
        {{"op","component.set"},{"id",id(1)},{"type","Transform"},{"value",{
            {"position",{0,10,0}},{"rotation",{0,0,0,1}},{"scale",{1,1,1}}}}},
        {{"op","component.set"},{"id",id(1)},{"type","BoxCollider"},{"value",{
            {"half_extents",{.5,.5,.5}},{"motion","dynamic"},{"mass",1},{"friction",.5},{"restitution",0}}}}})}});
}
Json game(Rpc& rpc,const std::string& session) {return rpc.call("runtime.gameplay.inspect",{{"session_id",session},{"include_schema",true}});}
const char* load_method(const Json& config) {return config.contains("descriptor") ? "runtime.gameplay.load_native" : "runtime.gameplay.load";}
Json load_parameters(Rpc& rpc,const Json& config,const std::string& session,unsigned tick,const Json& revision) {
    auto params=config;
    params["session_id"]=session;params["request_id"]=rpc.receipt();
    params["expected_tick"]=tick;params["expected_revision"]=revision;
    return params;
}
void start(Rpc& rpc,const Json& config,const std::string& session,long throw_tick=-1) {
    rpc.call("runtime.start",{{"session_id",session},{"revision",1}});
    rpc.call(load_method(config),load_parameters(rpc,config,session,0,0));
    const auto observed=game(rpc,session);
    check(observed.at("module").at("schema").at("identity")=="poima.test.gameplay-save-requests","Wrong compiled save fixture.");
    rpc.call("runtime.gameplay.edit",{{"session_id",session},{"request_id",rpc.receipt()},
        {"expected_tick",0},{"expected_revision",observed.at("revision")},{"values",{
            {"Mode",1},{"TriggerTick","0"},{"ThrowTick",std::to_string(throw_tick)},{"ExpectedGeneration","-1"}}}});
}
Json step(Rpc& rpc,const std::string& session,unsigned tick) {
    return {{"session_id",session},{"request_id",rpc.receipt()},{"expected_tick",tick},{"ticks",1}};
}
Json entity(Rpc& rpc,const std::string& session) {auto r=rpc.call("runtime.entity",{{"session_id",session},{"id",id(1)}});r.erase("session_id");return r;}
void zero_workers(const fs::path& root) {
    Policy policy("0");auto executor=poima::jobs::owner_executor();
    check(executor->config().workers==0,"Zero-worker test did not own the serial pool.");
    poima::WorldSession world(text(root/"serial.json"));Rpc rpc{world};
    const auto before=rpc.call("save.status");
    rpc.reject("save.configure",configure(rpc,root,0,"background"),-32070,"at least one job worker");
    check(rpc.call("save.status")==before,"Rejected async configuration changed serial storage routing.");
    rpc.call("session.close");
}
void synchronous_default(const fs::path& root,const Json& config) {
    fs::create_directory(root/"sync");poima::WorldSession world(text(root/"sync.json"));Rpc rpc{world};
    check(rpc.call("save.status").at("write_mode")=="synchronous","Default write mode changed.");
    const auto schema=rpc.call("world.describe",{{"view","method"},{"name","save.configure"}});
    check(schema.at("schema_revision").get<unsigned>()>=76,"Background save discovery did not invalidate cached schemas.");
    check(schema.at("methods").at("save.configure").at("properties").at("write_mode").at("enum")==Json::array({"synchronous","background"}),"Write-mode discovery differs.");
    const auto unconfigured=rpc.call("save.status");
    for(const auto& value:Json::array({nullptr,1,true,Json::array(),Json::object()})) {
        auto malformed=configure(rpc,root/"sync");malformed["write_mode"]=value;
        rpc.reject("save.configure",malformed,-32602,"write_mode");
        check(rpc.call("save.status")==unconfigured,"Malformed write mode changed storage routing.");
    }
    rpc.reject("save.configure",configure(rpc,root/"sync",0,"unknown"),-32602,"write_mode");
    check(rpc.call("save.status")==unconfigured,"Invalid write mode changed storage routing.");
    const auto params=configure(rpc,root/"sync"),configured=rpc.call("save.configure",params);
    auto replayed=rpc.call("save.configure",params);
    check(replayed.at("replayed")==true,"Default configuration retry was not a receipt replay.");replayed["replayed"]=false;
    check(replayed==configured,"Default-mode configuration retry differs.");
    auto explicit_default=params;explicit_default["write_mode"]="synchronous";
    replayed=rpc.call("save.configure",explicit_default);
    check(replayed.at("replayed")==true,"Explicit default configuration was not a receipt replay.");replayed["replayed"]=false;
    check(replayed==configured,"Omitted and explicit default modes have different receipt identities.");
    const auto configured_status=rpc.call("save.status");
    auto reused=params;reused["write_mode"]="background";
    rpc.reject("save.configure",reused,-32010,"request ID reused");
    check(rpc.call("save.status")==configured_status,"Changed-mode receipt reuse altered storage routing.");
    author(rpc);const auto session=id(10);start(rpc,config,session);
    const auto r=rpc.call("runtime.step",step(rpc,session,0));
    check(r.at("save_serviced")==true && r.at("save_operation").at("state")==3 && r.at("save_operation").at("committed_tick")==1,"Default Save stopped committing synchronously.");
    rpc.call("runtime.stop",{{"session_id",session}});rpc.call("session.close");
}
void background(const fs::path& root,const Json& config) {
    const auto world_path=root/"background.json",storage=root/"background";fs::create_directory(storage);
    Json saved_values,saved_entity,live_values,live_entity;
    auto executor=poima::jobs::owner_executor();
    check(executor->config().workers==2 && executor->config().frame_reserved_workers==1,"Controlled test requires two workers and one frame reservation.");
    {
        poima::WorldSession world(text(world_path));Rpc rpc{world};
        rpc.call("save.configure",configure(rpc,storage,0,"background"));author(rpc);
        const auto authored=bytes(world_path),session=id(20);start(rpc,config,session,2);
        Gate gate(executor);
        const auto capture=id(40);
        rpc.call("profiler.start",{{"capture_id",capture},{"expected_capture_id",nullptr},{"capacity",2048}});
        const auto first_params=step(rpc,session,0),first=rpc.call("runtime.step",first_params);
        gate.healthy();
        check(first.at("tick")==1 && first.at("save_serviced")==true && !first.at("runtime_replaced").get<bool>() &&
            first.at("save_operation").at("state")==1 && first.at("save_operation").at("committed_tick")==1,"Save did not return queued at its captured boundary.");
        saved_values=game(rpc,session).at("module").at("values");saved_entity=entity(rpc,session);
        check(saved_values.at("Ticks")==1 && saved_values.at("BusyRejection")==2 && !fs::exists(storage/"slot-quick"),"Compiled Save did not remain queued behind the held worker.");
        const Json ticket={{"epoch",first.at("save_operation").at("epoch")},{"sequence",first.at("save_operation").at("sequence")}};
        const auto later=rpc.call("runtime.step",step(rpc,session,1));gate.healthy();
        check(later.at("tick")==2 && !later.at("save_serviced").get<bool>() && later.at("save_operation").at("committed_tick")==1,"Continued Tick recaptured the save or reset storage timing.");
        live_values=game(rpc,session).at("module").at("values");live_entity=entity(rpc,session);
        check(live_values.at("Ticks")==2 && live_entity.at("world_matrix")!=saved_entity.at("world_matrix"),"Live simulation did not move past the captured checkpoint.");
        const auto before=rpc.call("runtime.inspect",{{"session_id",session}});
        rpc.reject("runtime.step",step(rpc,session,2),-32040,"Fixture later-tick failure");
        check(rpc.call("runtime.inspect",{{"session_id",session}})==before && game(rpc,session).at("module").at("values")==live_values && entity(rpc,session)==live_entity,"Throwing continued Tick did not roll back complete live state.");
        const auto revision=game(rpc,session).at("revision");
        rpc.reject("save.configure",configure(rpc,storage,1,"synchronous"),-32070,"pending gameplay save");
        rpc.reject("runtime.stop",{{"session_id",session}},-32070,"pending gameplay save");
        const Json edit={{"session_id",session},{"request_id",rpc.receipt()},{"expected_tick",2},{"expected_revision",revision},{"values",{{"Mode",0}}}};
        rpc.reject("runtime.gameplay.edit",edit,-32060,"pending gameplay save");
        rpc.reject(load_method(config),load_parameters(rpc,config,session,2,revision),-32060,"pending gameplay save");
        rpc.reject("save.load",{{"request_id",rpc.receipt()},{"configuration_generation",1},{"slot","quick"},{"expected_generation",1},
            {"revision",1},{"expected_session_id",session},{"expected_tick",2},{"expected_gameplay_revision",revision},{"new_session_id",id(21)}},-32070,"pending gameplay save");
        auto replayed=rpc.call("runtime.step",first_params);
        check(replayed.at("replayed")==true,"Pending step retry was not a receipt replay.");replayed["replayed"]=false;
        check(replayed==first && game(rpc,session).at("module").at("values")==live_values && entity(rpc,session)==live_entity,"Accepted step retry reran Tick or changed its queued receipt.");
        check(rpc.call("runtime.save.result",ticket).at("state")==1 && bytes(world_path)==authored,"Pending guards discarded the save or rewrote authored content.");
        gate.finish();
        const auto deadline=Clock::now()+std::chrono::seconds(5);
        while(executor->status().background.groups) {
            check(Clock::now()<deadline,"Background durable write did not retire.");std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(!world.player_active() && !world.poll_player(),"Completion pump unexpectedly created a player.");
        // Collect the actual retired worker trace before sealing the capture.
        // Its attribution comes from the Save dispatch, not this later poll.
        rpc.call("jobs.status");
        const auto profile=rpc.call("profiler.stop",{{"capture_id",capture}});
        check(profile.at("dropped")==0 && profile.at("open")==0,"Durable worker profile did not close without loss.");
        const auto exported=rpc.call("profiler.export",{{"capture_id",capture}});
        std::uint64_t owner_thread=0;unsigned durable_events=0;
        for(const auto& row:exported.at("trace").at("traceEvents"))
            if(row.at("name")=="runtime.step" && row.at("ph")=="X")owner_thread=row.at("tid").get<std::uint64_t>();
        check(owner_thread!=0,"Profile omitted the actual owner Tick thread.");
        for(const auto& row:exported.at("trace").at("traceEvents")) {
            if(row.at("name")!="save.write.durable")continue;
            ++durable_events;const auto& args=row.at("args");
            check(row.at("ph")=="X" && row.at("cat")=="cpu" && !args.at("failed").get<bool>() &&
                args.at("source")=="request" && args.at("session")==session && args.at("tick")==1 &&
                args.at("job").at("lane")=="background" && args.at("job").at("group").get<std::uint64_t>()!=0 &&
                row.at("tid").get<std::uint64_t>()!=0 && row.at("tid").get<std::uint64_t>()!=owner_thread,
                "Durable write trace lost its dispatch session/tick or ran on the owner.");
        }
        check(durable_events==1,"Expected exactly one actual durable worker write span.");
        const auto completed=rpc.call("runtime.save.result",ticket);
        check(completed.at("state")==3 && completed.at("generation")==1 && completed.at("committed_tick")==1 &&
            rpc.call("runtime.save.status",{{"session_id",session}}).at("pending").is_null(),"Paused owner pump did not publish the original save completion.");
        check(game(rpc,session).at("module").at("values")==live_values && entity(rpc,session)==live_entity,"Completion pump advanced or rewound gameplay.");
        const auto stored=Json::parse(poima::saves::Store(storage/"slot-quick").read().bytes);
        check(stored.at("snapshot").at("payload").at("tick")==1 && stored.at("snapshot").at("payload").at("gameplay").at("values")==saved_values,"Durable worker wrote later gameplay instead of the immutable checkpoint.");
        replayed=rpc.call("runtime.step",first_params);
        check(replayed.at("replayed")==true,"Completed step retry was not a receipt replay.");replayed["replayed"]=false;
        check(replayed==first && bytes(world_path)==authored,"Completed write changed its original step receipt or authored source.");
        rpc.call("runtime.stop",{{"session_id",session}});rpc.call("session.close");
    }
    {
        poima::WorldSession fresh(text(world_path));Rpc rpc{fresh};rpc.call("save.configure",configure(rpc,storage));
        const auto session=id(30);
        const auto restored=rpc.call("save.load",{{"request_id",rpc.receipt()},{"configuration_generation",1},{"slot","quick"},{"expected_generation",1},
            {"revision",1},{"expected_session_id",nullptr},{"expected_tick",nullptr},{"expected_gameplay_revision",nullptr},
            {"new_session_id",session},{"gameplay",config}});
        check(restored.at("tick")==1 && game(rpc,session).at("module").at("values")==saved_values && entity(rpc,session)==saved_entity,
            "Fresh owner did not restore captured gameplay and body exactly.");
        rpc.call("runtime.stop",{{"session_id",session}});rpc.call("session.close");
    }
}
}
int main(int argc,char** argv) {
    const auto root=fs::temp_directory_path()/("poima-background-world-"+std::to_string(Clock::now().time_since_epoch().count()));
    try {
        const bool native=argc==3 && std::string(argv[1])=="--native";
        check(argc==4 || native,"Usage: world-gameplay-save-background-test HOSTFXR BRIDGE ASSEMBLY | --native DESCRIPTOR");
        Json config;
        if(native)config={{"descriptor",text(fs::canonical(argv[2]))}};
        else config={{"hostfxr",text(fs::canonical(argv[1]))},{"bridge",text(fs::canonical(argv[2]))},
            {"assembly",text(fs::canonical(argv[3]))},{"type","Poima.Tests.GameplaySaveProbe"}};
        check(fs::create_directory(root),"Cannot reserve world fixture directory.");
        zero_workers(root);
        {Policy policy("2");synchronous_default(root,config);background(root,config);}
        fs::remove_all(root);
        std::cout<<"Compiled "<<(native ? "NativeAOT" : "CoreCLR")<<" background World saves: three groups passed (serial rejection/default/discovery, compiled dispatch/continuation/guards/rollback/retries, idle pump and fresh exact restore).\n";return 0;
    }catch(const std::exception& e) {std::cerr<<e.what()<<"\nFixture retained: "<<root<<'\n';return 1;}
}
