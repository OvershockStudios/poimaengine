// SPDX-License-Identifier: Apache-2.0
#include "poima/game_launch.hpp"
#include "poima/project.hpp"
#include "poima/world.hpp"
#include "poima/runtime.hpp"
#include "poima/build_metadata.hpp"
#include "poima/local_session.hpp"
#include "poima/shared_session.hpp"
#include "world_storage.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>

namespace poima {
namespace {
using Json=nlohmann::json;
namespace fs=std::filesystem;
void require(bool condition,const std::string& message) { if(!condition)throw std::runtime_error(message); }
fs::path path_of(const std::string& text) { return fs::path(std::u8string(text.begin(),text.end())); }
std::string text_of(const fs::path& path) { const auto text=path.u8string();return {text.begin(),text.end()}; }
std::string id() { std::random_device random;std::string value(32,'0');for(auto& c:value)c="0123456789abcdef"[random()&15];return value; }
bool inside(const fs::path& path,const fs::path& root) {
    auto at=path.begin();for(const auto& part:root) { if(at==path.end() || !world_detail::same_path_name(*at,part))return false;++at; }return true;
}
fs::path normalized(const std::string& text) { require(!text.empty() && text.find('\0')==std::string::npos,"File paths must be nonempty and NUL-free.");return fs::weakly_canonical(fs::absolute(path_of(text))); }
void output_path(const std::string& text,const GameDefinition& game,const std::string& replay) {
    if(text.empty())return;
    const auto path=normalized(text);
    require(!inside(path,normalized(game.root)),"Game outputs must be outside the immutable bundle.");
    require(fs::is_directory(path.parent_path()),"Game output parent directory does not exist.");
    require(!fs::exists(path_of(text)) && !fs::is_symlink(fs::symlink_status(path_of(text))),"Game outputs require new files.");
    if(!replay.empty())require(!world_detail::same_path_name(path,normalized(replay)),"Game output overlaps its replay input.");
}
void write_report(const std::string& name,const Json& result) {
    if(name.empty())return;
    const auto bytes=result.dump(2)+"\n";
#ifdef _WIN32
    const auto handle=CreateFileW(path_of(name).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(handle!=INVALID_HANDLE_VALUE,"Cannot exclusively create game report.");DWORD written=0;
    const bool ok=WriteFile(handle,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) && written==bytes.size() && FlushFileBuffers(handle);CloseHandle(handle);
    require(ok,"Could not flush game report.");
#else
    const auto file=::open(path_of(name).c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);require(file>=0,"Cannot exclusively create game report.");
    std::size_t at=0;while(at<bytes.size()) { const auto n=::write(file,bytes.data()+at,bytes.size()-at);if(n<0 && errno==EINTR)continue;if(n<=0) { ::close(file);throw std::runtime_error("Could not write game report."); }at+=static_cast<std::size_t>(n); }
    const bool ok=::fsync(file)==0;::close(file);require(ok,"Could not flush game report.");
#endif
}
Json replay_file(const std::string& name) {
    const auto path=path_of(name);require(fs::is_regular_file(path) && fs::file_size(path)<=1024*1024,"Replay must be a regular JSON file of at most 1 MiB.");
    std::ifstream stream(path,std::ios::binary);std::vector<std::set<std::string>> keys;
    auto value=Json::parse(stream,[&](int depth,Json::parse_event_t event,Json& item) {
        require(depth<=64,"Replay JSON nesting exceeds 64 levels.");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key)require(keys.back().insert(item.get<std::string>()).second,"Duplicate replay field.");
        if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    });
    require(value.is_array() && !value.empty() && value.size()<=256,"Replay must be a nonempty array of at most 256 segments.");return value;
}
Json call(WorldSession& world,const std::string& method,Json params=Json::object()) {
    auto reply=Json::parse(world.request(Json{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",std::move(params)}}.dump()));
    if(reply.contains("error"))throw std::runtime_error(method+": "+reply.at("error").at("message").get<std::string>());
    return reply.at("result");
}
void validate_game_host(const GameDefinition& game) {
    require(game.target_os==build_metadata().target_os && game.target_arch==build_metadata().target_arch,"Game bundle targets a different platform or architecture.");
    require(Runtime::available(),"Game launch requires simulation in this executable.");
    require(!game.audio || POIMA_AUDIO,"Game audio is unavailable in this executable.");
}
std::string prepare_game(WorldSession& world,const GameDefinition& game,const std::string& save_root) {
    if(!save_root.empty()) {
        require(save_root.find('\0')==std::string::npos,"Save root must be NUL-free.");
        // Keep aliases for the existing storage validator; CLI-relative paths
        // resolve against the caller's working directory in both launch modes.
        const auto root=text_of(fs::absolute(path_of(save_root)).lexically_normal());
        call(world,"save.configure",{{"request_id",id()},{"expected_generation",0},{"root",root}});
    }
    const auto session=id();
    call(world,"runtime.start",{{"session_id",session},{"revision",game.revision}});
    if(!game.gameplay_descriptor.empty())call(world,"runtime.gameplay.load_native",{
        {"session_id",session},{"request_id",id()},{"expected_tick",0},{"expected_revision",0},
        {"descriptor",game.gameplay_descriptor},{"expected_descriptor_sha256",game.gameplay_descriptor_sha256},
        {"values",Json::parse(game.gameplay_values)}});
    return session;
}
}
Reply serve_game(const GameServeOptions& options) {
    try {
        // A duplicate endpoint must fail before bundle/runtime/module work.
        LocalSessionServer host(options.endpoint);
        const auto game=load_game(options.manifest);validate_game_host(game);
        WorldSession world(game.world,WorldOpenMode::read_only_runtime,game.root);
        prepare_game(world,game,options.save_root);
        return {run_shared_session(world,host,options.endpoint),{}};
    }catch(const std::exception& error) {
        return {4,Json{{"protocol_version",1},{"request_id",nullptr},{"command","game.serve"},{"status","error"},
            {"result",nullptr},{"diagnostics",Json::array({{{"code","game.serve_failed"},{"message",error.what()}}})}}.dump()};
    }
}
Reply run_game(const GameLaunchOptions& options) {
    Json result=nullptr,diagnostics=Json::array();bool success=false;
    try {
        const auto game=load_game(options.manifest);
        validate_game_host(game);
        require(POIMA_RENDER_SMOKE,"Game launch requires the Vulkan player in this executable.");
        require(options.max_frames<=36000,"Interactive frame limit must be 0..36000.");
        require(options.replay.empty() || !options.max_frames,"Replay and an interactive frame limit cannot be combined.");
        output_path(options.render.capture,game,options.replay);output_path(options.report,game,options.replay);
        if(!options.render.capture.empty() && !options.report.empty())require(!world_detail::same_path_name(normalized(options.render.capture),normalized(options.report)),"Game capture and report must differ.");
        Json sequence;if(!options.replay.empty())sequence=replay_file(options.replay);
        WorldSession world(game.world,WorldOpenMode::read_only_runtime,game.root);
        const auto session=prepare_game(world,game,options.save_root);
        Json params={{"session_id",session},{"request_id",id()},{"expected_tick",0},{"controller",game.controller},{"camera",game.camera},
            {"mode",options.replay.empty() ? "interactive" : "replay"},{"audio",game.audio},{"width",options.render.width},{"height",options.render.height},
            {"samples",options.render.samples},{"culling",options.render.culling},{"profile",options.render.profile}};
        const bool has_settings=!options.settings_profile.empty() || !options.settings_overrides.empty() || options.settings_revision.has_value();
        if(has_settings && !options.samples_explicit)params.erase("samples");
        if(options.frames_in_flight_explicit)params["frames_in_flight"]=options.render.frames_in_flight;
        if(!options.settings_profile.empty())params["settings_profile"]=text_of(fs::absolute(path_of(options.settings_profile)).lexically_normal());
        if(options.settings_revision)params["settings_revision"]=*options.settings_revision;
        if(!options.settings_overrides.empty()) {
            require(options.settings_overrides.size()<=65536,"Settings overrides exceed 64 KiB.");
            std::vector<std::set<std::string>> keys;
            params["settings_overrides"]=Json::parse(options.settings_overrides,[&](int depth,Json::parse_event_t event,Json& value) {
                require(depth<=16,"Settings overrides exceed the nesting limit.");
                if(event==Json::parse_event_t::object_start)keys.emplace_back();
                if(event==Json::parse_event_t::key)require(!keys.empty() && keys.back().insert(value.get<std::string>()).second,"Duplicate settings override ID.");
                if(event==Json::parse_event_t::object_end)keys.pop_back();
                return true;
            });
        }
        if(options.render.gpu>=0)params["gpu"]=options.render.gpu;
        if(!options.render.capture.empty())params["path"]=text_of(normalized(options.render.capture));
        if(!options.replay.empty())params["sequence"]=std::move(sequence);else params["max_frames"]=options.max_frames;
        if(!game.input_profile.empty())params["input_profile"]=game.input_profile;
        const auto play=call(world,"runtime.play",std::move(params));
        const auto current=world.runtime_status();
        require(current.active,"Player finished without an active runtime to report.");
        const auto& final_session=current.session_id;
        result={{"game",{{"name",game.name},{"manifest",text_of(normalized(options.manifest))}}},{"play",play},
            {"runtime",call(world,"runtime.inspect",{{"session_id",final_session}})},
            {"entities",{{game.controller,call(world,"runtime.entity",{{"session_id",final_session},{"id",game.controller}})},
                         {game.camera,call(world,"runtime.entity",{{"session_id",final_session},{"id",game.camera}})}}}};
        success=play.value("success",false);result["success"]=success;
        if(!game.gameplay_descriptor.empty())result["gameplay"]=call(world,"runtime.gameplay.inspect",{{"session_id",final_session},{"include_schema",true}});
        if(!success)diagnostics.push_back({{"code","game.play_failed"},{"message","Player stopped with an engine error; inspect result.play."}});
        output_path(options.report,game,options.replay);write_report(options.report,result);
    }catch(const std::exception& error) { success=false;diagnostics.push_back({{"code","game.failed"},{"message",error.what()}}); }
    return {success ? 0 : 4,Json{{"protocol_version",1},{"request_id",nullptr},{"command","game.run"},{"status",success ? "ok" : "error"},{"result",result},{"diagnostics",diagnostics}}.dump()};
}
}
