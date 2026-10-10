// SPDX-License-Identifier: Apache-2.0
#include "development_service.hpp"
#include "development_diagnostics.hpp"
#include <algorithm>
#include <cctype>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace poima::development {
namespace {
using Json=nlohmann::json;
void need(bool value,const std::string& message,int code=-32602) { if(!value)throw ServiceError(code,message); }
void fields(const Json& p,std::initializer_list<const char*> allowed,std::initializer_list<const char*> required={}) {
    need(p.is_object(),"Expected development parameters.");
    for(const auto& [name,value]:p.items()) { (void)value;need(std::find(allowed.begin(),allowed.end(),name)!=allowed.end(),"Unknown development parameter: "+name); }
    for(const auto* name:required)need(p.contains(name),"Missing development parameter: "+std::string(name));
}
std::string text(const Json& value,std::size_t max=4096) {
    need(value.is_string(),"Expected a string.");auto result=value.get<std::string>();
    need(!result.empty() && result.size()<=max && result.find('\0')==std::string::npos,"Invalid development string.");return result;
}
std::filesystem::path absolute(const Json& value) {
    const auto raw=text(value);auto result=std::filesystem::path(std::u8string(raw.begin(),raw.end()));need(result.is_absolute(),"Development paths must be absolute.");return result;
}
std::string utf8(const std::filesystem::path& value) { const auto s=value.u8string();return std::string(reinterpret_cast<const char*>(s.data()),s.size()); }
namespace fs=std::filesystem;
bool within(const fs::path& child,const fs::path& root) {
    auto a=child.begin(),b=root.begin();
    for(;b!=root.end();++a,++b) {
        if(a==child.end())return false;
#ifdef _WIN32
        if(CompareStringOrdinal(a->c_str(),-1,b->c_str(),-1,TRUE)!=CSTR_EQUAL)return false;
#else
        if(*a!=*b)return false;
#endif
    }
    return true;
}
bool overlap(const fs::path& a,const fs::path& b) {return within(a,b) || within(b,a);}
void checked_components(const fs::path& path) {
    auto current=path.root_path();
    for(const auto& part:path.relative_path()) {
        current/=part;std::error_code error;const auto status=fs::symlink_status(current,error);
        need(!error || error==std::errc::no_such_file_or_directory,"Cannot inspect development path.");
        need(!fs::is_symlink(status),"Development paths cannot contain symlinks.");
#ifdef _WIN32
        const auto attributes=GetFileAttributesW(current.c_str());
        need(attributes==INVALID_FILE_ATTRIBUTES || !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Development paths cannot contain reparse points.");
#endif
        need(status.type()==fs::file_type::not_found || fs::is_directory(status) || current==path,
             "Development path has a nondirectory parent.");
    }
}
void configured_path(const fs::path& path,bool directory) {
    need(path.is_absolute() && path.native().find(fs::path::value_type{})==fs::path::string_type::npos,"Profile paths must be absolute and NUL-free.");
    checked_components(path);
    need(directory ? fs::is_directory(path):fs::is_regular_file(path),"Profile requires an existing directory or tool file.");
    need(fs::canonical(path)==path.lexically_normal(),"Profile paths must be canonical.");
}
std::string relative(const Json& value) {
    const auto name=text(value,1024);need(name.find('\\')==std::string::npos && name.find(':')==std::string::npos,"Use relative portable paths.");
    const fs::path path(std::u8string(name.begin(),name.end()));need(!path.is_absolute() && !path.has_root_path(),"Development project/output must be relative.");
    for(const auto& part:path) {
        need(!part.empty() && part!="." && part!="..","Development paths cannot contain dot or empty components.");
        const auto component=utf8(part);
        need(component.back()!='.' && component.back()!=' ' && component.find_first_of("<>\"|?*")==std::string::npos &&
             std::none_of(component.begin(),component.end(),[](unsigned char c){return c<32 || c==127;}),"Invalid portable development path component.");
        auto base=component.substr(0,component.find('.'));std::transform(base.begin(),base.end(),base.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        need(base!="con" && base!="prn" && base!="aux" && base!="nul" && base!="conin$" && base!="conout$" &&
            !(base.size()==4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3]>='1' && base[3]<='9'),"Reserved development path component.");
    }
    need(name.back()!='/' && name.find("//")==std::string::npos,"Invalid relative development path.");
    return name;
}
fs::path contained(const fs::path& root,const std::string& name,bool file) {
    const auto path=(root/fs::path(std::u8string(name.begin(),name.end()))).lexically_normal();
    need(within(path,root) && path!=root,"Development path escapes its configured root.");checked_components(path);
    if(file)need(fs::is_regular_file(path),"Development project file must exist.");
    return path;
}
bool ascii_name(const std::string& value) {
    return !value.empty() && value.size()<=64 && std::all_of(value.begin(),value.end(),[](char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-';
    });
}
bool game_type(const std::string& value) {
    bool first=true;
    for(char c:value) {
        if(c=='.' || c=='+') {if(first)return false;first=true;continue;}
        const bool letter=(c>='A' && c<='Z') || (c>='a' && c<='z') || c=='_';
        if(!letter && (first || c<'0' || c>'9'))return false;
        first=false;
    }
    return !first;
}
std::string verified_result(const std::string& value) {
    need(!value.empty() && value.size()<=65536,"Development verifier must return 1..65536 bytes of JSON.");
    std::vector<std::set<std::string>> keys;
    Json parsed;
    try {
        parsed=Json::parse(value,[&](int depth,Json::parse_event_t event,Json& item) {
            need(depth<=16,"Development verification result exceeds 16 nesting levels.");
            if(event==Json::parse_event_t::object_start)keys.emplace_back();
            else if(event==Json::parse_event_t::key)need(!keys.empty() && keys.back().insert(item.get<std::string>()).second,"Duplicate development verification result key.");
            else if(event==Json::parse_event_t::object_end)keys.pop_back();
            return true;
        });
    }catch(const std::exception&) {throw ServiceError(-32602,"Development verifier returned invalid, duplicate or excessively nested JSON.");}
    need(parsed.is_object() && !parsed.empty(),"Development verifier must return a nonempty JSON object.");
    return parsed.dump();
}
std::string request_id(const Json& value) {
    auto result=text(value,32);need(result.size()==32 && std::all_of(result.begin(),result.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');}),"Request ID needs 32 lowercase hex characters.");return result;
}
JobId job_id(const Json& value) {
    need(value.is_number_integer() && value>0 && value<=9007199254740991LL,"Invalid development job ID.");return value.get<JobId>();
}
Json safe_diagnostic(const std::string& value) {
    // Tool output is bytes, not necessarily UTF-8. Retained tails can also begin
    // inside a multibyte character; replacement keeps status JSON deliverable.
    return Json::parse(Json(value).dump(-1,' ',false,Json::error_handler_t::replace));
}
Json status(const Status& value,bool diagnostics) {
    Json result={{"job_id",value.id},{"state",state_name(value.state)},{"terminal",terminal(value.state)},
        {"exit_code",value.exit_code ? Json(*value.exit_code):Json(nullptr)},{"elapsed_ms",value.elapsed.count()},
        {"cancellation_requested",value.cancellation_requested},{"stdout_bytes",value.output_bytes},{"stderr_bytes",value.error_bytes},
        {"stdout_truncated",value.output_truncated},{"stderr_truncated",value.error_truncated}};
    result["result"]=nullptr;
    if(value.state==State::succeeded && !value.result.empty()) {
        auto verified=Json::parse(value.result,nullptr,false);if(!verified.is_discarded())result["result"]=std::move(verified);
    }
    if(diagnostics) { result["stdout"]=safe_diagnostic(value.standard_output);result["stderr"]=safe_diagnostic(value.standard_error);result["error"]=safe_diagnostic(value.error); }
    return result;
}
Json object(Json properties,Json required=Json::array()) { return {{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}}; }
}
void Service::configure(std::vector<Profile> profiles) {
    need(!configured_ && used_requests_.empty(),"Development profiles can be configured once before any accepted job.",-32090);
    need(profiles.size()<=16,"At most 16 development profiles are allowed.");
    std::map<std::string,Profile> selected;
    for(auto& profile:profiles) {
        need(ascii_name(profile.name),"Profile name must contain 1..64 ASCII letters, digits, '_' or '-'.");
        need(profile.fingerprint.size()==64 && std::all_of(profile.fingerprint.begin(),profile.fingerprint.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');}),"Profile fingerprint requires 64 lowercase hex characters.");
        need(profile.target_rid=="win-x64" || profile.target_rid=="linux-x64","Unsupported profile target RID.");
        const bool publish=bool(profile.verify_publish),export_game=bool(profile.verify_export);
        need(publish || export_game,"Profile needs an enabled operation and its native verifier.");
        need(!publish || (!profile.python.empty() && !profile.publisher.empty() && !profile.dotnet.empty()),"Publish profile requires Python, publisher and .NET tools.");
        need(!export_game || (!profile.exporter.empty() && !profile.runtime_root.empty()),"Export profile requires exporter and runtime roots.");
        configured_path(profile.project_root,true);configured_path(profile.output_root,true);
        if(!profile.runtime_root.empty())configured_path(profile.runtime_root,true);
        for(const auto* path:{&profile.python,&profile.publisher,&profile.dotnet,&profile.exporter})if(!path->empty())configured_path(*path,false);
        need(!overlap(profile.output_root,profile.project_root) && (profile.runtime_root.empty() || !overlap(profile.output_root,profile.runtime_root)),"Profile output root overlaps its inputs.");
        for(const auto* path:{&profile.python,&profile.publisher,&profile.dotnet,&profile.exporter})
            if(!path->empty())need(!overlap(profile.output_root,path->parent_path()),"Profile output root overlaps tools or publisher resources.");
        Request validation;validation.executable=publish ? profile.python:profile.exporter;validation.working_directory=profile.project_root;validation.environment=profile.environment;
        try {validate_request(validation);}catch(const std::exception& error) {throw ServiceError(-32602,error.what());}
        need(!selected.contains(profile.name),"Duplicate development profile name.");selected.emplace(profile.name,std::move(profile));
    }
    // An output root cannot write into another configured profile's authority.
    for(const auto& [unused,a]:selected)for(const auto& [also_unused,b]:selected) {
        (void)unused;(void)also_unused;
        need(!overlap(a.output_root,b.project_root) && (b.runtime_root.empty() || !overlap(a.output_root,b.runtime_root)),"Profile output root overlaps configured inputs.");
        for(const auto* tool:{&b.python,&b.publisher,&b.dotnet,&b.exporter})if(!tool->empty())
            need(!overlap(a.output_root,tool->parent_path()),"Profile output root overlaps configured tools.");
    }
    profiles_=std::move(selected);configured_=true;
}
Json Service::dispatch(const std::string& method,const Json& p) {
    if(method=="development.profiles") {
        fields(p,{});Json result=Json::array();
        for(const auto& [name,profile]:profiles_)result.push_back({{"name",name},{"fingerprint",profile.fingerprint},{"target_rid",profile.target_rid},{"publish",bool(profile.verify_publish)},{"export",bool(profile.verify_export)}});
        return {{"profiles",result}};
    }
    if(method=="development.compile" || method=="development.publish" || method=="development.export") {
        const bool compile=method=="development.compile",publish=method=="development.publish";
        if(compile)fields(p,{"request_id","executable","project","output","configuration","timeout_ms"},{"request_id","executable","project","output"});
        else if(publish)fields(p,{"request_id","profile","project","output","type","timeout_ms"},{"request_id","profile","project","output","type"});
        else fields(p,{"request_id","profile","project","output","timeout_ms"},{"request_id","profile","project","output"});
        const auto id=request_id(p.at("request_id"));Request request;std::optional<fs::path> reservation;
        const auto timeout=p.value("timeout_ms",Json(compile ? 600000:900000));
        need(timeout.is_number_integer() && timeout>=100 && timeout<=(compile ? 600000:900000),"Invalid development timeout.");
        Json normalized={{"method",method},{"request_id",id},{"timeout_ms",timeout}};
        std::string project_name,output_name,type;const Profile* profile=nullptr;
        if(compile) {
            const auto executable=absolute(p.at("executable")),project=absolute(p.at("project")),output=absolute(p.at("output"));
            auto extension=project.extension().string();std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
            need(extension==".csproj","Compile expects a C# project (.csproj).");
            const auto configuration=p.contains("configuration") ? text(p.at("configuration"),32):std::string("Debug");need(configuration=="Debug" || configuration=="Release","Configuration must be Debug or Release.");
            normalized.update({{"executable",utf8(executable)},{"project",utf8(project)},{"output",utf8(output)},{"configuration",configuration}});
            request.executable=executable;request.working_directory=project.parent_path();
            request.arguments={"build",utf8(project),"-c",configuration,"--nologo","--disable-build-servers","--artifacts-path",utf8(output/"artifacts"),"-o",utf8(output/"managed")};
        }else {
            const auto name=text(p.at("profile"),64);const auto selected=profiles_.find(name);need(selected!=profiles_.end(),"Development profile is unavailable.",-32004);profile=&selected->second;
            need(publish ? bool(profile->verify_publish):bool(profile->verify_export),"Development profile does not enable this operation.",-32004);
            project_name=relative(p.at("project"));output_name=relative(p.at("output"));
            normalized.update({{"profile",name},{"fingerprint",profile->fingerprint},{"project",project_name},{"output",output_name}});
            if(publish) {type=text(p.at("type"),512);need(game_type(type),"Invalid compiled C# gameplay type name.");normalized["type"]=type;}
        }
        if(const auto prior=receipts_.find(id);prior!=receipts_.end()) {
            need(prior->second.params==normalized,"Development request ID was reused with different parameters.",-32009);
            return {{"job_id",prior->second.job},{"replayed",true},{"receipt_scope","session"}};
        }
        need(!used_requests_.contains(id),"Development receipt expired; inspect existing jobs instead of resubmitting this ID.",-32009);
        need(used_requests_.size()<4096,"Session development request budget exhausted.",-32090);
        if(!compile) {
            const auto project=contained(profile->project_root,project_name,true),output=contained(profile->output_root,output_name,false);
            auto extension=project.extension().string();std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
            need(extension==(publish ? ".csproj":".json"),"Unexpected development project extension.");
            need(!fs::exists(fs::symlink_status(output)),"Development output must name a new directory.",-32009);
            for(const auto& prior:output_reservations_)need(!overlap(output,prior),"Development output is already reserved.",-32009);
            request.working_directory=profile->project_root;request.environment=profile->environment;reservation=output;
            if(publish) {
                request.executable=profile->python;request.arguments={utf8(profile->publisher),"--project",utf8(project),"--type",type,"--output",utf8(output),"--dotnet",utf8(profile->dotnet),"--rid",profile->target_rid};
                request.verify_success=[verify=profile->verify_publish,output,type] {return verified_result(verify(output,type));};
            }else {
                request.executable=profile->exporter;request.arguments={"project","build",utf8(project),"--output",utf8(output),"--runtime",utf8(profile->runtime_root)};
                request.verify_success=[verify=profile->verify_export,output] {return verified_result(verify(output));};
            }
        }
        request.timeout=std::chrono::milliseconds(timeout.get<int>());
        if(!jobs_)jobs_=std::make_unique<Jobs>();
        // Prepare all receipt allocations before accepting the asynchronous job.
        auto [entry,inserted]=receipts_.emplace(id,Receipt{normalized,0});(void)inserted;
        try { receipt_order_.push_back(id);used_requests_.insert(id);if(reservation)output_reservations_.push_back(*reservation); }
        catch(...) { receipts_.erase(entry);if(!receipt_order_.empty() && receipt_order_.back()==id)receipt_order_.pop_back();used_requests_.erase(id);throw; }
        try {
            entry->second.job=jobs_->submit(std::move(request));
        } catch(const std::exception& error) {
            if(reservation)output_reservations_.pop_back();
            receipts_.erase(entry);receipt_order_.pop_back();used_requests_.erase(id);throw ServiceError(-32090,error.what());
        }
        const auto accepted=entry->second.job;
        if(receipt_order_.size()>128) { receipts_.erase(receipt_order_.front());receipt_order_.pop_front(); }
        return {{"job_id",accepted},{"replayed",false},{"receipt_scope","session"}};
    }
    if(method=="development.jobs") {
        fields(p,{});Json result=Json::array();if(jobs_)for(const auto& item:jobs_->list())result.push_back(status(item,false));
        return {{"jobs",result},{"retained_limit",32},{"request_budget_remaining",4096-used_requests_.size()}};
    }
    const bool inspect=method=="development.inspect",diagnostics=method=="development.diagnostics",cancel=method=="development.cancel",forget=method=="development.forget";
    need(inspect || diagnostics || cancel || forget,"Unknown development operation.",-32601);
    if(diagnostics)fields(p,{"job_id","limit"},{"job_id"});else fields(p,{"job_id"},{"job_id"});
    const auto limit=p.value("limit",Json(32));
    if(diagnostics)need(limit.is_number_integer() && limit>=1 && limit<=128,"Diagnostic limit must be 1..128.");
    const auto id=job_id(p.at("job_id"));
    const auto prior=jobs_ ? jobs_->poll(id):std::optional<Status>{};need(prior.has_value(),"Development job is unknown or forgotten.",-32004);
    if(forget) { need(terminal(prior->state),"Only terminal jobs can be forgotten.",-32090);need(jobs_->forget(id),"Job could not be forgotten.",-32090);return {{"job_id",id},{"forgotten",true}}; }
    if(cancel) { const bool requested=jobs_->cancel(id);auto result=status(*jobs_->poll(id),false);result["requested"]=requested;return result; }
    if(diagnostics) {
        const auto report=parse_diagnostics(prior->standard_output,prior->output_truncated,prior->standard_error,prior->error_truncated,
            terminal(prior->state),limit.get<std::size_t>());
        auto result=status(*prior,false);result["diagnostics"]=Json::array();
        for(const auto& item:report.diagnostics) {
            Json location=nullptr;
            if(item.location)location={{"line",item.location->line},
                {"column",item.location->column ? Json(*item.location->column):Json(nullptr)},
                {"end_line",item.location->end_line ? Json(*item.location->end_line):Json(nullptr)},
                {"end_column",item.location->end_column ? Json(*item.location->end_column):Json(nullptr)}};
            result["diagnostics"].push_back({{"severity",item.severity==Severity::error ? "error":"warning"},
                {"code",safe_diagnostic(item.code)},{"origin",safe_diagnostic(item.origin)},
                {"message",safe_diagnostic(item.message)},{"project",item.project.empty() ? Json(nullptr):safe_diagnostic(item.project)},
                {"location",location}});
        }
        result["recognized_lines"]=report.recognized_count;result["more"]=report.more;result["incomplete"]=report.incomplete;
        result["limit"]=limit;result["parser"]="msbuild-csharp-subset-v1";return result;
    }
    return status(*prior,true);
}
Json Service::schemas() {
    const Json path={{"type","string"},{"minLength",1},{"maxLength",4096},{"description","Absolute UTF-8 path."}},
        id={{"type","integer"},{"minimum",1},{"maximum",9007199254740991LL}};
    const Json portable={{"type","string"},{"minLength",1},{"maxLength",1024},{"description","Relative portable path under the selected host profile root; no traversal or links."}},
        profile={{"type","string"},{"pattern","^[A-Za-z0-9_-]{1,64}$"}},
        receipt={{"type","string"},{"pattern","^[0-9a-f]{32}$"}},
        timeout={{"type","integer"},{"minimum",100},{"maximum",900000},{"default",900000}};
    return {
        {"development.profiles",object(Json::object())},
        {"development.publish",object({{"request_id",receipt},{"profile",profile},{"project",portable},{"output",portable},
            {"type",{{"type","string"},{"minLength",1},{"maxLength",512},{"pattern","^[A-Za-z_][A-Za-z0-9_]*([.+][A-Za-z_][A-Za-z0-9_]*)*$"}}},{"timeout_ms",timeout}},
            {"request_id","profile","project","output","type"})},
        {"development.export",object({{"request_id",receipt},{"profile",profile},{"project",portable},{"output",portable},{"timeout_ms",timeout}},
            {"request_id","profile","project","output"})},
        {"development.compile",object({{"request_id",{{"type","string"},{"pattern","^[0-9a-f]{32}$"}}},{"executable",path},{"project",path},{"output",path},
            {"configuration",{{"enum",{"Debug","Release"}},{"default","Debug"}}},{"timeout_ms",{{"type","integer"},{"minimum",100},{"maximum",600000},{"default",600000}}}}, {"request_id","executable","project","output"})},
        {"development.jobs",object(Json::object())},
        {"development.inspect",object({{"job_id",id}},{"job_id"})},
        {"development.diagnostics",object({{"job_id",id},{"limit",{{"type","integer"},{"minimum",1},{"maximum",128},{"default",32}}}},{"job_id"})},
        {"development.cancel",object({{"job_id",id}},{"job_id"})},
        {"development.forget",object({{"job_id",id}},{"job_id"})}};
}
}
