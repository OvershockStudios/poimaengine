// SPDX-License-Identifier: Apache-2.0
#include "poima/desktop_bridge.h"
#include "poima/world.hpp"
#include "poima/local_session.hpp"
#include "poima/hosted_viewport.hpp"
#include "poima/animation.hpp"
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
#include <unordered_map>
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
using Point=std::array<double,3>;
using Parents=std::unordered_map<std::string,std::string>;
double dot(const Point& a,const Point& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Point subtract(const Point& a,const Point& b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
Point cross(const Point& a,const Point& b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
Point transformed(const Matrix4& matrix,const Point& p,double w) {
    Point result{};for(std::size_t row=0;row<3;++row)result[row]=matrix[row]*p[0]+matrix[4+row]*p[1]+matrix[8+row]*p[2]+matrix[12+row]*w;return result;
}
double number(const Json& value,double low,double high,const char* message) {
    require(value.is_number(),message);const double result=value;require(std::isfinite(result) && result>=low && result<=high,message);return result;
}
Bounds local_bounds(const SceneObject& object) {
    return object.skin ? posed_bounds(skin_bounds(*object.mesh),object.skin->palette) : mesh_bounds(object.mesh.get());
}
std::optional<std::pair<double,double>> ray_bounds(const Bounds& bounds,const Point& origin,const Point& ray) {
    double low=-std::numeric_limits<double>::infinity(),high=std::numeric_limits<double>::infinity();
    for(std::size_t axis=0;axis<3;++axis) {
        if(ray[axis]==0) { if(origin[axis]<bounds.minimum[axis] || origin[axis]>bounds.maximum[axis])return {}; }
        else { auto a=(bounds.minimum[axis]-origin[axis])/ray[axis],b=(bounds.maximum[axis]-origin[axis])/ray[axis];if(a>b)std::swap(a,b);low=std::max(low,a);high=std::min(high,b); }
    }
    if(low>high)return {};return std::pair{low,high};
}
Point mesh_position(const SceneObject& object,std::uint32_t index) {
    const auto& mesh=*object.mesh;const auto& position=mesh.vertices.at(index).position;
    const Point p{position[0],position[1],position[2]};if(!object.skin)return p;
    Point result{};const auto& influence=mesh.influences.at(index);
    for(std::size_t i=0;i<4;++i) {
        if(influence.weights[i]==0)continue;
        const auto posed=transformed(object.skin->palette.at(influence.joints[i]),p,1);
        for(std::size_t axis=0;axis<3;++axis)result[axis]+=influence.weights[i]*posed[axis];
    }
    return result;
}
std::optional<double> hit_object(const SceneObject& object,const Point& origin,const Point& ray,double near_distance,double far_distance,bool preview) {
    const auto inverse=inverse_affine(object.world);const auto o=transformed(inverse,origin,1),d=transformed(inverse,ray,0);
    const auto interval=ray_bounds(local_bounds(object),o,d);
    if(!interval || interval->second<near_distance || interval->first>far_distance)return {};
    const bool cull=object.material ? !object.material->double_sided : !preview;
    if(!object.mesh) {
        const double distance=interval->first>=near_distance ? interval->first : cull ? -1 : interval->second;
        return distance>=near_distance && distance<=far_distance ? std::optional<double>(distance) : std::nullopt;
    }
    std::optional<double> nearest;
    const auto& indices=object.mesh->indices;
    for(std::size_t i=0;i+2<indices.size();i+=3) {
        const auto a=mesh_position(object,indices[i]),b=mesh_position(object,indices[i+1]),c=mesh_position(object,indices[i+2]);
        const auto e1=subtract(b,a),e2=subtract(c,a),p=cross(d,e2);const double determinant=dot(e1,p);
        // Relative degeneracy tolerance also supports very small/large models.
        const double tolerance=32*std::numeric_limits<double>::epsilon()*std::sqrt(dot(e1,e1)*dot(p,p));
        if(cull ? determinant<=tolerance : std::abs(determinant)<=tolerance)continue;
        const auto offset=subtract(o,a);const double u=dot(offset,p)/determinant;if(u<0 || u>1)continue;
        const auto q=cross(offset,e1);const double v=dot(d,q)/determinant;if(v<0 || u+v>1)continue;
        const double distance=dot(e2,q)/determinant;
        if(std::isfinite(distance) && distance>=near_distance && distance<=far_distance && (!nearest || distance<*nearest))nearest=distance;
    }
    return nearest;
}
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
    Parents runtime_parents;
    std::string runtime_parent_session;
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
    Parents parents() {
        Parents result;Json params={{"limit",256}};
        do {
            const auto page=world_call("entity.query",params);
            for(const auto& entity:page.at("entities"))result.emplace(entity.at("id").get<std::string>(),entity.at("parent").is_null() ? std::string{} : entity.at("parent").get<std::string>());
            params["after"]=page.at("next_after");
        }while(!params.at("after").is_null());
        return result;
    }
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
    Json pick(const Json& params) {
        fields(params,{"revision","x","y","aspect"},{"revision","x","y","aspect"});
        const auto expected=integer(params.at("revision"));
        const double x=number(params.at("x"),0,1,"Pick x must be in [0,1]."),y=number(params.at("y"),0,1,"Pick y must be in [0,1].");
        const double aspect=number(params.at("aspect"),.01,100,"Viewport aspect must be in [0.01,100].");
        require(expected==revision(),"Authored revision conflict.",-32009);
        const auto scene=snapshot();const auto runtime=world->runtime_status();
        constexpr double pi=3.14159265358979323846;const double tangent=std::tan(scene.vertical_fov*pi/360);
        const Point local{(2*x-1)*aspect*tangent,(1-2*y)*tangent,-1};const double length=std::sqrt(dot(local,local));
        auto ray=transformed(scene.camera_world,local,0);for(auto& value:ray)value/=length;
        const Point origin{scene.camera_world[12],scene.camera_world[13],scene.camera_world[14]};
        const double near_distance=scene.near_plane*length;double nearest=scene.far_plane*length;std::string id;
        for(const auto& object:scene.objects)if(const auto distance=hit_object(object,origin,ray,near_distance,nearest,scene.lighting.preview)) {
            if(id.empty() || *distance<nearest || (*distance==nearest && object.entity_id<id)) { nearest=*distance;id=object.entity_id; }
        }
        return {{"id",id.empty() ? Json(nullptr) : Json(id)},{"distance",id.empty() ? Json(nullptr) : Json(nearest)},
            {"revision",expected},{"scene_revision",scene.revision},{"source",runtime.active ? "runtime" : "authored"},{"tick",runtime.active ? Json(runtime.tick) : Json(nullptr)}};
    }
    Json frame(const Json& params) {
        fields(params,{"revision","id","aspect"},{"revision","aspect"});const auto expected=integer(params.at("revision"));
        const double aspect=number(params.at("aspect"),.01,100,"Viewport aspect must be in [0.01,100].");
        const auto id=params.contains("id") ? identifier(params.at("id")) : selected;require(!id.empty(),"Select an entity or supply id to frame.");
        require(expected==revision(),"Authored revision conflict.",-32009);const auto runtime=world->runtime_status();
        Matrix4 fallback;Parents hierarchy;
        if(runtime.active) {
            require(runtime_parent_session==runtime.session_id,"Runtime hierarchy is unavailable.",-32003);
            fallback=world_call("runtime.entity",{{"session_id",runtime.session_id},{"id",id},{"tick",runtime.tick}}).at("world_matrix").get<Matrix4>();hierarchy=runtime_parents;
        } else { fallback=world_call("entity.world_transform",{{"id",id}}).at("matrix").get<Matrix4>();hierarchy=parents(); }
        std::unordered_map<std::string,std::vector<std::string>> children;
        for(const auto& [child,parent]:hierarchy)children[parent].push_back(child);
        std::set<std::string> descendants;std::vector<std::string> pending{id};
        while(!pending.empty()) { auto current=std::move(pending.back());pending.pop_back();if(!descendants.insert(current).second)continue;for(const auto& child:children[current])pending.push_back(child); }
        const auto scene=snapshot();std::optional<Bounds> combined;
        for(const auto& object:scene.objects)if(descendants.contains(object.entity_id)) {
            const auto bounds=transform_bounds(local_bounds(object),object.world);
            if(!combined)combined=bounds;
            else for(std::size_t axis=0;axis<3;++axis) { combined->minimum[axis]=std::min(combined->minimum[axis],bounds.minimum[axis]);combined->maximum[axis]=std::max(combined->maximum[axis],bounds.maximum[axis]); }
        }
        if(!combined) { combined=Bounds{};for(std::size_t axis=0;axis<3;++axis) { combined->minimum[axis]=fallback[12+axis]-.5;combined->maximum[axis]=fallback[12+axis]+.5; } }
        Point target{};for(std::size_t axis=0;axis<3;++axis)target[axis]=(combined->minimum[axis]+combined->maximum[axis])*.5;
        const auto basis=camera.native().world;const Point right{basis[0],basis[1],basis[2]},up{basis[4],basis[5],basis[6]},back{basis[8],basis[9],basis[10]};
        constexpr double pi=3.14159265358979323846;const double tangent_y=std::tan(camera.fov*pi/360),tangent_x=tangent_y*aspect;
        double distance=1,minimum_depth_offset=std::numeric_limits<double>::infinity();
        for(unsigned mask=0;mask<8;++mask) {
            Point corner{};for(std::size_t axis=0;axis<3;++axis)corner[axis]=((mask&(1U<<axis)) ? combined->maximum[axis] : combined->minimum[axis])-target[axis];
            const double z=dot(corner,back);minimum_depth_offset=std::min(minimum_depth_offset,z);
            distance=std::max({distance,z+std::abs(dot(corner,right))*1.1/tangent_x,z+std::abs(dot(corner,up))*1.1/tangent_y,z+camera.near_plane*1.1});
        }
        Point position{};for(std::size_t axis=0;axis<3;++axis)position[axis]=target[axis]+back[axis]*distance;
        Camera candidate=camera;candidate.update({{"position",position},{"far",std::max(camera.far_plane,(distance-minimum_depth_offset)*1.1)}});
        if(candidate.json()!=camera.json()) { require(camera_revision<max_integer,"Camera revision exhausted.");camera=candidate;++camera_revision; }
        expire_capture();return {{"camera",camera.json()},{"target",target},{"distance",distance}};
    }
    Json describe() const {
        auto object=[](Json properties,Json required=Json::array()) { return Json{{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}}; };
        const Json integer_schema={{"type","integer"},{"minimum",0},{"maximum",max_integer}};
        Json methods=Json::object();methods["desktop.describe"]=object(Json::object());methods["desktop.inspect"]=object(Json::object());
        methods["desktop.select"]=object({{"id",{{"type",{"string","null"}},{"pattern","^[0-9a-f]{32}$"}}}},{"id"});
        const Json aspect_schema={{"type","number"},{"minimum",.01},{"maximum",100}},unit_schema={{"type","number"},{"minimum",0},{"maximum",1}};
        methods["desktop.pick"]=object({{"revision",integer_schema},{"x",unit_schema},{"y",unit_schema},{"aspect",aspect_schema}},{"revision","x","y","aspect"});
        methods["desktop.frame"]=object({{"revision",integer_schema},{"id",{{"type","string"},{"pattern","^[0-9a-f]{32}$"}}},{"aspect",aspect_schema}},{"revision","aspect"});
        methods["desktop.camera"]=object({{"position",{{"type","array"},{"items",{{"type","number"},{"minimum",-1e9},{"maximum",1e9}}},{"minItems",3},{"maxItems",3}}},
            {"yaw",{{"type","number"},{"minimum",-1e9},{"maximum",1e9}}},{"pitch",{{"type","number"},{"minimum",-89},{"maximum",89}}},{"vertical_fov",{{"type","number"},{"minimum",5},{"maximum",150}}},
            {"near",{{"type","number"},{"minimum",.001}}},{"far",{{"type","number"},{"maximum",1e7}}}});
        methods["desktop.capture"]=object({{"revision",integer_schema},{"path",{{"type","string"},{"minLength",1}}}},{"revision","path"});
        methods["desktop.capture.status"]=object({{"capture_id",{{"type","integer"},{"minimum",1},{"maximum",max_integer}}}},{"capture_id"});
        return {{"methods",methods},{"world_methods","world.describe"},{"capture",{{"completion","Asynchronous: queue returns capture_id/state; inspect status after poll/draw."},{"capacity",1},{"retained_results",1},{"timeout_ms",2000},{"guards","Authored revision, camera revision, runtime session and tick; pause automatic stepping while queued."},{"format","BMP; native viewport only; exclusive new path"}}},
            {"picking",{{"coordinates","Normalized viewport coordinates, top-left origin; aspect is width/height."},{"geometry","Nearest visible snapshot geometry: transformed boxes or CPU triangle intersections, including posed skin vertices, near/far clipping and material backface culling. No GPU readback; subpixel rasterization is not reproduced."},{"mutation","None; select explicitly using desktop.select."}}},
            {"framing",{{"result","camera, target, distance"},{"bounds","Visible entity/descendant bounds in authored or frozen runtime hierarchy; non-rendered entities use a unit bound at their current world position."}}},
            {"threading","One creating UI thread; draw does not advance simulation."},{"lifetime","Detach/re-attach preserves the world and IPC endpoint."}};
    }
    std::string request(const std::string& bytes) {
        Json id=nullptr;bool notification=false;
        try {
            const auto message=parse(bytes);require(message.is_object() && message.value("jsonrpc",Json())=="2.0" && message.contains("method") && message.at("method").is_string(),"Invalid JSON-RPC 2.0 request.",-32600);
            if(message.contains("id")) { id=message.at("id");require(id.is_null() || id.is_string() || id.is_number_integer(),"Invalid JSON-RPC ID.",-32600); }else notification=true;
            const auto method=message.at("method").get<std::string>();
            if(!method.starts_with("desktop.")) {
                const bool starting=method=="runtime.start" && !world->runtime_status().active;
                Parents starting_parents;if(starting)starting_parents=parents();
                auto response=world->request(bytes,WorldRequestScope::shared_editor);
                if(starting) { const auto state=world->runtime_status();if(state.active) { runtime_parents=std::move(starting_parents);runtime_parent_session=state.session_id; } }
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
            else if(method=="desktop.pick")result=pick(params);
            else if(method=="desktop.frame")result=frame(params);
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
