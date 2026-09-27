// SPDX-License-Identifier: Apache-2.0
#include "poima/editor.hpp"
#include "poima/editor_viewport.hpp"
#include "poima/world.hpp"
#include "poima/runtime.hpp"
#include "poima/animation.hpp"
#include "world_storage.hpp"
#include <nlohmann/json.hpp>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
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
    void refresh() {
        revision=call("world.inspect").at("revision");entities=Json::array();Json params={{"limit",256},{"revision",revision}};
        for(;;) { auto page=call("entity.query",params);for(auto& e:page.at("entities"))entities.push_back(e);if(page.at("next_after").is_null())break;params["after"]=page.at("next_after"); }
        history=call("world.history");value=Json::object();
        if(!selected.empty()) { auto found=std::find_if(entities.begin(),entities.end(),[&](const Json& e){return e.at("id")==selected;});if(found==entities.end())selected.clear();else value=call("entity.get",{{"id",selected}}).at("value"); }
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
        if(op=="import_asset") { auto r=call("asset.import",{{"source",a.at("path")}});assets.push_back(r);return r; }
        if(op=="instantiate_asset") { const auto created=a.value("id",uid());auto r=transact(Json::array({{{"op","asset.instantiate"},{"id",created},{"asset",a.at("asset")},{"name",a.value("name",std::string("Model"))}}}));selected=created;refresh();r["created"]=created;return r; }
        throw std::runtime_error("Unknown editor action: "+op);
    }
};
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
void panel(const char* title,float x,float y,float w,float h,ImGuiWindowFlags extra=0) {
    ImGui::SetNextWindowPos({x,y});ImGui::SetNextWindowSize({std::max(w,1.f),std::max(h,1.f)});
    ImGui::Begin(title,nullptr,ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse|extra);
}
// Explicit Apply keeps drafts across frames and gives one useful undo entry per edit.
void inspect_component(const std::string& type,Json& draft) {
    if(type=="Transform") {
        auto p=draft["position"].get<std::array<float,3>>(),s=draft["scale"].get<std::array<float,3>>();auto q=draft["rotation"].get<std::array<float,4>>();
        if(ImGui::DragFloat3("Position",p.data(),.05f))draft["position"]=p;
        if(ImGui::DragFloat4("Rotation XYZW",q.data(),.01f)) { double n=0;for(auto v:q)n+=v*v;if(n>1e-12) { for(auto& v:q)v=static_cast<float>(v/std::sqrt(n));draft["rotation"]=q; } }
        if(ImGui::DragFloat3("Scale",s.data(),.02f,.001f,100000.f))draft["scale"]=s;
    }else if(type=="Camera") {
        for(const auto* key:{"vertical_fov","near","far"}) { float x=draft.at(key);if(ImGui::DragFloat(key,&x,.1f))draft[key]=x; }
    }else if(type=="MeshRenderer" || type=="PbrMaterial" || type=="Light") {
        const auto key=type=="MeshRenderer" ? "albedo" : type=="Light" ? "color" : "base_color";auto c=draft.at(key).get<std::array<float,3>>();if(ImGui::ColorEdit3(key,c.data()))draft[key]=c;
        if(type=="PbrMaterial") { auto e=draft["emissive"].get<std::array<float,3>>();if(ImGui::ColorEdit3("Emissive",e.data()))draft["emissive"]=e;for(const auto* k:{"metallic","roughness"}) { float x=draft[k];if(ImGui::SliderFloat(k,&x,0,1))draft[k]=x; }bool b=draft["double_sided"];if(ImGui::Checkbox("Double sided",&b))draft["double_sided"]=b; }
        if(type=="Light") { ImGui::Text("Kind: %s",draft["kind"].get<std::string>().c_str());for(const auto* k:{"intensity","range","inner_angle","outer_angle"})if(draft.contains(k)) { float x=draft[k];if(ImGui::DragFloat(k,&x,.1f,0,1e9f))draft[k]=x; }bool shadow=draft.contains("shadow") && draft["shadow"].value("enabled",false);if(ImGui::Checkbox("Cast shadows",&shadow))draft["shadow"]["enabled"]=shadow; }
        if(type!="PbrMaterial") { const auto k=type=="Light" ? "enabled" : "visible";bool b=draft[k];if(ImGui::Checkbox(k,&b))draft[k]=b; }
    }else ImGui::TextWrapped("Present in this entity. Specialized controls are not implemented yet.");
}
} // namespace

Reply run_editor(const EditorOptions& options) {
    Json report={{"success",false},{"frames",0},{"actions",Json::array()},{"resource_samples",Json::array()},{"source",options.script.empty() ? "native editor interaction" : "scripted editor model actions; not physical UI qualification"}};
    std::unique_ptr<EditorViewport> viewport;bool context=false,backend=false,outputs_validated=false;
    auto cleanup=[&] { if(backend) { ImGui_ImplSDL3_Shutdown();backend=false; }viewport.reset();if(context) { ImGui::DestroyContext();context=false; } };
    try {
        for(const auto* p:{&options.world,&options.render.capture,&options.report,&options.script})require(p->find('\0')==std::string::npos,"Editor paths cannot contain NUL bytes.");
        new_output(options.render.capture,options);new_output(options.report,options);
        if(!options.report.empty() && !options.render.capture.empty())require(!world_detail::same_path_name(fs::weakly_canonical(fs::absolute(path_of(options.report))),fs::weakly_canonical(fs::absolute(path_of(options.render.capture)))),"Capture and report must differ.");
        const auto actions=read_script(options);outputs_validated=true;std::uint32_t limit=options.max_frames;
        if(!options.script.empty()) { const auto needed=actions.empty() ? 1u : actions.back().at("frame").get<std::uint32_t>()+2;require(!limit || limit>=needed,"Editor frame limit does not cover scripted actions and final presentation.");if(!limit)limit=needed; }
        EditorModel model(options.world);CameraState camera;auto scene=model.session.authored_snapshot(camera.camera());
        IMGUI_CHECKVERSION();ImGui::CreateContext();context=true;ImGui::GetIO().IniFilename=nullptr;ImGui::StyleColorsDark();
        auto& style=ImGui::GetStyle();style.WindowRounding=0;style.FrameRounding=3;style.WindowBorderSize=1;style.Colors[ImGuiCol_WindowBg]={.075f,.085f,.105f,1};
        viewport=std::make_unique<EditorViewport>(options.render,scene);require(ImGui_ImplSDL3_InitForVulkan(static_cast<SDL_Window*>(viewport->native_window())),"ImGui SDL initialization failed.");backend=true;
        std::size_t action_index=0;std::uint32_t frame=0;bool quit=false;double accumulator=0;auto last=std::chrono::steady_clock::now();
        Json drafts;std::string draft_selected;std::uint64_t draft_revision=UINT64_MAX;std::array<char,512> name{};std::array<char,4096> import_path{};
        std::function<Json(const Json&)> act=[&](const Json& a)->Json {
            const auto op=a.at("op").get<std::string>();Json result;
            if(op=="frame_selected") { auto s=model.playing ? model.session.runtime_snapshot(camera.camera()) : model.session.authored_snapshot(camera.camera());frame_selected(model,camera,s);result={{"position",camera.position}}; }
            else if(op=="camera") { if(a.contains("position"))camera.position=a["position"].get<std::array<double,3>>();camera.yaw=a.value("yaw",camera.yaw);camera.pitch=a.value("pitch",camera.pitch);require(std::isfinite(camera.yaw) && std::isfinite(camera.pitch),"Camera angles must be finite.");for(auto v:camera.position)require(std::isfinite(v) && std::abs(v)<=1e9,"Camera position must be finite and bounded.");camera.pitch=std::clamp(camera.pitch,-89.0,89.0);result={{"position",camera.position},{"yaw",camera.yaw},{"pitch",camera.pitch}}; }
            else result=model.action(a);
            if(report["actions"].size()<512)report["actions"].push_back({{"frame",frame},{"op",op},{"result",result}});return result;
        };
        auto ui_act=[&](Json a) { try { act(a); }catch(const std::exception& e) { model.note(e.what()); } };
        while(!quit) {
            SDL_Event event;while(SDL_PollEvent(&event)) { ImGui_ImplSDL3_ProcessEvent(&event);if(event.type==SDL_EVENT_QUIT || event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED)quit=true;if(event.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type==SDL_EVENT_WINDOW_RESIZED)viewport->resize(); }
            const auto now=std::chrono::steady_clock::now();const double dt=std::clamp(std::chrono::duration<double>(now-last).count(),0.0,.1);last=now;
            const auto actions_before=report["actions"].size();while(action_index<actions.size() && actions[action_index].at("frame")==frame)act(actions[action_index++]);
            if(model.playing && !model.paused && options.script.empty()) { accumulator+=dt;const auto ticks=static_cast<int>(accumulator*60);if(ticks) { try { model.action({{"op","step"},{"ticks",ticks}});accumulator-=ticks/60.0; }catch(const std::exception& e) { model.paused=true;accumulator=0;model.note(std::string("Play paused: ")+e.what()); } } }else accumulator=0;
            ImGui_ImplSDL3_NewFrame();ImGui::NewFrame();auto& io=ImGui::GetIO();const float width=io.DisplaySize.x,height=io.DisplaySize.y,toolbar=43,left=std::min(240.f,width*.22f),right=std::min(340.f,width*.28f),bottom=std::min(190.f,height*.25f);
            panel("Toolbar",0,0,width,toolbar,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoScrollbar);
            ImGui::TextUnformatted("POIMA");ImGui::SameLine();ImGui::BeginDisabled(model.playing);
            if(ImGui::Button("Create cube"))ui_act({{"op","create"},{"kind","cube"}});ImGui::SameLine();
            if(ImGui::Button("Create..."))ImGui::OpenPopup("Create entity");if(ImGui::BeginPopup("Create entity")) { for(const auto* kind:{"empty","light","camera"})if(ImGui::MenuItem(kind))ui_act({{"op","create"},{"kind",kind}});ImGui::EndPopup(); }
            ImGui::SameLine();ImGui::BeginDisabled(model.history.value("undo_count",0)==0);if(ImGui::Button("Undo"))ui_act({{"op","undo"}});ImGui::EndDisabled();ImGui::SameLine();ImGui::BeginDisabled(model.history.value("redo_count",0)==0);if(ImGui::Button("Redo"))ui_act({{"op","redo"}});ImGui::EndDisabled();ImGui::EndDisabled();
            ImGui::SameLine();if(!model.playing) { ImGui::BeginDisabled(!Runtime::available());if(ImGui::Button("Play"))ui_act({{"op","play"}});ImGui::EndDisabled(); }else { if(ImGui::Button(model.paused ? "Resume" : "Pause"))ui_act({{"op","pause"}});ImGui::SameLine();if(ImGui::Button("Stop"))ui_act({{"op","stop"}});ImGui::SameLine();if(ImGui::Button("Step"))ui_act({{"op","step"},{"ticks",1}}); }
            ImGui::SameLine();ImGui::Text("Rev %llu | %s",static_cast<unsigned long long>(model.revision),model.playing ? "PLAY" : "EDIT");ImGui::End();
            if(!io.WantTextInput) { if(!model.playing && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z,false))ui_act({{"op",io.KeyShift ? "redo" : "undo"}});if(!model.playing && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y,false))ui_act({{"op","redo"}});if(!model.selected.empty() && ImGui::IsKeyPressed(ImGuiKey_F,false))ui_act({{"op","frame_selected"}});if(!model.playing && !model.selected.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete,false))ui_act({{"op","delete"}}); }
            panel("Hierarchy",0,toolbar,left,height-toolbar-bottom);
            if(model.entities.empty())ImGui::TextWrapped("Create a cube or import a glTF model to begin.");
            // Draw a stable copy; selecting refreshes service state while widgets iterate.
            const auto hierarchy=model.entities;std::function<void(const Json&,int)> row=[&](const Json& e,int depth) {
                const auto id=e.at("id").get<std::string>();bool child=std::any_of(hierarchy.begin(),hierarchy.end(),[&](const Json& x){return x.at("parent")==id;});
                auto flags=ImGuiTreeNodeFlags_OpenOnArrow|ImGuiTreeNodeFlags_SpanAvailWidth;if(!child || depth>=64)flags|=ImGuiTreeNodeFlags_Leaf;if(model.selected==id)flags|=ImGuiTreeNodeFlags_Selected;
                ImGui::PushID(id.c_str());const bool open=ImGui::TreeNodeEx("entity",flags,"%s",e.at("name").get<std::string>().c_str());if(ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())ui_act({{"op","select"},{"id",id}});
                if(open) { if(depth<64)for(const auto& c:hierarchy)if(c.at("parent")==id)row(c,depth+1);ImGui::TreePop(); }ImGui::PopID();
            };for(const auto& e:hierarchy)if(e.at("parent").is_null())row(e,0);ImGui::End();
            panel("Inspector",width-right,toolbar,right,height-toolbar-bottom);
            if(model.selected.empty())ImGui::TextWrapped("Select an entity in the hierarchy or scene.");else {
                if(draft_selected!=model.selected || draft_revision!=model.revision) { drafts=model.value.at("components");draft_selected=model.selected;draft_revision=model.revision;const auto text=model.value.at("name").get<std::string>();std::fill(name.begin(),name.end(),0);std::copy_n(text.begin(),std::min(text.size(),name.size()-1),name.begin()); }
                ImGui::BeginDisabled(model.playing);ImGui::SetNextItemWidth(-1);const bool enter=ImGui::InputText("##Entity name",name.data(),name.size(),ImGuiInputTextFlags_EnterReturnsTrue);if(enter)ui_act({{"op","rename"},{"name",std::string(name.data())}});
                if(ImGui::Button("Rename"))ui_act({{"op","rename"},{"name",std::string(name.data())}});ImGui::SameLine();if(ImGui::Button("Delete"))ui_act({{"op","delete"}});ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Frame"))ui_act({{"op","frame_selected"}});
                ImGui::TextDisabled("%.12s...",model.selected.c_str());if(model.playing)ImGui::TextWrapped("Runtime is isolated. Stop to edit authored values.");
                ImGui::BeginDisabled(model.playing);for(auto& [type,draft]:drafts.items()) { ImGui::PushID(type.c_str());if(ImGui::CollapsingHeader(type.c_str(),ImGuiTreeNodeFlags_DefaultOpen)) { inspect_component(type,draft);if(type=="Transform" || type=="Camera" || type=="Light" || type=="MeshRenderer" || type=="PbrMaterial")if(ImGui::Button("Apply"))ui_act({{"op","component"},{"type",type},{"value",draft}}); }ImGui::PopID(); }
                if(!drafts.contains("PbrMaterial") && ImGui::Button("Add material"))ui_act({{"op","component"},{"type","PbrMaterial"},{"value",{{"base_color",{.8,.8,.8}},{"emissive",{0,0,0}},{"metallic",0},{"roughness",.7},{"double_sided",false}}}});ImGui::EndDisabled();
            }ImGui::End();
            panel("Assets",0,height-bottom,width*.55f,bottom);ImGui::BeginDisabled(model.playing);ImGui::SetNextItemWidth(std::max(80.f,width*.55f-90));ImGui::InputText("##Import path",import_path.data(),import_path.size());ImGui::SameLine();if(ImGui::Button("Import"))ui_act({{"op","import_asset"},{"path",std::string(import_path.data())}});ImGui::TextDisabled("glTF / GLB path; importing can briefly block the UI.");
            for(const auto& asset:model.assets) { const auto id=asset.at("asset").get<std::string>();ImGui::PushID(id.c_str());ImGui::Text("%.16s...",id.c_str());ImGui::SameLine();if(ImGui::Button("Instantiate"))ui_act({{"op","instantiate_asset"},{"asset",id}});ImGui::PopID(); }ImGui::EndDisabled();ImGui::End();
            panel("Activity",width*.55f,height-bottom,width*.45f,bottom);for(const auto& line:model.log)ImGui::TextWrapped("%s",line.c_str());ImGui::End();
            panel("Scene",left,toolbar,width-left-right,height-toolbar-bottom,ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
            const auto top=ImGui::GetCursorScreenPos(),size=ImGui::GetContentRegionAvail();EditorRect rect{top.x,top.y,std::max(0.f,size.x),std::max(0.f,size.y)};ImGui::InvisibleButton("Scene input",{std::max(1.f,size.x),std::max(1.f,size.y)},ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight|ImGuiButtonFlags_MouseButtonMiddle);const bool hovered=ImGui::IsItemHovered();
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
            ImGui::End();ImGui::Render();scene=model.playing ? model.session.runtime_snapshot(camera.camera()) : model.session.authored_snapshot(camera.camera());
            const bool final=quit || (limit && frame+1>=limit);if(final && !options.render.capture.empty())new_output(options.render.capture,options);
            const bool drawn=viewport->draw(scene,rect,ImGui::GetDrawData(),final && !options.render.capture.empty());if(!drawn)SDL_Delay(20);
            if(drawn && report["resource_samples"].size()<512 && (frame==0 || final || frame%60==0 || report["actions"].size()!=actions_before)) { const auto r=viewport->resources();std::size_t skinned=0;for(const auto& obj:scene.objects)if(obj.skin)++skinned;report["resource_samples"].push_back({{"frame",frame},{"meshes",r.meshes},{"materials",r.materials},{"images",r.images},{"skin_instances",r.skin_instances},{"texture_bytes",r.texture_bytes},{"skin_bytes",r.skin_bytes},{"scene_objects",scene.objects.size()},{"scene_lights",scene.lighting.lights.size()},{"scene_skinned",skinned},{"revision",model.revision}}); }
            ++frame;report["frames"]=frame;if(final)quit=true;
        }
        require(action_index==actions.size(),"Editor closed before all scripted actions ran.");const auto r=viewport->report();report["render"]={{"gpu",r.gpu_name},{"hardware",r.hardware},{"width",r.width},{"height",r.height},{"capture_written",r.capture_written},{"nvrhi_errors",r.validation_errors},{"frames_presented",r.frames_presented}};
        report["authored_revision"]=model.revision;report["selected"]=model.selected;report["playing"]=model.playing;report["paused"]=model.paused;report["runtime_tick"]=model.tick;
        require(r.validation_errors==0,"Renderer reported validation errors.");require(r.frames_presented>0,"No editor frame was presented.");require(options.render.capture.empty() || r.capture_written,"Editor capture was not written.");report["success"]=true;cleanup();
    }catch(const std::exception& e) { report["error"]=e.what();cleanup(); }
    report["status"]=report.value("success",false) ? "ok" : "error";
    try { if(outputs_validated) { new_output(options.report,options);exclusive_report(options.report,report.dump(2)+"\n"); } }catch(const std::exception& e) { report["success"]=false;report["report_error"]=e.what();report["status"]="error"; }
    const bool success=report.value("success",false);Json diagnostics=Json::array();if(!success)diagnostics.push_back({{"code","editor.failed"},{"message",report.value("report_error",report.value("error",std::string("Editor failed.")))}});
    Json envelope={{"protocol_version",1},{"request_id",nullptr},{"command","editor"},{"status",success ? "ok" : "error"},{"result",report},{"diagnostics",diagnostics}};
    return {success ? 0 : 4,envelope.dump()};
}
} // namespace poima
