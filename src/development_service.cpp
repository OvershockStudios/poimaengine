// SPDX-License-Identifier: Apache-2.0
#include "development_service.hpp"
#include "development_diagnostics.hpp"
#include <algorithm>
#include <cctype>
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
    if(diagnostics) { result["stdout"]=safe_diagnostic(value.standard_output);result["stderr"]=safe_diagnostic(value.standard_error);result["error"]=safe_diagnostic(value.error); }
    return result;
}
Json object(Json properties,Json required=Json::array()) { return {{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}}; }
}
Json Service::dispatch(const std::string& method,const Json& p) {
    if(method=="development.compile") {
        fields(p,{"request_id","executable","project","output","configuration","timeout_ms"},{"request_id","executable","project","output"});
        const auto id=request_id(p.at("request_id"));const auto executable=absolute(p.at("executable")),project=absolute(p.at("project")),output=absolute(p.at("output"));
        auto extension=project.extension().string();std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        need(extension==".csproj","Compile expects a C# project (.csproj).");
        const auto configuration=p.contains("configuration") ? text(p.at("configuration"),32) : std::string("Debug");need(configuration=="Debug" || configuration=="Release","Configuration must be Debug or Release.");
        const auto timeout=p.value("timeout_ms",Json(600000));need(timeout.is_number_integer() && timeout>=100 && timeout<=600000,"Timeout must be 100..600000 ms.");
        Json normalized={{"request_id",id},{"executable",utf8(executable)},{"project",utf8(project)},{"output",utf8(output)},
            {"configuration",configuration},{"timeout_ms",timeout}};
        if(const auto prior=receipts_.find(id);prior!=receipts_.end()) {
            need(prior->second.params==normalized,"Compile request ID was reused with different parameters.",-32009);
            return {{"job_id",prior->second.job},{"replayed",true},{"receipt_scope","session"}};
        }
        need(!used_requests_.contains(id),"Compile receipt expired; inspect existing jobs instead of resubmitting this ID.",-32009);
        need(used_requests_.size()<4096,"Session compile request budget exhausted.",-32090);
        if(!jobs_)jobs_=std::make_unique<Jobs>();
        // Prepare all receipt allocations before accepting the asynchronous job.
        auto [entry,inserted]=receipts_.emplace(id,Receipt{normalized,0});(void)inserted;
        try { receipt_order_.push_back(id);used_requests_.insert(id); }
        catch(...) { receipts_.erase(entry);if(!receipt_order_.empty() && receipt_order_.back()==id)receipt_order_.pop_back();throw; }
        try {
            Request request;request.executable=executable;request.working_directory=project.parent_path();request.timeout=std::chrono::milliseconds(timeout.get<int>());
            request.arguments={"build",utf8(project),"-c",configuration,"--nologo","--disable-build-servers","--artifacts-path",utf8(output/"artifacts"),"-o",utf8(output/"managed")};
            entry->second.job=jobs_->submit(std::move(request));
        } catch(const std::exception& error) {
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
    return {
        {"development.compile",object({{"request_id",{{"type","string"},{"pattern","^[0-9a-f]{32}$"}}},{"executable",path},{"project",path},{"output",path},
            {"configuration",{{"enum",{"Debug","Release"}},{"default","Debug"}}},{"timeout_ms",{{"type","integer"},{"minimum",100},{"maximum",600000},{"default",600000}}}}, {"request_id","executable","project","output"})},
        {"development.jobs",object(Json::object())},
        {"development.inspect",object({{"job_id",id}},{"job_id"})},
        {"development.diagnostics",object({{"job_id",id},{"limit",{{"type","integer"},{"minimum",1},{"maximum",128},{"default",32}}}},{"job_id"})},
        {"development.cancel",object({{"job_id",id}},{"job_id"})},
        {"development.forget",object({{"job_id",id}},{"job_id"})}};
}
}
