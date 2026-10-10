// SPDX-License-Identifier: Apache-2.0
// Real self-child adapter contract. Fixture markers are not Native AOT artifacts.
#include "development_service.hpp"
#include "poima/development_profiles.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif
namespace fs=std::filesystem;
namespace dev=poima::development;
using Json=nlohmann::json;
using namespace std::chrono_literals;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::string utf8(const fs::path& path){const auto s=path.u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};}
fs::path path(const std::string& value){return fs::path(std::u8string(value.begin(),value.end()));}
std::string environment(const char* key){
#ifdef _WIN32
    const auto name=path(key).wstring();const auto count=GetEnvironmentVariableW(name.c_str(),nullptr,0);
    if(!count)return {};
    std::wstring value(count,L'\0');const auto copied=GetEnvironmentVariableW(name.c_str(),value.data(),count);
    check(copied>0 && copied<count,"Cannot read child environment.");
    const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(copied),nullptr,0,nullptr,nullptr);
    check(bytes>0,"Cannot encode child environment.");std::string result(bytes,'\0');
    check(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(copied),result.data(),bytes,nullptr,nullptr)==bytes,"Cannot encode child value.");return result;
#else
    const auto* value=std::getenv(key);return value?value:"";
#endif
}
Json read(const fs::path& file){std::ifstream stream(file,std::ios::binary);check(bool(stream),"Fixture marker missing.");Json value;stream>>value;return value;}
void directory_link(const fs::path& target,const fs::path& link,std::error_code& error){
#ifdef _WIN32
    // Developer Mode permits this flag without elevation; retry ordinary mode
    // for hosts where the flag is unsupported but the process has privilege.
    if(CreateSymbolicLinkW(link.c_str(),target.c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY|0x2) ||
       CreateSymbolicLinkW(link.c_str(),target.c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY)){error.clear();return;}
    error=std::error_code(static_cast<int>(GetLastError()),std::system_category());
#else
    fs::create_directory_symlink(target,link,error);
#endif
}
int child(int argc,char** argv){
    Json arguments=Json::array();for(int index=1;index<argc;++index)arguments.push_back(argv[index]);
    const bool exporting=argc>2 && std::string(argv[1])=="project" && std::string(argv[2])=="build";
    const bool compiling=argc>2 && std::string(argv[1])=="build";
    const auto option=[&](const std::string& key){for(int index=1;index+1<argc;++index)if(argv[index]==key)return std::string(argv[index+1]);return std::string{};};
    const auto selected=exporting ? (argc>3?std::string(argv[3]):""):compiling?std::string(argv[2]):option("--project");
    const auto output_name=option(compiling?"-o":"--output");
    check(!selected.empty() && !output_name.empty(),"Child received incomplete generated arguments.");
    std::cout<<"profile-child-ready\n"<<std::flush;
    if(path(selected).filename()=="slow.csproj")std::this_thread::sleep_for(60s);
    if(path(selected).filename()=="nonzero.csproj"){std::cerr<<"fixture deliberate compiler failure\n";return 7;}
    const auto output=path(output_name);fs::create_directories(output);
    Json marker{{"fixture","self-child-only"},{"kind",exporting?"export":compiling?"compile":"publish"},{"arguments",arguments},
        {"cwd",utf8(fs::current_path())},{"environment",environment("POIMA_PROFILE_VALUE")}};
    std::ofstream stream(output/"fixture.json",std::ios::binary);check(bool(stream),"Cannot create fixture marker.");stream<<marker.dump()<<'\n';stream.close();check(bool(stream),"Cannot finish fixture marker.");
    std::cout<<marker.dump()<<'\n';return 0;
}
std::string id(unsigned value){std::string result(32,'0');for(int index=31;value && index>=0;--index,value/=16)result[index]="0123456789abcdef"[value%16];return result;}
}
int contract_main(int argc,char** argv){
#ifdef _WIN32
    _setmode(_fileno(stdout),_O_BINARY);_setmode(_fileno(stderr),_O_BINARY);
#endif
    try{
        if(argc>1)return child(argc,argv);
        const auto executable=fs::canonical(fs::absolute(path(argv[0])));
        const auto root=fs::canonical(fs::temp_directory_path())/path("poima-profile-\xE2\x98\x83-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root/"projects");fs::create_directories(root/"outputs");fs::create_directories(root/"tooling");fs::create_directories(root/"runtime");
        struct Cleanup{fs::path root;~Cleanup(){std::error_code error;fs::remove_all(root,error);}}cleanup{root};
        const std::string literal="literal $(no-shell); & \"wine\" \xE2\x98\x83";
        const std::string project_name="Game $(literal) 'wine' \xE2\x98\x83.csproj";
        for(const auto& name:{project_name,std::string("slow.csproj"),std::string("nonzero.csproj"),std::string("bad-marker.csproj"),std::string("project.json")})std::ofstream(root/"projects"/path(name))<<"fixture\n";
        std::ofstream(root/"outside.csproj")<<"fixture\n";std::ofstream(root/"tooling/publisher.py")<<"self-child fixture\n";
        auto publish_calls=std::make_shared<std::atomic<int>>(0),export_calls=std::make_shared<std::atomic<int>>(0);
        dev::Profile profile;profile.name="fixture";profile.fingerprint=std::string(64,'a');
#ifdef _WIN32
        profile.target_rid="win-x64";
#else
        profile.target_rid="linux-x64";
#endif
        profile.project_root=root/"projects";profile.output_root=root/"outputs";profile.python=executable;profile.publisher=root/"tooling/publisher.py";
        profile.dotnet=executable;profile.exporter=executable;profile.runtime_root=root/"runtime";
        profile.environment={{"POIMA_PROFILE_VALUE",literal}};
#ifdef _WIN32
        const auto system_root=environment("SystemRoot");if(!system_root.empty())profile.environment.emplace_back("SystemRoot",system_root);
#endif
        profile.verify_publish=[publish_calls](const fs::path& output,const std::string& type){
            ++*publish_calls;const auto marker=read(output/"fixture.json");check(marker.at("fixture")=="self-child-only" && marker.at("kind")=="publish" && type=="Fixture.Game","Invalid publish fixture marker.");
            return Json{{"verified_fixture","publish"}}.dump();};
        profile.verify_export=[export_calls](const fs::path& output){++*export_calls;const auto marker=read(output/"fixture.json");check(marker.at("fixture")=="self-child-only" && marker.at("kind")=="export","Invalid export fixture marker.");return Json{{"verified_fixture","export"}}.dump();};
        dev::Service service;unsigned sequence=1;
        const auto params=[&](bool exporting,const std::string& output){Json value{{"request_id",id(sequence++)},{"profile","fixture"},{"project",exporting?"project.json":project_name},{"output",output},{"timeout_ms",5000}};if(!exporting)value["type"]="Fixture.Game";return value;};
        const auto reject=[&](const std::string& method,const Json& value,int expected_code=0){
            const auto before=service.dispatch("development.jobs",Json::object());bool rejected=false;
            try{service.dispatch(method,value);}catch(const dev::ServiceError& error){if(expected_code)check(error.code==expected_code,"Wrong profile rejection code.");rejected=true;}
            check(rejected,"Expected profile request rejection.");const auto after=service.dispatch("development.jobs",Json::object());
            check(after.at("request_budget_remaining")==before.at("request_budget_remaining") && after.at("jobs").size()==before.at("jobs").size(),"Rejected request retained a job or consumed receipt budget.");
            for(std::size_t index=0;index<before.at("jobs").size();++index)check(before.at("jobs")[index].at("job_id")==after.at("jobs")[index].at("job_id"),"Rejected request replaced a retained job.");};
        const auto wait=[&](const Json& accepted){const auto deadline=std::chrono::steady_clock::now()+10s;for(;;){const auto status=service.dispatch("development.inspect",{{"job_id",accepted.at("job_id")}});if(status.at("terminal")==true)return status;check(std::chrono::steady_clock::now()<deadline,"Profile job exceeded qualification timeout.");std::this_thread::sleep_for(5ms);}};
        reject("development.publish",params(false,"no-profile"));reject("development.export",params(true,"no-profile-export"));
        check(service.dispatch("development.profiles",Json::object()).at("profiles").empty(),"Unconfigured service exposed profiles.");
        const auto bad_configuration=[&](dev::Profile candidate){dev::Service unconfigured;bool rejected=false;try{unconfigured.configure({std::move(candidate)});}catch(const std::exception&){rejected=true;}check(rejected,"Invalid profile configuration accepted.");};
        auto bad=profile;bad.project_root="relative";bad_configuration(bad);
        bad=profile;bad.output_root=profile.project_root;bad_configuration(bad);
        fs::create_directory(profile.project_root/"nested-output");bad=profile;bad.output_root=profile.project_root/"nested-output";bad_configuration(bad);
        bad=profile;bad.fingerprint="not-a-pin";bad_configuration(bad);bad=profile;bad.name="invalid/name";bad_configuration(bad);
        bad=profile;bad.python=root/"missing-python";bad_configuration(bad);
        bad=profile;bad.verify_publish={};bad.verify_export={};bad_configuration(bad);
        bad=profile;bad.environment={{"DUP","one"},{"DUP","two"}};bad_configuration(bad);
        bad=profile;bad.environment={{"BAD=KEY","value"}};bad_configuration(bad);
        bad=profile;bad.environment={{"KEY",std::string("bad\0value",9)}};bad_configuration(bad);
        service.configure({profile});const auto catalog=service.dispatch("development.profiles",Json::object());
        check(catalog==Json{{"profiles",Json::array({{{"name","fixture"},{"fingerprint",std::string(64,'a')},{"target_rid",profile.target_rid},{"publish",true},{"export",true}}})}},"Profile catalog differs or leaks host details.");
        check(catalog.dump().find(literal)==std::string::npos && catalog.dump().find(utf8(root))==std::string::npos,"Profile discovery leaked private paths/environment.");
        bool immutable=false;try{service.configure({profile});}catch(const std::exception&){immutable=true;}check(immutable,"Configured service was mutable.");
        {
            dev::Service publish_only;auto limited=profile;limited.verify_export={};limited.exporter.clear();limited.runtime_root.clear();publish_only.configure({limited});
            const auto exposed=publish_only.dispatch("development.profiles",Json::object()).at("profiles");
            check(exposed.size()==1 && exposed[0].at("publish")==true && exposed[0].at("export")==false,"Disabled export was advertised.");
            bool denied=false;try{publish_only.dispatch("development.export",params(true,"disabled-export"));}catch(const dev::ServiceError& error){check(error.code==-32004,"Wrong disabled-operation error.");denied=true;}
            check(denied && publish_only.dispatch("development.jobs",Json::object()).at("jobs").empty(),"Publish-only profile granted export authority.");
        }
        dev::Service after_compile;const auto legacy=after_compile.dispatch("development.compile",{{"request_id",id(sequence++)},{"executable",utf8(executable)},{"project",utf8(profile.project_root/path(project_name))},{"output",utf8(root/"legacy-output")}});
        immutable=false;try{after_compile.configure({profile});}catch(const std::exception&){immutable=true;}check(immutable,"Profiles configured after an accepted legacy compile.");
        const auto legacy_deadline=std::chrono::steady_clock::now()+10s;
        for(;;){const auto done=after_compile.dispatch("development.inspect",{{"job_id",legacy.at("job_id")}});if(done.at("terminal")==true){check(done.at("state")=="succeeded","Legacy compile fixture failed.");break;}check(std::chrono::steady_clock::now()<legacy_deadline,"Legacy compile fixture timed out.");std::this_thread::sleep_for(5ms);}
        for(const auto& project:std::vector<std::string>{"../outside.csproj",utf8(root/"outside.csproj"),"sub/../"+project_name,"..\\outside.csproj",std::string("x\0.csproj",9)}){auto invalid=params(false,"bad-path");invalid["project"]=project;reject("development.publish",invalid);}
        for(const auto& output:std::vector<std::string>{"../outside",utf8(root/"outside-output"),"folder/../child","child\\nested"}){auto invalid=params(false,"unused");invalid["output"]=output;reject("development.publish",invalid);}
        for(const auto& extra:{"executable","environment","arguments"}){auto invalid=params(false,"extra-field");invalid[extra]="caller-override";reject("development.publish",invalid);}
        for(const auto& timeout:Json::array({99,900001,true,"1000"})){auto invalid=params(false,"bad-timeout");invalid["timeout_ms"]=timeout;reject("development.publish",invalid);}
        auto malformed=params(false,"bad-type");malformed["type"]=true;reject("development.publish",malformed);
        malformed=params(true,"extra-type");malformed["type"]="Fixture.Game";reject("development.export",malformed);
        malformed=params(false,"unknown-profile");malformed["profile"]="missing";reject("development.publish",malformed);
        malformed=params(false,"wrong-extension");malformed["project"]="project.json";reject("development.publish",malformed);
        fs::create_directory(profile.output_root/"preexisting");malformed=params(false,"preexisting");reject("development.publish",malformed,-32009);
        unsigned skipped_links=0;std::error_code link_error;
        directory_link(profile.project_root,root/"linked-projects",link_error);
        if(!link_error){bad=profile;bad.project_root=root/"linked-projects";bad_configuration(bad);}else ++skipped_links;
        link_error.clear();directory_link(root,profile.project_root/"escape",link_error);
        if(!link_error){malformed=params(false,"symlink-project");malformed["project"]="escape/outside.csproj";reject("development.publish",malformed);}else ++skipped_links;
        link_error.clear();directory_link(root,profile.output_root/"escape",link_error);
        if(!link_error){malformed=params(false,"escape/child");reject("development.publish",malformed);}else ++skipped_links;
        const auto publish_params=params(false,"publish & ; \xE2\x98\x83"),published=service.dispatch("development.publish",publish_params);
        const auto published_status=wait(published);check(published_status.at("state")=="succeeded" && published_status.at("exit_code")==0 && published_status.at("result")==Json{{"verified_fixture","publish"}},"Publish result was not verified.");
        const auto marker=read(profile.output_root/path(publish_params.at("output").get<std::string>())/"fixture.json");
        check(marker.at("environment")==literal,"Profile environment was not literal.");
        const Json expected={utf8(profile.publisher),"--project",utf8(profile.project_root/path(project_name)),"--type","Fixture.Game","--output",utf8(profile.output_root/path(publish_params.at("output").get<std::string>())),"--dotnet",utf8(profile.dotnet),"--rid",profile.target_rid};
        check(marker.at("arguments")==expected,"Publish arguments changed or expanded.");
        check(marker.at("cwd")==utf8(profile.project_root),"Publish working directory escaped project root.");
        const auto retried=service.dispatch("development.publish",publish_params);check(retried.at("job_id")==published.at("job_id") && retried.at("replayed")==true && *publish_calls==1,"Completed retry launched or verified again.");
        malformed=publish_params;malformed.erase("type");malformed["project"]="project.json";reject("development.export",malformed,-32009);
        malformed=params(false,publish_params.at("output").get<std::string>());reject("development.publish",malformed);
        const auto export_params=params(true,"exported"),exported=service.dispatch("development.export",export_params);
        const auto export_status=wait(exported);check(export_status.at("state")=="succeeded" && export_status.at("result")==Json{{"verified_fixture","export"}} && *export_calls==1,"Export was not verified.");
        check(service.dispatch("development.export",export_params).at("replayed")==true && *export_calls==1,"Export retry verified twice.");
        const auto exported_marker=read(profile.output_root/"exported/fixture.json");check(exported_marker.at("arguments")==Json{"project","build",utf8(profile.project_root/"project.json"),"--output",utf8(profile.output_root/"exported"),"--runtime",utf8(profile.runtime_root)},"Export arguments differ.");
        auto failed_params=params(false,"nonzero");failed_params["project"]="nonzero.csproj";const auto before_failure=publish_calls->load();
        const auto failed=wait(service.dispatch("development.publish",failed_params));check(failed.at("state")=="failed" && failed.at("exit_code")==7 && failed.at("result").is_null() && *publish_calls==before_failure,"Nonzero child entered verifier or published result.");
        const auto slow_params=[&](){auto value=params(false,"reserved");value["project"]="slow.csproj";return value;}();
        const auto slow=service.dispatch("development.publish",slow_params);
        auto competing=params(false,"reserved");reject("development.publish",competing);competing=params(true,"reserved/inside");reject("development.export",competing);
#ifdef _WIN32
        competing=params(false,"ReSeRvEd");reject("development.publish",competing,-32009);
#endif
        const auto ready_deadline=std::chrono::steady_clock::now()+5s;
        for(;;){const auto running=service.dispatch("development.inspect",{{"job_id",slow.at("job_id")}});if(running.at("stdout").get<std::string>().find("profile-child-ready")!=std::string::npos)break;check(running.at("terminal")==false && std::chrono::steady_clock::now()<ready_deadline,"Slow profile child never became ready.");std::this_thread::sleep_for(5ms);}
        check(service.dispatch("development.cancel",{{"job_id",slow.at("job_id")}}).at("requested")==true,"Profile cancellation rejected.");
        const auto cancelled=wait(slow);check(cancelled.at("state")=="cancelled" && cancelled.at("result").is_null() && *publish_calls==before_failure,"Cancelled job published or verified.");
        dev::Service verifier_failure;auto rejecting_profile=profile;rejecting_profile.verify_publish=[publish_calls](const fs::path&,const std::string&)->std::string{++*publish_calls;throw std::runtime_error("fixture verifier rejects marker");};
        verifier_failure.configure({rejecting_profile});auto bad_verify=params(false,"bad-verify");const auto accepted=verifier_failure.dispatch("development.publish",bad_verify);
        Json rejected_result;const auto deadline=std::chrono::steady_clock::now()+10s;do{rejected_result=verifier_failure.dispatch("development.inspect",{{"job_id",accepted.at("job_id")}});if(rejected_result.at("terminal")==true)break;check(std::chrono::steady_clock::now()<deadline,"Verifier-failure fixture timed out.");std::this_thread::sleep_for(5ms);}while(true);
        check(rejected_result.at("state")=="failed" && rejected_result.at("exit_code")==0 && rejected_result.at("result").is_null() && rejected_result.at("error").get<std::string>().find("fixture verifier rejects marker")!=std::string::npos,"Zero exit bypassed verifier failure.");
        std::vector<std::string> invalid_results={"", "not-json", "[]", "null", "{}", "{\"duplicate\":1,\"duplicate\":2}",std::string(65537,'x')};
        std::string nested="1";for(int index=0;index<20;++index)nested="{\"child\":"+nested+"}";invalid_results.push_back(nested);
        for(std::size_t index=0;index<invalid_results.size();++index){
            dev::Service invalid_verifier;auto invalid_profile=profile;
            invalid_profile.verify_publish=[payload=invalid_results[index]](const fs::path& output,const std::string& type){check(read(output/"fixture.json").at("kind")=="publish" && type=="Fixture.Game","Invalid-result test did not execute its real child.");return payload;};
            invalid_verifier.configure({invalid_profile});const auto invalid_job=invalid_verifier.dispatch("development.publish",params(false,"invalid-result-"+std::to_string(index)));
            const auto result_deadline=std::chrono::steady_clock::now()+10s;Json outcome;
            for(;;){outcome=invalid_verifier.dispatch("development.inspect",{{"job_id",invalid_job.at("job_id")}});if(outcome.at("terminal")==true)break;check(std::chrono::steady_clock::now()<result_deadline,"Invalid-result fixture timed out.");std::this_thread::sleep_for(5ms);}
            check(outcome.at("state")=="failed" && outcome.at("exit_code")==0 && outcome.at("result").is_null() && outcome.at("error").get<std::string>().find("Development verifier")!=std::string::npos,"Invalid verifier JSON published success.");
        }
        std::cout<<Json{{"passed",true},{"scope","Real self-child profile adapter, literal argv/environment and fixture-only verification; no SDK/AOT/exported-game qualification"},{"skipped_symlink_checks",skipped_links},{"publish_verifier_calls",publish_calls->load()},{"export_verifier_calls",export_calls->load()}}.dump()<<'\n';return 0;
    }catch(const std::exception& failure){std::cerr<<failure.what()<<'\n';return 1;}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){std::vector<std::string> storage;for(int index=0;index<argc;++index){const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[index],-1,nullptr,0,nullptr,nullptr);if(bytes<=0)return 1;std::string value(bytes,'\0');if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[index],-1,value.data(),bytes,nullptr,nullptr)!=bytes)return 1;value.pop_back();storage.push_back(std::move(value));}std::vector<char*> arguments;for(auto& value:storage)arguments.push_back(value.data());return contract_main(argc,arguments.data());}
#else
int main(int argc,char** argv){return contract_main(argc,argv);}
#endif
