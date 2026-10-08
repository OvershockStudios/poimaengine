// SPDX-License-Identifier: Apache-2.0
#include "development_service.hpp"
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
int main(int argc,char** argv) {
    if(argc==5 && std::string(argv[1])=="--sdk") {
        try {
            Service service;const auto accepted=service.dispatch("development.compile",{{"request_id",std::string(31,'0')+"1"},
                {"executable",argv[2]},{"project",argv[3]},{"output",argv[4]},{"configuration","Release"}});
            const auto begin=std::chrono::steady_clock::now();Json result;
            do {
                result=service.dispatch("development.inspect",{{"job_id",accepted.at("job_id")}});
                if(result.at("terminal")==true) {std::cout<<result.dump(2)<<'\n';return result.at("state")=="succeeded" ? 0:1;}
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }while(std::chrono::steady_clock::now()-begin<std::chrono::minutes(11));
            throw std::runtime_error("SDK compile wait timed out.");
        }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
    }
    if(argc>1 && std::string(argv[1])=="build") {
        if(std::string(argv[2]).find("slow.csproj")!=std::string::npos)std::this_thread::sleep_for(std::chrono::seconds(5));
        if(std::string(argv[2]).find("bytes.csproj")!=std::string::npos) { std::cout<<"invalid:"<<static_cast<char>(0xff);return 0; }
        Json arguments=Json::array();for(int i=1;i<argc;++i)arguments.push_back(argv[i]);
        std::cout<<Json({{"arguments",arguments},{"cwd",utf8(fs::current_path())}}).dump()<<'\n';return 0;
    }
    try {
        const auto root=fs::temp_directory_path()/("poima-development-service-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
        struct Cleanup { fs::path path;~Cleanup(){std::error_code e;fs::remove_all(path,e);} } cleanup{root};
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
        const auto first=service.dispatch("development.compile",params);check(!first.at("replayed").get<bool>(),"Initial compile replayed.");
        check(service.dispatch("development.compile",params).at("job_id")==first.at("job_id"),"Retry created another job.");
        check(service.dispatch("development.jobs",Json::object()).at("jobs").size()==1,"Retry duplicated job.");
        invalid=params;invalid["configuration"]="Release";error("development.compile",invalid,-32009);
        const auto result=wait(first);check(result.at("state")=="succeeded","Generated compiler command failed.");
        const auto echoed=Json::parse(result.at("stdout").get<std::string>());
        const Json expected={"build",utf8(project),"-c","Debug","--nologo","--disable-build-servers","--artifacts-path",utf8(root/"output space"/"artifacts"),"-o",utf8(root/"output space"/"managed")};
        check(echoed.at("arguments")==expected,"Argument vector changed or shell-expanded.");
        check(echoed.at("cwd")==utf8(root),"Compiler cwd differs from project parent.");
        check(!service.dispatch("development.jobs",Json::object()).at("jobs")[0].contains("stdout"),"Catalog unexpectedly returns diagnostics.");
        check(service.dispatch("development.forget",{{"job_id",first.at("job_id")}}).at("forgotten")==true,"Terminal forget failed.");
        error("development.inspect",{{"job_id",first.at("job_id")}},-32004);
        check(service.dispatch("development.compile",params).at("replayed")==true,"Forgotten job was compiled again.");
        auto failed=service.dispatch("development.compile",compile("2",project,root/"missing-tool"));check(wait(failed).at("state")=="launch_failed","Missing compiler status lost.");
        auto bytes=service.dispatch("development.compile",compile("3",root/"bytes.csproj",executable));auto bytesResult=wait(bytes);
        check(bytesResult.at("stdout")=="invalid:\xEF\xBF\xBD","Non-UTF8 diagnostic was not safely replaced.");
        auto slow=service.dispatch("development.compile",compile("4",root/"slow.csproj",executable));
        error("development.forget",{{"job_id",slow.at("job_id")}},-32090);
        check(service.dispatch("development.cancel",{{"job_id",slow.at("job_id")}}).at("requested")==true,"Cancellation not requested.");
        check(wait(slow).at("state")=="cancelled","Cancellation did not complete.");
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
        const auto schemas=Service::schemas();check(schemas.size()==5,"Operation discovery incomplete.");
        std::cout<<Json({{"passed",true},{"checks",{"literal_argv_and_cwd","incremental_build_arguments","invalid_parameters","exact_retry","changed_retry_rejected","compact_catalog","forget_does_not_recompile","launch_failure","invalid_utf8_diagnostics","cancel_and_terminal_forget","capacity_failure_retry_rollback","expired_receipt_tombstone","discovery"}},
            {"scope","Development service and real self-child processes; no actual SDK or world-host qualification."}}).dump(2)<<'\n';
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
