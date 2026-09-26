// SPDX-License-Identifier: Apache-2.0
#include "poima/core.hpp"
#include "poima/build_info.hpp"

#include <algorithm>
#include <array>

namespace poima {
namespace {
std::string quote(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch == '"' || ch == '\\') {
            result += '\\';
            result += static_cast<char>(ch);
        } else if (ch < 0x20) {
            result += "\\u00";
            result += hex[ch >> 4];
            result += hex[ch & 0xf];
        } else {
            result += static_cast<char>(ch);
        }
    }
    return result + '"';
}

std::string boolean(bool value) { return value ? "true" : "false"; }

Reply envelope(std::string_view command, std::string result, int exit_code = 0,
               std::string_view code = {}, std::string_view detail = {}) {
    const auto diagnostics = code.empty() ? "[]" :
        "[{\"code\":" + quote(code) + ",\"message\":" + quote(detail) + "}]";
    return {exit_code, "{\"protocol_version\":1,\"request_id\":null,\"command\":" + quote(command) +
        ",\"status\":" + quote(exit_code == 0 ? "ok" : "error") + ",\"result\":" + result +
        ",\"diagnostics\":" + diagnostics + "}"};
}

std::string build_info() {
    return "{\"version\":" + quote(POIMA_VERSION) + ",\"compiler\":" + quote(POIMA_COMPILER_ID) +
        ",\"compiler_version\":" + quote(POIMA_COMPILER_VERSION) +
        ",\"target_os\":" + quote(POIMA_BUILD_SYSTEM) + ",\"target_arch\":" + quote(POIMA_BUILD_ARCH) + "}";
}

struct Operation {
    std::string_view name;
    std::string_view summary;
    std::string_view arguments;
    std::string_view properties;
    std::string_view required;
};
constexpr std::array operations{
    Operation{"help", "List implemented commands and their CLI arguments.", "",
        "{}", "[]"},
    Operation{"version", "Inspect this compiled executable's version and toolchain.", "",
        "{}", "[]"},
    Operation{"capabilities", "Discover implemented features without loading a graphics driver.", "",
        "{}", "[]"},
    Operation{"doctor", "Inspect this host, optionally enumerating Vulkan devices.", "[--graphics] [--require-hardware]",
        R"({"graphics":{"type":"boolean","default":false},"require_hardware":{"type":"boolean","default":false,"description":"Implies graphics. Require an enumerated discrete/integrated Vulkan 1.3 device with a graphics queue; this is not renderer qualification."}})", "[]"},
    Operation{"world", "Open a persistent authored-world session over newline-delimited JSON-RPC 2.0.", "<path.json>",
        R"({"path":{"type":"string","minLength":1,"description":"World file; parent directory must exist. Reserves .lock/.pending/.previous sidecars. Use world.describe inside the session for method schemas."}})", "[\"path\"]"},
    Operation{"schema", "Discover an implemented command's request schema.", "<command>",
        R"({"command":{"type":"string","enum":["help","version","capabilities","doctor","schema","render-smoke","world"]}})", "[\"command\"]"},
    Operation{"render-smoke", "Present a bounded Vulkan triangle test; optionally save the final GPU frame as BMP.",
        "[--frames N] [--width N] [--height N] [--gpu N] [--allow-software] [--capture path.bmp]",
        R"({"frames":{"type":"integer","minimum":1,"maximum":10000,"default":120},"width":{"type":"integer","minimum":128,"maximum":4096,"default":960},"height":{"type":"integer","minimum":128,"maximum":4096,"default":540},"gpu":{"type":"integer","minimum":0,"maximum":4095,"description":"Vulkan device enumeration index; omission prefers a discrete GPU."},"allow_software":{"type":"boolean","default":false},"capture":{"type":"string","minLength":1,"description":"Optional BMP output path; overwrites an existing file."}})", "[]"}
};

std::string operation_list() {
    std::string json = "[";
    for (const auto& operation : operations) {
        if (json.size() > 1) json += ',';
        json += "{\"name\":" + quote(operation.name) + ",\"summary\":" + quote(operation.summary) +
            ",\"arguments\":" + quote(operation.arguments) + ",\"mutates_project\":" + boolean(operation.name == "world") + "}";
    }
    return json + ']';
}

std::string graphics_json(const GraphicsProbe& probe) {
    std::string devices = "[";
    for (const auto& device : probe.devices) {
        if (devices.size() > 1) devices += ',';
        devices += "{\"name\":" + quote(device.name) + ",\"type\":" + quote(device.type) +
            ",\"api_version\":" + quote(device.api_version) +
            ",\"vendor_id\":" + std::to_string(device.vendor_id) +
            ",\"device_id\":" + std::to_string(device.device_id) +
            ",\"hardware\":" + boolean(device.hardware) +
            ",\"api_at_least_1_3\":" + boolean(device.api_at_least_1_3) +
            ",\"graphics_queue\":" + boolean(device.graphics_queue) + '}';
    }
    return "{\"status\":" + quote(probe.status) + ",\"loader_api_version\":" + quote(probe.loader_api_version) +
        ",\"detail\":" + quote(probe.detail) + ",\"devices\":" + devices + "]}";
}
} // namespace

Reply capabilities() {
    return envelope("capabilities", "{\"build\":" + build_info() + ",\"commands\":" + operation_list() +
        ",\"features\":{\"native_cli\":true,\"host_inspection\":true,\"vulkan_device_inspection\":" +
        boolean(POIMA_VULKAN_PROBE != 0) +
        ",\"render_smoke\":" + boolean(POIMA_RENDER_SMOKE != 0) +
        ",\"simulation\":" + boolean(POIMA_SIMULATION != 0) +
        ",\"player_viewport\":" + boolean(POIMA_SIMULATION != 0 && POIMA_RENDER_SMOKE != 0) +
        ",\"scene_capture\":" + boolean(POIMA_RENDER_SMOKE != 0) +
        ",\"renderer\":false,\"scene_editing\":true,\"animation\":false,\"vfx\":false,"
        "\"hot_reload\":false,\"mcp\":false,\"editor\":false},\"qualification\":\"bootstrap_with_authored_world\"}");
}

Reply doctor(DoctorOptions options) {
    const auto host = inspect_host();
    const auto graphics = (options.graphics || options.require_hardware) ? inspect_vulkan() :
        GraphicsProbe{"not_requested", "", "Use --graphics to enumerate Vulkan devices.", {}};
    const bool hardware_found = std::any_of(graphics.devices.begin(), graphics.devices.end(),
        [](const GraphicsDevice& device) {
            return device.hardware && device.api_at_least_1_3 && device.graphics_queue;
        });
    const auto result = "{\"build\":" + build_info() + ",\"host\":{\"os\":" + quote(host.os) +
        ",\"kernel\":" + quote(host.kernel) + ",\"wsl\":" + boolean(host.wsl) +
        ",\"logical_cpus\":" + std::to_string(host.logical_cpus) +
        ",\"physical_memory_bytes\":" + std::to_string(host.physical_memory_bytes) +
        "},\"graphics\":" + graphics_json(graphics) +
        ",\"hardware_vulkan13_found\":" + boolean(hardware_found) +
        ",\"renderer_qualified\":false}";
    if (graphics.status == "error") {
        return envelope("doctor", result, 4, "graphics_probe_failed", graphics.detail);
    }
    if (options.require_hardware && !hardware_found) {
        return envelope("doctor", result, 3, "hardware_vulkan13_unavailable",
            "No discrete/integrated Vulkan 1.3 device with a graphics queue was enumerated. Inspect graphics.status and devices.");
    }
    return envelope("doctor", result);
}

Reply render_smoke(const RenderOptions& options) {
    if (options.frames < 1 || options.frames > 10000 || options.width < 128 || options.width > 4096 ||
        options.height < 128 || options.height > 4096 || options.gpu < -1 || options.gpu > 4095)
        return usage_error("Render bounds: frames 1..10000, width/height 128..4096, GPU index 0..4095.");
    const auto report = run_render_smoke(options);
    const auto result = "{\"build\":" + build_info() + ",\"available\":" + boolean(report.available) +
        ",\"pattern\":\"rgb_triangle_v1\",\"gpu\":" + quote(report.gpu_name) +
        ",\"hardware\":" + boolean(report.hardware) + ",\"frames_presented\":" + std::to_string(report.frames_presented) +
        ",\"width\":" + std::to_string(report.width) + ",\"height\":" + std::to_string(report.height) +
        ",\"capture_written\":" + boolean(report.capture_written) + ",\"capture_path\":" +
        (report.capture_written ? quote(options.capture) : "null") +
        ",\"nvrhi_errors\":" + std::to_string(report.validation_errors) +
        ",\"renderer_qualified\":false,\"detail\":" + quote(report.detail) + "}";
    if (!report.available) return envelope("render-smoke", result, 3, "render_smoke_not_built", report.detail);
    if (!report.success) return envelope("render-smoke", result, 4, "render_smoke_failed", report.detail);
    return envelope("render-smoke", result);
}

Reply schema(std::string_view command) {
    const auto found = std::find_if(operations.begin(), operations.end(),
        [command](const Operation& operation) { return operation.name == command; });
    if (found == operations.end()) {
        return envelope("schema", "null", 2, "unknown_command", "Unknown schema command: " + std::string(command));
    }
    return envelope("schema", "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"title\":" +
        quote(found->name) + ",\"description\":" + quote(found->summary) +
        ",\"type\":\"object\",\"additionalProperties\":false,\"properties\":" +
        std::string(found->properties) + ",\"required\":" + std::string(found->required) + "}");
}

Reply version() { return envelope("version", build_info()); }

Reply help() {
    return envelope("help", "{\"usage\":\"poima <command> [arguments]\",\"commands\":" + operation_list() +
        ",\"exit_codes\":{\"0\":\"request completed; inspect nested capability status\","
        "\"2\":\"invalid arguments\",\"3\":\"required capability unavailable\",\"4\":\"execution/internal failure\"}}");
}

Reply usage_error(std::string_view detail) {
    return envelope("cli", "null", 2, "invalid_arguments", detail);
}
} // namespace poima
