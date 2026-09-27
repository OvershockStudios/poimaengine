// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"
#include "poima/world.hpp"
#include "poima/editor.hpp"
#include "poima/shared_session.hpp"
#include "poima/project.hpp"
#include "poima/game_launch.hpp"

#include <exception>
#include <algorithm>
#include <charconv>
#include <set>
#include <iostream>
#include <string_view>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
bool endpoint_name(std::string_view text) {
    return !text.empty() && text.size()<=64 && std::all_of(text.begin(),text.end(),[](char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-';
    });
}
poima::Reply run(int argc, char** argv) {
    if (argc == 1) return poima::help();
    const std::string_view command = argv[1];
    if(command=="project") {
        if(argc<4)return poima::usage_error("Use: project create <directory> --name <name> | inspect <project.json> | build <project.json> --output <new-directory> --runtime <installed-runtime>.");
        const std::string_view action=argv[2];
        if(action=="create" && argc==6 && std::string_view(argv[4])=="--name" && *argv[3] && *argv[5])return poima::create_project(argv[3],argv[5]);
        if(action=="inspect" && argc==4 && *argv[3])return poima::inspect_project(argv[3]);
        if(action=="build" && argc==8 && *argv[3]) {
            std::string output,runtime;for(int i=4;i<8;i+=2) {
                const std::string_view key=argv[i];if(!*argv[i+1])return poima::usage_error("Project build requires nonempty paths.");
                if(key=="--output" && output.empty())output=argv[i+1];else if(key=="--runtime" && runtime.empty())runtime=argv[i+1];else return poima::usage_error("Unknown or repeated project build argument.");
            }
            if(!output.empty() && !runtime.empty())return poima::build_project(argv[3],output,runtime);
        }
        return poima::usage_error("Invalid project command. Use: schema project");
    }
    if(command=="game") {
        if(argc<4 || !*argv[3])return poima::usage_error("Use: game inspect <game.json> | run <game.json> [options].");
        const std::string_view action=argv[2];if(action=="inspect" && argc==4)return poima::inspect_game(argv[3]);
        if(action!="run")return poima::usage_error("Invalid game command. Use: schema game");
        poima::GameLaunchOptions options;options.manifest=argv[3];std::set<std::string_view> seen;
        for(int i=4;i<argc;++i) {
            const std::string_view key=argv[i];
            if(!seen.insert(key).second || (key!="--gpu" && key!="--frames" && key!="--replay" && key!="--capture" && key!="--report" && key!="--width" && key!="--height" && key!="--samples"))return poima::usage_error("Unknown or repeated game run argument.");
            if(++i>=argc)return poima::usage_error("Missing game run argument value.");
            const std::string_view value=argv[i];
            if(key=="--replay" || key=="--capture" || key=="--report") {
                if(value.empty() || value.starts_with("--"))return poima::usage_error("Game file arguments need nonempty paths.");
                if(key=="--replay")options.replay=value;else if(key=="--capture")options.render.capture=value;else options.report=value;continue;
            }
            unsigned n=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),n);
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size())return poima::usage_error("Game numeric arguments require unsigned decimal integers.");
            if(key=="--gpu") { if(n>4095)return poima::usage_error("GPU must be 0..4095.");options.render.gpu=static_cast<int>(n); }
            else if(key=="--frames") { if(n<1 || n>36000)return poima::usage_error("Frames must be 1..36000; omit for interactive play.");options.max_frames=n; }
            else if(key=="--samples") { if(n!=1 && n!=4)return poima::usage_error("Samples must be 1 or 4.");options.render.samples=n; }
            else { if(n<128 || n>4096)return poima::usage_error("Game dimensions must be 128..4096.");if(key=="--width")options.render.width=n;else options.render.height=n; }
        }
        if(!options.replay.empty() && options.max_frames)return poima::usage_error("Replay and interactive frame limit cannot be combined.");
        return poima::run_game(options);
    }
    if(command=="serve") {
        if(argc!=5 || std::string_view(argv[2]).empty() || std::string_view(argv[2]).starts_with("--") || std::string_view(argv[3])!="--endpoint" || !endpoint_name(argv[4]))
            return poima::usage_error("Use: serve <world.json> --endpoint <name>; name is 1..64 ASCII letters/digits/_/-.");
        return {poima::run_shared_world(argv[2],argv[4]),{}};
    }
    if(command=="connect") {
        if((argc!=3 && argc!=5) || !endpoint_name(argv[2]))return poima::usage_error("Use: connect <endpoint> [--timeout-ms N].");
        unsigned timeout=30000;
        if(argc==5) {
            const std::string_view value=argv[4];const auto parsed=std::from_chars(value.data(),value.data()+value.size(),timeout);
            if(std::string_view(argv[3])!="--timeout-ms" || parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || timeout<100 || timeout>600000)
                return poima::usage_error("Connection timeout must be 100..600000 milliseconds.");
        }
        return {poima::run_connected_session(argv[2],timeout),{}};
    }
    if(command=="editor") {
        if(argc<3 || std::string_view(argv[2]).empty() || std::string_view(argv[2]).starts_with("--"))
            return poima::usage_error("Use: editor <world.json> [--endpoint name] [--layout path.ini | --no-layout] [--gpu N] [--samples 1|4] [--frames N] [--width N] [--height N] [--capture path.bmp] [--script path.json] [--report path.json]");
        poima::EditorOptions options;options.world=argv[2];options.render.width=1440;options.render.height=900;
        std::set<std::string_view> seen;
        for(int index=3;index<argc;++index) {
            const std::string_view argument=argv[index];
            if(!seen.insert(argument).second)return poima::usage_error("Repeated editor argument.");
            if(argument=="--no-layout") { options.no_layout=true;continue; }
            if(argument!="--samples" && argument!="--gpu" && argument!="--frames" && argument!="--width" && argument!="--height" && argument!="--capture" && argument!="--script" && argument!="--report" && argument!="--endpoint" && argument!="--layout")
                return poima::usage_error("Unknown editor argument. Use: schema editor");
            if(++index>=argc)return poima::usage_error("Missing editor argument value.");
            const std::string_view value=argv[index];
            if(argument=="--endpoint") {
                if(!endpoint_name(value))return poima::usage_error("Endpoint name must be 1..64 ASCII letters/digits/_/-.");
                options.endpoint=value;continue;
            }
            if(argument=="--capture" || argument=="--script" || argument=="--report" || argument=="--layout") {
                if(value.empty() || value.starts_with("--"))return poima::usage_error("Editor file arguments require a nonempty path.");
                if(argument=="--capture")options.render.capture=value;
                else if(argument=="--script")options.script=value;
                else if(argument=="--layout")options.layout=value;
                else options.report=value;
                continue;
            }
            unsigned number=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),number);
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size())return poima::usage_error("Editor numeric arguments require an unsigned decimal integer.");
            if(argument=="--gpu") { if(number>4095)return poima::usage_error("GPU index must be 0..4095.");options.render.gpu=static_cast<int>(number); }
            else if(argument=="--samples") { if(number!=1 && number!=4)return poima::usage_error("Editor samples must be 1 or 4.");options.render.samples=number; }
            else if(argument=="--frames") { if(number<1 || number>36000)return poima::usage_error("Editor frame limit must be 1..36000; omit for interactive use.");options.max_frames=number; }
            else { if(number<640 || number>4096)return poima::usage_error("Editor dimensions must be 640..4096.");if(argument=="--width")options.render.width=number;else options.render.height=number; }
        }
        if(options.no_layout && !options.layout.empty())return poima::usage_error("--layout and --no-layout cannot be combined.");
        return poima::run_editor(options);
    }
    if (command == "render-smoke") {
        poima::RenderOptions options;
        std::set<std::string_view> seen;
        for (int index = 2; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (!seen.insert(argument).second) return poima::usage_error("Repeated render-smoke argument.");
            if (argument == "--allow-software") { options.allow_software = true; continue; }
            if (argument != "--frames" && argument != "--width" && argument != "--height" &&
                argument != "--gpu" && argument != "--capture")
                return poima::usage_error("Unknown render-smoke argument. Use: schema render-smoke");
            if (++index >= argc) return poima::usage_error("Missing render-smoke argument value.");
            const std::string_view value = argv[index];
            if (argument == "--capture") {
                if (value.empty() || value.starts_with("--")) return poima::usage_error("Capture needs a nonempty BMP path.");
                options.capture = value;
                continue;
            }
            unsigned number = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                return poima::usage_error("Render numeric arguments require an unsigned decimal integer.");
            if (argument == "--frames") options.frames = number;
            else if (argument == "--width") options.width = number;
            else if (argument == "--height") options.height = number;
            else {
                if (number > 4095) return poima::usage_error("GPU index must be in [0, 4095].");
                options.gpu = static_cast<int>(number);
            }
        }
        return poima::render_smoke(options);
    }
    if (command == "doctor") {
        poima::DoctorOptions options;
        bool graphics_seen = false;
        bool require_seen = false;
        for (int index = 2; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--graphics" && !graphics_seen) {
                graphics_seen = options.graphics = true;
            } else if (argument == "--require-hardware" && !require_seen) {
                require_seen = options.require_hardware = true;
            } else {
                return poima::usage_error("Unknown or repeated doctor argument. Use: doctor [--graphics] [--require-hardware]");
            }
        }
        return poima::doctor(options);
    }
    if (command == "schema") {
        if (argc != 3) return poima::usage_error("Use: schema <command>");
        return poima::schema(argv[2]);
    }
    if (argc != 2) return poima::usage_error("This command does not accept additional arguments.");
    if (command == "help" || command == "--help") return poima::help();
    if (command == "version" || command == "--version") return poima::version();
    if (command == "capabilities") return poima::capabilities();
    return poima::usage_error("Unknown command. Use: poima help");
}
} // namespace

int main_utf8(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "world") return poima::run_world_session(argv[2]);
        const auto reply = run(argc, argv);
        if(!reply.json.empty())std::cout << reply.json << '\n';
        return reply.exit_code;
    } catch (const std::exception& error) {
        std::cerr << "Poima internal failure: " << error.what() << '\n';
        std::cout << "{\"protocol_version\":1,\"request_id\":null,\"command\":\"cli\",\"status\":\"error\","
            "\"result\":null,\"diagnostics\":[{\"code\":\"internal_failure\",\"message\":\"Inspect stderr.\"}]}\n";
        return 4;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> utf8;
    std::vector<char*> pointers;
    for (int i = 0; i < argc; ++i) {
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) return 4;
        std::string text(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, text.data(), bytes, nullptr, nullptr);
        text.pop_back(); utf8.push_back(std::move(text));
    }
    for (auto& text : utf8) pointers.push_back(text.data());
    return main_utf8(argc, pointers.data());
}
#else
int main(int argc, char** argv) { return main_utf8(argc, argv); }
#endif
