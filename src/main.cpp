// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"

#include <exception>
#include <charconv>
#include <set>
#include <iostream>
#include <string_view>

namespace {
poima::Reply run(int argc, char** argv) {
    if (argc == 1) return poima::help();
    const std::string_view command = argv[1];
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

int main(int argc, char** argv) {
    try {
        const auto reply = run(argc, argv);
        std::cout << reply.json << '\n';
        return reply.exit_code;
    } catch (const std::exception& error) {
        std::cerr << "Poima internal failure: " << error.what() << '\n';
        std::cout << "{\"protocol_version\":1,\"request_id\":null,\"command\":\"cli\",\"status\":\"error\","
            "\"result\":null,\"diagnostics\":[{\"code\":\"internal_failure\",\"message\":\"Inspect stderr.\"}]}\n";
        return 4;
    }
}
