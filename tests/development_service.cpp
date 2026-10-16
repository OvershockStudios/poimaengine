// SPDX-License-Identifier: Apache-2.0
#include "development_service.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using Json=nlohmann::json;
namespace fs=std::filesystem;
using namespace poima::development;
std::string utf8(const fs::path& path) { const auto text=path.u8string();return std::string(reinterpret_cast<const char*>(text.data()),text.size()); }
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
void project_operations(const fs::path& root) {
    const auto projects=root/"project-routing",outputs=root/"artifact-routing";
    fs::create_directory(projects);fs::create_directory(outputs);std::ofstream(projects/"project.json")<<"{}";
    Profile profile;profile.name="projects";profile.fingerprint=std::string(64,'a');profile.target_rid="win-x64";
    profile.project_root=fs::canonical(projects);profile.output_root=fs::canonical(outputs);
    unsigned inspections=0,adoptions=0;Json forwarded;
    profile.inspect_project=[&](const fs::path& manifest) {check(manifest==projects/"project.json","Inspection path escaped profile root.");++inspections;return Json{{"manifest_sha256",std::string(64,'b')}}.dump();};
    profile.adopt_gameplay=[&](const fs::path& manifest,const fs::path& artifact,const std::string& request) {
        check(manifest==projects/"project.json" && artifact==outputs/"retained/native-gameplay.json","Adoption selectors escaped configured roots.");
        forwarded=Json::parse(request);++adoptions;
        if(forwarded.value("values",Json::object()).contains("reject"))throw ServiceError(-32009,"Native journal conflict");
        if(forwarded.value("values",Json::object()).contains("io"))throw std::runtime_error("Native retained artifact failed");
        return Json{{"receipt_scope","project"},{"replayed",adoptions>1}}.dump();
    };
    Service service;service.configure({profile});
    const auto capabilities=service.dispatch("development.profiles",Json::object()).at("profiles").at(0);
    check(capabilities.at("project_inspect")==true && capabilities.at("gameplay_adopt")==true && capabilities.at("publish")==false && capabilities.at("export")==false,"Project-only profile capabilities differ.");
    const Json inspect={{"profile","projects"},{"project","project.json"}};
    check(service.dispatch("project.inspect",inspect).at("manifest_sha256")==std::string(64,'b') && inspections==1,"Project callback result changed.");
    Json adopt={{"profile","projects"},{"project","project.json"},{"artifact","retained/native-gameplay.json"},{"request_id",std::string(32,'1')},
        {"expected_project_sha256",std::string(64,'b')},{"expected_artifact_sha256",std::string(64,'c')}};
    check(service.dispatch("project.gameplay.adopt",adopt).at("receipt_scope")=="project","Adoption did not return synchronous project receipt.");
    auto expected=adopt;for(const auto* key:{"profile","project","artifact"})expected.erase(key);
    check(forwarded==expected && !forwarded.contains("values"),"Service altered omitted values or forwarded profile selectors.");
    check(service.dispatch("project.gameplay.adopt",adopt).at("replayed")==true && adoptions==2,"Service intercepted durable core replay.");
    auto reject=[&](const std::string& method,const Json& params,int code) {
        try{service.dispatch(method,params);}catch(const ServiceError& failure){check(failure.code==code,"Project routing error code differs.");return;}
        throw std::runtime_error("Expected project routing rejection.");
    };
    auto malformed=inspect;malformed["profile"]="missing";reject("project.inspect",malformed,-32004);
    malformed=inspect;malformed["project"]="../project.json";reject("project.inspect",malformed,-32602);
    malformed=inspect;malformed["request_id"]=std::string(32,'1');reject("project.inspect",malformed,-32602);
    for(const Json& bad:Json::array({nullptr,true,1,Json::array(),"values"})) {malformed=adopt;malformed["values"]=bad;reject("project.gameplay.adopt",malformed,-32602);}
    malformed=adopt;malformed["expected_project_sha256"]=std::string(64,'A');reject("project.gameplay.adopt",malformed,-32602);
    malformed=adopt;malformed["artifact"]="../escape.json";reject("project.gameplay.adopt",malformed,-32602);
    malformed=adopt;malformed["values"]={{"reject",1}};reject("project.gameplay.adopt",malformed,-32009);
    malformed=adopt;malformed["values"]={{"io",1}};reject("project.gameplay.adopt",malformed,-32091);
    Service limited;profile.adopt_gameplay={};limited.configure({profile});
    try{limited.dispatch("project.gameplay.adopt",adopt);throw std::runtime_error("Inspection-only profile granted adoption.");}
    catch(const ServiceError& failure){check(failure.code==-32004,"Disabled adoption error differs.");}
    const auto jobs=service.dispatch("development.jobs",Json::object());
    check(jobs.at("jobs").empty() && jobs.at("request_budget_remaining")==4096,"Synchronous project calls consumed compile jobs/receipts.");
}
int main(int argc,char** argv) {
    const bool sdk_isolated=argc==5 && std::string(argv[1])=="--sdk";
    const bool sdk_server=argc==6 && std::string(argv[1])=="--sdk-server";
    if(sdk_isolated || sdk_server) {
        try {
            Service service;Json parameters={{"request_id",std::string(31,'0')+"1"},
                {"executable",argv[2]},{"project",argv[3]},{"output",argv[4]},{"configuration","Release"}};
            if(sdk_server)parameters["compiler_server"]=argv[5];
            const auto accepted=service.dispatch("development.compile",parameters);
            const auto begin=std::chrono::steady_clock::now();Json result;
            do {
                result=service.dispatch("development.inspect",{{"job_id",accepted.at("job_id")}});
                if(result.at("terminal")==true) {
                    // Optional real SDK invocation also checks compact ownership
                    // metadata against the exact terminal full projection.
                    const auto explicit_full=service.dispatch("development.inspect",{{"job_id",accepted.at("job_id")},{"include_output",true}});
                    check(explicit_full.dump()==result.dump(),"Explicit full inspect changed default output.");
                    auto projected=result;projected.erase("stdout");projected.erase("stderr");
                    if(projected.contains("compiler_server")) {
                        auto& server=projected["compiler_server"];server.erase("log_segment");
                        server["diagnostics"].erase("stdout");server["diagnostics"].erase("stderr");
                    }
                    const auto compact=service.dispatch("development.inspect",{{"job_id",accepted.at("job_id")},{"include_output",false}});
                    check(compact.dump()==projected.dump(),"Compact terminal SDK identity/proof/counters/result/error differ.");
                    result["diagnostic_report"]=service.dispatch("development.diagnostics",{{"job_id",accepted.at("job_id")}});
                    std::cout<<result.dump(2)<<'\n';return result.at("state")=="succeeded" ? 0:1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }while(std::chrono::steady_clock::now()-begin<std::chrono::minutes(11));
            throw std::runtime_error("SDK compile wait timed out.");
        }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
    }
    if(argc>1 && std::string(argv[1])=="build") {
        if(std::string(argv[2]).find("diagnostic-bytes.csproj")!=std::string::npos) {
            std::cout<<"Game.cs(2,3): error CS1002: invalid:"<<static_cast<char>(0xff)<<'\n';return 1;
        }
        if(std::string(argv[2]).find("diagnostics.csproj")!=std::string::npos) {
            std::cout<<"Game (copy).cs(2,3): error CS1002: ; expected [Game.csproj]\n"
                <<"Game (copy).cs(1,1): warning CS1030: fixture warning [Game.csproj]\n";
            std::cerr<<"Game (copy).cs(2,3): error CS1002: ; expected [Game.csproj]\n";return 1;
        }
        if(std::string(argv[2]).find("slow.csproj")!=std::string::npos)std::this_thread::sleep_for(std::chrono::seconds(5));
        if(std::string(argv[2]).find("bytes.csproj")!=std::string::npos) { std::cout<<"invalid:"<<static_cast<char>(0xff);return 0; }
        Json arguments=Json::array();for(int i=1;i<argc;++i)arguments.push_back(argv[i]);
        std::cout<<Json({{"arguments",arguments},{"cwd",utf8(fs::current_path())}}).dump()<<'\n';return 0;
    }
    try {
        const auto root=fs::temp_directory_path()/("poima-development-service-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
        struct Cleanup { fs::path path;~Cleanup(){std::error_code e;fs::remove_all(path,e);} } cleanup{root};
        project_operations(root);
        const auto executable=fs::absolute(argv[0]);
        fs::create_directory(root/"subdir");
        const auto project=root/"subdir"/".."/"P $(literal) 'quotes'.csproj";std::ofstream(project)<<"<Project/>";
        Service service;
        auto compile=[&](std::string id,const fs::path& file,const fs::path& tool) {
            return Json{{"request_id",std::string(31,'0')+id},{"project",utf8(file)},{"executable",utf8(tool)},{"output",utf8(root/"output space")}};
        };
        auto error=[&](const std::string& method,const Json& params,int code) {
            try {service.dispatch(method,params);}catch(const ServiceError& failure){check(failure.code==code,"Wrong error code.");return;}
            throw std::runtime_error("Expected guarded rejection.");
        };
        auto wait=[&](const Json& accepted) {
            const auto begin=std::chrono::steady_clock::now();Json result;
            do {
                result=service.dispatch("development.inspect",{{"job_id",accepted.at("job_id")}});
                if(result.at("terminal")==true)return result;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }while(std::chrono::steady_clock::now()-begin<std::chrono::seconds(10));
            throw std::runtime_error("Job timed out during contract.");
        };
        check(service.dispatch("development.jobs",Json::object()).at("jobs").empty(),"Unexpected initial jobs.");
        auto params=compile("1",project,executable);auto invalid=params;invalid["project"]="relative.csproj";
        error("development.compile",invalid,-32602);invalid=params;invalid["configuration"]="Bogus";error("development.compile",invalid,-32602);
        invalid=params;invalid["configuration"]=7;error("development.compile",invalid,-32602);
        invalid=params;invalid["project"]=utf8(root/"not-a-project.txt");error("development.compile",invalid,-32602);
        const auto invalid_budget=service.dispatch("development.jobs",Json::object()).at("request_budget_remaining");
        for(const Json& value:Json::array({nullptr,0,1,0.0,1.0,"false",Json::array(),Json::object()})) {
            invalid=params;invalid["restore"]=value;error("development.compile",invalid,-32602);
            check(service.dispatch("development.jobs",Json::object()).at("jobs").empty(),"Malformed restore launched a job.");
            check(service.dispatch("development.jobs",Json::object()).at("request_budget_remaining")==invalid_budget,"Malformed restore consumed a receipt.");
        }
        const auto compiler_budget=service.dispatch("development.jobs",Json::object()).at("request_budget_remaining");
        for(const Json& value:Json::array({nullptr,false,true,0,1,0.0,Json::array(),Json::object(),"",
                                         "relative/VBCSCompiler.dll",utf8(root/"csc.dll")})) {
            invalid=params;invalid["compiler_server"]=value;error("development.compile",invalid,-32602);
            check(service.dispatch("development.jobs",Json::object()).at("jobs").empty(),"Malformed server selector launched a job.");
            check(service.dispatch("development.jobs",Json::object()).at("request_budget_remaining")==compiler_budget,
                  "Malformed server selector consumed a receipt.");
        }
#ifndef _WIN32
        invalid=params;invalid["compiler_server"]=utf8(root/"VBCSCompiler.dll");
        error("development.compile",invalid,-32602);
        check(service.dispatch("development.jobs",Json::object()).at("jobs").empty(),"Unsupported compiler mode launched a job.");
#endif
        const auto first=service.dispatch("development.compile",params);check(!first.at("replayed").get<bool>(),"Initial compile replayed.");
        check(service.dispatch("development.compile",params).at("job_id")==first.at("job_id"),"Retry created another job.");
        auto explicit_restore=params;explicit_restore["restore"]=true;
        check(service.dispatch("development.compile",explicit_restore)==Json({{"job_id",first.at("job_id")},{"replayed",true},{"receipt_scope","session"}}),"Omitted and explicit restore:true differ.");
        invalid=params;invalid["restore"]=false;error("development.compile",invalid,-32009);
#ifdef _WIN32
        // Changed mode conflicts before tool existence is inspected. This is
        // an inert receipt test, not a fake successful shared compilation.
        invalid=params;invalid["compiler_server"]=utf8(root/"VBCSCompiler.dll");
        error("development.compile",invalid,-32009);
#endif
        check(service.dispatch("development.jobs",Json::object()).at("jobs").size()==1,"Retry duplicated job.");
        invalid=params;invalid["configuration"]="Release";error("development.compile",invalid,-32009);
        const auto result=wait(first);check(result.at("state")=="succeeded","Generated compiler command failed.");
        {
            const auto id=first.at("job_id");
            check(!result.contains("process") && service.dispatch("development.inspect",{{"job_id",id},{"include_process",false}}).dump()==result.dump(),
                "Default/false process selector changed legacy full status.");
            auto enriched=service.dispatch("development.inspect",{{"job_id",id},{"include_process",true}});
            const auto& process=enriched.at("process");
            check(process.is_object() && process.size()==9 && process.at("version")==1 && process.at("launched")==true &&
                process.at("leader_pid").is_number_integer() && process.at("leader_pid")>0 && process.at("group_assigned")==true &&
                process.at("leader_exit_observed")==true && process.at("cleanup").at("finished")==true &&
                process.at("cleanup").at("requests_overflow")==false && process.at("cleanup").at("requests").size()>=1,
                "Opt-in completed process observation incomplete.");
#ifdef _WIN32
            check(process.at("platform")=="windows" && process.at("leader_reaped").is_null() &&
                process.at("termination").at("kind")=="windows_exit" && process.at("termination").at("windows_exit_code")==0 &&
                process.at("termination").at("posix_wait_status").is_null(),"Windows process JSON loses raw exit provenance.");
#else
            check(process.at("platform")=="posix" && process.at("leader_reaped")==true &&
                process.at("termination").at("kind")=="posix_exit" && process.at("termination").at("exit_status")==0 &&
                process.at("termination").at("signal").is_null() && process.at("termination").at("windows_exit_code").is_null(),"POSIX process JSON loses exit provenance.");
            for(const auto& cleanup:process.at("cleanup").at("requests"))check(cleanup.at("before").is_null() && cleanup.at("after").is_null(),"POSIX JSON fabricated group accounting.");
#endif
            auto compact=service.dispatch("development.inspect",{{"job_id",id},{"include_output",false},{"include_process",true}});
            check(compact.at("process").dump()==process.dump(),"Compact inspection changed retained process observations.");
            compact.erase("process");
            check(compact.dump()==service.dispatch("development.inspect",{{"job_id",id},{"include_output",false}}).dump(),"Opt-in compact projection changed legacy status.");
            enriched.erase("process");check(enriched.dump()==result.dump(),"Opt-in full projection changed legacy status.");
            for(const Json& malformed:Json::array({nullptr,0,1,0.0,1.0,"true",Json::array(),Json::object()}))
                error("development.inspect",{{"job_id",id},{"include_process",malformed}},-32602);
            error("development.diagnostics",{{"job_id",id},{"include_process",true}},-32602);
            error("development.cancel",{{"job_id",id},{"include_process",true}},-32602);
            check(service.dispatch("development.compile",params)==Json({{"job_id",id},{"replayed",true},{"receipt_scope","session"}}),"Process observation changed completed compile receipt.");
        }
        const auto echoed=Json::parse(result.at("stdout").get<std::string>());
        const Json expected={"build",utf8(project),"-c","Debug","--nologo","--disable-build-servers","--artifacts-path",utf8(root/"output space"/"artifacts"),"-o",utf8(root/"output space"/"managed")};
        check(echoed.at("arguments")==expected,"Argument vector changed or shell-expanded.");
        check(echoed.at("cwd")==utf8(root),"Compiler cwd differs from project parent.");
        check(!service.dispatch("development.jobs",Json::object()).at("jobs")[0].contains("stdout"),"Catalog unexpectedly returns diagnostics.");
        check(service.dispatch("development.forget",{{"job_id",first.at("job_id")}}).at("forgotten")==true,"Terminal forget failed.");
        error("development.inspect",{{"job_id",first.at("job_id")}},-32004);
        check(service.dispatch("development.compile",params).at("replayed")==true,"Forgotten job was compiled again.");
        auto failed=service.dispatch("development.compile",compile("2",project,root/"missing-tool"));check(wait(failed).at("state")=="launch_failed","Missing compiler status lost.");
        const auto missing_process=service.dispatch("development.inspect",{{"job_id",failed.at("job_id")},{"include_process",true}}).at("process");
        check(missing_process.is_object() && missing_process.at("launched")==false && missing_process.at("leader_pid").is_null() && missing_process.at("termination").is_null() &&
            missing_process.at("cleanup").at("requests").empty(),"Launch failure fabricated a process termination.");
        const auto no_diagnostics=service.dispatch("development.diagnostics",{{"job_id",failed.at("job_id")}});
        check(no_diagnostics.at("state")=="launch_failed" && no_diagnostics.at("diagnostics").empty(),"No recognized diagnostics changed launch failure status.");
        auto bytes=service.dispatch("development.compile",compile("3",root/"bytes.csproj",executable));auto bytesResult=wait(bytes);
        check(bytesResult.at("stdout")=="invalid:\xEF\xBF\xBD","Non-UTF8 diagnostic was not safely replaced.");
        auto slow=service.dispatch("development.compile",compile("4",root/"slow.csproj",executable));
        error("development.forget",{{"job_id",slow.at("job_id")}},-32090);
        check(service.dispatch("development.cancel",{{"job_id",slow.at("job_id")}}).at("requested")==true,"Cancellation not requested.");
        check(wait(slow).at("state")=="cancelled","Cancellation did not complete.");
        auto skipped=compile("7",project,executable);skipped["restore"]=false;
        const auto skipped_job=service.dispatch("development.compile",skipped);
        const auto skipped_result=wait(skipped_job);
        check(skipped_result.at("state")=="succeeded","Explicit no-restore command failed.");
        auto no_restore_expected=expected;no_restore_expected.push_back("--no-restore");
        const auto skipped_echo=Json::parse(skipped_result.at("stdout").get<std::string>());
        check(skipped_echo.at("arguments")==no_restore_expected && skipped_echo.at("cwd")==utf8(root),"No-restore changed other arguments or cwd.");
        check(service.dispatch("development.compile",skipped)==Json({{"job_id",skipped_job.at("job_id")},{"replayed",true},{"receipt_scope","session"}}),"No-restore retry created another job.");
        auto changed_restore=skipped;changed_restore["restore"]=true;error("development.compile",changed_restore,-32009);
        changed_restore.erase("restore");error("development.compile",changed_restore,-32009);
        auto diagnostic_request=compile("5",root/"diagnostics.csproj",executable);diagnostic_request["restore"]=false;
        auto diagnostic_job=service.dispatch("development.compile",diagnostic_request);
        check(wait(diagnostic_job).at("state")=="failed","Compiler error result was lost.");
        const auto diagnostics=service.dispatch("development.diagnostics",{{"job_id",diagnostic_job.at("job_id")}});
        check(diagnostics.at("diagnostics").size()==2 && diagnostics.at("recognized_lines")==3 && diagnostics.at("more")==false,"Compiler diagnostics were not deduplicated.");
        check(!diagnostics.contains("stdout") && !diagnostics.contains("stderr"),"Structured feedback leaked full logs.");
        const auto& first_error=diagnostics.at("diagnostics")[0];
        check(first_error.at("code")=="CS1002" && first_error.at("severity")=="error" && first_error.at("origin")=="Game (copy).cs"
            && first_error.at("project")=="Game.csproj" && first_error.at("location").at("line")==2
            && first_error.at("location").at("column")==3,"Compiler source location/code changed.");
        const auto limited=service.dispatch("development.diagnostics",{{"job_id",diagnostic_job.at("job_id")},{"limit",1}});
        check(limited.at("diagnostics").size()==1 && limited.at("more")==true,"Diagnostic limit silently discarded records.");
        for(const Json& limit:Json::array({0,129,"32",true}))
            error("development.diagnostics",{{"job_id",diagnostic_job.at("job_id")},{"limit",limit}},-32602);
        auto diagnostic_bytes=service.dispatch("development.compile",compile("6",root/"diagnostic-bytes.csproj",executable));wait(diagnostic_bytes);
        check(service.dispatch("development.diagnostics",{{"job_id",diagnostic_bytes.at("job_id")}}).at("diagnostics")[0].at("message")=="invalid:\xEF\xBF\xBD",
            "Structured diagnostic was not UTF-8 safe.");
        error("development.unknown",Json::object(),-32601);
        auto stress=[&](int n) { auto value=params;std::string id(32,'0');for(int i=31;n && i>=0;--i,n/=16)id[i]="0123456789abcdef"[n%16];value["request_id"]=id;return value; };
        int sequence=256;
        while(service.dispatch("development.jobs",Json::object()).at("jobs").size()<32)
            wait(service.dispatch("development.compile",stress(sequence++)));
        const auto retry=stress(4095);const auto budget=service.dispatch("development.jobs",Json::object()).at("request_budget_remaining");
        error("development.compile",retry,-32090);
        check(service.dispatch("development.jobs",Json::object()).at("request_budget_remaining")==budget,"Rejected submission consumed receipt budget.");
        auto catalog=service.dispatch("development.jobs",Json::object()).at("jobs");
        service.dispatch("development.forget",{{"job_id",catalog[0].at("job_id")}});
        check(wait(service.dispatch("development.compile",retry)).at("state")=="succeeded","Capacity failure poisoned retry ID.");
        catalog=service.dispatch("development.jobs",Json::object()).at("jobs");
        for(const auto& job:catalog) {wait(job);service.dispatch("development.forget",{{"job_id",job.at("job_id")}});}
        for(int i=0;i<129;++i) {auto accepted=service.dispatch("development.compile",stress(sequence++));wait(accepted);service.dispatch("development.forget",{{"job_id",accepted.at("job_id")}});}
        error("development.compile",params,-32009); // Tombstone protects an expired receipt.
        const auto schemas=Service::schemas();check(schemas.size()==11 && schemas.contains("development.diagnostics") && schemas.contains("project.inspect") && schemas.contains("project.gameplay.adopt"),"Operation discovery incomplete.");
        const auto& restore_schema=schemas.at("development.compile").at("properties").at("restore");
        check(restore_schema.at("type")=="boolean" && restore_schema.at("default")==true,"Restore discovery differs from admission default.");
        const auto& server_schema=schemas.at("development.compile").at("properties").at("compiler_server");
        check(server_schema.at("type")=="string" && !server_schema.contains("default"),"Server discovery implies a default or nonstring selector.");
        const auto& required=schemas.at("development.compile").at("required");
        check(std::find(required.begin(),required.end(),Json("restore"))==required.end(),"Restore became required.");
        check(std::find(required.begin(),required.end(),Json("compiler_server"))==required.end(),"Owned compiler selection became required.");
        const auto& process_schema=schemas.at("development.inspect").at("properties").at("include_process");
        check(process_schema.at("type")=="boolean" && process_schema.at("default")==false &&
            schemas.at("development.inspect").at("required")==Json::array({"job_id"}),"Process discovery changed opt-in admission.");
        std::cout<<Json({{"passed",true},{"checks",{"literal_argv_and_cwd","incremental_build_arguments","explicit_restore_argv","restore_boolean_admission","restore_retry_normalization","compiler_server_selector_admission","compiler_server_receipt_guard","invalid_parameters","exact_retry","changed_retry_rejected","compact_catalog","forget_does_not_recompile","launch_failure","invalid_utf8_diagnostics","cancel_and_terminal_forget","capacity_failure_retry_rollback","expired_receipt_tombstone","discovery"}},
            {"diagnostic_checks",{"structured_source_location","summary_deduplication","limited_results_explicit","invalid_limits","invalid_utf8_diagnostic","unrecognized_output_preserves_failure"}},
            {"project_checks",{"host_profile_callbacks","synchronous_no_jobs","native_request_forwarding","missing_artifact_replay_delegation","path_and_type_rejection","native_error_codes","disabled_adoption"}},
            {"scope","Development service and real self-child processes; project routing callbacks are fixture-only, no actual SDK or native artifact/world-host qualification."}}).dump(2)<<'\n';
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
