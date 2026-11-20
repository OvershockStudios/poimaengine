// SPDX-License-Identifier: Apache-2.0
#include "poima/game_launch.hpp"
#include "poima/assets.hpp"
#include "poima/project.hpp"
#include "poima/world.hpp"
#include "poima/runtime.hpp"
#include "poima/build_metadata.hpp"
#include "poima/local_session.hpp"
#include "poima/shared_session.hpp"
#include "world_stdio.hpp"
#include "world_storage.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <initializer_list>
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
void validate_save_launch(const std::string& save_root,const std::optional<std::string>& write_mode,bool default_root=false) {
    if(!write_mode)return;
    require(*write_mode=="synchronous" || *write_mode=="background","Save write mode must be synchronous or background.");
    require(!save_root.empty() || default_root,"Save write mode requires --save-root.");
}
// Exported Windows launchers opt in here. Explicit roots keep
// their existing validation and are never created here. Ordinary ancestry checks
// assume a trusted environment with no concurrent hostile filesystem changes.
std::string launch_save_root(const GameLaunchOptions& options,const GameDefinition& game) {
    if(!options.save_root.empty() || !options.default_save_root)return options.save_root;
#ifdef _WIN32
    const auto length=GetEnvironmentVariableW(L"LOCALAPPDATA",nullptr,0);
    require(length>1 && length<=4097,"Default saves require a nonempty LOCALAPPDATA path.");
    std::wstring buffer(length,L'\0');
    const auto count=GetEnvironmentVariableW(L"LOCALAPPDATA",buffer.data(),length);
    require(count>0 && count<length,"LOCALAPPDATA changed or could not be read.");buffer.resize(count);
    const fs::path base(buffer);
    require(base.is_absolute(),"Default saves require an absolute LOCALAPPDATA path.");
    for(const auto& component:base.relative_path())require(component!="." && component!="..","LOCALAPPDATA cannot contain dot path components.");
    require(game.project_id.size()==32 && game.project_id.find_first_not_of("0123456789abcdef")==std::string::npos,"Default saves require a verified project ID.");
    const auto root=base/"Poima"/"Games"/game.project_id/"Saves";
    require(text_of(root).size()<=4096,"Default save root exceeds the save path limit.");
    auto ordinary=[](const fs::path& at,bool missing_allowed) {
        std::error_code error;const auto status=fs::symlink_status(at,error);
        if(status.type()==fs::file_type::not_found && (!error || error==std::errc::no_such_file_or_directory)) {
            require(missing_allowed,"LOCALAPPDATA must be an existing ordinary directory.");return false;
        }
        require(!error && fs::is_directory(status) && !fs::is_symlink(status),"Default save root requires ordinary directories without symbolic links.");
        const auto attributes=GetFileAttributesW(at.c_str());
        require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Default save root cannot contain a reparse point.");return true;
    };
    auto ancestry=[&](const fs::path& path,bool missing_allowed) {
        auto at=path.root_path();ordinary(at,false);
        for(const auto& component:path.relative_path()) { at/=component;if(!ordinary(at,missing_allowed))break; }
    };
    // Validate every existing prefix before canonicalization or any mkdir.
    ancestry(base,false);ancestry(root,true);
    const auto resolved=fs::weakly_canonical(root);
    require(!inside(resolved,normalized(game.root)),"Default save root must be outside the immutable game bundle.");
    require(!inside(resolved,normalized(game.world+".assets")),"Default save root cannot be inside the immutable asset store.");
    auto at=base;
    for(const auto& part:{std::string("Poima"),std::string("Games"),game.project_id,std::string("Saves")}) {
        at/=part;std::error_code error;fs::create_directory(at,error);
        require(!error,"Could not create default save directory.");ordinary(at,false);
    }
    ancestry(root,false);return text_of(root);
#else
    (void)game;
    throw std::runtime_error("Default save roots are supported only on Windows; provide --save-root.");
#endif
}
std::string prepare_game(WorldSession& world,const GameDefinition& game,const std::string& save_root,
    const std::optional<std::string>& write_mode) {
    if(!save_root.empty()) {
        require(save_root.find('\0')==std::string::npos,"Save root must be NUL-free.");
        // Keep aliases for the existing storage validator; CLI-relative paths
        // resolve against the caller's working directory in both launch modes.
        const auto root=text_of(fs::absolute(path_of(save_root)).lexically_normal());
        Json params={{"request_id",id()},{"expected_generation",0},{"root",root}};
        if(write_mode)params["write_mode"]=*write_mode;
        call(world,"save.configure",std::move(params));
    }
    const auto session=id();
    call(world,"runtime.start",{{"session_id",session},{"revision",game.revision}});
    if(!game.gameplay_descriptor.empty())call(world,"runtime.gameplay.load_native",{
        {"session_id",session},{"request_id",id()},{"expected_tick",0},{"expected_revision",0},
        {"descriptor",game.gameplay_descriptor},{"expected_descriptor_sha256",game.gameplay_descriptor_sha256},
        {"values",Json::parse(game.gameplay_values)}});
    return session;
}

// Keep the exact suite bytes for its identity. Every operation's shape is
// admitted before loading gameplay, and its detailed parameters use the native
// runtime's ordinary bounded request validation.
void suite_fields(const Json& value,std::initializer_list<std::string_view> required,
    std::initializer_list<std::string_view> optional={}) {
    require(value.is_object(),"Expected an object in the verification suite.");
    for(const auto field:required)require(value.contains(field),"Missing verification field: "+std::string(field));
    for(auto item=value.begin();item!=value.end();++item)
        require(std::find(required.begin(),required.end(),item.key())!=required.end() ||
            std::find(optional.begin(),optional.end(),item.key())!=optional.end(),"Unknown verification field: "+item.key());
}

Json parse_suite(std::string_view bytes) {
    require(!bytes.empty() && bytes.size()<=1024*1024 && bytes.find('\0')==std::string_view::npos,
        "Verification suite must contain 1 byte to 1 MiB of JSON without NUL bytes.");
    std::vector<std::set<std::string>> keys;
    auto suite=Json::parse(bytes,[&](int depth,Json::parse_event_t event,Json& item) {
        require(depth<=32,"Verification suite nesting exceeds 32 levels.");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key)require(keys.back().insert(item.get<std::string>()).second,"Duplicate verification field.");
        if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    });
    suite_fields(suite,{"format","version","groups"});
    require(suite.at("format")=="poima.game-verification" && suite.at("version").is_number_integer() && suite.at("version")==1,
        "Unsupported game verification suite format or version.");
    const auto& groups=suite.at("groups");
    require(groups.is_array() && !groups.empty() && groups.size()<=32,"Verification requires 1 to 32 groups.");
    std::set<std::string> names;
    std::size_t operations=0;
    std::uint64_t ticks=0;
    for(const auto& group:groups) {
        suite_fields(group,{"name","sequence"});
        const auto label=group.at("name").get<std::string>();
        require(!label.empty() && label.size()<=96 && names.insert(label).second,"Verification group names must be unique and 1 to 96 bytes.");
        const auto& sequence=group.at("sequence");
        require(sequence.is_array() && !sequence.empty(),"Each verification group needs a nonempty sequence.");
        operations+=sequence.size();
        require(operations<=512,"Verification suite exceeds 512 operations.");
        bool has_expectation=false;
        for(const auto& operation:sequence) {
            suite_fields(operation,{}, {"step","expect"});
            require(operation.size()==1,"Each operation must contain one step or expectation.");
            if(operation.contains("step")) {
                const auto& step=operation.at("step");
                suite_fields(step,{"ticks"},{"inputs"});
                const auto& count=step.at("ticks");
                require(count.is_number_integer() && count>=1 && count<=600,"Verification step must be 1 to 600 ticks.");
                require(!step.contains("inputs") || (step.at("inputs").is_array() && step.at("inputs").size()<=32),
                    "Verification step accepts up to 32 native input objects.");
                ticks+=count.get<std::uint64_t>();
                require(ticks<=36000,"Verification suite exceeds 36000 ticks.");
                continue;
            }
            const auto& expect=operation.at("expect");
            suite_fields(expect,{"label","method","path"},{"params","equals","between"});
            const auto check=expect.at("label").get<std::string>();
            require(!check.empty() && check.size()<=96,"Expectation labels must be 1 to 96 bytes.");
            const auto method=expect.at("method").get<std::string>();
            static const std::set<std::string_view> observers={"runtime.inspect","runtime.entity","runtime.gameplay.inspect",
                "runtime.ui.inspect","runtime.component.get","runtime.raycast"};
            require(observers.contains(method),"Unsupported verification observation: "+method);
            if(expect.contains("params")) {
                const auto& params=expect.at("params");
                require(params.is_object() && !params.contains("session_id") && !params.contains("tick"),
                    "Observation parameters must omit session_id and tick.");
            }
            const auto pointer=expect.at("path").get<std::string>();
            require(pointer.size()<=512,"Verification JSON pointer exceeds 512 bytes.");
            (void)Json::json_pointer(pointer);
            require(expect.contains("equals")!=expect.contains("between"),"Use exactly one equals or between comparison.");
            if(expect.contains("between")) {
                const auto& range=expect.at("between");
                require(range.is_array() && range.size()==2 && range[0].is_number() && range[1].is_number() &&
                    range[0].get<double>()<=range[1].get<double>(),"Between needs an ordered pair of numbers.");
            }
            has_expectation=true;
        }
        require(has_expectation,"Each verification group needs an expectation.");
    }
    return suite;
}

std::pair<Json,std::string> read_suite(const std::string& name) {
    const auto path=path_of(name);
    require(fs::is_regular_file(path),"Verification suite must be a regular file.");
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    const auto size=input.tellg();
    require(size>0 && size<=1024*1024,"Verification suite must contain 1 byte to 1 MiB.");
    std::string bytes(static_cast<std::size_t>(size),'\0');
    input.seekg(0);
    require(bool(input.read(bytes.data(),size)) && input.peek()==std::char_traits<char>::eof(),
        "Verification suite changed or could not be read.");
    return {parse_suite(bytes),sha256(std::as_bytes(std::span(bytes)))};
}

fs::path running_executable() {
#ifdef _WIN32
    std::wstring path(32768,L'\0');
    const auto size=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    require(size>0 && size<path.size(),"Cannot identify the running game executable.");
    path.resize(size);
    return fs::path(path);
#else
    return fs::canonical("/proc/self/exe");
#endif
}

Json evaluate_group(const GameDefinition& game,const Json& group) {
    Json result={{"name",group.at("name")},{"passed",true},{"ticks",0},{"checks",Json::array()}};
    std::size_t index=0;
    try {
        WorldSession world(game.world,WorldOpenMode::read_only_runtime,game.root);
        const auto session=prepare_game(world,game,{},std::nullopt);
        for(const auto& operation:group.at("sequence")) {
            ++index;
            const auto current=world.runtime_status();
            if(operation.contains("step")) {
                auto step=operation.at("step");
                const auto ticks=step.at("ticks").get<std::uint64_t>();
                step.update({{"session_id",session},{"request_id",id()},{"expected_tick",current.tick},
                    {"expected_structure_revision",current.structure_revision}});
                const auto advanced=call(world,"runtime.step",std::move(step));
                require(advanced.at("current_session_id")==session && advanced.at("current_tick")==current.tick+ticks,
                    "Runtime replaced or did not advance the requested ticks.");
                result["ticks"]=current.tick+ticks;
                continue;
            }
            const auto& expect=operation.at("expect");
            auto params=expect.value("params",Json::object());
            const auto method=expect.at("method").get<std::string>();
            params["session_id"]=session;
            if(method!="runtime.inspect")params["tick"]=current.tick;
            const auto observed=call(world,method,std::move(params));
            const Json::json_pointer pointer(expect.at("path").get<std::string>());
            const bool present=observed.contains(pointer);
            const Json& value=present ? observed.at(pointer) : Json(nullptr);
            const bool exact=expect.contains("equals");
            const auto& comparison=expect.at(exact ? "equals" : "between");
            const bool matches=present && (exact ? value==comparison : value.is_number() &&
                value.get<double>()>=comparison[0].get<double>() && value.get<double>()<=comparison[1].get<double>());
            // Observing a large native collection should not inflate a receipt.
            const bool omitted=value.dump().size()>2048;
            result["checks"].push_back({{"label",expect.at("label")},{"tick",current.tick},{"passed",matches},
                {"method",method},{"path",expect.at("path")},{"observed",present},
                {"actual",omitted ? Json(nullptr) : value},{"actual_omitted",omitted}});
            if(!matches)result["passed"]=false;
        }
    }catch(const std::exception& error) {
        result["passed"]=false;
        result["issue"]={{"operation",index},{"message",std::string(error.what()).substr(0,2048)}};
    }
    return result;
}
}

void validate_game_verification_suite(std::string_view bytes) { (void)parse_suite(bytes); }

Reply verify_gameplay(const std::string& manifest,const std::string& suite_file,const std::string& report) {
    Json result=nullptr,diagnostics=Json::array();
    bool passed=false;
    try {
        const auto [suite,suite_hash]=read_suite(suite_file);
        const auto game=load_game(manifest);
        validate_game_host(game);
        require(fs::equivalent(running_executable(),path_of(game.executable)),"Run verification with this bundle's own runtime executable.");
        output_path(report,game,suite_file);
        Json groups=Json::array();
        std::size_t count=0;
        for(const auto& group:suite.at("groups")) {
            auto observed=evaluate_group(game,group);
            if(observed.at("passed")==true)++count;
            groups.push_back(std::move(observed));
        }
        require(load_game(manifest).manifest_sha256==game.manifest_sha256,"Game bundle changed during verification.");
        const auto total=groups.size();
        const bool all=count==total;
        result={{"format","poima.game-verification-result"},{"version",1},{"project_id",game.project_id},
            {"revision",game.revision},{"bundle_sha256",game.manifest_sha256},{"suite_sha256",suite_hash},
            {"engine_version",build_metadata().version},{"groups_passed",count},{"groups_total",total},{"passed",all},
            {"groups",std::move(groups)}};
        if(!all)diagnostics.push_back({{"code","game.verification_failed"},{"message","One or more gameplay check groups failed."}});
        output_path(report,game,suite_file);
        write_report(report,result);
        passed=all;
    } catch(const std::exception& error) {
        result=nullptr;
        diagnostics.push_back({{"code","game.verify_failed"},{"message",error.what()}});
    }
    return {passed ? 0 : 4,Json{{"protocol_version",1},{"request_id",nullptr},{"command","game.verify"},
        {"status",passed ? "ok" : "error"},{"result",result},{"diagnostics",diagnostics}}.dump(-1,' ',false,Json::error_handler_t::replace)};
}
Reply serve_game(const GameServeOptions& options) {
    try {
        validate_save_launch(options.save_root,options.save_write_mode);
        // A duplicate endpoint must fail before bundle/runtime/module work.
        LocalSessionServer host(options.endpoint);
        const auto game=load_game(options.manifest);validate_game_host(game);
        WorldSession world(game.world,WorldOpenMode::read_only_runtime,game.root);
        prepare_game(world,game,options.save_root,options.save_write_mode);
        return {run_shared_session(world,host,options.endpoint),{}};
    }catch(const std::exception& error) {
        return {4,Json{{"protocol_version",1},{"request_id",nullptr},{"command","game.serve"},{"status","error"},
            {"result",nullptr},{"diagnostics",Json::array({{{"code","game.serve_failed"},{"message",error.what()}}})}}.dump()};
    }
}
Reply stdio_game(const GameStdioOptions& options) {
    try {
        validate_save_launch(options.save_root,options.save_write_mode);
        const auto game=load_game(options.manifest);validate_game_host(game);
        WorldSession world(game.world,WorldOpenMode::read_only_runtime,game.root);
        prepare_game(world,game,options.save_root,options.save_write_mode);
        return {run_world_stdio(world),{}};
    }catch(const std::exception& error) {
        return {4,Json{{"protocol_version",1},{"request_id",nullptr},{"command","game.stdio"},{"status","error"},
            {"result",nullptr},{"diagnostics",Json::array({{{"code","game.stdio_failed"},{"message",error.what()}}})}}.dump()};
    }
}
Reply run_game(const GameLaunchOptions& options) {
    Json result=nullptr,diagnostics=Json::array();bool success=false;
    try {
        require(options.replay.empty() || !options.lens_interpolation,"Lens interpolation is interactive only.");
        require(options.replay.empty() || !options.pose_interpolation,"Pose interpolation is interactive only.");
        validate_save_launch(options.save_root,options.save_write_mode,options.default_save_root);
        const auto game=load_game(options.manifest);
        validate_game_host(game);
        require(POIMA_RENDER_SMOKE,"Game launch requires the Vulkan player in this executable.");
        require(options.max_frames<=36000,"Interactive frame limit must be 0..36000.");
        require(options.replay.empty() || !options.max_frames,"Replay and an interactive frame limit cannot be combined.");
        output_path(options.render.capture,game,options.replay);output_path(options.report,game,options.replay);
        if(!options.render.capture.empty() && !options.report.empty())require(!world_detail::same_path_name(normalized(options.render.capture),normalized(options.report)),"Game capture and report must differ.");
        Json sequence;if(!options.replay.empty())sequence=replay_file(options.replay);
        WorldSession world(game.world,WorldOpenMode::read_only_runtime,game.root);
        const auto save_root=launch_save_root(options,game);
        const auto session=prepare_game(world,game,save_root,options.save_write_mode);
        Json params={{"session_id",session},{"request_id",id()},{"expected_tick",0},{"controller",game.controller},{"camera",game.camera},
            {"mode",options.replay.empty() ? "interactive" : "replay"},{"audio",game.audio},{"width",options.render.width},{"height",options.render.height},
            {"samples",options.render.samples},{"culling",options.render.culling},{"draw_instancing",options.render.draw_instancing},{"profile",options.render.profile}};
        if(options.render.viewmodel_samples!=1)params["viewmodel_samples"]=options.render.viewmodel_samples;
        if(options.lens_interpolation)params["lens_interpolation"]=true;
        if(options.pose_interpolation)params["pose_interpolation"]=true;
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
