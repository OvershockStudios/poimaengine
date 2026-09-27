// SPDX-License-Identifier: Apache-2.0
#include "poima/editor.hpp"
#include "poima/editor_viewport.hpp"
#include "poima/editor_style.hpp"
#include "poima/world.hpp"
#include "poima/local_session.hpp"
#include "poima/runtime.hpp"
#include "poima/animation.hpp"
#include "world_storage.hpp"
#include <nlohmann/json.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_sdl3.h>
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <fstream>
#include <functional>
#include <deque>
#include <optional>
#include <map>
#include <random>
#include <set>

namespace poima {
namespace {
using Json=nlohmann::json;
namespace fs=std::filesystem;
constexpr double pi=3.14159265358979323846;
void require(bool value,const std::string& message) { if(!value)throw std::runtime_error(message); }
fs::path path_of(const std::string& value) { return fs::path(std::u8string(value.begin(),value.end())); }
std::string path_text(const fs::path& value) { const auto text=value.u8string();return std::string(text.begin(),text.end()); }
std::string uid() { std::random_device r;std::string s(32,'0');for(auto& c:s)c="0123456789abcdef"[r()&15];return s; }
bool inside(const fs::path& child,const fs::path& parent) {
    auto a=child.begin();for(auto b=parent.begin();b!=parent.end();++b,++a)if(a==child.end() || !world_detail::same_path_name(*a,*b))return false;return true;
}
void new_output(const std::string& name,const EditorOptions& o) {
    if(name.empty())return;
    const auto raw=path_of(name);require(fs::is_directory(fs::absolute(raw).parent_path()),"Editor output parent directory does not exist.");
    require(!fs::exists(raw) && !fs::is_symlink(fs::symlink_status(raw)),"Editor outputs must be new files: "+name);
    const auto p=fs::weakly_canonical(fs::absolute(raw));const auto w=fs::weakly_canonical(fs::absolute(path_of(o.world)));
    for(const auto* suffix:{"",".lock",".pending",".previous",".previous.pending"}) {
        auto reserved=w;reserved+=suffix;require(!world_detail::same_path_name(p,reserved),"Editor output overlaps reserved world storage.");
    }
    auto assets=w;assets+=".assets";assets=fs::weakly_canonical(assets);require(!inside(p,assets),"Editor output overlaps the immutable asset store.");
    if(!o.script.empty())require(!world_detail::same_path_name(p,fs::weakly_canonical(fs::absolute(path_of(o.script)))),"Editor output overlaps its script.");
}
void exclusive_report(const std::string& name,const std::string& bytes) {
    if(name.empty())return;
#ifdef _WIN32
    const auto h=CreateFileW(path_of(name).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(h!=INVALID_HANDLE_VALUE,"Cannot exclusively create editor report.");DWORD written=0;
    const bool ok=WriteFile(h,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) && written==bytes.size() && FlushFileBuffers(h);CloseHandle(h);require(ok,"Could not write editor report.");
#else
    const auto fd=::open(path_of(name).c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);require(fd>=0,"Cannot exclusively create editor report.");
    std::size_t offset=0;while(offset<bytes.size()) { const auto n=::write(fd,bytes.data()+offset,bytes.size()-offset);if(n<=0) { ::close(fd);throw std::runtime_error("Could not write editor report."); }offset+=static_cast<std::size_t>(n); }
    const auto ok=::fsync(fd)==0;::close(fd);require(ok,"Could not flush editor report.");
#endif
}
Json read_script(const EditorOptions& o) {
    if(o.script.empty())return Json::array();
    require(fs::is_regular_file(path_of(o.script)) && fs::file_size(path_of(o.script))<=1024*1024,"Editor script must be a regular file of at most 1 MiB.");
    std::ifstream stream(path_of(o.script),std::ios::binary);std::vector<std::set<std::string>> keys;
    Json root=Json::parse(stream,[&](int depth,Json::parse_event_t event,Json& value) {
        require(depth<=64,"Editor script JSON nesting exceeds 64 levels.");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key)require(keys.back().insert(value.get<std::string>()).second,"Duplicate editor script JSON field.");
        if(event==Json::parse_event_t::object_end)keys.pop_back();return true;
    });
    require(root.is_object() && root.size()==1 && root.contains("actions") && root["actions"].is_array() && root["actions"].size()<=256,"Script needs actions array, at most 256 entries.");
    std::uint32_t previous=0;for(const auto& a:root["actions"]) {
        require(a.is_object() && a.contains("frame") && a["frame"].is_number_integer() && a["frame"]>=0 && a["frame"]<=35999 && a.contains("op") && a["op"].is_string(),"Script action needs frame 0..35999 and op.");
        require(!a.contains("expected_error") || a.at("expected_error").is_boolean(),"expected_error must be boolean.");
        const auto frame=a["frame"].get<std::uint32_t>();require(frame>=previous,"Script frames must be nondecreasing.");previous=frame;
    }
    return root["actions"];
}
Json transform() { return {{"position",{0,0,0}},{"rotation",{0,0,0,1}},{"scale",{1,1,1}}}; }
class EditorModel {
public:
    WorldSession session;
    std::uint64_t revision=0,tick=0,rpc_id=0;
    std::string selected,runtime_id;
    Json entities=Json::array(),value=Json::object(),history,assets=Json::array();
    bool playing=false,paused=false;
    std::vector<std::string> log;
    explicit EditorModel(const std::string& world):session(world) { refresh();note("Ready. Edits commit immediately; Undo is session-local.");note("Imports and commits currently run synchronously."); }
    void note(const std::string& text) { if(log.size()==128)log.erase(log.begin());log.push_back(text); }
    Json call(const char* method,const Json& params=Json::object()) {
        auto response=Json::parse(session.request(Json{{"jsonrpc","2.0"},{"id",++rpc_id},{"method",method},{"params",params}}.dump()));
        if(response.contains("error"))throw std::runtime_error(response["error"].value("message",std::string("World service failed.")));
        return response.at("result");
    }
    void refresh(bool keep_missing=false) {
        revision=call("world.inspect").at("revision");entities=Json::array();Json params={{"limit",256},{"revision",revision}};
        for(;;) { auto page=call("entity.query",params);for(auto& e:page.at("entities"))entities.push_back(e);if(page.at("next_after").is_null())break;params["after"]=page.at("next_after"); }
        history=call("world.history");value=Json::object();
        if(!selected.empty()) { auto found=std::find_if(entities.begin(),entities.end(),[&](const Json& e){return e.at("id")==selected;});if(found==entities.end()) { if(!keep_missing)selected.clear(); }else value=call("entity.get",{{"id",selected}}).at("value"); }
    }
    Json transact(Json ops) { auto r=call("world.transact",{{"request_id",uid()},{"base_revision",revision},{"ops",std::move(ops)}});refresh();return r; }
    Json action(const Json& a) {
        const auto op=a.at("op").get<std::string>();const auto id=a.value("id",selected);
        if(op=="select") { require(std::any_of(entities.begin(),entities.end(),[&](const Json& e){return e.at("id")==id;}),"Selected entity does not exist.");selected=id;refresh();return {{"selected",selected}}; }
        if(op=="play") { require(!playing,"Already playing.");const auto session_id=uid();auto r=call("runtime.start",{{"session_id",session_id},{"revision",revision}});runtime_id=session_id;playing=true;paused=false;tick=0;return r; }
        if(op=="pause") { require(playing,"Play must be running.");paused=a.value("paused",!paused);return {{"paused",paused},{"tick",tick}}; }
        if(op=="step") { require(playing,"Play must be running.");auto r=call("runtime.step",{{"session_id",runtime_id},{"request_id",uid()},{"expected_tick",tick},{"ticks",a.value("ticks",1)}});tick=r.at("tick");return r; }
        if(op=="stop") { require(playing,"Play is not running.");auto r=call("runtime.stop",{{"session_id",runtime_id}});playing=false;paused=false;tick=0;runtime_id.clear();return r; }
        require(!playing,"Stop Play before editing authored state.");
        if(op=="create") {
            const auto kind=a.value("kind",std::string("cube"));require(kind=="cube" || kind=="empty" || kind=="light" || kind=="camera","Unknown create kind.");const auto created=a.value("id",uid());auto t=transform();for(const auto* k:{"position","rotation","scale"})if(a.contains(k))t[k]=a[k];
            Json ops=Json::array({{{"op","entity.create"},{"id",created},{"name",a.value("name",kind=="cube" ? "Cube" : kind=="light" ? "Light" : kind=="camera" ? "Camera" : "Entity")}},{{"op","component.set"},{"id",created},{"type","Transform"},{"value",t}}});
            if(kind=="cube")ops.push_back({{"op","component.set"},{"id",created},{"type","MeshRenderer"},{"value",{{"primitive","box"},{"albedo",{.55,.65,.8}},{"visible",true}}}});
            if(kind=="camera")ops.push_back({{"op","component.set"},{"id",created},{"type","Camera"},{"value",{{"vertical_fov",60},{"near",.1},{"far",1000}}}});
            if(kind=="light")ops.push_back({{"op","component.set"},{"id",created},{"type","Light"},{"value",{{"kind","point"},{"color",{1,1,1}},{"intensity",100},{"range",20},{"enabled",true}}}});
            auto r=transact(ops);selected=created;refresh();r["created"]=created;return r;
        }
        if(op=="rename")return transact(Json::array({{{"op","entity.rename"},{"id",id},{"name",a.at("name")}}}));
        if(op=="delete")return transact(Json::array({{{"op","entity.delete"},{"id",id},{"recursive",true}}}));
        if(op=="component")return transact(Json::array({{{"op","component.set"},{"id",id},{"type",a.at("type")},{"value",a.at("value")}}}));
        if(op=="undo" || op=="redo") { auto r=call(op=="undo" ? "world.undo" : "world.redo",{{"request_id",uid()},{"base_revision",revision}});refresh();return r; }
        if(op=="import_asset") {
            auto r=call("asset.import",{{"source",a.at("path")}});r["display_name"]=path_text(path_of(a.at("path").get<std::string>()).filename());
            const auto existing=std::find_if(assets.begin(),assets.end(),[&](const Json& value){return value.at("asset")==r.at("asset");});
            if(existing==assets.end())assets.push_back(r);else *existing=r;return r;
        }
        if(op=="instantiate_asset") { const auto created=a.value("id",uid());auto r=transact(Json::array({{{"op","asset.instantiate"},{"id",created},{"asset",a.at("asset")},{"name",a.value("name",std::string("Model"))}}}));selected=created;refresh();r["created"]=created;return r; }
        throw std::runtime_error("Unknown editor action: "+op);
    }
};
struct InspectorDraft {
    std::string entity,baseline_name;
    std::array<char,512> name{};
    Json components=Json::object(),baseline=Json::object();
    std::uint64_t base_revision=0;
    bool reload_pending=false;
    bool dirty() const { return !entity.empty() && (components!=baseline || std::string(name.data())!=baseline_name); }
    bool conflict(std::uint64_t revision) const { return dirty() && base_revision!=revision; }
    void load(const EditorModel& model) {
        entity=model.selected;base_revision=model.revision;components=Json::object();baseline=components;baseline_name.clear();name.fill(0);reload_pending=false;
        if(entity.empty() || model.value.empty()) { entity.clear();return; }
        components=model.value.at("components");baseline=components;baseline_name=model.value.at("name").get<std::string>();std::copy_n(baseline_name.begin(),std::min(baseline_name.size(),name.size()-1),name.begin());
    }
    void sync(const EditorModel& model) { if(reload_pending || !dirty()) { if(reload_pending || entity!=model.selected || base_revision!=model.revision)load(model); } }
    Json inspect(const EditorModel& model) const { return {{"entity",entity.empty() ? Json(nullptr) : Json(entity)},{"base_revision",base_revision},{"dirty",dirty()},{"conflict",conflict(model.revision)},{"name",std::string(name.data())},{"components",components}}; }
    static bool same_shape(const Json& a,const Json& b) {
        if(a.is_number() && b.is_number())return true;
        if(a.type()!=b.type())return false;
        if(a.is_object()) { if(a.size()!=b.size())return false;for(const auto& [key,value]:a.items())if(!b.contains(key) || !same_shape(value,b.at(key)))return false; }
        if(a.is_array()) { if(a.size()!=b.size())return false;for(std::size_t i=0;i<a.size();++i)if(!same_shape(a[i],b[i]))return false; }return true;
    }
    void edit(const Json& action,const EditorModel& model) {
        sync(model);require(!entity.empty(),"Select an entity before editing a draft.");const auto type=action.at("type").get<std::string>();require(components.contains(type) && same_shape(components.at(type),action.at("value")),"Draft must preserve the complete component field shape and types.");components[type]=action.at("value");
    }
    Json apply(EditorModel& model,const Json& action) {
        require(!model.playing,"Stop Play before applying an inspector draft.");require(!entity.empty() && entity==model.selected,"Draft entity is not selected.");
        require(base_revision==model.revision,"Inspector draft conflicts with newer world changes. Reload before applying.");
        Json ops=Json::array();const auto type=action.value("type",std::string{});
        if(type=="name") { if(std::string(name.data())!=baseline_name)ops.push_back({{"op","entity.rename"},{"id",entity},{"name",std::string(name.data())}}); }
        else if(!type.empty()) { require(components.contains(type),"Unknown draft component.");if(components.at(type)!=baseline.at(type))ops.push_back({{"op","component.set"},{"id",entity},{"type",type},{"value",components.at(type)}}); }
        else { for(const auto& [key,value]:components.items())if(value!=baseline.at(key))ops.push_back({{"op","component.set"},{"id",entity},{"type",key},{"value",value}});if(std::string(name.data())!=baseline_name)ops.push_back({{"op","entity.rename"},{"id",entity},{"name",std::string(name.data())}}); }
        if(ops.empty())return {{"revision",model.revision},{"changed",false}};
        auto result=model.transact(ops);
        // Update only committed baselines. Other unfinished fields survive a
        // local Apply, and no map mutation invalidates the inspector iterator.
        if(type.empty()) { baseline=components;baseline_name=std::string(name.data()); }
        else if(type=="name")baseline_name=std::string(name.data());else baseline[type]=components.at(type);
        base_revision=model.revision;return result;
    }
};
Json bounded_json(std::string_view raw) {
    std::vector<std::set<std::string>> keys;
    return Json::parse(raw,[&](int depth,Json::parse_event_t event,Json& value) {
        require(depth<=64,"JSON nesting exceeds 64 levels.");if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key)require(keys.back().insert(value.get<std::string>()).second,"Duplicate JSON field.");
        if(event==Json::parse_event_t::object_end)keys.pop_back();return true;
    });
}
void rpc_fields(const Json& value,std::initializer_list<const char*> allowed,std::initializer_list<const char*> required={}) {
    require(value.is_object(),"Expected an object.");for(const auto& [key,unused]:value.items()) { (void)unused;require(std::find(allowed.begin(),allowed.end(),key)!=allowed.end(),"Unknown field: "+key); }for(const auto* key:required)require(value.contains(key),std::string("Missing field: ")+key);
}
std::string rpc_error(const Json& id,int code,const std::string& message) { return Json{{"jsonrpc","2.0"},{"id",id},{"error",{{"code",code},{"message",message}}}}.dump(); }
std::string rpc_result(const Json& id,const Json& value) { return Json{{"jsonrpc","2.0"},{"id",id},{"result",value}}.dump(); }

struct CameraState {
    std::array<double,3> position{6,4,8},target{0,0,0};double yaw=37,pitch=-22,distance=10;
    EditorCamera camera() const {
        const double y=yaw*pi/360,p=pitch*pi/360;EditorCamera c;c.world=local_matrix(position,{std::cos(y)*std::sin(p),std::sin(y)*std::cos(p),-std::sin(y)*std::sin(p),std::cos(y)*std::cos(p)},{1,1,1});return c;
    }
    void orbit() { const auto m=camera().world;for(std::size_t i=0;i<3;++i)position[i]=target[i]+m[8+i]*distance; }
};
Bounds object_bounds(const SceneObject& object) {
    const auto local=object.skin ? posed_bounds(skin_bounds(*object.mesh),object.skin->palette) : mesh_bounds(object.mesh.get());return transform_bounds(local,object.world);
}
bool descendant(const EditorModel& model,const std::string& candidate,const std::string& ancestor) {
    auto id=candidate;for(std::size_t guard=0;guard<=model.entities.size();++guard) { if(id==ancestor)return true;auto it=std::find_if(model.entities.begin(),model.entities.end(),[&](const Json& e){return e.at("id")==id;});if(it==model.entities.end() || it->at("parent").is_null())return false;id=it->at("parent").get<std::string>(); }return false;
}
void frame_selected(EditorModel& model,CameraState& camera,const SceneSnapshot& scene) {
    require(!model.selected.empty(),"Select an entity to frame.");Bounds all{};bool first=true;
    for(const auto& object:scene.objects)if(descendant(model,object.entity_id,model.selected)) { auto b=object_bounds(object);if(first) { all=b;first=false; }else for(std::size_t i=0;i<3;++i) { all.minimum[i]=std::min(all.minimum[i],b.minimum[i]);all.maximum[i]=std::max(all.maximum[i],b.maximum[i]); } }
    if(first) { const auto r=model.call("entity.world_transform",{{"id",model.selected}});const auto m=r.at("matrix").get<Matrix4>();for(std::size_t i=0;i<3;++i) { all.minimum[i]=m[12+i]-.5;all.maximum[i]=m[12+i]+.5; } }
    double radius2=0;for(std::size_t i=0;i<3;++i) { camera.target[i]=(all.minimum[i]+all.maximum[i])*.5;radius2+=std::pow((all.maximum[i]-all.minimum[i])*.5,2); }camera.distance=std::max(1.0,std::sqrt(radius2)*2.7);camera.orbit();
}
std::string pick(const SceneSnapshot& scene,const EditorRect& rect,ImVec2 mouse) {
    if(rect.width<=0 || rect.height<=0)return {};const double x=((mouse.x-rect.x)/rect.width*2-1)*(rect.width/rect.height)*std::tan(pi/6),y=(1-(mouse.y-rect.y)/rect.height*2)*std::tan(pi/6);
    std::array<double,3> ray{},origin{};for(std::size_t i=0;i<3;++i) { ray[i]=scene.camera_world[i]*x+scene.camera_world[4+i]*y-scene.camera_world[8+i];origin[i]=scene.camera_world[12+i]; }
    double nearest=1e30;std::string id;
    for(const auto& object:scene.objects) { auto b=object_bounds(object);double lo=0,hi=1e30;for(std::size_t i=0;i<3;++i) { if(std::abs(ray[i])<1e-12) { if(origin[i]<b.minimum[i] || origin[i]>b.maximum[i])hi=-1; }else { auto a=(b.minimum[i]-origin[i])/ray[i],c=(b.maximum[i]-origin[i])/ray[i];if(a>c)std::swap(a,c);lo=std::max(lo,a);hi=std::min(hi,c); } }if(lo<=hi && lo<nearest) { nearest=lo;id=object.entity_id; } }
    return id;
}
struct EditorLayout {
    static constexpr std::size_t max_bytes=64*1024;
    std::map<std::string,bool> open{{"Hierarchy",true},{"Inspector",true},{"Scene",true},{"Project",true},{"Console",true}};
    Json windows=Json::object();
    fs::path file;
    bool reset=true,loaded=false,persistent=false;
    struct Floating { ImVec2 position{300,180},size{480,360}; };
    std::map<std::string,Floating> floating;
    std::vector<std::pair<std::string,std::string>> docking;
    EditorRect scene_rect{};
    void configure(const EditorOptions& options) {
        require(!options.no_layout || options.layout.empty(),"--layout and --no-layout cannot be combined.");
        persistent=!options.no_layout && (options.script.empty() || !options.layout.empty());
        if(!persistent)return;
        if(options.layout.empty()) {
            char* pref=SDL_GetPrefPath("Poima","Editor");require(pref!=nullptr,std::string("Editor preferences: ")+SDL_GetError());
            file=path_of(pref)/"layout.ini";SDL_free(pref);
        }else file=path_of(options.layout);
        guard();
        file=fs::weakly_canonical(fs::absolute(file));
        const auto world=fs::weakly_canonical(fs::absolute(path_of(options.world)));
        if(!options.layout.empty())require(!inside(file,world.parent_path()),"Explicit editor layout must be outside the world/project directory.");
        for(const auto* suffix:{"",".lock",".pending",".previous",".previous.pending"})
            require(!world_detail::same_path_name(file,fs::path(world).concat(suffix)),"Editor layout overlaps reserved world storage.");
        require(!inside(file,fs::weakly_canonical(fs::path(world).concat(".assets"))),"Editor layout overlaps the immutable asset store.");
        for(const auto* reserved:{&options.report,&options.render.capture,&options.script})if(!reserved->empty())
            for(const auto& output:{file,fs::path(file).concat(".pending")})
                require(!world_detail::same_path_name(output,fs::weakly_canonical(fs::absolute(path_of(*reserved)))),"Editor layout or its staging file overlaps a reserved output or script.");
        require(fs::is_directory(file.parent_path()),"Editor layout parent directory does not exist.");
        guard();
        if(fs::exists(file)) {
            require(fs::is_regular_file(file) && fs::file_size(file)<=max_bytes,"Editor layout must be a regular file of at most 64 KiB.");
            std::ifstream input(file,std::ios::binary);std::string data((std::istreambuf_iterator<char>(input)),{});
            require(input.good() || input.eof(),"Could not read editor layout.");require(data.size()<=max_bytes && data.find('\0')==std::string::npos,"Invalid editor layout bytes.");
            ImGui::LoadIniSettingsFromMemory(data.data(),data.size());
            if(const auto marker=data.find("[Poima][Panels]\n");marker!=std::string::npos)for(auto& [name,visible]:open)
                visible=data.find("\n"+name+"=0\n",marker)==std::string::npos;
            loaded=true;reset=false;
        }
    }
    void guard() const {
        for(const auto& path:{file,fs::path(file).concat(".pending")}) {
            require(!fs::is_symlink(fs::symlink_status(path)),"Editor layout and staging paths cannot be symbolic links.");
            if(fs::exists(path))require(fs::is_regular_file(path) && fs::hard_link_count(path)==1,"Editor layout must be a regular file without hard-link aliases.");
        }
    }
    void save() {
        if(!persistent)return;
        std::size_t size=0;const char* data=ImGui::SaveIniSettingsToMemory(&size);std::string bytes(data,size);
        bytes+="\n[Poima][Panels]\n";for(const auto& [name,visible]:open)bytes+=name+(visible ? "=1\n" : "=0\n");
        require(bytes.size()<=max_bytes,"Editor layout exceeds its 64 KiB budget.");guard();
        const auto pending=fs::path(file).concat(".pending");world_detail::write_flushed(pending,bytes);guard();world_detail::replace_file(pending,file);
        ImGui::GetIO().WantSaveIniSettings=false;
    }
    void build(ImGuiID id,ImVec2 position,ImVec2 size) {
        if(!reset && ImGui::DockBuilderGetNode(id))return;
        reset=false;
        ImGui::DockBuilderRemoveNode(id);ImGui::DockBuilderAddNode(id,static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_DockSpace)|ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::DockBuilderSetNodePos(id,position);ImGui::DockBuilderSetNodeSize(id,size);
        auto center=id;ImGuiID right=0,left=0,bottom=0;
        ImGui::DockBuilderSplitNode(center,ImGuiDir_Right,std::clamp(330.f/size.x,.18f,.32f),&right,&center);
        ImGui::DockBuilderSplitNode(center,ImGuiDir_Left,std::clamp(250.f/(size.x-330.f),.18f,.35f),&left,&center);
        ImGui::DockBuilderSplitNode(center,ImGuiDir_Down,std::clamp(220.f/size.y,.2f,.4f),&bottom,&center);
        ImGui::DockBuilderDockWindow("Hierarchy",left);ImGui::DockBuilderDockWindow("Inspector",right);
        ImGui::DockBuilderDockWindow("Scene",center);ImGui::DockBuilderDockWindow("Project",bottom);ImGui::DockBuilderDockWindow("Console",bottom);
        ImGui::DockBuilderFinish(id);
    }
    void command(const Json& action) {
        const auto op=action.at("op").get<std::string>();
        if(op=="layout_reset") { reset=true;for(auto& [name,visible]:open)visible=true;floating.clear();docking.clear();return; }
        const auto panel=action.at("panel").get<std::string>();require(open.contains(panel),"Unknown editor panel.");open[panel]=true;
        if(op=="layout_show") { require(action.at("visible").is_boolean(),"Panel visibility must be boolean.");open[panel]=action.at("visible").get<bool>(); }
        else if(op=="layout_float") {
            Floating value;
            if(action.contains("position")) { const auto a=action.at("position").get<std::array<float,2>>();value.position={a[0],a[1]}; }
            if(action.contains("size")) { const auto a=action.at("size").get<std::array<float,2>>();value.size={a[0],a[1]}; }
            for(float v:{value.position.x,value.position.y,value.size.x,value.size.y})require(std::isfinite(v) && std::abs(v)<=16384,"Invalid floating panel bounds.");
            require(value.size.x>=160 && value.size.y>=100,"Floating panel must be at least 160 by 100.");floating[panel]=value;
        }else {
            require(op=="layout_dock","Unknown layout action.");const auto target=action.at("target").get<std::string>();
            require(open.contains(target) && target!=panel,"Dock target must be another editor panel.");open[target]=true;docking.emplace_back(panel,target);
        }
    }
    void prepare_docking() {
        for(const auto& [panel,target]:docking) { const auto* window=ImGui::FindWindowByName(target.c_str());require(window && window->DockId,"Target panel must already be docked.");ImGui::DockBuilderDockWindow(panel.c_str(),window->DockId); }
        docking.clear();
    }
    bool begin(const char* name,ImGuiWindowFlags flags=0) {
        if(const auto it=floating.find(name);it!=floating.end()) {
            ImGui::SetNextWindowDockID(0,ImGuiCond_Always);ImGui::SetNextWindowPos(it->second.position);ImGui::SetNextWindowSize(it->second.size);floating.erase(it);
        }
        const bool visible=ImGui::Begin(name,&open.at(name),flags);
        const auto p=ImGui::GetWindowPos(),s=ImGui::GetWindowSize();windows[name]={{"open",open.at(name)},{"visible",visible},{"dock_id",ImGui::GetWindowDockID()},{"rect",{p.x,p.y,s.x,s.y}}};
        return visible;
    }
    Json inspect() const {
        auto snapshot=windows;for(const auto& [name,value]:open)if(!value)snapshot[name]={{"open",false},{"visible",false},{"dock_id",0}};
        return {{"docking",true},{"native_multi_window",false},{"persistent",persistent},{"loaded",loaded},{"path",persistent ? Json(path_text(file)) : Json(nullptr)},
            {"font","Source Sans 3"},{"windows",snapshot},{"scene_viewport",{scene_rect.x,scene_rect.y,scene_rect.width,scene_rect.height}}};
    }
};

std::string lower(std::string value) { for(auto& c:value)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return value; }
void field(const char* label) { ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(label);ImGui::TableSetColumnIndex(1);ImGui::SetNextItemWidth(-1); }
bool vector_field(const char* label,std::array<float,3>& value,float speed,float minimum=0,float maximum=0) {
    field(label);bool changed=false;const float width=std::max(24.f,(ImGui::GetContentRegionAvail().x-34)/3);
    for(std::size_t i=0;i<3;++i) { if(i)ImGui::SameLine(0,3);ImGui::PushID(static_cast<int>(i));ImGui::TextColored(i==0 ? ImVec4(.9f,.5f,.5f,1) : i==1 ? ImVec4(.5f,.8f,.55f,1) : ImVec4(.5f,.65f,.9f,1),"%c","XYZ"[i]);ImGui::SameLine(0,2);ImGui::SetNextItemWidth(width-6);changed|=ImGui::DragFloat("##value",&value[i],speed,minimum,maximum,"%.2f");ImGui::PopID(); }
    return changed;
}
std::array<float,3> euler_degrees(const std::array<double,4>& q) {
    const auto [x,y,z,w]=q;return {static_cast<float>(std::atan2(2*(w*x+y*z),1-2*(x*x+y*y))*180/pi),
        static_cast<float>(std::asin(std::clamp(2*(w*y-z*x),-1.0,1.0))*180/pi),static_cast<float>(std::atan2(2*(w*z+x*y),1-2*(y*y+z*z))*180/pi)};
}
std::array<double,4> euler_quaternion(const std::array<float,3>& angles) {
    const double x=angles[0]*pi/360,y=angles[1]*pi/360,z=angles[2]*pi/360;
    const double cx=std::cos(x),sx=std::sin(x),cy=std::cos(y),sy=std::sin(y),cz=std::cos(z),sz=std::sin(z);
    return {sx*cy*cz-cx*sy*sz,cx*sy*cz+sx*cy*sz,cx*cy*sz-sx*sy*cz,cx*cy*cz+sx*sy*sz};
}
// Explicit Apply keeps drafts across frames and gives one undo entry per edit.
void inspect_component(const std::string& type,Json& draft) {
    if(!ImGui::BeginTable("Fields",2,ImGuiTableFlags_SizingStretchProp))return;
    ImGui::TableSetupColumn("Label",ImGuiTableColumnFlags_WidthFixed,76);ImGui::TableSetupColumn("Value",ImGuiTableColumnFlags_WidthStretch);
    if(type=="Transform") {
        auto position=draft["position"].get<std::array<float,3>>(),scale=draft["scale"].get<std::array<float,3>>();auto angles=euler_degrees(draft["rotation"].get<std::array<double,4>>());
        if(vector_field("Position",position,.05f))draft["position"]=position;
        if(vector_field("Rotation",angles,.3f))draft["rotation"]=euler_quaternion(angles);
        if(vector_field("Scale",scale,.02f,.001f,100000.f))draft["scale"]=scale;
    }else if(type=="Camera") {
        for(const auto& [key,label]:std::array<std::pair<const char*,const char*>,3>{{{"vertical_fov","Field of view"},{"near","Near clip"},{"far","Far clip"}}}) { field(label);float value=draft.at(key);if(ImGui::DragFloat((std::string("##")+key).c_str(),&value,.1f))draft[key]=value; }
    }else if(type=="MeshRenderer" || type=="PbrMaterial" || type=="Light") {
        const auto color_key=type=="MeshRenderer" ? "albedo" : type=="Light" ? "color" : "base_color";field("Color");auto color=draft.at(color_key).get<std::array<float,3>>();if(ImGui::ColorEdit3("##Color",color.data(),ImGuiColorEditFlags_NoInputs))draft[color_key]=color;
        if(type=="PbrMaterial") {
            field("Emission");auto emission=draft["emissive"].get<std::array<float,3>>();if(ImGui::ColorEdit3("##Emission",emission.data(),ImGuiColorEditFlags_NoInputs))draft["emissive"]=emission;
            for(const auto* key:{"metallic","roughness"}) { field(key);float value=draft[key];if(ImGui::SliderFloat((std::string("##")+key).c_str(),&value,0,1))draft[key]=value; }
            field("Two sided");bool value=draft["double_sided"];if(ImGui::Checkbox("##Two sided",&value))draft["double_sided"]=value;
        }
        if(type=="Light") {
            field("Type");ImGui::TextUnformatted(draft["kind"].get<std::string>().c_str());
            for(const auto* key:{"intensity","range","inner_angle","outer_angle"})if(draft.contains(key)) { field(key);float value=draft[key];if(ImGui::DragFloat((std::string("##")+key).c_str(),&value,.1f,0,1e9f))draft[key]=value; }
            field("Shadows");bool shadow=draft.contains("shadow") && draft["shadow"].value("enabled",false);if(ImGui::Checkbox("##Cast shadows",&shadow))draft["shadow"]["enabled"]=shadow;
        }
        if(type!="PbrMaterial") { field(type=="Light" ? "Enabled" : "Visible");const auto key=type=="Light" ? "enabled" : "visible";bool value=draft[key];if(ImGui::Checkbox("##Enabled",&value))draft[key]=value; }
    }else { field("Data");ImGui::TextWrapped("%s",draft.dump(1).c_str());ImGui::TextDisabled("Read-only in this inspector"); }
    ImGui::EndTable();
}
bool playback_button(const char* id,int icon,bool selected=false) {
    if(selected)ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.22f,.37f,.49f,1));const bool clicked=ImGui::Button(id,{32,24});if(selected)ImGui::PopStyleColor();
    const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();const ImVec2 c{(a.x+b.x)*.5f,(a.y+b.y)*.5f};auto* draw=ImGui::GetWindowDrawList();const auto color=ImGui::GetColorU32(ImGuiCol_Text);
    if(icon==0)draw->AddTriangleFilled({c.x-4,c.y-6},{c.x-4,c.y+6},{c.x+6,c.y},color);
    else if(icon==1) { draw->AddRectFilled({c.x-5,c.y-6},{c.x-1,c.y+6},color);draw->AddRectFilled({c.x+2,c.y-6},{c.x+6,c.y+6},color); }
    else if(icon==2)draw->AddRectFilled({c.x-5,c.y-5},{c.x+5,c.y+5},color);
    else { draw->AddTriangleFilled({c.x-6,c.y-6},{c.x-6,c.y+6},{c.x+3,c.y},color);draw->AddRectFilled({c.x+4,c.y-6},{c.x+6,c.y+6},color); }
    return clicked;
}
} // namespace

Reply run_editor(const EditorOptions& options) {
    Json report={{"success",false},{"frames",0},{"actions",Json::array()},{"resource_samples",Json::array()},{"source",options.script.empty() ? "native editor interaction" : "scripted editor model actions; not physical UI qualification"}};
    std::unique_ptr<EditorViewport> viewport;bool context=false,backend=false,outputs_validated=false;
    auto cleanup=[&] { if(backend) { ImGui_ImplSDL3_Shutdown();backend=false; }viewport.reset();if(context) { ImGui::DestroyContext();context=false; } };
    try {
        for(const auto* p:{&options.world,&options.render.capture,&options.report,&options.script,&options.layout})require(p->find('\0')==std::string::npos,"Editor paths cannot contain NUL bytes.");
        new_output(options.render.capture,options);new_output(options.report,options);
        if(!options.report.empty() && !options.render.capture.empty())require(!world_detail::same_path_name(fs::weakly_canonical(fs::absolute(path_of(options.report))),fs::weakly_canonical(fs::absolute(path_of(options.render.capture)))),"Capture and report must differ.");
        const auto actions=read_script(options);outputs_validated=true;std::uint32_t limit=options.max_frames;
        if(!options.script.empty()) { const auto needed=actions.empty() ? 1u : actions.back().at("frame").get<std::uint32_t>()+2;require(!limit || limit>=needed,"Editor frame limit does not cover scripted actions and final presentation.");if(!limit && options.endpoint.empty())limit=needed; }
        std::unique_ptr<LocalSessionServer> host;if(!options.endpoint.empty())host=std::make_unique<LocalSessionServer>(options.endpoint);EditorModel model(options.world);CameraState camera;
        SceneSnapshot scene;scene.world_id=model.call("world.inspect").at("world_id");scene.camera_id="editor";scene.camera_world=camera.camera().world;
        bool has_snapshot=false,scene_runtime=false;std::optional<std::uint64_t> scene_tick;std::string presentation_error;
        auto update_snapshot=[&] {
            try { auto candidate=model.playing ? model.session.runtime_snapshot(camera.camera()) : model.session.authored_snapshot(camera.camera());scene=std::move(candidate);has_snapshot=true;scene_runtime=model.playing;scene_tick=model.playing ? std::optional<std::uint64_t>(model.tick) : std::nullopt;presentation_error.clear(); }
            catch(const std::exception& e) { if(presentation_error!=e.what())model.note(std::string("Scene unavailable: ")+e.what());presentation_error=e.what(); }
        };
        auto scene_current=[&] { const auto state=model.session.runtime_status();return has_snapshot && presentation_error.empty() && scene_runtime==model.playing && scene.revision==(model.playing ? state.authored_revision : model.revision) && (!model.playing || scene_tick==std::optional<std::uint64_t>(model.tick)); };
        auto presentation=[&] { return Json{{"current",scene_current()},{"error",presentation_error.empty() ? Json(nullptr) : Json(presentation_error)},{"has_snapshot",has_snapshot},{"scene_revision",has_snapshot ? Json(scene.revision) : Json(nullptr)},{"source",has_snapshot ? (scene_runtime ? "runtime" : "authored") : "empty"},{"tick",scene_tick ? Json(*scene_tick) : Json(nullptr)}}; };
        update_snapshot();
        IMGUI_CHECKVERSION();ImGui::CreateContext();context=true;ImGui::GetIO().IniFilename=nullptr;configure_editor_style();
        viewport=std::make_unique<EditorViewport>(options.render,scene);require(ImGui_ImplSDL3_InitForVulkan(static_cast<SDL_Window*>(viewport->native_window())),"ImGui SDL initialization failed.");backend=true;
        EditorLayout layout;layout.configure(options);
        std::size_t action_index=0;std::uint32_t frame=0;bool quit=false;double accumulator=0;auto last=std::chrono::steady_clock::now();
        InspectorDraft draft;draft.load(model);std::array<char,4096> import_path{};std::array<char,256> hierarchy_search{},asset_search{};
        std::string selected_asset;bool focus_name=false,focus_import=false,show_help=false;
        std::function<Json(const Json&)> act=[&](const Json& a)->Json {
            const auto op=a.at("op").get<std::string>();draft.sync(model);Json result;
            if(op=="select" && a.at("id")!=model.selected)require(!draft.dirty(),"Inspector has an unfinished draft. Apply or Reload before changing selection.");
            if(op=="create" || op=="delete" || op=="undo" || op=="redo" || op=="instantiate_asset")require(!draft.dirty(),"Inspector has an unfinished draft. Apply or Reload before this action.");
            if(op=="layout_reset" || op=="layout_float" || op=="layout_dock" || op=="layout_show") { layout.command(a);result=layout.inspect(); }
            else if(op=="draft_component") { draft.edit(a,model);result=draft.inspect(model); }
            else if(op=="apply_draft")result=draft.apply(model,a);
            else if(op=="reload_draft") { model.refresh();draft.load(model);result=draft.inspect(model); }
            else if(op=="frame_selected") { auto s=model.playing ? model.session.runtime_snapshot(camera.camera()) : model.session.authored_snapshot(camera.camera());frame_selected(model,camera,s);result={{"position",camera.position}}; }
            else if(op=="camera") { if(a.contains("position"))camera.position=a["position"].get<std::array<double,3>>();camera.yaw=a.value("yaw",camera.yaw);camera.pitch=a.value("pitch",camera.pitch);require(std::isfinite(camera.yaw) && std::isfinite(camera.pitch),"Camera angles must be finite.");for(auto v:camera.position)require(std::isfinite(v) && std::abs(v)<=1e9,"Camera position must be finite and bounded.");camera.pitch=std::clamp(camera.pitch,-89.0,89.0);result={{"position",camera.position},{"yaw",camera.yaw},{"pitch",camera.pitch}}; }
            else { result=model.action(a);if(!draft.dirty() && (op=="select" || op=="create" || op=="delete" || op=="undo" || op=="redo" || op=="instantiate_asset"))draft.reload_pending=true; }
            if(report["actions"].size()<512)report["actions"].push_back({{"frame",frame},{"op",op},{"result",result}});return result;
        };
        auto ui_act=[&](Json a) { try { act(a); }catch(const std::exception& e) { model.note(e.what()); } };
        struct PendingCapture { LocalSessionRequest request;Json id;bool notification=false;std::uint64_t revision=0;std::string path;std::chrono::steady_clock::time_point deadline; };
        std::optional<PendingCapture> pending_capture;std::deque<LocalSessionRequest> incoming;
        std::optional<std::uint64_t> presented_revision,presented_tick;bool configured_capture_written=false;
        auto sync_remote=[&] {
            const auto revision=model.call("world.inspect").at("revision").get<std::uint64_t>();if(revision!=model.revision)model.refresh(draft.dirty());
            const auto state=model.session.runtime_status();
            if(state.active) { if(!model.playing || model.runtime_id!=state.session_id)model.paused=true;model.playing=true;model.runtime_id=state.session_id;model.tick=state.tick; }
            else { model.playing=false;model.paused=false;model.runtime_id.clear();model.tick=0; }
            draft.sync(model);
        };
        auto editor_inspect=[&] {
            const auto state=model.session.runtime_status();
            return Json{{"selected",model.selected.empty() ? Json(nullptr) : Json(model.selected)},{"revision",model.revision},{"draft",draft.inspect(model)},{"presentation",presentation()},
                {"runtime",{{"available",state.available},{"active",state.active},{"session_id",state.active ? Json(state.session_id) : Json(nullptr)},{"tick",state.tick},{"authored_revision",state.active ? Json(state.authored_revision) : Json(nullptr)},{"paused",model.paused}}},
                {"frame",frame},{"presented_revision",presented_revision ? Json(*presented_revision) : Json(nullptr)},{"presented_tick",presented_tick ? Json(*presented_tick) : Json(nullptr)},{"layout",layout.inspect()}};
        };
        auto finish_capture=[&](int code,const std::string& detail,const Json& result=Json::object()) {
            if(!pending_capture)return;const auto& pending=*pending_capture;
            host->reply(pending.request.token,pending.notification ? "" : code ? rpc_error(pending.id,code,detail) : rpc_result(pending.id,result));pending_capture.reset();
        };
        auto pump_host=[&] {
            if(!host)return;
            for(auto& request:host->poll()) { if(incoming.size()>=8)host->reply(request.token,rpc_error(nullptr,-32000,"Editor request queue is full."));else incoming.push_back(std::move(request)); }
            while(!pending_capture && !incoming.empty()) {
                auto request=std::move(incoming.front());incoming.pop_front();Json message,id=nullptr;bool notification=false;
                try { message=bounded_json(request.payload); }
                catch(const std::exception& e) { host->reply(request.token,rpc_error(nullptr,-32700,e.what()));continue; }
                try {
                    require(message.is_object() && message.value("jsonrpc",Json())=="2.0" && message.contains("method") && message.at("method").is_string(),"Invalid JSON-RPC 2.0 request.");
                    if(message.contains("id")) { id=message.at("id");require(id.is_null() || id.is_string() || id.is_number_integer(),"Invalid JSON-RPC request ID."); }
                    notification=!message.contains("id");
                }catch(const std::exception& e) { host->reply(request.token,rpc_error(nullptr,-32600,e.what()));continue; }
                const auto method=message.at("method").get<std::string>();
                if(!method.starts_with("editor.")) { host->reply(request.token,model.session.request(request.payload,WorldRequestScope::shared_editor));sync_remote();continue; }
                try {
                    rpc_fields(message,{"jsonrpc","id","method","params"},{"jsonrpc","method"});const auto params=message.value("params",Json::object());Json result;
                    if(method=="editor.describe") {
                        rpc_fields(params,{});Json methods=Json::object();
                        for(const auto* name:{"editor.describe","editor.inspect","editor.play","editor.stop"})methods[name]={{"params",{{"type","object"},{"additionalProperties",false}}}};
                        methods["editor.select"]={{"params",{{"type","object"},{"required",{"id"}},{"properties",{{"id",{{"type","string"},{"pattern","^[0-9a-f]{32}$"}}}}},{"additionalProperties",false}}}};
                        methods["editor.pause"]={{"params",{{"type","object"},{"required",{"paused"}},{"properties",{{"paused",{{"type","boolean"}}}}},{"additionalProperties",false}}}};
                        methods["editor.step"]={{"params",{{"type","object"},{"required",{"ticks"}},{"properties",{{"ticks",{{"type","integer"},{"minimum",1},{"maximum",600}}}}},{"additionalProperties",false}}}};
                        methods["editor.capture"]={{"params",{{"type","object"},{"required",{"revision","path"}},{"properties",{{"revision",{{"type","integer"},{"minimum",0}}},{"path",{{"type","string"},{"minLength",1}}}}},{"additionalProperties",false}}}};
                        result={{"methods",methods},{"endpoint",options.endpoint},{"capture_policy","Fresh current viewport; authored revision guard; runtime scene_revision and tick reported separately. New paths only; no automatic request retry."},{"draft_policy","Dirty Inspector fields retain their base revision; conflicts require explicit Reload."}};
                    }else if(method=="editor.inspect") { rpc_fields(params,{});result=editor_inspect(); }
                    else if(method=="editor.select") {
                        rpc_fields(params,{"id"},{"id"});require(params.at("id").is_string(),"Selection ID must be a string.");
                        if(draft.dirty() && params.at("id")!=model.selected) { host->reply(request.token,notification ? "" : rpc_error(id,-32009,"Inspector has an unfinished draft. Apply or Reload before changing selection."));continue; }
                        result=act({{"op","select"},{"id",params.at("id")}});draft.sync(model);
                    }else if(method=="editor.play" || method=="editor.stop") {
                        rpc_fields(params,{});result=act({{"op",method=="editor.play" ? "play" : "stop"}});if(method=="editor.play")model.paused=true;
                    }else if(method=="editor.pause") { rpc_fields(params,{"paused"},{"paused"});require(params.at("paused").is_boolean(),"paused must be boolean.");result=act({{"op","pause"},{"paused",params.at("paused")}}); }
                    else if(method=="editor.step") { rpc_fields(params,{"ticks"},{"ticks"});require(params.at("ticks").is_number_integer() && params.at("ticks")>=1 && params.at("ticks")<=600,"ticks must be 1..600.");result=act({{"op","step"},{"ticks",params.at("ticks")}}); }
                    else if(method=="editor.capture") {
                        rpc_fields(params,{"revision","path"},{"revision","path"});require(params.at("revision").is_number_integer() && params.at("revision")>=0 && params.at("revision")<=9007199254740991ULL,"revision must be a safe nonnegative integer.");
                        require(params.at("path").is_string(),"Capture path must be a string.");const auto path=params.at("path").get<std::string>();require(!path.empty() && path.find('\0')==std::string::npos,"Capture path must be nonempty and NUL-free.");new_output(path,options);
                        const auto normalized=fs::weakly_canonical(fs::absolute(path_of(path)));for(const auto* reserved:{&options.report,&options.render.capture})if(!reserved->empty())require(!world_detail::same_path_name(normalized,fs::weakly_canonical(fs::absolute(path_of(*reserved)))),"Remote capture overlaps a reserved editor output.");
                        if(params.at("revision")!=model.revision) { host->reply(request.token,notification ? "" : rpc_error(id,-32009,"Authored revision conflict."));continue; }
                        pending_capture=PendingCapture{std::move(request),id,notification,model.revision,path,std::chrono::steady_clock::now()+std::chrono::seconds(2)};continue;
                    }else { host->reply(request.token,notification ? "" : rpc_error(id,-32601,"Unknown editor method."));continue; }
                    host->reply(request.token,notification ? "" : rpc_result(id,result));sync_remote();
                }catch(const std::exception& e) { host->reply(request.token,notification ? "" : rpc_error(id,-32602,e.what())); }
            }
        };

        // Small bounded drain, never authoring dispatch. A broken/nonreading
        // client may still see disconnect; neither side automatically retries.
        auto flush_closing=[&] {
            if(!host)return;
            for(int pass=0;pass<8;++pass) {
                for(auto& request:host->poll())incoming.push_back(std::move(request));
                while(!incoming.empty()) { auto request=std::move(incoming.front());incoming.pop_front();Json id=nullptr;bool notification=false;try { const auto value=bounded_json(request.payload);if(value.is_object()) { id=value.value("id",Json());notification=!value.contains("id"); } }catch(...) {}host->reply(request.token,notification ? "" : rpc_error(id,-32003,"Editor is closing.")); }
                SDL_Delay(2);
            }
        };

        while(!quit) {
            SDL_Event event;while(SDL_PollEvent(&event)) { ImGui_ImplSDL3_ProcessEvent(&event);if(event.type==SDL_EVENT_QUIT || event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED)quit=true;if(event.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type==SDL_EVENT_WINDOW_RESIZED)viewport->resize(); }
            const auto frame_begin=std::chrono::steady_clock::now();const auto now=frame_begin;const double dt=std::clamp(std::chrono::duration<double>(now-last).count(),0.0,.1);last=now;
            pump_host();draft.sync(model);const auto actions_before=report["actions"].size();
            while(action_index<actions.size() && actions[action_index].at("frame")==frame) {
                const auto& action=actions[action_index++];const bool expected=action.value("expected_error",false);bool failed=false;
                try { act(action); }catch(const std::exception& e) { if(!expected)throw;failed=true;if(report["actions"].size()<512)report["actions"].push_back({{"frame",frame},{"op",action.at("op")},{"expected_error",true},{"error",e.what()}}); }
                require(!expected || failed,"Script action unexpectedly succeeded despite expected_error.");
            }
            if(model.playing && !model.paused && (options.script.empty() || host)) { accumulator+=dt;const auto ticks=static_cast<int>(accumulator*60);if(ticks) { try { model.action({{"op","step"},{"ticks",ticks}});accumulator-=ticks/60.0; }catch(const std::exception& e) { model.paused=true;accumulator=0;model.note(std::string("Play paused: ")+e.what()); } } }else accumulator=0;
            ImGui_ImplSDL3_NewFrame();ImGui::NewFrame();auto& io=ImGui::GetIO();const auto* main_viewport=ImGui::GetMainViewport();
            const auto origin=main_viewport->Pos;const float width=main_viewport->Size.x,height=main_viewport->Size.y;
            float menu_height=ImGui::GetFrameHeight();
            if(ImGui::BeginMainMenuBar()) {
                menu_height=ImGui::GetWindowHeight();
                if(ImGui::BeginMenu("File")) {
                    if(ImGui::MenuItem("Import asset...")) { layout.open["Project"]=true;focus_import=true;ImGui::SetWindowFocus("Project"); }
                    ImGui::Separator();ImGui::TextDisabled("Changes are saved on Apply");
                    if(ImGui::MenuItem("Close editor"))quit=true;ImGui::EndMenu();
                }
                if(ImGui::BeginMenu("Edit")) {
                    const bool editable=!model.playing && !draft.dirty();
                    if(ImGui::MenuItem("Undo","Ctrl+Z",false,editable && model.history.value("undo_count",0)>0))ui_act({{"op","undo"}});
                    if(ImGui::MenuItem("Redo","Ctrl+Y",false,editable && model.history.value("redo_count",0)>0))ui_act({{"op","redo"}});
                    ImGui::Separator();if(ImGui::MenuItem("Frame selected","F",false,!model.selected.empty()))ui_act({{"op","frame_selected"}});
                    if(ImGui::MenuItem("Delete selected","Delete",false,editable && !model.selected.empty()))ui_act({{"op","delete"}});ImGui::EndMenu();
                }
                if(ImGui::BeginMenu("GameObject")) {
                    ImGui::BeginDisabled(model.playing || draft.dirty());
                    if(ImGui::MenuItem("Create Empty"))ui_act({{"op","create"},{"kind","empty"}});
                    if(ImGui::BeginMenu("3D Object")) { if(ImGui::MenuItem("Cube"))ui_act({{"op","create"},{"kind","cube"}});ImGui::EndMenu(); }
                    if(ImGui::BeginMenu("Light")) { if(ImGui::MenuItem("Point"))ui_act({{"op","create"},{"kind","light"}});ImGui::EndMenu(); }
                    if(ImGui::MenuItem("Camera"))ui_act({{"op","create"},{"kind","camera"}});
                    ImGui::EndDisabled();ImGui::EndMenu();
                }
                if(ImGui::BeginMenu("Window")) {
                    for(auto& [name,visible]:layout.open)ImGui::MenuItem(name.c_str(),nullptr,&visible);
                    ImGui::Separator();if(ImGui::MenuItem("Reset layout"))ui_act({{"op","layout_reset"}});
                    if(ImGui::MenuItem("Save layout",nullptr,false,layout.persistent)) { try { layout.save();model.note("Layout saved to user preferences."); }catch(const std::exception& e) { model.note(e.what()); } }
                    ImGui::EndMenu();
                }
                if(ImGui::BeginMenu("Help")) { if(ImGui::MenuItem("Editor controls"))show_help=true;ImGui::EndMenu(); }
                ImGui::EndMainMenuBar();
            }
            if(show_help) { ImGui::OpenPopup("Editor controls");show_help=false; }
            if(ImGui::BeginPopupModal("Editor controls",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted("Scene: RMB + WASDQE to fly, Alt + LMB to orbit, MMB to pan.");
                ImGui::TextUnformatted("Wheel to zoom. F frames the selection. Ctrl+Z / Ctrl+Y undo / redo.");
                ImGui::Separator();ImGui::TextUnformatted("Drag panel tabs to move, dock or group panels in this window.");
                ImGui::TextUnformatted("Resize dock dividers. Window > Reset layout restores the default.");
                ImGui::TextUnformatted("Inspector edits remain drafts until Apply. Conflicts require Reload.");
                ImGui::TextDisabled("Native multi-window docking is not available in this renderer.");
                if(ImGui::Button("Close",{100,0}))ImGui::CloseCurrentPopup();ImGui::EndPopup();
            }
            const float toolbar_height=34,status_height=21;
            const ImGuiWindowFlags fixed=ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollbar;
            ImGui::SetNextWindowPos({origin.x,origin.y+menu_height});ImGui::SetNextWindowSize({width,toolbar_height});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize,{1,1});ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{8,5});
            ImGui::Begin("##Main toolbar",nullptr,fixed);
            ImGui::BeginDisabled(model.playing || draft.dirty());
            if(ImGui::Button("Create cube"))ui_act({{"op","create"},{"kind","cube"}});ImGui::SameLine();
            if(ImGui::Button("Create..."))ImGui::OpenPopup("Create root entity");
            if(ImGui::BeginPopup("Create root entity")) { for(const auto* kind:{"empty","light","camera"})if(ImGui::MenuItem(kind))ui_act({{"op","create"},{"kind",kind}});ImGui::EndPopup(); }
            ImGui::EndDisabled();
            ImGui::SameLine();ImGui::SetCursorPosX(std::max(210.f,(width-104)*.5f));
            ImGui::BeginDisabled(!Runtime::available());
            if(playback_button("##Play",model.playing ? 2 : 0,model.playing))ui_act({{"op",model.playing ? "stop" : "play"}});
            if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",model.playing ? "Stop Play" : "Play");ImGui::SameLine(0,3);
            ImGui::BeginDisabled(!model.playing);
            if(playback_button("##Pause",1,model.paused))ui_act({{"op","pause"}});if(ImGui::IsItemHovered())ImGui::SetTooltip("Pause / resume");ImGui::SameLine(0,3);
            if(playback_button("##Step",3)) { ui_act({{"op","pause"},{"paused",true}});ui_act({{"op","step"},{"ticks",1}}); }if(ImGui::IsItemHovered())ImGui::SetTooltip("Advance one fixed tick");
            ImGui::EndDisabled();ImGui::EndDisabled();
            ImGui::SameLine();ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),width-220));ImGui::TextDisabled("%s  |  Revision %llu",model.playing ? (model.paused ? "Paused" : "Playing") : "Edit",static_cast<unsigned long long>(model.revision));ImGui::End();ImGui::PopStyleVar(2);
            const ImVec2 dock_position{origin.x,origin.y+menu_height+toolbar_height},dock_size{width,std::max(1.f,height-menu_height-toolbar_height-status_height)};
            ImGui::SetNextWindowPos(dock_position);ImGui::SetNextWindowSize(dock_size);ImGui::SetNextWindowViewport(main_viewport->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0);ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0);
            ImGui::Begin("##Workspace",nullptr,fixed|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoNavFocus);
            const auto dock_id=ImGui::GetID("PoimaWorkspaceDockspace");layout.build(dock_id,dock_position,dock_size);layout.prepare_docking();
            ImGui::DockSpace(dock_id,{0,0},ImGuiDockNodeFlags_PassthruCentralNode);ImGui::End();ImGui::PopStyleVar(3);
            if(!io.WantTextInput) {
                if(!model.playing && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z,false))ui_act({{"op",io.KeyShift ? "redo" : "undo"}});
                if(!model.playing && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y,false))ui_act({{"op","redo"}});
                if(!model.selected.empty() && ImGui::IsKeyPressed(ImGuiKey_F,false))ui_act({{"op","frame_selected"}});
                if(!model.playing && !model.selected.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete,false))ui_act({{"op","delete"}});
            }
            if(layout.open["Hierarchy"]) {
                if(layout.begin("Hierarchy")) {
                    ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##Hierarchy search","Search entities",hierarchy_search.data(),hierarchy_search.size());
                    ImGui::Separator();const auto hierarchy=model.entities;const auto query=lower(hierarchy_search.data());
                    if(hierarchy.empty())ImGui::TextDisabled("No entities in this world");
                    auto context_menu=[&](const std::string& id) {
                        if(ImGui::BeginPopupContextItem()) {
                            if(model.selected!=id)ui_act({{"op","select"},{"id",id}});
                            ImGui::BeginDisabled(model.selected!=id || model.playing);
                            if(ImGui::MenuItem("Rename")) { layout.open["Inspector"]=true;focus_name=true;ImGui::SetWindowFocus("Inspector"); }
                            if(ImGui::MenuItem("Delete",nullptr,false,!draft.dirty()))ui_act({{"op","delete"}});
                            ImGui::EndDisabled();if(ImGui::MenuItem("Frame",nullptr,false,model.selected==id))ui_act({{"op","frame_selected"}});
                            ImGui::Separator();ImGui::BeginDisabled(model.playing || draft.dirty());
                            if(ImGui::MenuItem("Create root cube"))ui_act({{"op","create"},{"kind","cube"}});
                            if(ImGui::MenuItem("Create root entity"))ui_act({{"op","create"},{"kind","empty"}});
                            ImGui::EndDisabled();ImGui::EndPopup();
                        }
                    };
                    std::function<void(const Json&,int)> row=[&](const Json& e,int depth) {
                        const auto id=e.at("id").get<std::string>();const bool child=std::any_of(hierarchy.begin(),hierarchy.end(),[&](const Json& c){return c.at("parent")==id;});
                        auto flags=ImGuiTreeNodeFlags_OpenOnArrow|ImGuiTreeNodeFlags_SpanAvailWidth;
                        if(!child || depth>=64)flags|=ImGuiTreeNodeFlags_Leaf;if(model.selected==id)flags|=ImGuiTreeNodeFlags_Selected;
                        ImGui::PushID(id.c_str());const bool opened=ImGui::TreeNodeEx("entity",flags,"%s",e.at("name").get<std::string>().c_str());
                        if(ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())ui_act({{"op","select"},{"id",id}});context_menu(id);
                        if(opened) { if(depth<64)for(const auto& c:hierarchy)if(c.at("parent")==id)row(c,depth+1);ImGui::TreePop(); }ImGui::PopID();
                    };
                    for(const auto& e:hierarchy) {
                        if(query.empty()) { if(e.at("parent").is_null())row(e,0); }
                        else if(lower(e.at("name").get<std::string>()).find(query)!=std::string::npos) {
                            const auto id=e.at("id").get<std::string>();ImGui::PushID(id.c_str());if(ImGui::Selectable(e.at("name").get<std::string>().c_str(),model.selected==id))ui_act({{"op","select"},{"id",id}});context_menu(id);ImGui::PopID();
                        }
                    }
                    if(ImGui::BeginPopupContextWindow("Hierarchy empty",ImGuiPopupFlags_MouseButtonRight|ImGuiPopupFlags_NoOpenOverItems)) {
                        ImGui::BeginDisabled(model.playing || draft.dirty());if(ImGui::MenuItem("Create cube"))ui_act({{"op","create"},{"kind","cube"}});if(ImGui::MenuItem("Create empty"))ui_act({{"op","create"},{"kind","empty"}});ImGui::EndDisabled();ImGui::EndPopup();
                    }
                }ImGui::End();
            }
            if(layout.open["Inspector"]) {
                if(layout.begin("Inspector")) {
                    if(model.selected.empty())ImGui::TextDisabled("Select an entity to inspect.");else {
                        draft.sync(model);auto& name=draft.name;auto& drafts=draft.components;
                        ImGui::BeginDisabled(model.playing);ImGui::SetNextItemWidth(-1);if(focus_name) { ImGui::SetKeyboardFocusHere();focus_name=false; }
                        if(ImGui::InputText("##Entity name",name.data(),name.size(),ImGuiInputTextFlags_EnterReturnsTrue))ui_act({{"op","apply_draft"},{"type","name"}});
                        if(ImGui::Button("Rename"))ui_act({{"op","apply_draft"},{"type","name"}});ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Frame"))ui_act({{"op","frame_selected"}});
                        if(model.playing)ImGui::TextDisabled("Runtime preview - authored fields are locked");
                        if(draft.conflict(model.revision))ImGui::TextColored({1,.65f,.35f,1},"Draft conflicts with revision %llu",static_cast<unsigned long long>(model.revision));
                        if(draft.dirty()) { ImGui::TextDisabled("Unapplied changes (base %llu)",static_cast<unsigned long long>(draft.base_revision));if(ImGui::Button("Reload / discard draft"))ui_act({{"op","reload_draft"}}); }
                        ImGui::Separator();ImGui::BeginDisabled(model.playing);
                        auto component_ui=[&](const std::string& type,Json& value) {
                            ImGui::PushID(type.c_str());if(ImGui::CollapsingHeader(type.c_str(),ImGuiTreeNodeFlags_DefaultOpen)) {
                                inspect_component(type,value);
                                if(type=="Transform" || type=="Camera" || type=="Light" || type=="MeshRenderer" || type=="PbrMaterial") {
                                    ImGui::BeginDisabled(value==draft.baseline.at(type));if(ImGui::SmallButton("Apply"))ui_act({{"op","apply_draft"},{"type",type}});ImGui::EndDisabled();
                                }
                            }ImGui::PopID();
                        };
                        if(drafts.contains("Transform"))component_ui("Transform",drafts.at("Transform"));
                        for(auto& [type,value]:drafts.items())if(type!="Transform")component_ui(type,value);
                        if(!drafts.contains("PbrMaterial") && ImGui::Button("Add material",{-1,0}))ui_act({{"op","component"},{"type","PbrMaterial"},{"value",{{"base_color",{.8,.8,.8}},{"emissive",{0,0,0}},{"metallic",0},{"roughness",.7},{"double_sided",false}}}});
                        ImGui::EndDisabled();
                    }
                }ImGui::End();
            }
            if(layout.open["Project"]) {
                if(layout.begin("Project")) {
                    ImGui::BeginDisabled(model.playing);ImGui::SetNextItemWidth(std::max(100.f,ImGui::GetContentRegionAvail().x-67));if(focus_import) { ImGui::SetKeyboardFocusHere();focus_import=false; }
                    const bool import_enter=ImGui::InputTextWithHint("##Import path","glTF or GLB file path",import_path.data(),import_path.size(),ImGuiInputTextFlags_EnterReturnsTrue);ImGui::SameLine();
                    if(ImGui::Button("Import") || import_enter)ui_act({{"op","import_asset"},{"path",std::string(import_path.data())}});ImGui::EndDisabled();
                    ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##Asset search","Search imported models",asset_search.data(),asset_search.size());
                    if(model.assets.empty())ImGui::TextDisabled("Import a model to add it to this session's asset list.");
                    if(ImGui::BeginTable("Imported models",3,ImGuiTableFlags_RowBg|ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_ScrollY,{0,std::max(40.f,ImGui::GetContentRegionAvail().y)})) {
                        ImGui::TableSetupColumn("Name",ImGuiTableColumnFlags_WidthStretch);ImGui::TableSetupColumn("Type",ImGuiTableColumnFlags_WidthFixed,65);ImGui::TableSetupColumn("",ImGuiTableColumnFlags_WidthFixed,90);ImGui::TableHeadersRow();
                        for(const auto& asset:model.assets) {
                            const auto id=asset.at("asset").get<std::string>(),name=asset.value("display_name",std::string("Model"));if(lower(name).find(lower(asset_search.data()))==std::string::npos)continue;
                            ImGui::PushID(id.c_str());ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);if(ImGui::Selectable(name.c_str(),selected_asset==id))selected_asset=id;if(ImGui::IsItemHovered())ImGui::SetTooltip("Asset %s",id.c_str());
                            ImGui::TableSetColumnIndex(1);ImGui::TextDisabled("Model");ImGui::TableSetColumnIndex(2);ImGui::BeginDisabled(model.playing || draft.dirty());if(ImGui::SmallButton("Instantiate"))ui_act({{"op","instantiate_asset"},{"asset",id},{"name",path_text(path_of(name).stem())}});ImGui::EndDisabled();ImGui::PopID();
                        }ImGui::EndTable();
                    }
                }ImGui::End();
            }
            if(layout.open["Console"]) {
                if(layout.begin("Console")) {
                    if(ImGui::SmallButton("Clear"))model.log.clear();ImGui::SameLine();ImGui::TextDisabled("%zu messages",model.log.size());ImGui::Separator();
                    ImGui::BeginChild("Console messages",{0,0});for(const auto& line:model.log)ImGui::TextWrapped("%s",line.c_str());ImGui::EndChild();
                }ImGui::End();
            }
            update_snapshot();EditorRect rect{};
            if(layout.open["Scene"]) {
                if(layout.begin("Scene",ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse)) {
                    const auto top=ImGui::GetCursorScreenPos(),size=ImGui::GetContentRegionAvail();rect={top.x,top.y,std::max(0.f,size.x),std::max(0.f,size.y)};
                    if(size.x>0 && size.y>0) {
                        ImGui::Image(static_cast<ImTextureID>(2),size,{(rect.x-origin.x)/width,(rect.y-origin.y)/height},
                            {(rect.x+rect.width-origin.x)/width,(rect.y+rect.height-origin.y)/height});
                        ImGui::SetCursorScreenPos(top);ImGui::InvisibleButton("Scene input",size,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight|ImGuiButtonFlags_MouseButtonMiddle);
                    }
                    const bool hovered=size.x>0 && size.y>0 && ImGui::IsItemHovered();
            if(hovered && !io.WantTextInput) {
                const auto before=camera.camera().world;const auto delta=io.MouseDelta;
                if(ImGui::IsMouseDown(ImGuiMouseButton_Right) || (io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left))) { camera.yaw-=delta.x*.2;camera.pitch=std::clamp(camera.pitch-delta.y*.2,-89.0,89.0);if(io.KeyAlt)camera.orbit(); }
                if(ImGui::IsMouseDown(ImGuiMouseButton_Middle))for(std::size_t i=0;i<3;++i) { const double change=(-before[i]*delta.x+before[4+i]*delta.y)*camera.distance*.002;camera.position[i]+=change;camera.target[i]+=change; }
                if(io.MouseWheel!=0) { camera.distance=std::clamp(camera.distance*std::exp(-io.MouseWheel*.12),.1,100000.0);for(std::size_t i=0;i<3;++i)camera.position[i]-=before[8+i]*io.MouseWheel*std::max(.1,camera.distance*.1); }
                if(ImGui::IsMouseDown(ImGuiMouseButton_Right)) { const double speed=(io.KeyShift ? 20 : 5)*dt;const auto m=camera.camera().world;const double f=(ImGui::IsKeyDown(ImGuiKey_W) ? 1 : 0)-(ImGui::IsKeyDown(ImGuiKey_S) ? 1 : 0),r=(ImGui::IsKeyDown(ImGuiKey_D) ? 1 : 0)-(ImGui::IsKeyDown(ImGuiKey_A) ? 1 : 0),u=(ImGui::IsKeyDown(ImGuiKey_E) ? 1 : 0)-(ImGui::IsKeyDown(ImGuiKey_Q) ? 1 : 0);for(std::size_t i=0;i<3;++i)camera.position[i]+=speed*(-m[8+i]*f+m[i]*r+(i==1 ? u : 0)); }
                if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) { const auto id=pick(scene,rect,io.MousePos);if(!id.empty())ui_act({{"op","select"},{"id",id}}); }
            }
            ImGui::GetWindowDrawList()->AddText({rect.x+8,rect.y+8},IM_COL32(220,225,235,255),"RMB + WASDQE fly | Alt+LMB orbit | MMB pan | F frame");
            if(model.playing) { const auto text=std::string(model.paused ? "PAUSED - tick " : "PLAY - tick ")+std::to_string(model.tick);ImGui::GetWindowDrawList()->AddText({rect.x+8,rect.y+28},IM_COL32(110,210,255,255),text.c_str()); }
            if(!presentation_error.empty()) {
                const auto diagnostic=std::string("SCENE UNAVAILABLE - ")+(has_snapshot ? std::string("showing last valid revision ")+std::to_string(scene.revision) : std::string("empty fallback"))+"\n"+presentation_error;
                auto* draw=ImGui::GetWindowDrawList();draw->AddRectFilled({rect.x,rect.y+48},{rect.x+rect.width,rect.y+132},IM_COL32(45,22,20,240));draw->AddText(ImGui::GetFont(),ImGui::GetFontSize(),{rect.x+8,rect.y+54},IM_COL32(255,180,135,255),diagnostic.c_str(),nullptr,std::max(1.f,rect.width-16));
            }
                }ImGui::End();
            }
            layout.scene_rect=rect;
            ImGui::SetNextWindowPos({origin.x,origin.y+height-status_height});ImGui::SetNextWindowSize({width,status_height});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{8,2});ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize,{1,1});ImGui::Begin("##Status",nullptr,fixed);
            if(!presentation_error.empty())ImGui::TextColored({1,.65f,.35f,1},"Scene unavailable - see Console / Scene diagnostic");
            else ImGui::TextDisabled("%s%s",path_text(path_of(options.world).filename()).c_str(),draft.dirty() ? "  |  Inspector changes not applied" : "  |  All changes saved");
            ImGui::End();ImGui::PopStyleVar(2);
            ImGui::Render();scene.camera_world=camera.camera().world;
            if(layout.persistent && io.WantSaveIniSettings) { try { layout.save(); }catch(const std::exception& e) { model.note(std::string("Layout save: ")+e.what());io.WantSaveIniSettings=false; } }
            const bool final=quit || (limit && frame+1>=limit);if(final && !options.render.capture.empty())new_output(options.render.capture,options);
            if(pending_capture && pending_capture->revision!=model.revision)finish_capture(-32009,"Authored revision changed before capture presentation.");
            if(pending_capture && !scene_current())finish_capture(-32003,"Current scene is unavailable: "+presentation_error);
            if(pending_capture && std::chrono::steady_clock::now()>=pending_capture->deadline)finish_capture(-32003,"Capture timed out waiting for a presentable viewport.");
            const bool final_capture=final && !options.render.capture.empty() && scene_current();bool drawn=false;
            if(pending_capture) {
                try { new_output(pending_capture->path,options); }
                catch(const std::exception& e) { finish_capture(-32602,e.what()); }
            }
            if(pending_capture) {
                try { drawn=viewport->draw_capture(scene,rect,ImGui::GetDrawData(),pending_capture->path); }
                catch(const std::exception& e) { finish_capture(-32003,e.what());try { flush_closing(); }catch(...) {}throw; }
                if(drawn) { const auto r=viewport->report();const auto path=pending_capture->path;const auto revision=pending_capture->revision;finish_capture(0,"",{{"revision",revision},{"scene_revision",scene.revision},{"source",model.playing ? "runtime" : "authored"},{"tick",model.playing ? Json(model.tick) : Json(nullptr)},{"path",path},{"width",r.width},{"height",r.height},{"frame",frame}}); }
                if(final_capture) { const bool captured=viewport->draw(scene,rect,ImGui::GetDrawData(),true);configured_capture_written=captured;drawn=captured || drawn; }
            }else { drawn=viewport->draw(scene,rect,ImGui::GetDrawData(),final_capture);if(final_capture && drawn)configured_capture_written=true; }
            if(drawn) { presented_revision=has_snapshot ? std::optional<std::uint64_t>(scene.revision) : std::nullopt;presented_tick=scene_tick; }
            if(!drawn && options.script.empty() && !host)SDL_Delay(20);
            if(drawn && report["resource_samples"].size()<512 && (frame==0 || final || frame%60==0 || report["actions"].size()!=actions_before)) { const auto r=viewport->resources();std::size_t skinned=0;for(const auto& obj:scene.objects)if(obj.skin)++skinned;report["resource_samples"].push_back({{"frame",frame},{"meshes",r.meshes},{"materials",r.materials},{"images",r.images},{"skin_instances",r.skin_instances},{"texture_bytes",r.texture_bytes},{"skin_bytes",r.skin_bytes},{"scene_objects",scene.objects.size()},{"scene_lights",scene.lighting.lights.size()},{"scene_skinned",skinned},{"revision",model.revision},{"scene_revision",has_snapshot ? Json(scene.revision) : Json(nullptr)},{"scene_current",scene_current()}}); }
            ++frame;report["frames"]=frame;if(final)quit=true;
            if(final && pending_capture)finish_capture(-32003,"Editor closed before capture presentation.");
            if(host) { for(auto& request:host->poll()) { if(incoming.size()>=8)host->reply(request.token,rpc_error(nullptr,-32000,"Editor request queue is full."));else incoming.push_back(std::move(request)); } }
            if(!quit && (options.script.empty() || host)) { const auto flags=SDL_GetWindowFlags(static_cast<SDL_Window*>(viewport->native_window()));const double budget=(flags&SDL_WINDOW_INPUT_FOCUS) && !(flags&SDL_WINDOW_MINIMIZED) ? 1.0/60 : 1.0/30;const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-frame_begin).count();if(elapsed<budget)SDL_Delay(static_cast<Uint32>(std::ceil((budget-elapsed)*1000))); }
        }
        flush_closing();
        if(layout.persistent) { try { layout.save(); }catch(const std::exception& e) { model.note(std::string("Layout save: ")+e.what()); } }
        require(action_index==actions.size(),"Editor closed before all scripted actions ran.");const auto r=viewport->report();report["render"]={{"gpu",r.gpu_name},{"hardware",r.hardware},{"width",r.width},{"height",r.height},{"capture_written",options.render.capture.empty() ? r.capture_written : configured_capture_written},{"nvrhi_errors",r.validation_errors},{"frames_presented",r.frames_presented}};
        report["editor_state"]=editor_inspect();report["presentation_error"]=presentation_error.empty() ? Json(nullptr) : Json(presentation_error);report["presentation_current"]=scene_current();report["endpoint"]=options.endpoint;report["authored_revision"]=model.revision;report["selected"]=model.selected;report["playing"]=model.playing;report["paused"]=model.paused;report["runtime_tick"]=model.tick;
        require(r.validation_errors==0,"Renderer reported validation errors.");require(r.frames_presented>0,"No editor frame was presented.");require(options.render.capture.empty() || configured_capture_written,"Editor capture was not written.");report["success"]=true;cleanup();
    }catch(const std::exception& e) { report["error"]=e.what();cleanup(); }
    report["status"]=report.value("success",false) ? "ok" : "error";
    try { if(outputs_validated) { new_output(options.report,options);exclusive_report(options.report,report.dump(2)+"\n"); } }catch(const std::exception& e) { report["success"]=false;report["report_error"]=e.what();report["status"]="error"; }
    const bool success=report.value("success",false);Json diagnostics=Json::array();if(!success)diagnostics.push_back({{"code","editor.failed"},{"message",report.value("report_error",report.value("error",std::string("Editor failed.")))}});
    Json envelope={{"protocol_version",1},{"request_id",nullptr},{"command","editor"},{"status",success ? "ok" : "error"},{"result",report},{"diagnostics",diagnostics}};
    return {success ? 0 : 4,envelope.dump()};
}
} // namespace poima
