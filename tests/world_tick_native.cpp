// SPDX-License-Identifier: Apache-2.0
#include "poima/world.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>
namespace fs=std::filesystem;
using Json=nlohmann::json;
using namespace poima;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
template<class F> void rejects(F action,const char* message) {
    try { action(); }catch(const std::exception&) { return; }throw std::runtime_error(message);
}
std::string id(unsigned number) { std::ostringstream stream;stream<<std::hex<<std::setw(32)<<std::setfill('0')<<number;return stream.str(); }
std::string text(const fs::path& path) { const auto s=path.u8string();return {s.begin(),s.end()}; }
std::string bytes(const fs::path& path) { std::ifstream stream(path,std::ios::binary);return {std::istreambuf_iterator<char>(stream),{}}; }
Json call(WorldSession& session,const char* method,const Json& params=Json::object()) {
    const auto response=Json::parse(session.request(Json{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",params}}.dump()));
    if(response.contains("error"))throw std::runtime_error(response.at("error").dump());return response.at("result");
}
Json transform(Json position,Json scale=Json::array({1,1,1})) { return {{"position",position},{"rotation",{0,0,0,1}},{"scale",scale}}; }
Json collider(const char* motion="static") { return {{"half_extents",{.5,.5,.5}},{"motion",motion},{"mass",1},{"friction",.5},{"restitution",0}}; }
void component(Json& ops,unsigned entity,const char* type,Json value) {
    ops.push_back({{"op","component.set"},{"id",id(entity)},{"type",type},{"value",std::move(value)}});
}
void create(Json& ops,unsigned entity,const char* name,Json parent=nullptr) {
    ops.push_back({{"op","entity.create"},{"id",id(entity)},{"name",name},{"parent",std::move(parent)}});
}
void author(WorldSession& session) {
    Json ops=Json::array();
    create(ops,1,"Floor");component(ops,1,"Transform",transform({0,-.5,0},{20,1,20}));component(ops,1,"BoxCollider",collider());
    create(ops,2,"Player");component(ops,2,"Transform",transform({0,1,2}));
    create(ops,3,"Camera",id(2));component(ops,3,"Transform",transform({0,1.6,0}));
    component(ops,3,"Camera",{{"vertical_fov",60},{"near",.1},{"far",1000}});
    component(ops,2,"CharacterController",{{"radius",.3},{"height",1.8},{"speed",4},{"jump_speed",5},{"camera",id(3)}});
    create(ops,4,"Falling box");component(ops,4,"Transform",transform({2,3,0}));component(ops,4,"BoxCollider",collider("dynamic"));
    call(session,"world.transact",{{"request_id",id(500)},{"base_revision",0},{"ops",ops}});
}
Json entity(WorldSession& session,unsigned entity_id,std::uint64_t tick) {
    return call(session,"runtime.entity",{{"session_id",id(900)},{"id",id(entity_id)},{"tick",tick}});
}
void parity_and_guards(const fs::path& directory) {
    const auto first_path=directory/"typed.json",second_path=directory/"rpc.json";
    WorldSession typed(text(first_path)),rpc(text(second_path));author(typed);author(rpc);
    rejects([&]{(void)typed.advance_tick(id(900),0);},"Missing runtime accepted.");
    if(!Runtime::available()) {
        call(typed,"session.close");rejects([&]{(void)typed.advance_tick(id(900),0);},"Closed session accepted.");return;
    }
    call(typed,"runtime.start",{{"session_id",id(900)},{"revision",1}});
    call(rpc,"runtime.start",{{"session_id",id(900)},{"revision",1}});
    const auto authored=bytes(first_path);const auto history=call(typed,"world.history");
    rejects([&]{(void)typed.advance_tick("invalid",0);},"Malformed runtime identity accepted.");
    rejects([&]{(void)typed.advance_tick(id(901),0);},"Wrong runtime identity accepted.");
    rejects([&]{(void)typed.advance_tick(id(900),1);},"Future tick accepted.");
    rejects([&]{(void)typed.advance_tick(id(900),UINT64_MAX);},"Oversized tick accepted.");
    RuntimeInput invalid;invalid.entity=id(2);
    rejects([&]{(void)typed.advance_tick(id(900),0,std::vector<RuntimeInput>(33,invalid));},"Oversized native input array accepted.");
    invalid.look[0]=std::numeric_limits<float>::quiet_NaN();
    rejects([&]{(void)typed.advance_tick(id(900),0,{invalid});},"Nonfinite native input accepted.");
    invalid.look={};invalid.entity="invalid";
    rejects([&]{(void)typed.advance_tick(id(900),0,{invalid});},"Malformed input identity accepted.");
    check(typed.runtime_status().tick==0 && entity(typed,4,0)==entity(rpc,4,0),"Rejected guards changed state.");
    typed.profiler().start(4096);
    {
        profiling::Binding owner(&typed.profiler(),profiling::Source::editor_poll);
        for(std::uint64_t tick=0;tick<120;++tick) {
            RuntimeInput input;input.entity=id(2);input.move={0,.35f};input.look={.25f,0};input.jump=tick==60;input.use=tick==12;
            const auto advanced=typed.advance_tick(id(900),tick,{input});
            check(advanced.committed_tick==tick+1 && advanced.current_tick==tick+1 && !advanced.replaced && !advanced.save_serviced,"Typed tick returned wrong committed boundary.");
            const Json raw={{"entity",input.entity},{"move",input.move},{"look",input.look},{"jump",input.jump},{"use",input.use}};
            call(rpc,"runtime.step",{{"session_id",id(900)},{"request_id",id(1000+static_cast<unsigned>(tick))},{"expected_tick",tick},{"ticks",1},{"inputs",Json::array({raw})}});
            for(unsigned e:{2,3,4})check(entity(typed,e,tick+1)==entity(rpc,e,tick+1),"Typed step differs from one-tick RPC physics/input state.");
        }
    }
    typed.profiler().stop();
    bool tick_seen=false;
    for(const auto& event:typed.profiler().events())if(std::string_view(event.name.data())=="runtime.tick") {
        tick_seen=true;check(event.source==profiling::Source::editor_poll && std::string_view(event.session.data())==id(900),"Typed step lost owner profiler context.");
    }
    check(tick_seen && typed.profiler().status().open==0 && typed.profiler().status().dropped==0,"Typed trace missing or truncated.");
    rejects([&]{(void)typed.advance_tick(id(900),119);},"Already-committed typed tick replayed.");
    check(typed.runtime_status().tick==120 && bytes(first_path)==authored && call(typed,"world.history")==history,"Automatic ticks changed authoring/history.");
    // Internal ticks never consume the public request receipt namespace.
    auto once=call(typed,"runtime.step",{{"session_id",id(900)},{"request_id",id(1000)},{"expected_tick",120},{"ticks",1}});
    auto retry=call(typed,"runtime.step",{{"session_id",id(900)},{"request_id",id(1000)},{"expected_tick",120},{"ticks",1}});
    check(!once.at("replayed").get<bool>() && retry.at("replayed").get<bool>() && typed.runtime_status().tick==121,"Typed steps polluted explicit RPC receipts.");
    // More than the 32-RPC receipt retention window must not evict an explicit
    // operation merely because the native owner keeps automatic Play running.
    for(std::uint64_t tick=121;tick<161;++tick)(void)typed.advance_tick(id(900),tick);
    retry=call(typed,"runtime.step",{{"session_id",id(900)},{"request_id",id(1000)},{"expected_tick",120},{"ticks",1}});
    check(retry.at("replayed").get<bool>() && retry.at("tick")==121 && typed.runtime_status().tick==161,
        "Automatic ticks evicted an explicit receipt or replay advanced state.");
    call(typed,"session.close");rejects([&]{(void)typed.advance_tick(id(900),161);},"Closed active session advanced.");
}
void failed_tick_rollback(const fs::path& directory) {
    if(!Runtime::available())return;
    const auto path=directory/"capacity.json";WorldSession world(text(path));
    Json ops=Json::array();std::uint64_t revision=0;
    // Same proven post-Jolt-update capacity fixture as runtime_native.cpp.
    // Batches of <=128 operations respect the authoring transaction bound.
    for(unsigned i=0;i<150;++i) {
        create(ops,10+i,"Overlapping dynamic body");component(ops,10+i,"BoxCollider",collider("dynamic"));
        if(ops.size()==128 || i==149) {
            call(world,"world.transact",{{"request_id",id(500+static_cast<unsigned>(revision))},{"base_revision",revision},{"ops",ops}});++revision;ops=Json::array();
        }
    }
    const auto authored=bytes(path);
    call(world,"runtime.start",{{"session_id",id(900)},{"revision",revision}});
    std::vector<Json> before;for(unsigned i=0;i<150;++i)before.push_back(entity(world,10+i,0));
    world.profiler().start(4096);
    rejects([&]{(void)world.advance_tick(id(900),0);},"Physics capacity fixture unexpectedly succeeded.");
    world.profiler().stop();
    check(world.runtime_status().tick==0,"Failed automatic tick was committed.");
    for(unsigned i=0;i<150;++i)check(entity(world,10+i,0)==before[i],"Failed automatic tick retained partial body changes.");
    bool rollback=false,failed=false;
    for(const auto& event:world.profiler().events()) {
        if(std::string_view(event.name.data())=="runtime.rollback")rollback=true;
        if(std::string_view(event.name.data())=="runtime.batch" && event.failed)failed=true;
    }
    check(rollback && failed && bytes(path)==authored,"Failure did not exercise native rollback or changed authored bytes.");
}
}
int main() {
    static_assert(std::is_trivially_copyable_v<WorldTickAdvance>);
    const auto root=fs::temp_directory_path()/("poima-world-tick-"+std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    try {
        fs::create_directory(root);parity_and_guards(root);failed_tick_rollback(root);fs::remove_all(root);
        std::cout<<(Runtime::available() ? "Typed world tick guards, RPC parity, receipt independence, profiling and post-physics rollback passed.\n":"Typed world tick unavailable/closed guards passed; simulation not built.\n");return 0;
    }catch(const std::exception& error) { std::cerr<<error.what()<<"; fixture="<<text(root)<<'\n';return 1; }
}
