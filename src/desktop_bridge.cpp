// SPDX-License-Identifier: Apache-2.0
#include "poima/desktop_bridge.h"
#include "poima/world.hpp"
#include "poima/local_session.hpp"
#include "poima/hosted_viewport.hpp"
#include "world_storage.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <windows.h>
namespace {
using namespace poima;
using Json=nlohmann::json;
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
constexpr std::uint64_t max_integer=9007199254740991ULL;
struct Failure : std::runtime_error { int code;Failure(int value,const std::string& message):std::runtime_error(message),code(value) {} };
void require(bool value,const std::string& message,int code=-32602) { if(!value)throw Failure(code,message); }
void fields(const Json& value,std::initializer_list<const char*> allowed,std::initializer_list<const char*> required={}) {
    require(value.is_object(),"Expected an object.");for(const auto& [name,unused]:value.items()) { (void)unused;require(std::find(allowed.begin(),allowed.end(),name)!=allowed.end(),"Unknown field: "+name); }
    for(const auto* name:required)require(value.contains(name),std::string("Missing field: ")+name);
}
std::uint64_t integer(const Json& value) { require(value.is_number_integer() && value>=0 && value<=max_integer,"Expected a safe nonnegative integer.");return value.get<std::uint64_t>(); }
std::string identifier(const Json& value) { require(value.is_string(),"Entity identity must be text.");const auto s=value.get<std::string>();require(s.size()==32 && s.find_first_not_of("0123456789abcdef")==std::string::npos,"Entity identity must be 32 lowercase hexadecimal characters.");return s; }
std::string bounded_string(const char* value,std::size_t limit,bool optional=false) {
    if(!value) { require(optional,"Required UTF-8 argument is null.");return {}; }
    std::size_t count=0;while(count<=limit && value[count])++count;require(count<=limit,"UTF-8 argument exceeds size limit.");return {value,count};
}
Json parse(const std::string& bytes) {
    require(bytes.size()<=local_session_request_limit,"Request exceeds 1 MiB.",-32700);std::vector<std::set<std::string>> keys;
    try { return Json::parse(bytes,[&](int depth,Json::parse_event_t event,Json& value) {
        require(depth<=64,"JSON nesting exceeds 64 levels.",-32700);
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        else if(event==Json::parse_event_t::key)require(!keys.empty() && keys.back().insert(value.get<std::string>()).second,"Duplicate JSON field.",-32700);
        else if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    }); }catch(const Json::exception&) { throw Failure(-32700,"Invalid JSON."); }
}
std::string error_reply(const Json& id,int code,const std::string& detail) { return Json{{"jsonrpc","2.0"},{"id",id},{"error",{{"code",code},{"message",detail}}}}.dump(); }
fs::path path_of(const std::string& value) { require(!value.empty() && value.find('\0')==std::string::npos,"Path must be nonempty and NUL-free.");return fs::path(std::u8string(value.begin(),value.end())); }
std::string path_text(const fs::path& path) { const auto value=path.u8string();return {reinterpret_cast<const char*>(value.data()),value.size()}; }
bool inside(const fs::path& child,const fs::path& parent) {
    auto a=child.begin();for(auto b=parent.begin();b!=parent.end();++a,++b)if(a==child.end() || !world_detail::same_path_name(*a,*b))return false;return true;
}
struct Camera {
    std::array<double,3> position{6,4,8};double yaw=37,pitch=-22,fov=60,near_plane=.1,far_plane=1000;
    EditorCamera native() const {
        constexpr double pi=3.14159265358979323846;const double y=yaw*pi/360,p=pitch*pi/360;EditorCamera camera;
        camera.world=local_matrix(position,{std::cos(y)*std::sin(p),std::sin(y)*std::cos(p),-std::sin(y)*std::sin(p),std::cos(y)*std::cos(p)},{1,1,1});camera.vertical_fov=fov;camera.near_plane=near_plane;camera.far_plane=far_plane;return camera;
    }
    Json json() const { return {{"position",position},{"yaw",yaw},{"pitch",pitch},{"vertical_fov",fov},{"near",near_plane},{"far",far_plane}}; }
    void update(const Json& value) {
        fields(value,{"position","yaw","pitch","vertical_fov","near","far"});Camera candidate=*this;
        if(value.contains("position")) {
            const auto& position_value=value.at("position");require(position_value.is_array() && position_value.size()==3,"Camera position needs three numbers.");
            for(std::size_t i=0;i<3;++i) { require(position_value[i].is_number(),"Camera position must be numeric.");candidate.position[i]=position_value[i];require(std::isfinite(candidate.position[i]) && std::abs(candidate.position[i])<=1e9,"Camera position is out of range."); }
        }
        const std::pair<const char*,double*> members[]={{"yaw",&candidate.yaw},{"pitch",&candidate.pitch},{"vertical_fov",&candidate.fov},{"near",&candidate.near_plane},{"far",&candidate.far_plane}};
        for(const auto& [name,destination]:members)if(value.contains(name)) { require(value.at(name).is_number(),"Camera parameters must be numeric.");*destination=value.at(name);require(std::isfinite(*destination),"Camera parameters must be finite."); }
        require(std::abs(candidate.yaw)<=1e9 && std::abs(candidate.pitch)<=89 && candidate.fov>=5 && candidate.fov<=150 && candidate.near_plane>=.001 && candidate.far_plane<=1e7 && candidate.far_plane>candidate.near_plane,"Camera parameters are out of range.");
        const auto native=candidate.native();(void)perspective(native.vertical_fov,1,native.near_plane,native.far_plane);*this=candidate;
    }
};
struct Capture {
    std::uint64_t id=0,revision=0,tick=0,camera_revision=0;std::string path,session,state="queued",detail;int code=0;bool runtime=false;
    Clock::time_point deadline;Json result=Json::object();
    Json json() const {
        Json out={{"capture_id",id},{"state",state},{"revision",revision},{"path",path}};
        if(state=="complete")out["result"]=result;
        if(state=="error")out["error"]={{"code",code},{"message",detail}};
        return out;
    }
};
struct Bridge {
    std::thread::id owner=std::this_thread::get_id();
    std::unique_ptr<LocalSessionServer> server;
    std::unique_ptr<WorldSession> world;
    std::unique_ptr<HostedViewport> viewport;
    fs::path world_path;RenderOptions render;Camera camera;std::string endpoint,selected,output,error,graphics_error;
    std::uint64_t camera_revision=0,next_capture=1,presented_frames=0;
    std::optional<Capture> capture;
    std::optional<std::uint64_t> observed_revision,presented_revision,presented_tick;
    Json observed_runtime;
    std::optional<SceneSnapshot> cached_snapshot;
    Json snapshot_key;
    std::optional<RenderReport> last_render;
    bool faulted=false;
    Bridge(const std::string& path,const std::string& local_endpoint,int gpu,std::uint32_t samples):endpoint(local_endpoint) {
        require(gpu>=-1 && gpu<=4095 && (samples==1 || samples==4),"GPU must be -1..4095 and samples must be 1 or 4.");render.gpu=gpu;render.samples=samples;
        world_path=fs::weakly_canonical(fs::absolute(path_of(path)));
        if(!endpoint.empty())server=std::make_unique<LocalSessionServer>(endpoint);
        world=std::make_unique<WorldSession>(path_text(world_path));
    }
    void thread() const { require(owner==std::this_thread::get_id(),"Desktop bridge calls must run on the creating UI thread.",-32000); }
    Json world_call(const std::string& method,const Json& params=Json::object()) {
        const auto response=Json::parse(world->request(Json{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",params}}.dump(),WorldRequestScope::shared_editor));
        if(response.contains("error"))throw Failure(response.at("error").at("code"),response.at("error").at("message"));
        return response.at("result");
    }
    std::uint64_t revision() { return world_call("world.inspect").at("revision").get<std::uint64_t>(); }
    Json runtime_json() const {
        const auto value=world->runtime_status();return {{"available",value.available},{"active",value.active},{"session_id",value.active ? Json(value.session_id) : Json(nullptr)},
            {"tick",value.active ? Json(value.tick) : Json(nullptr)},{"authored_revision",value.active ? Json(value.authored_revision) : Json(nullptr)}};
    }
    void fail_capture(int code,const std::string& detail) {
        if(capture && capture->state=="queued") { capture->state="error";capture->code=code;capture->detail=detail; }
    }
    void expire_capture() {
        if(!capture || capture->state!="queued")return;
        if(Clock::now()>=capture->deadline) { fail_capture(-32003,"Capture timed out waiting for a drawable native viewport.");return; }
        const auto state=world->runtime_status();
        if(revision()!=capture->revision || camera_revision!=capture->camera_revision || state.active!=capture->runtime || (state.active && (state.session_id!=capture->session || state.tick!=capture->tick)))
            fail_capture(-32009,"World, runtime tick, or camera changed before capture presentation.");
    }
    std::string capture_path(const Json& value) const {
        require(value.is_string(),"Capture path must be text.");const auto path=path_of(value.get<std::string>());require(fs::is_directory(fs::absolute(path).parent_path()),"Capture parent directory does not exist.");
        require(fs::symlink_status(path).type()==fs::file_type::not_found,"Capture requires a previously absent file.");const auto normalized=fs::weakly_canonical(fs::absolute(path));
        for(const auto* suffix:{"",".lock",".pending",".previous",".previous.pending"}) { auto reserved=world_path;reserved+=suffix;require(!world_detail::same_path_name(normalized,reserved),"Capture overlaps reserved world storage."); }
        auto assets=world_path;assets+=".assets";require(!inside(normalized,fs::weakly_canonical(assets)),"Capture overlaps the world asset store.");return path_text(normalized);
    }
    Json render_json() const {
        if(!last_render)return nullptr;
        const auto& report=*last_render;
        return {{"available",report.available},{"success",report.success},{"gpu",report.gpu_name},{"hardware",report.hardware},
            {"width",report.width},{"height",report.height},{"samples",report.samples},{"frames_presented",report.frames_presented},
            {"nvrhi_errors",report.validation_errors},{"capture_written",report.capture_written},{"detail",report.detail}};
    }
    void remember_render() {
        if(!viewport)return;
        auto report=viewport->report();
        if(!last_render || !report.gpu_name.empty())last_render=std::move(report);
    }
    Json inspect() {
        expire_capture();return {{"revision",revision()},{"runtime",runtime_json()},{"selected",selected.empty() ? Json(nullptr) : Json(selected)},
            {"attached",bool(viewport)},{"graphics_error",graphics_error.empty() ? Json(nullptr) : Json(graphics_error)},{"camera",camera.json()},
            {"capture",capture ? capture->json() : Json(nullptr)},{"frames_presented",presented_frames},{"render",render_json()},
            {"presented_revision",presented_revision ? Json(*presented_revision) : Json(nullptr)},{"presented_tick",presented_tick ? Json(*presented_tick) : Json(nullptr)},
            {"endpoint",endpoint.empty() ? Json(nullptr) : Json(endpoint)}};
    }
    Json describe() const {
        auto object=[](Json properties,Json required=Json::array()) { return Json{{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}}; };
        const Json integer_schema={{"type","integer"},{"minimum",0},{"maximum",max_integer}};
        Json methods=Json::object();methods["desktop.describe"]=object(Json::object());methods["desktop.inspect"]=object(Json::object());
        methods["desktop.select"]=object({{"id",{{"type",{"string","null"}},{"pattern","^[0-9a-f]{32}$"}}}},{"id"});
        methods["desktop.camera"]=object({{"position",{{"type","array"},{"items",{{"type","number"},{"minimum",-1e9},{"maximum",1e9}}},{"minItems",3},{"maxItems",3}}},
            {"yaw",{{"type","number"},{"minimum",-1e9},{"maximum",1e9}}},{"pitch",{{"type","number"},{"minimum",-89},{"maximum",89}}},{"vertical_fov",{{"type","number"},{"minimum",5},{"maximum",150}}},
            {"near",{{"type","number"},{"minimum",.001}}},{"far",{{"type","number"},{"maximum",1e7}}}});
        methods["desktop.capture"]=object({{"revision",integer_schema},{"path",{{"type","string"},{"minLength",1}}}},{"revision","path"});
        methods["desktop.capture.status"]=object({{"capture_id",{{"type","integer"},{"minimum",1},{"maximum",max_integer}}}},{"capture_id"});
        return {{"methods",methods},{"world_methods","world.describe"},{"capture",{{"completion","Asynchronous: queue returns capture_id/state; inspect status after poll/draw."},{"capacity",1},{"retained_results",1},{"timeout_ms",2000},{"guards","Authored revision, camera revision, runtime session and tick; pause automatic stepping while queued."},{"format","BMP; native viewport only; exclusive new path"}}},
            {"threading","One creating UI thread; draw does not advance simulation."},{"lifetime","Detach/re-attach preserves the world and IPC endpoint."}};
    }
    std::string request(const std::string& bytes) {
        Json id=nullptr;bool notification=false;
        try {
            const auto message=parse(bytes);require(message.is_object() && message.value("jsonrpc",Json())=="2.0" && message.contains("method") && message.at("method").is_string(),"Invalid JSON-RPC 2.0 request.",-32600);
            if(message.contains("id")) { id=message.at("id");require(id.is_null() || id.is_string() || id.is_number_integer(),"Invalid JSON-RPC ID.",-32600); }else notification=true;
            const auto method=message.at("method").get<std::string>();
            if(!method.starts_with("desktop.")) {
                auto response=world->request(bytes,WorldRequestScope::shared_editor);
                if(method=="world.describe" && !notification) {
                    auto value=Json::parse(response);
                    if(value.contains("result"))value["result"]["editor_discovery"]="desktop.describe";
                    response=value.dump();
                }
                return response;
            }
            fields(message,{"jsonrpc","id","method","params"},{"jsonrpc","method"});const auto params=message.value("params",Json::object());Json result;
            if(method=="desktop.describe") { fields(params,{});result=describe(); }
            else if(method=="desktop.inspect") { fields(params,{});result=inspect(); }
            else if(method=="desktop.camera") {
                Camera candidate=camera;candidate.update(params);
                if(candidate.json()!=camera.json()) { require(camera_revision<max_integer,"Camera revision exhausted.");camera=candidate;++camera_revision; }
                expire_capture();result=camera.json();
            }
            else if(method=="desktop.select") {
                fields(params,{"id"},{"id"});std::string next;if(!params.at("id").is_null()) { next=identifier(params.at("id"));world_call("entity.get",{{"id",next}}); }selected=std::move(next);result={{"selected",selected.empty() ? Json(nullptr) : Json(selected)}};
            }else if(method=="desktop.capture") {
                fields(params,{"revision","path"},{"revision","path"});expire_capture();require(viewport && !faulted,"A working attached native viewport is required.",-32003);require(!capture || capture->state!="queued","One capture is already pending.",-32009);
                const auto expected=integer(params.at("revision"));require(expected==revision(),"Authored revision conflict.",-32009);const auto path=capture_path(params.at("path"));require(next_capture<=max_integer,"Capture identity limit reached.");const auto state=world->runtime_status();
                Capture candidate;candidate.id=next_capture++;candidate.revision=expected;candidate.path=path;candidate.camera_revision=camera_revision;candidate.runtime=state.active;candidate.session=state.session_id;candidate.tick=state.tick;candidate.deadline=Clock::now()+std::chrono::seconds(2);capture=std::move(candidate);result=capture->json();
            }else if(method=="desktop.capture.status") {
                fields(params,{"capture_id"},{"capture_id"});const auto value=integer(params.at("capture_id"));require(capture && capture->id==value,"Capture result is absent or was superseded.",-32004);expire_capture();result=capture->json();
            }else throw Failure(-32601,"Unknown desktop method.");
            return notification ? std::string{} : Json{{"jsonrpc","2.0"},{"id",id},{"result",result}}.dump();
        }catch(const Failure& error_value) { return notification ? std::string{} : error_reply(id,error_value.code,error_value.what()); }
        catch(const Json::exception& error_value) { return notification ? std::string{} : error_reply(id,-32602,error_value.what()); }
        catch(const std::exception& error_value) { return notification ? std::string{} : error_reply(id,-32000,error_value.what()); }
    }
    Json poll() {
        if(server)for(const auto& incoming:server->poll())server->reply(incoming.token,request(incoming.payload));
        auto state=inspect();const auto current=state.at("revision").get<std::uint64_t>();const auto runtime=state.at("runtime");
        state["world_changed"]=!observed_revision || *observed_revision!=current;state["runtime_changed"]=observed_runtime!=runtime;
        observed_revision=current;observed_runtime=runtime;return state;
    }
    SceneSnapshot snapshot() {
        const auto runtime=world->runtime_status();
        const Json key={{"revision",revision()},{"camera",camera_revision},{"active",runtime.active},
            {"session",runtime.active ? runtime.session_id : std::string{}},{"tick",runtime.active ? runtime.tick : 0}};
        if(!cached_snapshot || key!=snapshot_key) {
            auto candidate=runtime.active ? world->runtime_snapshot(camera.native()) : world->authored_snapshot(camera.native());
            cached_snapshot=std::move(candidate);snapshot_key=key;
        }
        return *cached_snapshot;
    }
    void attach(void* handle) {
        require(!viewport,"Detach the current viewport before attaching another HWND.");require(handle && IsWindow(static_cast<HWND>(handle)),"Attach requires a live HWND.");
        DWORD process=0;const auto thread_id=GetWindowThreadProcessId(static_cast<HWND>(handle),&process);require(process==GetCurrentProcessId() && thread_id==GetCurrentThreadId(),"HWND must belong to this process and UI thread.");
        const auto scene=snapshot();auto candidate=std::make_unique<HostedViewport>(render,scene,handle);viewport=std::move(candidate);faulted=false;graphics_error.clear();presented_revision.reset();presented_tick.reset();
    }
    void detach() {
        fail_capture(-32003,"Viewport detached before capture completed.");
        remember_render();
        viewport.reset();faulted=false;graphics_error.clear();presented_revision.reset();presented_tick.reset();
    }
    int draw() {
        expire_capture();if(!viewport)return 0;require(!faulted,"Graphics failed; detach and attach the viewport to recover.",-32003);
        const auto scene=snapshot();const auto runtime=world->runtime_status();bool presented=false;
        // Destination changes invalidate only the capture job. This preflight
        // has not touched GPU state and must not disable a healthy viewport.
        if(capture && capture->state=="queued") {
            try { capture_path(capture->path); }
            catch(const Failure& error_value) { fail_capture(error_value.code,error_value.what()); }
            catch(const std::exception& error_value) { fail_capture(-32003,error_value.what()); }
        }
        try {
            if(capture && capture->state=="queued")presented=viewport->draw_capture(scene,capture->path);
            else presented=viewport->draw(scene);
            remember_render();
        }catch(const std::exception& error_value) {
            try { remember_render(); }catch(...) {}
            fail_capture(-32003,error_value.what());faulted=true;graphics_error=error_value.what();throw;
        }
        if(!presented)return 0;
        ++presented_frames;presented_revision=scene.revision;presented_tick=runtime.active ? std::optional<std::uint64_t>(runtime.tick) : std::nullopt;
        if(capture && capture->state=="queued") {
            const auto& report=*last_render;capture->state="complete";capture->result={{"path",capture->path},{"revision",capture->revision},{"scene_revision",scene.revision},{"source",runtime.active ? "runtime" : "authored"},{"tick",runtime.active ? Json(runtime.tick) : Json(nullptr)},{"width",report.width},{"height",report.height},{"frame",presented_frames},{"render",render_json()}};
        }
        return 1;
    }
};
std::mutex host_mutex;Bridge* instance=nullptr;thread_local std::string last_error;
Bridge& checked(void* handle) { require(handle && handle==instance,"Desktop host handle is null or no longer valid.",-32000);instance->thread();return *instance; }
void record_error(void* handle,const char* detail) noexcept {
    try { last_error=detail ? detail : "Unknown native exception.";if(handle && handle==instance && instance->owner==std::this_thread::get_id())instance->error=last_error; }catch(...) {}
}
template<class T,class F>T guarded(void* handle,T failure,F&& action) noexcept {
    try { std::unique_lock lock(host_mutex,std::try_to_lock);if(!lock.owns_lock()) { record_error(nullptr,"Desktop bridge operation is already in progress.");return failure; }try { auto& host=checked(handle);host.error.clear();last_error.clear();return action(host); }catch(const std::exception& error) { record_error(handle,error.what()); }catch(...) { record_error(handle,"Unknown native exception."); } }
    catch(...) { record_error(nullptr,"Desktop bridge synchronization failed."); }return failure;
}
} // namespace
extern "C" {
void* poima_desktop_create(const char* world,const char* endpoint,int32_t gpu,uint32_t samples) {
    try { std::unique_lock lock(host_mutex,std::try_to_lock);if(!lock.owns_lock()) { record_error(nullptr,"Desktop bridge operation is already in progress.");return nullptr; }try { require(!instance,"Only one desktop host is allowed per process.",-32000);auto host=std::make_unique<Bridge>(bounded_string(world,32768),bounded_string(endpoint,64,true),gpu,samples);instance=host.release();last_error.clear();return instance; }
        catch(const std::exception& error) { record_error(nullptr,error.what()); }catch(...) { record_error(nullptr,"Unknown native exception."); } }
    catch(...) { record_error(nullptr,"Desktop bridge synchronization failed."); }return nullptr;
}
const char* poima_desktop_call(void* host,const char* jsonrpc) { return guarded<const char*>(host,nullptr,[&](Bridge& value){value.output=value.request(bounded_string(jsonrpc,local_session_request_limit));return value.output.c_str();}); }
const char* poima_desktop_poll(void* host) { return guarded<const char*>(host,nullptr,[](Bridge& value){value.output=value.poll().dump();return value.output.c_str();}); }
int poima_desktop_attach(void* host,void* hwnd) { return guarded<int>(host,0,[&](Bridge& value){value.attach(hwnd);return 1;}); }
int poima_desktop_draw(void* host) { return guarded<int>(host,-1,[](Bridge& value){return value.draw();}); }
void poima_desktop_detach(void* host) { (void)guarded<int>(host,0,[](Bridge& value){value.detach();return 1;}); }
void poima_desktop_destroy(void* host) {
    (void)guarded<int>(host,0,[](Bridge& value){value.detach();instance=nullptr;delete &value;return 1;});
}
const char* poima_desktop_error(void* host) {
    try { std::unique_lock lock(host_mutex,std::try_to_lock);if(!lock.owns_lock())return "Desktop bridge operation is already in progress.";if(host && host==instance && instance->owner==std::this_thread::get_id())return instance->error.c_str();return last_error.c_str(); }
    catch(...) { return "Desktop bridge synchronization failed."; }
}
}
