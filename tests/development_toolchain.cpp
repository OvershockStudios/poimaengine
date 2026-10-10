// SPDX-License-Identifier: Apache-2.0
// Trusted fixture adapter for actual toolchain qualification, not a public CLI.
#include "poima/development_jobs.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace dev=poima::development;
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
namespace {
void require(bool value,const char* message) {
    if(!value)throw std::invalid_argument(message);
}
std::filesystem::path utf8_path(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(),value.end()));
}
std::string text(const Json& value) {
    require(value.is_string(),"Request string has the wrong type.");
    auto result=value.get<std::string>();
    require(result.find('\0')==std::string::npos,"Request string contains NUL.");
    return result;
}
dev::Request request(const std::filesystem::path& path) {
    require(path.is_absolute(),"Trusted request file must be absolute.");
    std::ifstream input(path,std::ios::binary);
    require(bool(input),"Cannot read trusted request file.");
    std::string payload(1048577,'\0');
    input.read(payload.data(),static_cast<std::streamsize>(payload.size()));
    const auto count=input.gcount();
    require(!input.bad() && count>0 && count<=1048576,"Request file is empty, unreadable or exceeds 1 MiB.");
    payload.resize(static_cast<std::size_t>(count));
    std::vector<std::set<std::string>> keys;
    const auto document=Json::parse(payload,[&](int depth,Json::parse_event_t event,Json& item) {
        require(depth<=8,"Request JSON nesting exceeds eight levels.");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key)
            require(!keys.empty() && keys.back().insert(item.get<std::string>()).second,"Duplicate request JSON key.");
        if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    });
    const std::set<std::string> expected{"executable","working_directory","arguments","environment","timeout_ms"};
    require(document.is_object() && document.size()==expected.size(),"Request object must have exactly five fields.");
    for(const auto& [key,value]:document.items()) {
        (void)value;require(expected.contains(key),"Unknown request field.");
    }
    dev::Request result;
    result.executable=utf8_path(text(document.at("executable")));
    result.working_directory=utf8_path(text(document.at("working_directory")));
    require(result.executable.is_absolute() && result.working_directory.is_absolute(),"Request paths must be absolute.");
    const auto& arguments=document.at("arguments");
    require(arguments.is_array() && arguments.size()<=128,"Request arguments must be an array of at most 128 strings.");
    for(const auto& value:arguments)result.arguments.push_back(text(value));
    const auto& environment=document.at("environment");
    require(environment.is_array() && environment.size()<=256,"Explicit replacement environment must be an array of at most 256 pairs.");
    result.environment.emplace();
    for(const auto& entry:environment) {
        require(entry.is_array() && entry.size()==2,"Environment entries must be exact [key,value] pairs.");
        result.environment->emplace_back(text(entry[0]),text(entry[1]));
    }
    const auto& duration=document.at("timeout_ms");
    require(duration.is_number_integer(),"timeout_ms must be an integer.");
    require(duration>=Json(1) && duration<=Json(86400000),"timeout_ms must be between 1 ms and 24 hours.");
    const auto milliseconds=duration.get<std::uint64_t>();
    result.timeout=std::chrono::milliseconds(milliseconds);
    return result;
}
#ifdef _WIN32
using EnvironmentValue=std::optional<std::wstring>;
EnvironmentValue sentinel(const wchar_t* key) {
    SetLastError(ERROR_SUCCESS);
    const DWORD size=GetEnvironmentVariableW(key,nullptr,0);
    if(!size) {
        const DWORD error=GetLastError();
        if(error==ERROR_ENVVAR_NOT_FOUND)return std::nullopt;
        require(error==ERROR_SUCCESS,"Cannot inspect parent environment sentinel.");
        return std::wstring{};
    }
    std::wstring value(size,L'\0');
    SetLastError(ERROR_SUCCESS);
    const DWORD copied=GetEnvironmentVariableW(key,value.data(),size);
    require(copied<size && (copied>0 || GetLastError()==ERROR_SUCCESS),"Parent environment changed while reading sentinel.");
    value.resize(copied);return value;
}
std::array<EnvironmentValue,2> sentinels() {
    return {sentinel(L"POIMA_TOOLCHAIN_SENTINEL_A"),sentinel(L"POIMA_TOOLCHAIN_SENTINEL_B")};
}
#else
using EnvironmentValue=std::optional<std::string>;
std::array<EnvironmentValue,2> sentinels() {
    const auto read=[](const char* key)->EnvironmentValue {
        const char* value=std::getenv(key);
        return value?EnvironmentValue(std::string(value)):std::nullopt;
    };
    return {read("POIMA_TOOLCHAIN_SENTINEL_A"),read("POIMA_TOOLCHAIN_SENTINEL_B")};
}
#endif
}

int qualification_main(int argc,char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout),_O_BINARY);
#endif
    dev::Status status;status.state=dev::State::launch_failed;
    std::array<EnvironmentValue,2> before;
    bool captured_environment=false,unchanged=false;
    std::string error;
    try {
        before=sentinels();captured_environment=true;
        require(before[0].has_value() && before[1].has_value(),"Both parent qualification sentinels must be supplied.");
        require(argc==2,"Supply exactly one trusted absolute JSON request file.");
        auto launch=request(utf8_path(argv[1]));
        const auto deadline=Clock::now()+launch.timeout+std::chrono::seconds(15);
        {
            // The actual development worker owns its child and descendants.
            // Destructor cancellation/join completes before terminal JSON output.
            dev::Jobs jobs({1,1048576});
            const auto id=jobs.submit(std::move(launch));
            for(;;) {
                const auto polled=jobs.poll(id);
                require(polled.has_value(),"Submitted toolchain job disappeared.");
                status=*polled;
                if(dev::terminal(status.state))break;
                if(Clock::now()>=deadline) {
                    jobs.cancel(id);
                    error="Toolchain qualification polling exceeded timeout plus 15 seconds.";
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if(!dev::terminal(status.state))status.state=dev::State::failed;
    }catch(const std::exception& failure) {
        error=failure.what();
    }catch(...) {
        error="Unknown toolchain qualification failure.";
    }
    if(!dev::terminal(status.state))status.state=dev::State::failed;
    try {
        unchanged=captured_environment && before[0].has_value() && before[1].has_value() && before==sentinels();
        if(!unchanged) {
            if(!error.empty())error+=' ';
            error+="Parent qualification environment changed.";
        }
    }catch(const std::exception& failure) {
        unchanged=false;
        if(!error.empty())error+=' ';
        error+=failure.what();
    }
    if(!status.error.empty()) {
        if(!error.empty())error+=' ';
        error+=status.error;
    }
    const bool success=error.empty() && unchanged && status.state==dev::State::succeeded && status.exit_code==0;
    Json report{{"state",dev::state_name(status.state)},{"exit_code",status.exit_code?Json(*status.exit_code):Json(nullptr)},
        {"elapsed_ms",status.elapsed.count()},{"stdout",status.standard_output},{"stderr",status.standard_error},
        {"stdout_bytes",status.output_bytes},{"stderr_bytes",status.error_bytes},
        {"stdout_truncated",status.output_truncated},{"stderr_truncated",status.error_truncated},
        {"error",error},{"unchanged_environment",unchanged}};
    std::cout<<report.dump(-1,' ',false,Json::error_handler_t::replace)<<'\n';
    return success?0:2;
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> storage;
    for(int index=0;index<argc;++index) {
        const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[index],-1,nullptr,0,nullptr,nullptr);
        if(size<=0) {
            const Json failure{{"state","launch_failed"},{"exit_code",nullptr},{"elapsed_ms",0},
                {"stdout",""},{"stderr",""},{"stdout_bytes",0},{"stderr_bytes",0},
                {"stdout_truncated",false},{"stderr_truncated",false},
                {"error","Windows request-file argument is not valid Unicode."},{"unchanged_environment",false}};
            std::cout<<failure.dump()<<'\n';return 2;
        }
        std::string value(static_cast<std::size_t>(size),'\0');
        WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[index],-1,value.data(),size,nullptr,nullptr);
        value.pop_back();storage.push_back(std::move(value));
    }
    std::vector<char*> arguments;
    for(auto& value:storage)arguments.push_back(value.data());
    return qualification_main(argc,arguments.data());
}
#else
int main(int argc,char** argv) { return qualification_main(argc,argv); }
#endif
