// SPDX-License-Identifier: Apache-2.0
#define NOMINMAX
#include <windows.h>
#include "poima/assets.hpp"
#include "poima/capture_image.hpp"
#include "poima/core.hpp"
#include "poima/hosted_viewport.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {
using namespace poima;
using Json = nlohmann::json;
using Color = std::array<float, 3>;
using Chart = std::array<Color, 4>;
using Extent = std::array<unsigned, 2>;

constexpr Extent initial_extent{384, 216}, larger_extent{480, 270};
constexpr std::array<unsigned, 5> columns{0, 4, 30, 57, 64};
constexpr unsigned sample_count = 64 * 36;
constexpr float manual_exposure = .75f;
constexpr double seconds = 1.0 / 60;
constexpr AutoExposureSettings settings{-4, 4, 2, 4};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void close_to(double actual, double expected, double tolerance, const char* message) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

void pump() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

struct Window {
    HWND parent{}, child{};

    Window() {
        const auto module = GetModuleHandleW(nullptr);
        parent = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"Poima auto exposure qualification",
            WS_POPUP | WS_VISIBLE, 80, 80, larger_extent[0], larger_extent[1], nullptr, nullptr, module, nullptr);
        require(parent, "Could not create exposure parent window");
        child = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            0, 0, initial_extent[0], initial_extent[1], parent, nullptr, module, nullptr);
        if (!child) {
            DestroyWindow(parent);
            throw std::runtime_error("Could not create exposure child window");
        }
        pump();
    }

    ~Window() {
        DestroyWindow(child);
        DestroyWindow(parent);
    }

    void resize(Extent size) {
        require(SetWindowPos(child, nullptr, 0, 0, size[0], size[1], SWP_NOZORDER | SWP_NOACTIVATE),
            "Could not resize exposure child window");
        pump();
    }
};

Chart uniform(Color value) { return {value, value, value, value}; }
Chart gray(float value) { return uniform({value, value, value}); }

SceneObject patch(std::string id, float left, float right, float half_height) {
    auto mesh = std::make_shared<MeshAsset>();
    for (auto position : {std::array<float, 3>{left, -half_height, 0}, {right, -half_height, 0},
            {right, half_height, 0}, {left, half_height, 0}}) {
        MeshVertex vertex;
        vertex.position = position;
        vertex.normal = {0, 0, 1};
        mesh->vertices.push_back(vertex);
    }
    mesh->indices = {0, 1, 2, 0, 2, 3};
    mesh->material.base_color = {0, 0, 0};
    SceneObject object{};
    object.incarnation = 1;
    object.entity_id = std::move(id);
    object.world = identity_matrix();
    object.material = mesh->material;
    object.mesh = std::move(mesh);
    return object;
}

SceneSnapshot scene() {
    SceneSnapshot state;
    state.world_id = "exposure-world";
    state.camera_id = "exposure-camera";
    state.presentation_source_id = new_presentation_source_id();
    state.camera_world = local_matrix({0, 0, 4}, {0, 0, 0, 1}, {1, 1, 1});
    state.vertical_fov = 60;
    state.near_plane = .1;
    state.far_plane = 100;
    state.lighting.preview = false;
    state.lighting.environment.exposure = manual_exposure;
    const auto half_height = 4 * std::tan(std::numbers::pi_v<float> / 6);
    const auto half_width = half_height * initial_extent[0] / initial_extent[1];
    const auto x = [half_width](unsigned column) { return (column / 32.f - 1.f) * half_width; };
    for (unsigned i = 0; i < columns.size() - 1; ++i) {
        const auto left = i ? x(columns[i]) : -1.1f * half_width;
        const auto right = i < 3 ? x(columns[i + 1]) : 1.1f * half_width;
        state.objects.push_back(patch("chart-" + std::to_string(i), left, right, 1.1f * half_height));
    }
    return state;
}

void illuminate(SceneSnapshot& state, const Chart& radiance) {
    for (unsigned i = 0; i < radiance.size(); ++i) state.objects[i].material->emissive = radiance[i];
}

double meter_log2_luminance(const Chart& radiance) {
    // Each column covers 36 sites. The lower trim removes the 144 darkest sites
    // and part of a middle patch. The upper trim retains 22 of the brightest sites.
    std::array<double, sample_count> sites{};
    for (unsigned i = 0; i < radiance.size(); ++i) {
        const auto [r, g, b] = radiance[i];
        const auto luminance = .2126 * r + .7152 * g + .0722 * b;
        constexpr double bin_width = 24.0 / 64;
        const auto bin = std::min(63.0, std::floor((std::clamp(std::log2(luminance), -12.0, 12.0) + 12) / bin_width));
        const auto center = -12 + (bin + .5) * bin_width;
        std::fill(sites.begin() + columns[i] * 36, sites.begin() + columns[i + 1] * 36, center);
    }
    std::sort(sites.begin(), sites.end());
    constexpr auto trim = sites.size() / 10;
    return std::accumulate(sites.begin() + trim, sites.end() - trim, 0.0) / (sites.size() - 2 * trim);
}

double grade_curve(double exposed) {
    constexpr double shoulder = .125;
    return exposed <= shoulder ? exposed * .5 : shoulder * .5 + (exposed - shoulder) * 1.25;
}

int display_byte(double radiance, double multiplier, bool grade) {
    double exposed = radiance * multiplier * manual_exposure;
    if (grade) exposed = grade_curve(exposed);
    const double mapped = exposed / (1 + exposed);
    const double srgb = mapped <= .0031308 ? mapped * 12.92 : 1.055 * std::pow(mapped, 1.0 / 2.4) - .055;
    return static_cast<int>(std::lround(srgb * 255));
}

void verify(const std::string& prefix, RenderOptions options, Json& evidence) {
    Window window;
    auto state = scene();
    auto& environment = state.lighting.environment;
    auto render_extent = initial_extent;
    auto radiance = gray(.375f);
    illuminate(state, radiance);
    HostedViewport view(options, state, window.child);
    const bool reconstruct = options.reconstruction != ReconstructionMode::none;
    evidence["cases"].push_back({{"pipeline", options.deferred ? "deferred" : "forward"},
        {"mode", reconstruction_mode_name(options.reconstruction)}, {"samples", options.samples},
        {"slots", options.frames_in_flight}, {"captures", Json::array()}});
    auto& row = evidence["cases"].back();
    std::uint64_t accepted = 0;
    double expected_stops = 0;

    auto capture = [&](const char* label, bool reset, bool verify_pixels = true) {
        const auto path = prefix + "-" + std::to_string(options.samples) + "x-" + label + ".bmp";
        auto& attempt = row["last_attempt"];
        attempt = {{"phase", label}, {"path", path}, {"pixels", Json::array()}};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        for (;;) {
            pump();
            require(view.report().diagnostics.frame_execution.submitted == accepted, "Unexpected earlier exposure submission");
            const auto presented = view.draw_capture(state, path);
            const auto submitted = view.report().diagnostics.frame_execution.submitted;
            attempt["submitted"] = submitted;
            if (presented) {
                require(submitted == accepted + 1, "Exposure capture did not submit exactly one frame");
                ++accepted;
                break;
            }
            require(submitted == accepted, "Exposure capture consumed an unpresented frame");
            require(std::chrono::steady_clock::now() < deadline, "Exposure capture timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const auto report = view.report();
        const auto& d = report.diagnostics;
        const auto& meter = d.auto_exposure;
        attempt.update({{"gpu", report.gpu_name}, {"detail", report.detail}, {"errors", report.validation_errors},
            {"extent", {report.width, report.height}}, {"active", meter.has_value()}, {"presented", report.frames_presented},
            {"completed", d.completed_submissions}, {"expected_reset", reset}, {"checked_pixels", verify_pixels}});
        require(report.success && report.hardware && report.capture_written && report.validation_errors == 0,
            "Hardware exposure capture failed");
        require(report.width == render_extent[0] && report.height == render_extent[1] && report.samples == options.samples,
            "Exposure fixture framebuffer differs");
        require(report.frames_presented == accepted && d.completed_submissions == accepted && !d.frame_execution.outstanding,
            "Exposure capture was not drained after the expected presentation");
        require(d.deferred == options.deferred && d.reconstruction.active == reconstruct &&
            d.reconstruction.mode == options.reconstruction && d.frame_execution.limit == options.frames_in_flight,
            "Exposure fixture render configuration differs");
        require(meter.has_value() == environment.auto_exposure.has_value(), "Exposure override does not match authoring");
        if (meter) {
            const auto luma = meter_log2_luminance(radiance);
            const auto target = std::clamp(std::log2(.18) - luma, double(settings.min_stops), double(settings.max_stops));
            const auto step = std::clamp(target - expected_stops, -settings.darken_stops_per_second * seconds,
                settings.brighten_stops_per_second * seconds);
            expected_stops = reset ? target : expected_stops + step;
            attempt.update({{"reset", meter->history_reset}, {"seconds", meter->seconds}, {"sites", meter->samples},
                {"expected_meter", {expected_stops, target, luma, std::exp2(expected_stops)}}});
            require(meter->measured && meter->history_reset == reset && meter->samples == sample_count,
                "Exposure meter readback, reset or site count differs");
            attempt["meter"] = *meter->measured;
            close_to(meter->seconds, seconds, 1e-7, "Capture exposure cadence is not 60 Hz");
            close_to((*meter->measured)[0], expected_stops, 2e-5, "Adapted stops differ from bounded rate oracle");
            close_to((*meter->measured)[1], target, 2e-5, "Target differs from trimmed middle-gray oracle");
            close_to((*meter->measured)[2], luma, 2e-5, "Meter differs from authored log luminance oracle");
            close_to((*meter->measured)[3], std::exp2(expected_stops), 4e-5, "Exposure multiplier differs from adapted stops");
        } else {
            expected_stops = 0;
        }

        const auto bytes_size = std::filesystem::file_size(path);
        require(bytes_size <= std::uint64_t(render_extent[0]) * render_extent[1] * 4 + 1024, "Exposure BMP is larger than fixture bounds");
        std::string bytes(bytes_size, '\0');
        std::ifstream bmp(path, std::ios::binary);
        require(bool(bmp.read(bytes.data(), bytes.size())), "Could not read original exposure BMP");
        const auto image = capture_image(std::as_bytes(std::span(bytes.data(), bytes.size())));
        attempt["bmp_sha256"] = image.source_sha256;
        require(image.width == render_extent[0] && image.height == render_extent[1], "Exposure BMP dimensions differ");
        if (verify_pixels) {
            const auto texture = decode_texture(image.png, true);
            require(texture && !texture->mips.empty(), "Could not decode exposure pixels");
            const auto& rgba = texture->mips.front().rgba;
            for (unsigned i = 0; i < radiance.size(); ++i) {
                const auto x = (columns[i] + columns[i + 1]) * image.width / 128;
                const auto y = image.height / 2;
                const auto index = (std::size_t(y) * image.width + x) * 4;
                const auto color = state.viewmodel ? state.viewmodel->objects[0].material->emissive : radiance[i];
                const auto peak = *std::max_element(color.begin(), color.end());
                const auto bloom = environment.bloom;
                const auto contribution = bloom.intensity * std::max(peak - bloom.threshold, 0.f) / peak;
                std::array<int, 3> actual{}, expected{};
                for (unsigned c = 0; c < actual.size(); ++c) {
                    actual[c] = rgba.at(index + c);
                    expected[c] = display_byte(color[c] * (1 + contribution), std::exp2(expected_stops),
                        environment.output_grade.has_value());
                }
                attempt["pixels"].push_back({{"pixel", {x, y}}, {"actual", actual}, {"expected", expected}});
                for (unsigned c = 0; c < actual.size(); ++c) close_to(actual[c], expected[c], reconstruct ? 2 : 1,
                    "Display pixel differs from independent exposure, grading and sRGB oracle");
            }
        }
        row["captures"].push_back(attempt);
        row.erase("last_attempt");
        return bytes;
    };

    const auto manual = capture("manual-reference", false);
    auto set = [&](const Chart& chart, bool cut) {
        radiance = chart;
        illuminate(state, radiance);
        if (cut) ++state.view_cut_generation;
    };
    environment.auto_exposure = settings;
    set({Color{9.f / 4096, 9.f / 4096, 9.f / 4096}, {.140625f, .140625f, .140625f},
        {.625f, .625f, .625f}, {12, 12, 12}}, false);
    capture("enabled-trimmed-chart", true, !reconstruct);
    set(gray(.0078125f), true);
    capture("cut-dark-upper-clamp", true);
    set(gray(8.5f), false);
    capture("adapt-darken-first", false, !reconstruct);
    capture("adapt-darken-second", false, !reconstruct);
    ++state.view_cut_generation;
    capture("cut-bright-lower-clamp", true);
    set(gray(.0078125f), false);
    capture("adapt-brighten-first", false, !reconstruct);
    capture("adapt-brighten-second", false, !reconstruct);
    set(uniform({.25f, .625f, .125f}), true);
    capture("rec709-chromatic-meter", true);

    set(gray(.625f), true);
    capture("world-baseline", true);
    auto foreground = patch("bright-foreground", -100, 100, 100);
    foreground.material->emissive = {4, 4, 4};
    state.viewmodel = SceneViewmodel{state.world_lens(), {std::move(foreground)}};
    capture("independent-viewmodel-excluded-from-meter", false);
    state.viewmodel.reset();
    environment.bloom = {.5f, .25f, 8};
    capture("bloom-excluded-from-meter", false);
    OutputGrade grade;
    grade.input_max = 1;
    for (unsigned i = 0; i < grade.knots.size(); ++i) {
        grade.knots[i].fill(static_cast<float>(grade_curve(i / 32.0)));
    }
    environment.output_grade = grade;
    capture("grade-after-exposure", false);
    environment.bloom = {};
    environment.output_grade.reset();
    environment.auto_exposure.reset();
    set(gray(.375f), true);
    row["manual_bytes_equal"] = capture("manual-restored", false) == manual;
    require(row["manual_bytes_equal"].get<bool>(), "Disabling exposure changed the complete reference BMP");

    environment.auto_exposure = settings;
    set(gray(.0078125f), true);
    capture("reenabled-dark", true);
    set(gray(8.5f), false);
    capture("before-resize-adapting", false, !reconstruct);
    render_extent = larger_extent;
    window.resize(render_extent);
    capture("resize-snaps-to-current-bright-target", true);
    capture("after-resize-steady", false);

    if (options.frames_in_flight == 2) {
        constexpr std::array levels{.0078125f, .0234375f, .375f, .09375f};
        const auto before = view.report();
        auto& burst = row["buffered_sequence"];
        burst = {{"first_submitted", before.diagnostics.frame_execution.submitted},
            {"first_completed", before.diagnostics.completed_submissions},
            {"first_peak_outstanding", before.diagnostics.frame_execution.peak_outstanding},
            {"radiance", levels}, {"presented_without_capture", 0}};
        unsigned presented = 0;
        for (const auto level : levels) {
            // All four authored targets remain above the running adapted value.
            set(gray(level), false);
            pump();
            require(view.draw(state), "Buffered exposure frame did not present");
            burst["presented_without_capture"] = ++presented;
        }
        // Poll once after the burst so retirement between draws is left to the renderer.
        const auto after = view.report();
        const auto& diagnostics = after.diagnostics;
        const auto& frames = diagnostics.frame_execution;
        burst.update({{"submitted", frames.submitted}, {"presented", after.frames_presented},
            {"completed", diagnostics.completed_submissions}, {"outstanding", frames.outstanding},
            {"peak_outstanding", frames.peak_outstanding}, {"validation_errors", after.validation_errors}});
        require(after.success && after.hardware && after.validation_errors == 0 &&
            frames.submitted == accepted + levels.size() && after.frames_presented == frames.submitted,
            "Buffered exposure burst did not accept and present every frame");
        require(diagnostics.completed_submissions >= before.diagnostics.completed_submissions &&
            diagnostics.completed_submissions + frames.outstanding == frames.submitted &&
            frames.peak_outstanding <= options.frames_in_flight, "Buffered exposure frame accounting differs");
        if (diagnostics.completed_submissions > before.diagnostics.completed_submissions) {
            require(diagnostics.auto_exposure && !diagnostics.auto_exposure->measured,
                "An uncaptured exposure frame reported a GPU readback");
        }
        accepted = frames.submitted;
        expected_stops += levels.size() * settings.brighten_stops_per_second * seconds;
        capture("after-buffered-exposure-steps", false, !reconstruct);
    }
}
}

int main(int argc, char** argv) {
    Json evidence{{"passed", false}, {"cases", Json::array()}};
    try {
        require(argc >= 3 && argc <= 5,
            "Usage: poima-auto-exposure-test OUTPUT_PREFIX GPU [forward|deferred] [none|fsr3_native|fsr3_quality]");
        const std::string pipeline = argc > 3 ? argv[3] : "forward";
        const std::string mode = argc > 4 ? argv[4] : "none";
        require(pipeline == "forward" || pipeline == "deferred", "Expected forward or deferred pipeline");
        RenderOptions options;
        options.width = initial_extent[0];
        options.height = initial_extent[1];
        options.gpu = std::stoi(argv[2]);
        options.deferred = pipeline == "deferred";
        options.samples = 1;
        options.frames_in_flight = 1;
        options.capture_exclusive = true;
        if (mode == "fsr3_native") options.reconstruction = ReconstructionMode::fsr3_native;
        else if (mode == "fsr3_quality") {
            options.reconstruction = ReconstructionMode::fsr3_quality;
            options.frames_in_flight = 2;
        } else require(mode == "none", "Expected none, fsr3_native or fsr3_quality reconstruction");
        const auto prefix = std::string(argv[1]) + "-" + pipeline + "-" + mode;
        verify(prefix, options, evidence);
        if (!options.deferred && options.reconstruction == ReconstructionMode::none) {
            options.samples = 4;
            options.frames_in_flight = 2;
            verify(prefix, options, evidence);
        }
        evidence["passed"] = true;
    } catch (const std::exception& ex) {
        evidence["error"] = ex.what();
    }
    std::cout << evidence.dump(2) << '\n';
    return evidence["passed"].get<bool>() ? 0 : 1;
}
