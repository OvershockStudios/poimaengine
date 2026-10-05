// SPDX-License-Identifier: Apache-2.0
#include "poima/desktop_bridge.h"
#include "poima/world.hpp"
#include "poima/local_session.hpp"
#include "poima/hosted_viewport.hpp"
#include "poima/animation.hpp"
#include "poima/editor_gizmo.hpp"
#include "poima/player.hpp"
#include "poima/input_profile.hpp"
#include "input_profile_store.hpp"
#include "world_storage.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>
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
    std::uint64_t id=0,revision=0,tick=0,camera_revision=0,gizmo_generation=0,view_revision=0;std::string path,session,state="queued",detail,target="legacy";int code=0;bool runtime=false;
    Clock::time_point deadline;Json result=Json::object();
    Json json() const {
        Json out={{"capture_id",id},{"state",state},{"revision",revision},{"path",path},{"view",target}};
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
const char* axis_name(EditorGizmoAxis axis) { return axis==EditorGizmoAxis::x ? "x" : axis==EditorGizmoAxis::y ? "y" : "z"; }
Matrix4 transform_matrix(const Json& value) {
    return local_matrix(value.at("position").get<Point>(),value.at("rotation").get<std::array<double,4>>(),value.at("scale").get<Point>());
}
Json decompose_transform(const Matrix4& matrix) {
    Point scale{};std::array<Point,3> columns;
    for(std::size_t i=0;i<3;++i) {
        columns[i]={matrix[i*4],matrix[i*4+1],matrix[i*4+2]};scale[i]=std::sqrt(dot(columns[i],columns[i]));
        require(std::isfinite(scale[i]) && scale[i]>0,"Gizmo result has a singular scale.");
        for(auto& x:columns[i])x/=scale[i];
    }
    require(std::abs(dot(columns[0],columns[1]))<1e-8 && std::abs(dot(columns[0],columns[2]))<1e-8 && std::abs(dot(columns[1],columns[2]))<1e-8 && dot(cross(columns[0],columns[1]),columns[2])>0,
        "This world-space edit would introduce shear. Use Local space or change the parent scale.");
    // Stable matrix-to-quaternion conversion; choose the largest diagonal when
    // the trace is negative (including rotations near 180 degrees).
    const double a=columns[0][0],b=columns[1][1],c=columns[2][2];std::array<double,4> q{};
    if(a+b+c>0) { const auto t=2*std::sqrt(1+a+b+c);q={(columns[1][2]-columns[2][1])/t,(columns[2][0]-columns[0][2])/t,(columns[0][1]-columns[1][0])/t,t/4}; }
    else if(a>b && a>c) { const auto t=2*std::sqrt(1+a-b-c);q={t/4,(columns[1][0]+columns[0][1])/t,(columns[2][0]+columns[0][2])/t,(columns[1][2]-columns[2][1])/t}; }
    else if(b>c) { const auto t=2*std::sqrt(1+b-a-c);q={(columns[1][0]+columns[0][1])/t,t/4,(columns[2][1]+columns[1][2])/t,(columns[2][0]-columns[0][2])/t}; }
    else { const auto t=2*std::sqrt(1+c-a-b);q={(columns[2][0]+columns[0][2])/t,(columns[2][1]+columns[1][2])/t,t/4,(columns[0][1]-columns[1][0])/t}; }
    double length=0;for(auto x:q)length+=x*x;length=std::sqrt(length);for(auto& x:q)x/=length;
    return {{"position",{matrix[12],matrix[13],matrix[14]}},{"rotation",q},{"scale",scale}};
}
struct GizmoGesture {
    std::uint64_t id=0,revision=0,camera_revision=0;
    std::uint32_t width=0,height=0;
    std::string entity;
    EditorGizmoDrag drag;
    EditorGizmoMode mode=EditorGizmoMode::move;
    bool local=false;
    Matrix4 parent=identity_matrix(),world=identity_matrix();
    std::array<Point,3> basis{};
    Json original,transform;
    std::optional<SceneSnapshot> preview;
};
Json gesture_transform(const GizmoGesture& gesture,double value) {
    auto result=gesture.original;
    const auto index=static_cast<std::size_t>(gesture.drag.axis);
    const auto position=gesture.original.at("position").get<Point>();
    const auto rotation=gesture.original.at("rotation").get<std::array<double,4>>();
    auto scale=gesture.original.at("scale").get<Point>();
    if(gesture.mode==EditorGizmoMode::move) {
        if(value==0)return result;
        auto axis=gesture.basis[index];const auto length=std::sqrt(dot(axis,axis));for(auto& x:axis)x=x/length*value;
        const auto delta=transformed(inverse_affine(gesture.parent),axis,0);auto p=position;for(std::size_t i=0;i<3;++i)p[i]+=delta[i];result["position"]=p;
    } else if(gesture.local) {
        if(gesture.mode==EditorGizmoMode::scale) { if(value==1)return result;scale[index]*=value;result["scale"]=scale; }
        else {
            if(value==0)return result;
            std::array<double,4> q{0,0,0,std::cos(value/2)};q[index]=std::sin(value/2);
            const auto m=multiply(local_matrix(position,rotation,{1,1,1}),local_matrix({0,0,0},q,scale));result=decompose_transform(m);
        }
    } else {
        if(value==(gesture.mode==EditorGizmoMode::scale ? 1 : 0))return result;
        auto delta=identity_matrix();
        if(gesture.mode==EditorGizmoMode::scale)delta[index*4+index]=value;
        else { std::array<double,4> q{0,0,0,std::cos(value/2)};q[index]=std::sin(value/2);delta=local_matrix({0,0,0},q,{1,1,1}); }
        const Point pivot{gesture.world[12],gesture.world[13],gesture.world[14]};const auto rotated=transformed(delta,pivot,0);
        for(std::size_t i=0;i<3;++i)delta[12+i]=pivot[i]-rotated[i];
        result=decompose_transform(multiply(inverse_affine(gesture.parent),multiply(delta,gesture.world)));
    }
    for(const auto* field:{"position","rotation","scale"})for(const auto& item:result.at(field)) {
        const double v=item;require(std::isfinite(v) && std::abs(v)<=1e9 && (std::string_view(field)!="scale" || v>0),"Gizmo transform exceeds authored numeric limits.");
    }
    return result;
}
struct ViewportState {
    std::unique_ptr<HostedViewport> viewport;
    void* hwnd=nullptr;
    std::string graphics_error,preparation_error;
    std::uint64_t presented_frames=0;
    std::optional<std::uint64_t> presented_revision,presented_tick;
    std::optional<SceneSnapshot> cached_snapshot;
    Json snapshot_key;
    std::optional<RenderReport> last_render;
    bool faulted=false;
};
struct Bridge : ViewportState {
    std::thread::id owner=std::this_thread::get_id();
    std::unique_ptr<LocalSessionServer> server;
    std::unique_ptr<WorldSession> world;
    ViewportState game_view;
    bool explicit_views=false;
    std::string game_camera;
    std::uint64_t game_camera_revision=0;
    fs::path world_path;RenderOptions render;Camera camera;std::string endpoint,selected,output,error;
    std::uint64_t camera_revision=0,next_capture=1;
    std::optional<Capture> capture;
    std::optional<std::uint64_t> observed_revision;
    Json observed_runtime;
    Parents runtime_parents;
    std::string runtime_parent_session;
    std::string gizmo_mode="move",gizmo_space="world";
    std::uint64_t gizmo_generation=0,next_drag=1;
    std::optional<GizmoGesture> gesture;
    Json committed_gesture=nullptr;
    PlayerClock play_clock;Clock::time_point play_last=Clock::now();
    std::string play_session,play_error,view_mode="scene",view_camera;
    std::uint64_t play_tick=0,view_revision=0;
    bool playing=false,capture_hold=false;
    std::optional<BoundPlayerInput> game_input;
    std::string input_controller,input_camera;
    Json input_profile=nullptr,last_input_applied=nullptr;
    bool input_focused=false;
    std::uint64_t input_batches=0;
    std::vector<Json> input_receipts;
    std::uint64_t gameplay_generation=0,gameplay_cached_revision=0;
    std::string gameplay_cached_session;
    Json gameplay_profile=nullptr,gameplay_cached_module=nullptr;
    std::vector<Json> gameplay_receipts;
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
    void release_input(bool forget=false) {
        input_focused=false;
        if(game_input)game_input->clear();
        if(forget) { game_input.reset();input_controller.clear();input_camera.clear();input_profile=nullptr;last_input_applied=nullptr;input_batches=0;input_receipts.clear(); }
    }
    static Json input_frame(const RuntimeInput& value,bool entity=false) {
        Json result={{"move",value.move},{"look",value.look},{"jump",value.jump},{"use",value.use}};
        if(entity)result["entity"]=value.entity;
        return result;
    }
    Json input_json() const {
        return {{"configured",bool(game_input)},{"session_id",game_input ? Json(play_session) : Json(nullptr)},
            {"controller",game_input ? Json(input_controller) : Json(nullptr)},{"camera",game_input ? Json(input_camera) : Json(nullptr)},
            {"focused",input_focused},{"profile",input_profile},{"accepted_batches",input_batches},
            {"pending",input_frame(game_input ? game_input->peek(input_controller) : RuntimeInput{})},{"last_applied",last_input_applied}};
    }
    Json controllers_json() {
        const auto runtime=world->runtime_status();Json values=Json::array();
        for(const auto& value:world->controllers(runtime.active))values.push_back({{"id",value.id},{"camera",value.camera}});
        return {{"source",runtime.active ? "runtime" : "authored"},{"revision",revision()},
            {"session_id",runtime.active ? Json(runtime.session_id) : Json(nullptr)},{"tick",runtime.active ? Json(runtime.tick) : Json(nullptr)},{"controllers",values}};
    }
    Json input_command(const std::string& method,const Json& params) {
        sync_playback();
        if(method=="desktop.input.inspect") { fields(params,{});return input_json(); }
        require(method=="desktop.input.configure" || method=="desktop.input.focus" || method=="desktop.input.events","Unknown desktop input method.",-32601);
        if(method=="desktop.input.configure")fields(params,{"session_id","controller","input_profile","input_revision"},{"session_id","controller"});
        else if(method=="desktop.input.focus")fields(params,{"session_id","focused"},{"session_id","focused"});
        else fields(params,{"session_id","request_id","events"},{"session_id","request_id","events"});
        const auto session=identifier(params.at("session_id"));
        require(!play_session.empty() && session==play_session,"Runtime session conflict.",-32009);
        if(method=="desktop.input.configure") {
            const auto controller=identifier(params.at("controller"));const auto values=world->controllers(true);
            const auto found=std::find_if(values.begin(),values.end(),[&](const auto& value){return value.id==controller;});
            require(found!=values.end(),"Input requires a runtime CharacterController entity.",-32004);
            require(!params.contains("input_revision") || params.contains("input_profile"),"input_revision requires input_profile.");
            auto profile=default_input_profile();Json metadata={{"source","defaults"},{"path",nullptr},{"revision",0},{"content_hash",nullptr},{"format","poima.input.v1"}};
            if(params.contains("input_profile")) {
                require(params.at("input_profile").is_string(),"Input profile path must be text.");const auto text=params.at("input_profile").get<std::string>();
                require(!text.empty() && text.size()<=4096 && text.find('\0')==std::string::npos,"Invalid input profile path.");
                auto path=path_of(text);if(path.is_relative())path=world_path.parent_path()/path;
                try {
                    auto loaded=input_profiles::load_read_only(path);
                    if(params.contains("input_revision"))require(integer(params.at("input_revision"))==loaded.revision,"Input profile revision conflict.",-32009);
                    profile=std::move(loaded.profile);metadata={{"source","profile"},{"path",path_text(fs::absolute(path).lexically_normal())},{"revision",loaded.revision},{"content_hash",loaded.content_hash},{"format",loaded.format}};
                }catch(const input_profiles::ProfileError& error_value) { throw Failure(error_value.code,error_value.what()); }
            }
            BoundPlayerInput candidate(std::move(profile));
            game_input=std::move(candidate);input_controller=controller;input_camera=found->camera;input_profile=std::move(metadata);
            input_focused=false;last_input_applied=nullptr;input_batches=0;return input_json();
        }
        if(method=="desktop.input.focus") {
            require(params.at("focused").is_boolean(),"focused must be Boolean.");
            if(!params.at("focused").get<bool>()) { release_input();return input_json(); }
            require(game_input.has_value() && playing && game_selected() && selected_game_camera()==input_camera,"Input focus requires configured playing Game view with its controller camera.",-32009);
            input_focused=true;return input_json();
        }
        identifier(params.at("request_id"));
        for(const auto& receipt:input_receipts)if(receipt.at("params").at("request_id")==params.at("request_id")) {
            require(receipt.at("params")==params,"Input request ID reused with different parameters.",-32010);
            auto result=receipt.at("result");result["replayed"]=true;return result;
        }
        require(game_input.has_value() && input_focused && playing && game_selected() && selected_game_camera()==input_camera,"Gameplay input requires focused configured playing Game view.",-32009);
        const auto& events=params.at("events");require(events.is_array() && events.size()<=256,"Input accepts at most 256 events per batch.");
        require(input_batches<max_integer,"Input batch counter exhausted.");
        auto staged=*game_input;
        for(const auto& event:events) {
            require(event.is_object(),"Input event must be an object.");
            if(event.contains("control")) {
                fields(event,{"control","down"},{"control","down"});require(event.at("control").is_string() && event.at("down").is_boolean(),"Invalid input control event.");
                const auto id=event.at("control").get<std::string>();const auto controls=input_controls();
                const auto found=std::find_if(controls.begin(),controls.end(),[&](const auto& value){return value.id==id;});
                require(found!=controls.end() && !found->reserved && found->kind!=InputControlKind::gamepad_button,"Unknown, reserved or unsupported gameplay control.");
                staged.control(found->kind,found->code,event.at("down").get<bool>());
            } else {
                fields(event,{"motion"},{"motion"});const auto& motion=event.at("motion");require(motion.is_array() && motion.size()==2,"Mouse motion needs two values.");
                for(const auto& value:motion)require(value.is_number() && std::isfinite(value.get<double>()) && std::abs(value.get<double>())<=1e6,"Mouse motion is out of bounds.");
                staged.motion(motion[0].get<double>(),motion[1].get<double>());
            }
        }
        auto result=input_json();result["pending"]=input_frame(staged.peek(input_controller));result["accepted_batches"]=input_batches+1;result["replayed"]=false;
        auto receipts=input_receipts;if(receipts.size()==32)receipts.erase(receipts.begin());
        receipts.push_back({{"params",params},{"result",result}});
        game_input=std::move(staged);++input_batches;input_receipts.swap(receipts);return result;
    }
    Json gameplay_runtime() {
        const auto status=world->gameplay_status();
        if(!status.active) { gameplay_cached_session.clear();gameplay_cached_module=nullptr;return nullptr; }
        if(gameplay_cached_session!=status.session_id || gameplay_cached_revision!=status.revision) {
            const auto state=world_call("runtime.gameplay.inspect",{{"session_id",status.session_id},{"tick",status.tick}});
            const auto& module=state.at("module");
            gameplay_cached_module=module.is_null() ? Json(nullptr) : Json{{"identity",module.at("identity")},{"assembly_sha256",module.at("assembly_sha256")}};
            gameplay_cached_session=status.session_id;gameplay_cached_revision=status.revision;
        }
        return {{"session_id",status.session_id},{"tick",status.tick},{"revision",status.revision},{"module",gameplay_cached_module}};
    }
    Json gameplay_json(bool full) {
        Json result={{"generation",gameplay_generation},{"runtime",gameplay_runtime()}};
        if(full)result["profile"]=gameplay_profile;else result["configured"]=!gameplay_profile.is_null();
        return result;
    }
    Json gameplay_command(const std::string& method,const Json& params) {
        if(method=="desktop.gameplay.inspect") { fields(params,{});return gameplay_json(true); }
        require(method=="desktop.gameplay.configure","Unknown desktop gameplay method.",-32601);
        fields(params,{"request_id","expected_generation","profile"},{"request_id","expected_generation","profile"});
        identifier(params.at("request_id"));const auto expected=integer(params.at("expected_generation"));
        auto normalized=params;auto& profile=normalized.at("profile");
        if(!profile.is_null()) {
            fields(profile,{"hostfxr","bridge","assembly","type","values"},{"hostfxr","bridge","assembly","type"});
            for(const auto* name:{"hostfxr","bridge","assembly","type"}) {
                require(profile.at(name).is_string(),"Gameplay paths and type must be text.");const auto& text=profile.at(name).get_ref<const std::string&>();
                require(!text.empty() && text.size()<=(std::string_view(name)=="type" ? 512U : 4096U) && text.find('\0')==std::string::npos,"Gameplay paths/type exceed limits or contain NUL.");
            }
            if(!profile.contains("values"))profile["values"]=Json::object();
            const auto& values=profile.at("values");require(values.is_object() && values.size()<=128,"Gameplay values need at most 128 fields.");
            for(const auto& [name,value]:values.items()) {
                require(!name.empty() && name.size()<=64 && name.find('\0')==std::string::npos,"Invalid gameplay field name.");
                require((value.is_number() && std::isfinite(value.get<double>())) || (value.is_string() && value.get_ref<const std::string&>().size()<=4096 && value.get_ref<const std::string&>().find('\0')==std::string::npos),"Gameplay field values must be finite numbers or bounded NUL-free strings.");
            }
            require(profile.dump().size()<=65536,"Gameplay profile exceeds 64 KiB.");
        }
        for(const auto& receipt:gameplay_receipts)if(receipt.at("params").at("request_id")==params.at("request_id")) {
            require(receipt.at("params")==normalized,"Gameplay configuration request ID reused with different parameters.",-32010);
            auto result=receipt.at("result");result["replayed"]=true;return result;
        }
        require(!world->runtime_status().active,"Stop runtime before changing the gameplay launch profile.",-32009);
        require(expected==gameplay_generation,"Gameplay configuration generation conflict.",-32009);
        require(gameplay_generation<max_integer,"Gameplay configuration generation exhausted.");
        auto candidate=profile;
        if(!candidate.is_null())for(const auto* name:{"hostfxr","bridge","assembly"}) {
            auto path=path_of(candidate.at(name).get<std::string>());if(path.is_relative())path=world_path.parent_path()/path;
            std::error_code error_code;path=fs::canonical(path,error_code);
            require(!error_code && fs::is_regular_file(path,error_code) && !error_code,"Gameplay launch file does not exist or is not a regular file.");
            const auto text=path_text(path);require(text.size()<=4096,"Resolved gameplay launch path exceeds 4096 bytes.");candidate[name]=text;
        }
        require(candidate.is_null() || candidate.dump().size()<=65536,"Resolved gameplay profile exceeds 64 KiB.");
        const Json result={{"generation",gameplay_generation+1},{"profile",candidate},{"replayed",false}};
        auto receipts=gameplay_receipts;if(receipts.size()==32)receipts.erase(receipts.begin());
        receipts.push_back({{"params",std::move(normalized)},{"result",result}});
        gameplay_profile=std::move(candidate);++gameplay_generation;gameplay_receipts.swap(receipts);return result;
    }
    void sync_playback() {
        const auto state=world->runtime_status();
        if((!state.active && !play_session.empty()) || (state.active && state.session_id!=play_session)) {
            release_input(true);runtime_parents.clear();runtime_parent_session.clear();
            playing=false;play_session=state.active ? state.session_id : std::string{};play_tick=state.active ? state.tick : 0;
            play_clock=PlayerClock{};play_last=Clock::now();capture_hold=false;play_error.clear();
        } else if(state.tick!=play_tick) {
            if(playing) { playing=false;release_input();play_error="Runtime advanced outside the desktop playback clock; resume explicitly."; }
            play_tick=state.tick;play_clock.advance(0,false);play_last=Clock::now();capture_hold=false;
        }
        if(state.active && runtime_parent_session!=state.session_id) {
            // A save restore can replace the session without runtime.start.
            // Query the actual frozen runtime, never the current authored tree.
            Parents hierarchy;
            for(const auto& [child,parent]:world->runtime_hierarchy())hierarchy.emplace(child,parent);
            runtime_parents.swap(hierarchy);runtime_parent_session=state.session_id;
        }
    }
    Json playback_json() const {
        return {{"state",play_session.empty() ? "stopped" : playing ? "playing" : "paused"},
            {"session_id",play_session.empty() ? Json(nullptr) : Json(play_session)},{"tick",play_session.empty() ? Json(nullptr) : Json(play_tick)},
            {"fixed_dt",Runtime::fixed_dt},{"max_ticks_per_poll",8},{"dropped_seconds",play_clock.dropped_seconds()},
            {"last_error",play_error.empty() ? Json(nullptr) : Json(play_error)},
            {"suspended",playing && capture && capture->state=="queued" ? Json("capture") : Json(nullptr)}};
    }
    Json play_command(const std::string& method,const Json& params) {
        sync_playback();
        if(method=="desktop.play.inspect") { fields(params,{});return playback_json(); }
        if(method=="desktop.play.start") {
            fields(params,{"revision","session_id","paused","expected_gameplay_generation"},{"revision","session_id"});
            integer(params.at("revision"));identifier(params.at("session_id"));require(!params.contains("paused") || params.at("paused").is_boolean(),"Paused must be boolean.");
            require(gameplay_profile.is_null() || params.contains("expected_gameplay_generation"),"Configured gameplay requires expected_gameplay_generation.");
            if(params.contains("expected_gameplay_generation"))require(integer(params.at("expected_gameplay_generation"))==gameplay_generation,"Gameplay configuration generation conflict.",-32009);
            const bool fresh=!world->runtime_status().active;
            const auto started=world_call("runtime.start",{{"revision",params.at("revision")},{"session_id",params.at("session_id")}});
            if(fresh && !gameplay_profile.is_null()) {
                // A fresh runtime has no receipts. Its session ID is therefore
                // a collision-free internal load receipt in this new table.
                auto load=gameplay_profile;load["session_id"]=params.at("session_id");load["request_id"]=params.at("session_id");load["expected_tick"]=0;load["expected_revision"]=0;
                try { (void)world_call("runtime.gameplay.load",load); }
                catch(const std::exception& failure) {
                    const std::string detail=failure.what();
                    (void)world_call("runtime.stop",{{"session_id",params.at("session_id")}});
                    sync_playback();play_error=detail;expire_gizmo();expire_capture();throw;
                }
            }
            sync_playback();
            // Replaying start must not resume a runtime that was later paused.
            if(!started.at("replayed").get<bool>()) { playing=!params.value("paused",false);play_last=Clock::now(); }
            expire_gizmo();expire_capture();return playback_json();
        }
        if(method=="desktop.play.stop") {
            fields(params,{"session_id"},{"session_id"});identifier(params.at("session_id"));world_call("runtime.stop",params);sync_playback();expire_capture();return playback_json();
        }
        if(method=="desktop.play.step") {
            require(!playing,"Pause desktop playback before stepping.",-32009);
            const auto result=world_call("runtime.step",params);sync_playback();expire_capture();return result;
        }
        require(method=="desktop.play.pause" || method=="desktop.play.resume","Unknown desktop playback method.",-32601);
        fields(params,{"session_id"},{"session_id"});const auto id=identifier(params.at("session_id"));
        require(!play_session.empty() && id==play_session,"Runtime session conflict.",-32009);
        const bool next=method=="desktop.play.resume";
        if(!next)release_input();
        if(next!=playing) { playing=next;play_clock.advance(0,false);play_last=Clock::now();capture_hold=false; }
        if(next)play_error.clear();return playback_json();
    }
    void pump_playback() {
        sync_playback();expire_capture();const auto now=Clock::now();
        const double elapsed=std::chrono::duration<double>(now-play_last).count();play_last=now;
        const bool held=capture && capture->state=="queued";
        if(!playing || held || capture_hold) { play_clock.advance(0,false);capture_hold=held;return; }
        try {
            const auto ticks=play_clock.advance(elapsed,true);if(!ticks)return;
            const bool apply=game_input && input_focused;
            for(std::uint32_t index=0;index<ticks;++index) {
                // Prepare all allocating input/inspection data before entering
                // the transaction. Only a successful tick consumes its edges
                // and publishes this poll's committed input prefix.
                std::vector<RuntimeInput> inputs;
                Json applied=nullptr;
                if(apply) {
                    inputs.push_back(game_input->peek(input_controller));
                    auto frame=input_frame(inputs.front(),true);
                    applied=index==0 ? Json{{"first_tick",play_tick+1},{"ticks",0},{"input",frame},{"frames",Json::array()}} : last_input_applied;
                    applied["frames"].push_back(std::move(frame));
                    applied["ticks"]=index+1;
                }
                const auto result=world->advance_tick(play_session,play_tick,inputs);
                if(result.replaced) {
                    // No source-world input or presentation may reach the
                    // restored world, even when catch-up ticks remain.
                    sync_playback();expire_gizmo();expire_capture();play_last=Clock::now();return;
                }
                play_tick=result.current_tick;
                if(apply) { game_input->commit_tick();last_input_applied.swap(applied); }
                // Each automatic tick is its own commit. Earlier successful
                // ticks survive a later failure; explicit RPC batches retain
                // their existing all-or-nothing rollback contract.
                // Synchronous storage time must not become catch-up work.
                if(result.save_serviced)play_last=Clock::now();
            }
        }catch(const std::exception& failure) {
            playing=false;release_input();play_clock.advance(0,false);capture_hold=false;
            const auto state=world->runtime_status();play_tick=state.active ? state.tick : 0;
            play_error=failure.what();
        }
    }
    bool game_selected() const { return explicit_views || view_mode=="game"; }
    const std::string& selected_game_camera() const { return explicit_views ? game_camera : view_camera; }
    static void validate_view(const std::string& view) { require(view=="scene" || view=="game","View must be scene or game."); }
    ViewportState& slot(const std::string& view) { return view=="game" ? game_view : static_cast<ViewportState&>(*this); }
    const ViewportState& slot(const std::string& view) const { return view=="game" ? game_view : static_cast<const ViewportState&>(*this); }
    Json explicit_view_json(const std::string& view) const {
        return {{"mode",view},{"camera",view=="game" && !game_camera.empty() ? Json(game_camera) : Json(nullptr)}};
    }
    Json set_game_camera(const Json& params) {
        fields(params,{"camera"},{"camera"});std::string next;
        require(explicit_views || !viewport,"Detach the legacy viewport before selecting an independent Game camera.",-32009);
        if(!params.at("camera").is_null()) {
            next=identifier(params.at("camera"));const auto runtime=world->runtime_status();
            (void)(runtime.active ? world->runtime_camera_snapshot(next) : world->authored_camera_snapshot(next));
        }
        if(next!=game_camera) {
            require(game_camera_revision<max_integer,"Game camera revision exhausted.");
            release_input();game_camera=std::move(next);++game_camera_revision;
            game_view.cached_snapshot.reset();game_view.preparation_error.clear();
            fail_capture(-32009,"Game camera changed before capture presentation.","game");
        }
        return explicit_view_json("game");
    }
    Json view_json() const { return {{"mode",view_mode},{"camera",view_camera.empty() ? Json(nullptr) : Json(view_camera)}}; }
    Json cameras_json() {
        const auto runtime=world->runtime_status();Json cameras=Json::array();
        for(const auto& item:world->cameras(runtime.active))cameras.push_back({{"id",item.id},{"vertical_fov",item.vertical_fov},{"near",item.near_plane},{"far",item.far_plane}});
        return {{"source",runtime.active ? "runtime" : "authored"},{"revision",revision()},
            {"session_id",runtime.active ? Json(runtime.session_id) : Json(nullptr)},{"tick",runtime.active ? Json(runtime.tick) : Json(nullptr)},{"cameras",cameras}};
    }
    Json set_view(const Json& params) {
        require(!explicit_views,"Independent viewports use desktop.game.camera; Scene is always independently available.",-32009);
        fields(params,{"mode","camera"},{"mode"});require(params.at("mode").is_string(),"View mode must be a string.");
        const auto mode=params.at("mode").get<std::string>();require(mode=="scene" || mode=="game","View mode must be scene or game.");
        std::string id;
        if(mode=="game") {
            require(params.contains("camera"),"Game view requires a Camera entity.");id=identifier(params.at("camera"));
            const auto runtime=world->runtime_status();
            // Prepare before changing selection: invalid lenses, absent cameras,
            // missing assets or nonrigid hierarchies preserve the current view.
            (void)(runtime.active ? world->runtime_camera_snapshot(id) : world->authored_camera_snapshot(id));
        } else require(!params.contains("camera"),"Scene view uses the inspection camera; omit camera.");
        if(mode!=view_mode || id!=view_camera) {
            require(view_revision<max_integer,"View revision exhausted.");release_input();cancel_gizmo();view_mode=mode;view_camera=std::move(id);++view_revision;
            fail_capture(-32009,"Viewport camera changed before capture presentation.");
        }
        return view_json();
    }
    void scene_view_required() const { require(explicit_views || view_mode=="scene","Switch to Scene view to navigate or manipulate authored entities.",-32009); }
    void fail_capture(int code,const std::string& detail,const std::string& target={}) {
        if(capture && capture->state=="queued" && (target.empty() || capture->target==target)) { capture->state="error";capture->code=code;capture->detail=detail; }
    }
    void expire_capture() {
        if(!capture || capture->state!="queued")return;
        if(Clock::now()>=capture->deadline) { fail_capture(-32003,"Capture timed out waiting for a drawable native viewport.");return; }
        const auto state=world->runtime_status();
        const bool camera_changed=capture->target=="game" ? game_camera_revision!=capture->camera_revision :
            camera_revision!=capture->camera_revision || gizmo_generation!=capture->gizmo_generation || (capture->target=="legacy" && view_revision!=capture->view_revision);
        if(revision()!=capture->revision || camera_changed || state.active!=capture->runtime || (state.active && (state.session_id!=capture->session || state.tick!=capture->tick)))
            fail_capture(-32009,"World, runtime tick, or camera changed before capture presentation.");
    }
    std::string capture_path(const Json& value) const {
        require(value.is_string(),"Capture path must be text.");const auto path=path_of(value.get<std::string>());require(fs::is_directory(fs::absolute(path).parent_path()),"Capture parent directory does not exist.");
        require(fs::symlink_status(path).type()==fs::file_type::not_found,"Capture requires a previously absent file.");const auto normalized=fs::weakly_canonical(fs::absolute(path));
        for(const auto* suffix:{"",".lock",".pending",".previous",".previous.pending"}) { auto reserved=world_path;reserved+=suffix;require(!world_detail::same_path_name(normalized,reserved),"Capture overlaps reserved world storage."); }
        auto assets=world_path;assets+=".assets";require(!inside(normalized,fs::weakly_canonical(assets)),"Capture overlaps the world asset store.");return path_text(normalized);
    }
    static Json render_json(const ViewportState& view) {
        if(!view.last_render)return nullptr;
        const auto& report=*view.last_render;
        return {{"available",report.available},{"success",report.success},{"gpu",report.gpu_name},{"hardware",report.hardware},
            {"width",report.width},{"height",report.height},{"samples",report.samples},{"frames_presented",report.frames_presented},
            {"nvrhi_errors",report.validation_errors},{"capture_written",report.capture_written},{"detail",report.detail}};
    }
    Json render_json() const { return render_json(*this); }
    static void remember_render(ViewportState& view) {
        if(!view.viewport)return;
        auto report=view.viewport->report();
        if(!view.last_render || !report.gpu_name.empty())view.last_render=std::move(report);
    }
    void remember_render() { remember_render(*this); }
    Json views_json() const {
        Json result=Json::object();
        for(const auto* name:{"scene","game"}) {
            const auto& view=slot(name);const auto extent=view.viewport ? view.viewport->extent() : std::array<std::uint32_t,2>{};
            result[name]={{"attached",bool(view.viewport)},{"camera",std::string_view(name)=="scene" ? camera.json() : (game_camera.empty() ? Json(nullptr) : Json(game_camera))},
                {"graphics_error",view.graphics_error.empty() ? Json(nullptr) : Json(view.graphics_error)},
                {"preparation_error",view.preparation_error.empty() ? Json(nullptr) : Json(view.preparation_error)},
                {"extent",extent},{"presented_revision",view.presented_revision ? Json(*view.presented_revision) : Json(nullptr)},
                {"presented_tick",view.presented_tick ? Json(*view.presented_tick) : Json(nullptr)},{"frames_presented",view.presented_frames},{"render",render_json(view)}};
        }
        return result;
    }
    void changed_gizmo() {
        require(gizmo_generation<max_integer,"Gizmo generation exhausted.");++gizmo_generation;
        fail_capture(-32009,"Gizmo or selection changed before capture presentation.",explicit_views ? "scene" : "legacy");
    }
    void cancel_gizmo() { if(gesture) { gesture.reset();changed_gizmo(); } }
    void expire_gizmo() {
        if(!gesture)return;
        bool changed=revision()!=gesture->revision || camera_revision!=gesture->camera_revision || selected!=gesture->entity || world->runtime_status().active;
        if(viewport) { const auto size=viewport->extent();changed|=size[0]!=gesture->width || size[1]!=gesture->height; }
        if(changed)cancel_gizmo();
    }
    Json gizmo_state() const {
        Json active=nullptr;
        if(gesture)active={{"drag_id",gesture->id},{"axis",axis_name(gesture->drag.axis)},{"transform",gesture->transform}};
        return {{"mode",gizmo_mode},{"space",gizmo_space},{"generation",gizmo_generation},{"active",active}};
    }
    std::array<std::uint32_t,2> gizmo_extent(const Json& params) const {
        const auto w=integer(params.at("width")),h=integer(params.at("height"));
        require(w>=1 && h>=1 && w<=16384 && h<=16384,"Gizmo viewport dimensions must be 1..16384 physical pixels.");
        if(viewport) { const auto actual=viewport->extent();require(w==actual[0] && h==actual[1],"Gizmo dimensions differ from attached viewport.",-32009); }
        return {static_cast<std::uint32_t>(w),static_cast<std::uint32_t>(h)};
    }
    EditorGizmoMode gizmo_kind() const { return gizmo_mode=="rotate" ? EditorGizmoMode::rotate : gizmo_mode=="scale" ? EditorGizmoMode::scale : EditorGizmoMode::move; }
    std::array<Point,3> gizmo_basis(const Matrix4& matrix,const Json* transform=nullptr) const {
        if(gizmo_space=="world")return {Point{1,0,0},Point{0,1,0},Point{0,0,1}};
        std::array<Point,3> basis{Point{matrix[0],matrix[1],matrix[2]},Point{matrix[4],matrix[5],matrix[6]},Point{matrix[8],matrix[9],matrix[10]}};
        // A local rotation is parent*R*Q*S. Its ring follows parent*R, not
        // the object's own scale S, which remains after the inserted Q.
        if(gizmo_mode=="rotate" && transform)for(std::size_t i=0;i<3;++i)for(auto& x:basis[i])x/=transform->at("scale").at(i).get<double>();
        return basis;
    }
    EditorGizmo gizmo_geometry(std::uint32_t width,std::uint32_t height) {
        if((!explicit_views && view_mode!="scene") || gizmo_mode=="none" || selected.empty() || world->runtime_status().active)return {};
        Matrix4 matrix;Json transform;
        if(gesture)matrix=multiply(gesture->parent,transform_matrix(gesture->transform));
        else {
            try { matrix=world_call("entity.world_transform",{{"id",selected}}).at("matrix").get<Matrix4>(); }
            catch(const Failure& failure) { if(failure.code==-32004)return {};throw; }
        }
        if(gizmo_mode=="rotate" && gizmo_space=="local")transform=gesture ? gesture->transform : world_call("entity.get",{{"id",selected},{"component","Transform"}}).at("value");
        SceneSnapshot scene;scene.camera_world=camera.native().world;scene.vertical_fov=camera.fov;scene.near_plane=camera.near_plane;scene.far_plane=camera.far_plane;
        return make_editor_gizmo(scene,width,height,{matrix[12],matrix[13],matrix[14]},gizmo_basis(matrix,transform.is_null() ? nullptr : &transform),gizmo_kind(),gesture ? std::optional(gesture->drag.axis) : std::nullopt);
    }
    Json gizmo_inspect(const Json& params) {
        fields(params,{"width","height"},{"width","height"});const auto size=gizmo_extent(params);expire_gizmo();
        const auto geometry=gizmo_geometry(size[0],size[1]);auto result=gizmo_state();result["handles"]=Json::array();
        for(std::size_t i=0;i<3;++i)result["handles"].push_back({{"axis",axis_name(static_cast<EditorGizmoAxis>(i))},{"visible",geometry.handles[i].visible},{"points",geometry.handles[i].screen_points}});
        result["width"]=size[0];result["height"]=size[1];result["overlay_vertices"]=geometry.triangles.size();return result;
    }
    Json gizmo_configure(const Json& params) {
        fields(params,{"mode","space"},{"mode","space"});require(params.at("mode").is_string() && params.at("space").is_string(),"Gizmo mode and space must be strings.");
        const auto mode=params.at("mode").get<std::string>(),space=params.at("space").get<std::string>();
        require(mode=="none" || mode=="move" || mode=="rotate" || mode=="scale","Unknown gizmo mode.");require(space=="world" || space=="local","Unknown gizmo space.");
        if(mode!=gizmo_mode || space!=gizmo_space) { cancel_gizmo();gizmo_mode=mode;gizmo_space=space;changed_gizmo(); }
        return gizmo_state();
    }
    Json gizmo_begin(const Json& params) {
        scene_view_required();
        fields(params,{"revision","width","height","x","y"},{"revision","width","height","x","y"});
        const auto expected=integer(params.at("revision"));const auto size=gizmo_extent(params);
        const double x=number(params.at("x"),0,size[0],"Gizmo x is outside the viewport."),y=number(params.at("y"),0,size[1],"Gizmo y is outside the viewport.");
        expire_gizmo();require(expected==revision(),"Authored revision conflict.",-32009);require(!gesture,"A gizmo drag is already active.",-32009);
        require(!world->runtime_status().active,"Stop runtime before editing transforms.",-32009);
        const auto geometry=gizmo_geometry(size[0],size[1]);const auto axis=hit_test_editor_gizmo(geometry,x,y);
        if(!axis)return {{"started",false}};
        const auto drag=begin_editor_gizmo_drag(geometry,*axis,x,y);if(!drag)return {{"started",false}};
        require(next_drag<=max_integer,"Gizmo identity limit reached.");GizmoGesture candidate;
        candidate.id=next_drag;candidate.revision=expected;candidate.camera_revision=camera_revision;candidate.width=size[0];candidate.height=size[1];candidate.entity=selected;candidate.drag=*drag;
        candidate.mode=gizmo_kind();candidate.local=gizmo_space=="local";
        const auto entity=world_call("entity.get",{{"id",selected}}).at("value");candidate.original=entity.at("components").at("Transform");candidate.transform=candidate.original;
        if(!entity.at("parent").is_null())candidate.parent=world_call("entity.world_transform",{{"id",entity.at("parent")}}).at("matrix").get<Matrix4>();
        candidate.world=multiply(candidate.parent,transform_matrix(candidate.original));candidate.basis=gizmo_basis(candidate.world,&candidate.original);
        changed_gizmo();gesture=std::move(candidate);++next_drag;
        return {{"started",true},{"drag_id",gesture->id},{"axis",axis_name(*axis)}};
    }
    GizmoGesture& guarded_gesture(const Json& params) {
        const auto id=integer(params.at("drag_id"));expire_gizmo();require(gesture && gesture->id==id,"Gizmo drag is absent or invalidated.",-32009);return *gesture;
    }
    Json gizmo_update(const Json& params) {
        fields(params,{"drag_id","x","y","snap"},{"drag_id","x","y"});
        const double x=number(params.at("x"),-1e6,1e6,"Gizmo x is out of range."),y=number(params.at("y"),-1e6,1e6,"Gizmo y is out of range.");
        require(!params.contains("snap") || params.at("snap").is_boolean(),"Gizmo snap must be boolean.");auto& current=guarded_gesture(params);auto drag=current.drag;
        const auto delta=update_editor_gizmo_drag(drag,x,y);require(delta.has_value(),"Pointer cannot resolve a valid gizmo transform.");
        double value=*delta;
        if(params.value("snap",false)) {
            if(current.mode==EditorGizmoMode::move)value=std::round(value/.25)*.25;
            else if(current.mode==EditorGizmoMode::rotate) { constexpr double step=3.14159265358979323846/12;value=std::round(value/step)*step; }
            else value=std::max(.1,1+std::round((value-1)/.1)*.1);
        }
        auto transform=gesture_transform(current,value);
        if(transform!=current.transform) {
            // Prepare the entire preview first. Failures retain the previous
            // valid drag clock and snapshot; no authored state has been touched.
            auto preview=world->authored_preview(camera.native(),current.entity,transform.at("position").get<Point>(),transform.at("rotation").get<std::array<double,4>>(),transform.at("scale").get<Point>());
            changed_gizmo();current.transform=std::move(transform);current.preview=std::move(preview);
        }
        current.drag=std::move(drag);return {{"drag_id",current.id},{"transform",current.transform}};
    }
    Json gizmo_commit(const Json& params) {
        fields(params,{"drag_id","request_id"},{"drag_id","request_id"});const auto id=integer(params.at("drag_id"));
        const auto receipt=identifier(params.at("request_id"));
        if(!committed_gesture.is_null() && committed_gesture.at("drag_id")==id) {
            require(committed_gesture.at("request_id")==receipt,"Gizmo commit retry differs from its retained receipt.",-32009);return committed_gesture.at("result");
        }
        auto& current=guarded_gesture(params);
        Json result={{"revision",current.revision},{"changed",false}};
        if(current.transform!=current.original)result=world_call("world.transact",{{"base_revision",current.revision},{"request_id",receipt},
            {"ops",Json::array({{{"op","component.set"},{"id",current.entity},{"type","Transform"},{"value",current.transform}}})}});
        committed_gesture={{"drag_id",id},{"request_id",receipt},{"result",result}};cancel_gizmo();return result;
    }
    Json inspect() {
        sync_playback();expire_gizmo();expire_capture();const auto saves=world->save_status();
        return {{"saves",{{"generation",saves.generation},{"root",saves.root.empty() ? Json(nullptr) : Json(saves.root)}}},{"binding_mode",explicit_views ? "explicit" : "legacy"},{"views",views_json()},{"input",input_json()},{"playback",playback_json()},{"gameplay",gameplay_json(false)},{"view",view_json()},{"gizmo",gizmo_state()},{"revision",revision()},{"runtime",runtime_json()},{"selected",selected.empty() ? Json(nullptr) : Json(selected)},
            {"attached",bool(viewport)},{"graphics_error",graphics_error.empty() ? Json(nullptr) : Json(graphics_error)},{"camera",camera.json()},
            {"capture",capture ? capture->json() : Json(nullptr)},{"frames_presented",presented_frames},{"render",render_json()},
            {"presented_revision",presented_revision ? Json(*presented_revision) : Json(nullptr)},{"presented_tick",presented_tick ? Json(*presented_tick) : Json(nullptr)},
            {"endpoint",endpoint.empty() ? Json(nullptr) : Json(endpoint)}};
    }
    Json pick(const Json& params) {
        scene_view_required();
        fields(params,{"revision","x","y","aspect"},{"revision","x","y","aspect"});
        const auto expected=integer(params.at("revision"));
        const double x=number(params.at("x"),0,1,"Pick x must be in [0,1]."),y=number(params.at("y"),0,1,"Pick y must be in [0,1].");
        const double aspect=number(params.at("aspect"),.01,100,"Viewport aspect must be in [0.01,100].");
        require(expected==revision(),"Authored revision conflict.",-32009);
        expire_gizmo();const auto scene=gesture && gesture->preview ? *gesture->preview : snapshot();const auto runtime=world->runtime_status();
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
        scene_view_required();
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
        if(candidate.json()!=camera.json()) { require(camera_revision<max_integer,"Camera revision exhausted.");cancel_gizmo();camera=candidate;++camera_revision; }
        expire_capture();return {{"camera",camera.json()},{"target",target},{"distance",distance}};
    }
    Json describe() {
        auto object=[](Json properties,Json required=Json::array()) { return Json{{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}}; };
        const Json integer_schema={{"type","integer"},{"minimum",0},{"maximum",max_integer}};
        Json methods=Json::object();methods["desktop.describe"]=object(Json::object());methods["desktop.inspect"]=object(Json::object());
        const Json id_schema={{"type","string"},{"pattern","^[0-9a-f]{32}$"}};
        methods["desktop.play.start"]=object({{"revision",integer_schema},{"session_id",id_schema},{"paused",{{"type","boolean"}}},{"expected_gameplay_generation",integer_schema}},{"revision","session_id"});
        const Json gameplay_path={{"type","string"},{"minLength",1},{"maxLength",4096}};
        const auto gameplay_launch=object({{"hostfxr",gameplay_path},{"bridge",gameplay_path},{"assembly",gameplay_path},
            {"type",{{"type","string"},{"minLength",1},{"maxLength",512}}},{"values",{{"type","object"},{"maxProperties",128},{"additionalProperties",{{"type",{"number","string"}}}}}}},{"hostfxr","bridge","assembly","type"});
        methods["desktop.gameplay.configure"]=object({{"request_id",id_schema},{"expected_generation",integer_schema},{"profile",{{"oneOf",Json::array({gameplay_launch,Json{{"type","null"}}})}}}},{"request_id","expected_generation","profile"});
        methods["desktop.gameplay.inspect"]=object(Json::object());
        for(const auto* name:{"desktop.play.pause","desktop.play.resume","desktop.play.stop"})methods[name]=object({{"session_id",id_schema}},{"session_id"});
        methods["desktop.play.inspect"]=object(Json::object());
        methods["desktop.play.step"]=world_call("world.describe").at("methods").at("runtime.step");
        methods["desktop.cameras"]=object(Json::object());
        methods["desktop.controllers"]=object(Json::object());
        methods["desktop.input.inspect"]=object(Json::object());
        methods["desktop.input.configure"]=object({{"session_id",id_schema},{"controller",id_schema},
            {"input_profile",{{"type","string"},{"minLength",1},{"maxLength",4096}}},{"input_revision",integer_schema}},{"session_id","controller"});
        methods["desktop.input.focus"]=object({{"session_id",id_schema},{"focused",{{"type","boolean"}}}},{"session_id","focused"});
        const Json control_event=object({{"control",{{"type","string"},{"minLength",1},{"maxLength",64}}},{"down",{{"type","boolean"}}}},{"control","down"});
        const Json motion_event=object({{"motion",{{"type","array"},{"minItems",2},{"maxItems",2},{"items",{{"type","number"},{"minimum",-1e6},{"maximum",1e6}}}}}},{"motion"});
        methods["desktop.input.events"]=object({{"session_id",id_schema},{"request_id",id_schema},{"events",{{"type","array"},{"maxItems",256},{"items",{{"oneOf",Json::array({control_event,motion_event})}}}}}},{"session_id","request_id","events"});
        methods["desktop.view"]={{"oneOf",Json::array({object({{"mode",{{"const","scene"}}}},{"mode"}),object({{"mode",{{"const","game"}}},{"camera",id_schema}},{"mode","camera"})})}};
        methods["desktop.game.camera"]=object({{"camera",{{"type",{"string","null"}},{"pattern","^[0-9a-f]{32}$"}}}},{"camera"});
        methods["desktop.select"]=object({{"id",{{"type",{"string","null"}},{"pattern","^[0-9a-f]{32}$"}}}},{"id"});
        const Json aspect_schema={{"type","number"},{"minimum",.01},{"maximum",100}},unit_schema={{"type","number"},{"minimum",0},{"maximum",1}};
        methods["desktop.pick"]=object({{"revision",integer_schema},{"x",unit_schema},{"y",unit_schema},{"aspect",aspect_schema}},{"revision","x","y","aspect"});
        methods["desktop.frame"]=object({{"revision",integer_schema},{"id",{{"type","string"},{"pattern","^[0-9a-f]{32}$"}}},{"aspect",aspect_schema}},{"revision","aspect"});
        methods["desktop.camera"]=object({{"position",{{"type","array"},{"items",{{"type","number"},{"minimum",-1e9},{"maximum",1e9}}},{"minItems",3},{"maxItems",3}}},
            {"yaw",{{"type","number"},{"minimum",-1e9},{"maximum",1e9}}},{"pitch",{{"type","number"},{"minimum",-89},{"maximum",89}}},{"vertical_fov",{{"type","number"},{"minimum",5},{"maximum",150}}},
            {"near",{{"type","number"},{"minimum",.001}}},{"far",{{"type","number"},{"maximum",1e7}}}});
        const Json dimension={{"type","integer"},{"minimum",1},{"maximum",16384}},pixel={{"type","number"},{"minimum",-1e6},{"maximum",1e6}},receipt={{"type","string"},{"pattern","^[0-9a-f]{32}$"}};
        methods["desktop.gizmo.configure"]=object({{"mode",{{"enum",{"none","move","rotate","scale"}}}},{"space",{{"enum",{"world","local"}}}}},{"mode","space"});
        methods["desktop.gizmo.inspect"]=object({{"width",dimension},{"height",dimension}},{"width","height"});
        methods["desktop.gizmo.begin"]=object({{"revision",integer_schema},{"width",dimension},{"height",dimension},{"x",{{"type","number"},{"minimum",0},{"maximum",16384}}},{"y",{{"type","number"},{"minimum",0},{"maximum",16384}}}},{"revision","width","height","x","y"});
        methods["desktop.gizmo.update"]=object({{"drag_id",integer_schema},{"x",pixel},{"y",pixel},{"snap",{{"type","boolean"}}}},{"drag_id","x","y"});
        methods["desktop.gizmo.commit"]=object({{"drag_id",integer_schema},{"request_id",receipt}},{"drag_id","request_id"});
        methods["desktop.gizmo.cancel"]=object(Json::object());
        methods["desktop.capture"]=object({{"revision",integer_schema},{"path",{{"type","string"},{"minLength",1}}},{"view",{{"enum",{"scene","game"}}}}},{"revision","path"});
        methods["desktop.capture.status"]=object({{"capture_id",{{"type","integer"},{"minimum",1},{"maximum",max_integer}}}},{"capture_id"});
        return {{"methods",methods},{"world_methods","world.describe"},{"viewports",{{"names",{"scene","game"}},{"binding","Named HWNDs are independent and cannot be mixed with legacy viewport ABI. One creating UI thread; one shared world and owner poll clock."},{"camera","desktop.camera controls Scene; desktop.game.camera selects Game camera or null. Attach is lazy; missing camera/asset errors remain local and repairable."},{"state","desktop.inspect.views reports attachment, extent, graphics/preparation errors and presentation metadata per pane."}}},{"capture",{{"completion","Asynchronous: queue returns capture_id/state; inspect status after poll/draw."},{"capacity",1},{"retained_results",1},{"timeout_ms",2000},{"guards","Authored revision, runtime session and tick plus target camera; Scene also guards gizmo/selection. Only target draw completes the job. Default target is Scene for named panes or current legacy view."},{"format","BMP; native viewport only; exclusive new path"}}},
            {"playback",{{"clock","Owner poll only; fixed 60 Hz; at most 8 catch-up ticks/poll; each automatic tick commits independently. A failed tick pauses at the last successful tick. Explicit multi-tick runtime.step remains atomic. Excess wall time is dropped and reported. Inspect, draw and capture never step."},
                {"ownership","desktop.play.start starts running unless paused:true. Direct runtime.start remains paused. Pause before manual runtime step/audio replay/gameplay edits and save.configure/save.write/save.load. Successful save.load opens a fresh paused session with cleared input. Stop discards runtime without authored writes."},
                {"capture","A queued capture holds automatic ticking until completion/error; resume discards the held wall-time interval."},
                {"background","Playback continues while the owner polls, including hidden/detached viewports. Gameplay input has a separate explicit focus gate; editor audio playback is not connected."}}},
            {"gameplay",{{"configuration","Session-local CoreCLR launch profile; configure only while stopped. Paths resolve relative to the world and must name existing regular files; validation executes no code. Semantic type/field checks occur when Play loads the module."},
                {"limits","Profile at most 64 KiB; paths 4096 UTF-8 bytes, type 512 bytes; at most 128 scalar fields with names at most 64 bytes. No source build or file watcher."},
                {"receipts","Every accepted configuration increments generation. Latest 32 normalized exact request receipts replay before runtime, generation and file checks; changed request ID payload conflicts. Receipts and configuration do not persist after host destruction."},
                {"start","A configured profile requires expected_gameplay_generation on desktop.play.start. Module loads at tick zero before playing, with initial typed overrides. Active start retry never reloads or resumes. Failure stops the newly created runtime and retains configuration; that session ID is consumed, so retry with a fresh session ID."},
                {"inspection","desktop.gameplay.inspect returns configuration and compact runtime metadata. desktop.inspect/poll omit profile and values; gameplay revision changes expose paused edits/reloads. Use runtime.gameplay.inspect for typed values/schema."},
                {"reload","Pause, then runtime.gameplay.load with current session/tick/gameplay revision and a new request ID; existing state migration and rollback semantics apply. Trusted CoreCLR project code only; shipping Native AOT packages are separate."}}},
            {"input",{{"devices","Keyboard and mouse only; input.describe controls provide physical IDs, numeric SDL codes and reserved flags. V2 profiles retain keyboard/mouse bindings, but desktop gamepad events are not supported."},
                {"configure","Active runtime controller and read-only frozen profile; missing profile means keyboard/mouse v1 defaults. input_revision requires input_profile. Successful reconfiguration releases old input; validation failure preserves it."},
                {"focus","Explicit focus true requires playing Game view using the configured controller camera. Pause, Game camera change, Game detachment or focus false releases held controls and pending edges/look; Stop/session replacement drops configuration. Resume does not regain focus."},
                {"events","Ordered atomic batches with request_id and the latest 32 successful in-memory receipts per runtime session. Exact retries return the original accepted result with replayed:true without reinjection, even after focus loss, pause or reconfiguration; changed payload conflicts. Stop/session replacement discards receipts. Beyond retention, a forgotten ID is a new request; inspect before recovery. No authored or storage writes. Repeated control-down does not retrigger held actions. Capture hold retains pending input until a committed tick."},
                {"commit","Automatic playback evaluates and commits one tick at a time. Consume pending edges and up to 180 degrees of mouse backlog per axis only after each success; held movement and analog rates apply every tick. A failed tick pauses playback and clears focus/input, retaining earlier committed ticks and their input trace."},
                {"inspection","configured,session_id,controller,camera,focused,profile,pending,accepted_batches,last_applied. pending has move/look/jump/use. last_applied records first_tick (previous+1), ticks, the first input and frames (one complete input including entity per committed tick, at most 8) for the last controlled poll. On failure it retains the successful prefix. Use frames for exact replay, including mouse backlog across ticks."}}},
            {"views",{{"scene","Free inspection camera; authored or runtime world."},{"game","Explicit Camera entity, authored when stopped, frozen runtime lens and live pose while active. No automatic camera replacement if a camera is absent after Stop; choose another camera or Scene."}}},
            {"picking",{{"coordinates","Normalized viewport coordinates, top-left origin; aspect is width/height."},{"geometry","Nearest visible snapshot geometry: transformed boxes or CPU triangle intersections, including posed skin vertices, near/far clipping and material backface culling. No GPU readback; subpixel rasterization is not reproduced."},{"mutation","None; select explicitly using desktop.select."}}},
            {"gizmo",{{"coordinates","Physical client pixels, top-left; attached extent must match. begin hit-tests handles. World/local axes; default move/world."},
                {"preview","update changes only presentation, including child transforms, lights and skin palettes. commit produces one guarded world transaction; cancel writes nothing."},
                {"invalidation","Authored revision, camera, selection, mode/space, runtime start, attached resize or detach cancels active drag."},
                {"snapping","Control: 0.25 world meters for move, 15 degrees for rotate, 0.1 scale ratio."},
                {"limits","Positive TRS scales only; world edits that require shear reject and preserve previous preview. Rotation samples must be less than 180 degrees apart. One retained successful commit receipt permits exact retries."}}},
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
                sync_playback();
                if(playing && (method=="runtime.step" || method=="runtime.audio.replay" || method=="runtime.gameplay.load" || method=="runtime.gameplay.load_native" || method=="runtime.gameplay.edit" ||
                    method=="save.configure" || method=="save.write" || method=="save.load"))
                    throw Failure(-32009,"Pause desktop playback before manual runtime mutation or saving/loading.");
                auto response=world->request(bytes,WorldRequestScope::shared_editor);
                if(method=="world.describe" && !notification) {
                    auto value=Json::parse(response);
                    if(value.contains("result"))value["result"]["editor_discovery"]="desktop.describe";
                    response=value.dump();
                }
                sync_playback();expire_gizmo();expire_capture();return response;
            }
            fields(message,{"jsonrpc","id","method","params"},{"jsonrpc","method"});const auto params=message.value("params",Json::object());Json result;
            if(method=="desktop.describe") { fields(params,{});result=describe(); }
            else if(method=="desktop.inspect") { fields(params,{});result=inspect(); }
            else if(method.starts_with("desktop.play."))result=play_command(method,params);
            else if(method.starts_with("desktop.gameplay."))result=gameplay_command(method,params);
            else if(method.starts_with("desktop.input."))result=input_command(method,params);
            else if(method=="desktop.view")result=set_view(params);
            else if(method=="desktop.game.camera")result=set_game_camera(params);
            else if(method=="desktop.cameras") { fields(params,{});result=cameras_json(); }
            else if(method=="desktop.controllers") { fields(params,{});result=controllers_json(); }
            else if(method=="desktop.gizmo.configure")result=gizmo_configure(params);
            else if(method=="desktop.gizmo.inspect")result=gizmo_inspect(params);
            else if(method=="desktop.gizmo.begin")result=gizmo_begin(params);
            else if(method=="desktop.gizmo.update")result=gizmo_update(params);
            else if(method=="desktop.gizmo.commit")result=gizmo_commit(params);
            else if(method=="desktop.gizmo.cancel") { fields(params,{});cancel_gizmo();result=gizmo_state(); }
            else if(method=="desktop.pick")result=pick(params);
            else if(method=="desktop.frame")result=frame(params);
            else if(method=="desktop.camera") {
                scene_view_required();
                Camera candidate=camera;candidate.update(params);
                if(candidate.json()!=camera.json()) { require(camera_revision<max_integer,"Camera revision exhausted.");cancel_gizmo();camera=candidate;++camera_revision; }
                expire_capture();result=camera.json();
            }
            else if(method=="desktop.select") {
                fields(params,{"id"},{"id"});std::string next;if(!params.at("id").is_null()) { next=identifier(params.at("id"));world_call("entity.get",{{"id",next}}); }if(next!=selected) { cancel_gizmo();changed_gizmo(); }selected=std::move(next);result={{"selected",selected.empty() ? Json(nullptr) : Json(selected)}};
            }else if(method=="desktop.capture") {
                fields(params,{"revision","path","view"},{"revision","path"});expire_capture();
                std::string target=explicit_views ? "scene" : "legacy";
                if(params.contains("view")) {
                    require(params.at("view").is_string(),"Capture view must be text.");const auto requested=params.at("view").get<std::string>();validate_view(requested);
                    if(explicit_views)target=requested;else require(requested==view_mode,"Capture view differs from the legacy viewport mode.",-32009);
                }
                auto& destination=slot(target);
                require(destination.viewport && !destination.faulted,"A working attached target viewport is required.",-32003);
                require(!capture || capture->state!="queued","One capture is already pending.",-32009);
                const auto expected=integer(params.at("revision"));require(expected==revision(),"Authored revision conflict.",-32009);const auto path=capture_path(params.at("path"));require(next_capture<=max_integer,"Capture identity limit reached.");const auto state=world->runtime_status();
                Capture candidate;candidate.target=target;candidate.id=next_capture++;candidate.revision=expected;candidate.path=path;candidate.camera_revision=target=="game" ? game_camera_revision : camera_revision;candidate.gizmo_generation=gizmo_generation;candidate.view_revision=view_revision;candidate.runtime=state.active;candidate.session=state.session_id;candidate.tick=state.tick;candidate.deadline=Clock::now()+std::chrono::seconds(2);capture=std::move(candidate);play_clock.advance(0,false);play_last=Clock::now();capture_hold=true;result=capture->json();
            }else if(method=="desktop.capture.status") {
                fields(params,{"capture_id"},{"capture_id"});const auto value=integer(params.at("capture_id"));require(capture && capture->id==value,"Capture result is absent or was superseded.",-32004);expire_capture();result=capture->json();
            }else throw Failure(-32601,"Unknown desktop method.");
            return notification ? std::string{} : Json{{"jsonrpc","2.0"},{"id",id},{"result",result}}.dump();
        }catch(const Failure& error_value) { return notification ? std::string{} : error_reply(id,error_value.code,error_value.what()); }
        catch(const Json::exception& error_value) { return notification ? std::string{} : error_reply(id,-32602,error_value.what()); }
        catch(const std::exception& error_value) { return notification ? std::string{} : error_reply(id,-32000,error_value.what()); }
    }
    Json poll() {
        profiling::Binding trace_binding(&world->profiler(),profiling::Source::editor_poll);
        if(server)for(const auto& incoming:server->poll())server->reply(incoming.token,request(incoming.payload));
        profiling::Scope trace_poll("editor.poll");
        pump_playback();auto state=inspect();const auto current=state.at("revision").get<std::uint64_t>();const auto runtime=state.at("runtime");
        state["world_changed"]=!observed_revision || *observed_revision!=current;state["runtime_changed"]=observed_runtime!=runtime;
        observed_revision=current;observed_runtime=runtime;return state;
    }
    SceneSnapshot snapshot(const std::string& target={}) {
        const auto runtime=world->runtime_status();const std::string effective=target.empty() ? (explicit_views ? "scene" : "legacy") : target;
        auto& destination=slot(effective);
        const bool game=effective=="game" || (effective=="legacy" && view_mode=="game");
        const auto& game_id=effective=="game" ? game_camera : view_camera;
        if(game)require(!game_id.empty(),"Select a Game camera.",-32004);
        const Json key={{"revision",revision()},{"camera",effective=="game" ? game_camera_revision : camera_revision},
            {"view",effective=="legacy" ? view_revision : 0},{"active",runtime.active},
            {"session",runtime.active ? runtime.session_id : std::string{}},{"tick",runtime.active ? runtime.tick : 0},{"mode",effective}};
        if(!destination.cached_snapshot || key!=destination.snapshot_key) {
            auto candidate=game
                ? (runtime.active ? world->runtime_camera_snapshot(game_id) : world->authored_camera_snapshot(game_id))
                : (runtime.active ? world->runtime_snapshot(camera.native()) : world->authored_snapshot(camera.native()));
            destination.cached_snapshot=std::move(candidate);destination.snapshot_key=key;
        }
        return *destination.cached_snapshot;
    }
    void attach_target(const std::string& target,void* handle) {
        auto& destination=slot(target);
        require(!destination.viewport,"Detach this viewport before attaching another HWND.");require(handle && IsWindow(static_cast<HWND>(handle)),"Attach requires a live HWND.");
        require(handle!=hwnd && handle!=game_view.hwnd,"Each viewport requires a distinct HWND.");
        DWORD process=0;const auto thread_id=GetWindowThreadProcessId(static_cast<HWND>(handle),&process);require(process==GetCurrentProcessId() && thread_id==GetCurrentThreadId(),"HWND must belong to this process and UI thread.");
        // Explicit panes may open before a Game camera exists or while assets
        // need repair. HostedViewport defers all GPU initialization until draw.
        const auto scene=target=="legacy" ? snapshot("legacy") : SceneSnapshot{};
        auto candidate=std::make_unique<HostedViewport>(render,scene,handle);
        destination.viewport=std::move(candidate);destination.hwnd=handle;destination.faulted=false;destination.graphics_error.clear();destination.preparation_error.clear();destination.presented_revision.reset();destination.presented_tick.reset();
    }
    void attach(void* handle) {
        require(!explicit_views,"Legacy viewport ABI cannot be mixed with named viewports.",-32009);attach_target("legacy",handle);
    }
    void attach_view(const std::string& view,void* handle) {
        validate_view(view);require(explicit_views || !viewport,"Detach the legacy viewport before using named viewports.",-32009);
        const bool previous=explicit_views;
        try { attach_target(view,handle);if(!explicit_views) { release_input();cancel_gizmo(); }explicit_views=true; }
        catch(...) { explicit_views=previous;throw; }
    }
    void detach_target(const std::string& target) {
        auto& destination=slot(target);
        if(target=="game" || target=="legacy")release_input();
        if(target!="game")cancel_gizmo();
        fail_capture(-32003,"Target viewport detached before capture completed.",target);
        remember_render(destination);
        destination.viewport.reset();destination.hwnd=nullptr;destination.faulted=false;destination.graphics_error.clear();destination.preparation_error.clear();destination.presented_revision.reset();destination.presented_tick.reset();
    }
    void detach() { require(!explicit_views,"Legacy viewport ABI cannot be mixed with named viewports.",-32009);detach_target("legacy"); }
    void detach_view(const std::string& view) { validate_view(view);require(explicit_views || !viewport,"Legacy viewport ABI cannot be mixed with named viewports.",-32009);if(explicit_views)detach_target(view); }
    void detach_all() { if(explicit_views) { detach_target("game");detach_target("scene"); }else detach_target("legacy"); }
    int draw_target(const std::string& target) {
        profiling::Binding trace_binding(&world->profiler(),target=="game" ? profiling::Source::editor_game : profiling::Source::editor_scene);
        const auto traced_runtime=profiling::active() ? world->profiler_context() : WorldProfilerContext{};
        profiling::SessionScope trace_session(traced_runtime.session.data());
        profiling::Scope trace_draw(target=="game" ? "editor.game.draw" : "editor.scene.draw",traced_runtime.tick);
        expire_gizmo();expire_capture();auto& destination=slot(target);if(!destination.viewport)return 0;
        require(!destination.faulted,"Graphics failed; detach and attach this viewport to recover.",-32003);
        auto pending=[&] { return capture && capture->state=="queued" && capture->target==target; };
        std::optional<SceneSnapshot> prepared;
        try {
            prepared=target!="game" && gesture && gesture->preview ? *gesture->preview : snapshot(target);
            destination.preparation_error.clear();
        }catch(const std::exception& failure) {
            destination.preparation_error=failure.what();fail_capture(-32003,failure.what(),target);
            if(target=="game" || (target=="legacy" && view_mode=="game"))release_input();
            if(target=="game" && game_camera.empty())return 0;
            throw;
        }
        const auto& scene=*prepared;const auto runtime=world->runtime_status();bool presented=false;
        if(target=="game")destination.viewport->set_overlay({});
        else { const auto extent=destination.viewport->extent();destination.viewport->set_overlay(gizmo_geometry(extent[0],extent[1]).triangles); }
        // Validate capture destination before touching GPU state.
        if(pending()) {
            try { capture_path(capture->path); }
            catch(const Failure& failure) { fail_capture(failure.code,failure.what(),target); }
            catch(const std::exception& failure) { fail_capture(-32003,failure.what(),target); }
        }
        try {
            if(pending())presented=destination.viewport->draw_capture(scene,capture->path);
            else presented=destination.viewport->draw(scene);
            remember_render(destination);
        }catch(const std::exception& failure) {
            try { remember_render(destination); }catch(...) {}
            fail_capture(-32003,failure.what(),target);destination.faulted=true;destination.graphics_error=failure.what();
            if(target=="game" || (target=="legacy" && view_mode=="game"))release_input();
            throw;
        }
        if(!presented)return 0;
        ++destination.presented_frames;destination.presented_revision=scene.revision;destination.presented_tick=runtime.active ? std::optional<std::uint64_t>(runtime.tick) : std::nullopt;
        if(pending()) {
            const auto& report=*destination.last_render;capture->state="complete";capture->result={{"path",capture->path},{"revision",capture->revision},{"scene_revision",scene.revision},{"source",runtime.active ? "runtime" : "authored"},{"tick",runtime.active ? Json(runtime.tick) : Json(nullptr)},{"width",report.width},{"height",report.height},{"frame",destination.presented_frames},{"gizmo",target=="game" ? Json(nullptr) : gizmo_state()},{"view",target=="legacy" ? view_json() : explicit_view_json(target)},{"camera_world",scene.camera_world},{"lens",{{"vertical_fov",scene.vertical_fov},{"near",scene.near_plane},{"far",scene.far_plane}}},{"preview",bool(target!="game" && gesture && gesture->preview)},{"render",render_json(destination)}};
        }
        return 1;
    }
    int draw() { require(!explicit_views,"Legacy viewport ABI cannot be mixed with named viewports.",-32009);return draw_target("legacy"); }
    int draw_view(const std::string& view) { validate_view(view);require(explicit_views || !viewport,"Legacy viewport ABI cannot be mixed with named viewports.",-32009);return explicit_views ? draw_target(view) : 0; }

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
int poima_desktop_attach_view(void* host,const char* view,void* hwnd) { return guarded<int>(host,0,[&](Bridge& value){value.attach_view(bounded_string(view,16),hwnd);return 1;}); }
int poima_desktop_draw_view(void* host,const char* view) { return guarded<int>(host,-1,[&](Bridge& value){return value.draw_view(bounded_string(view,16));}); }
void poima_desktop_detach_view(void* host,const char* view) { (void)guarded<int>(host,0,[&](Bridge& value){value.detach_view(bounded_string(view,16));return 1;}); }
void poima_desktop_destroy(void* host) {
    (void)guarded<int>(host,0,[](Bridge& value){value.detach_all();instance=nullptr;delete &value;return 1;});
}
const char* poima_desktop_error(void* host) {
    try { std::unique_lock lock(host_mutex,std::try_to_lock);if(!lock.owns_lock())return "Desktop bridge operation is already in progress.";if(host && host==instance && instance->owner==std::this_thread::get_id())return instance->error.c_str();return last_error.c_str(); }
    catch(...) { return "Desktop bridge synchronization failed."; }
}
}
