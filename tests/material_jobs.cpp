// SPDX-License-Identifier: Apache-2.0
#include "material_service.hpp"
#include "poima/jobs.hpp"
#include "poima/material_recipe.hpp"
#include "poima/profiler.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

namespace {
using Json = nlohmann::json;
namespace jobs = poima::jobs;
namespace materials = poima::materials;
namespace profiling = poima::profiling;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Callback> void rejects(Callback callback, int code, const char* message) {
    try { callback(); } catch (const materials::Error& error) {
        check(error.code == code, "Wrong material admission error code.");
        return;
    }
    throw std::runtime_error(message);
}
template<class Predicate> void eventually(Predicate predicate, const char* message) {
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (!predicate()) {
        if (Clock::now() >= deadline) throw std::runtime_error(message);
        std::this_thread::yield();
    }
}
struct Directory {
    fs::path path;
    Directory() {
        path = fs::temp_directory_path() / ("poima-material-jobs-" + std::to_string(Clock::now().time_since_epoch().count()));
        check(fs::create_directory(path), "Cannot create owned material jobs test directory.");
    }
    ~Directory() { std::error_code error; fs::remove_all(path, error); }
    fs::path child(const std::string& name) const {
        const auto result = path / name;
        check(fs::create_directory(result), "Cannot create owned material jobs child directory.");
        return result;
    }
};
Json recipe(std::uint32_t seed = 19, std::uint32_t size = 64) {
    return {{"format", "poima.material.recipe.v1"}, {"kind", "brick"},
            {"width", size}, {"height", size}, {"seed", seed}};
}
std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(bool(input), "Cannot read generated material package.");
    return {std::istreambuf_iterator<char>(input), {}};
}
std::map<std::string, std::string> files(const fs::path& directory) {
    std::map<std::string, std::string> result;
    for (const auto& entry : fs::directory_iterator(directory)) {
        check(entry.is_regular_file(), "Material jobs created a nonregular entry.");
        result.emplace(entry.path().filename().string(), read(entry.path()));
    }
    return result;
}
Json terminal(materials::Service& service, const std::string& id, const fs::path& path) {
    Json result;
    eventually([&] {
        result = service.dispatch("asset.material.job", {{"id", id}}, path);
        return result.at("state") != "baking";
    }, "Material group did not reach owner-visible terminal state.");
    return result;
}
jobs::Task task(std::function<void(jobs::Context&)> callback) {
    return {std::move(callback), {}, "material.test.blocker"};
}

void worker_modes(const Directory& scratch) {
    std::map<std::string, std::string> reference;
    const auto specification = recipe();
    const auto parsed = materials::parse_recipe(specification);
    const auto independent = materials::bake(parsed);
    for (std::uint32_t workers : {0u, 1u, 2u, 4u, 8u}) {
        const auto directory = scratch.child("workers-" + std::to_string(workers));
        jobs::Config config;
        config.workers = workers;
        auto executor = std::make_shared<jobs::Executor>(config);
        materials::Service service(executor);
        auto submitted = service.dispatch("asset.material.generate", {{"recipe", specification}}, directory);
        const auto id = submitted.at("id").get<std::string>();
        eventually([&] { return executor->status().background.tasks == 0; }, "Material CPU group never completed.");
        check(fs::is_empty(directory), "Material worker wrote packages before owner publication.");
        check(service.dispatch("asset.material.generate", {{"recipe", specification}}, directory) == submitted,
              "Canonical material duplicate changed retained identity or state.");
        check(service.dispatch("asset.material.jobs", Json::object(), directory).at("jobs")[0].at("state") == "baking",
              "Material list silently published completed CPU output.");
        const auto cancelled = service.dispatch("asset.material.cancel", {{"id", id}}, directory);
        check(cancelled.at("state") == "cancelled" && fs::is_empty(directory),
              "Completed-bake cancellation did not veto publication.");
        service.dispatch("asset.material.forget", {{"id", id}}, directory);
        const auto regenerated = service.dispatch("asset.material.generate", {{"recipe", specification}}, directory);
        check(regenerated.at("id") == id, "Material group worker policy changed recipe identity.");
        const auto published = terminal(service, id, directory);
        check(published.at("state") == "succeeded", "Material CPU group failed owner publication.");
        const auto manifest = published.at("result");
        check(service.dispatch("asset.material.inspect", {{"recipe", id}}, directory) == manifest,
              "Generated material manifest did not survive persisted inspection.");
        const char* names[] = {"base_color", "normal", "metallic_roughness"};
        for (std::size_t index = 0; index < 3; ++index) {
            const auto encoded = poima::encode_image(independent.images[index]);
            const auto expected = poima::sha256(std::as_bytes(std::span(encoded.data(), encoded.size())));
            check(manifest.at("maps").at(names[index]).at("asset") == expected &&
                  read(directory / (expected + ".pimage")) == encoded,
                  "Shared material jobs changed independent recipe/image bytes.");
        }
        const auto generated = files(directory);
        if (workers == 0) reference = generated;
        else check(generated == reference, "Material package bytes differ across actual worker counts.");
        const auto timestamp = fs::last_write_time(directory / (id + ".pmaterial"));
        service.dispatch("asset.material.forget", {{"id", id}}, directory);
        service.dispatch("asset.material.generate", {{"recipe", specification}}, directory);
        check(terminal(service, id, directory).at("state") == "succeeded" &&
              fs::last_write_time(directory / (id + ".pmaterial")) == timestamp && files(directory) == generated,
              "Rebaking replaced an immutable material package.");
    }
}

void bounded_shared_admission(const Directory& scratch) {
    const auto directory = scratch.child("admission");
    jobs::Config config;
    config.workers = 1;
    config.background = {1, 1, 0};
    auto executor = std::make_shared<jobs::Executor>(config);
    std::atomic<bool> entered = false;
    auto blocker = executor->submit({task([&](jobs::Context& context) {
        entered = true;
        eventually([&] { return context.cancel_requested(); }, "Shared admission blocker timed out.");
    })}, jobs::Lane::background);
    eventually([&] { return entered.load(); }, "Shared admission blocker never started.");
    materials::Service service(executor);
    rejects([&] { service.dispatch("asset.material.generate", {{"recipe", recipe()}}, directory); }, -32080,
            "Material job ignored occupied shared background capacity.");
    check(service.dispatch("asset.material.jobs", Json::object(), directory).at("jobs").empty() && fs::is_empty(directory),
          "Rejected shared admission retained a record or changed output.");
    blocker.cancel();
    executor->wait(blocker);
    auto accepted = service.dispatch("asset.material.generate", {{"recipe", recipe()}}, directory);
    const auto id = accepted.at("id").get<std::string>();
    service.dispatch("asset.material.cancel", {{"id", id}}, directory);
    check(terminal(service, id, directory).at("state") == "cancelled" && fs::is_empty(directory),
          "Admission failure contaminated subsequent material cancellation.");
}

void retained_records_and_teardown(const Directory& scratch) {
    const auto directory = scratch.child("records");
    jobs::Config config;
    config.workers = 1;
    auto executor = std::make_shared<jobs::Executor>(config);
    std::atomic<bool> entered = false, release = false, cancelled_external = false;
    auto blocker = executor->submit({task([&](jobs::Context& context) {
        entered = true;
        eventually([&] { return release.load() || context.cancel_requested(); }, "Retained-record blocker timed out.");
        cancelled_external = context.cancel_requested();
    })}, jobs::Lane::background);
    eventually([&] { return entered.load(); }, "Retained-record blocker did not start.");
    {
        materials::Service service(executor);
        auto first = service.dispatch("asset.material.generate", {{"recipe", recipe(1)}}, directory);
        const auto first_id = first.at("id").get<std::string>();
        check(service.dispatch("asset.material.generate", {{"recipe", recipe(1)}}, directory) == first,
              "Queued canonical material submission lost deduplication.");
        rejects([&] { service.dispatch("asset.material.generate", {{"recipe", recipe(2)}}, directory); }, -32080,
                "Service admitted two active material CPU groups.");
        rejects([&] { service.dispatch("asset.material.forget", {{"id", first_id}}, directory); }, -32080,
                "Nonterminal material group could be forgotten.");
        service.dispatch("asset.material.cancel", {{"id", first_id}}, directory);
        for (std::uint32_t seed = 2; seed <= 8; ++seed) {
            auto generated = service.dispatch("asset.material.generate", {{"recipe", recipe(seed)}}, directory);
            auto cancelled = service.dispatch("asset.material.cancel", {{"id", generated.at("id")}}, directory);
            check(cancelled.at("state") == "cancelled", "Queued material cancellation did not become terminal.");
        }
        rejects([&] { service.dispatch("asset.material.generate", {{"recipe", recipe(9)}}, directory); }, -32080,
                "Service exceeded eight retained material records.");
        check(service.dispatch("asset.material.jobs", Json::object(), directory).at("jobs").size() == 8 && fs::is_empty(directory),
              "Record-bound rejection changed stored output or record count.");
        service.dispatch("asset.material.forget", {{"id", first_id}}, directory);
        // Leave this group queued for destructor cancellation/join. It must
        // never execute or dereference the retiring service.
        service.dispatch("asset.material.generate", {{"recipe", recipe(9, 512)}}, directory);
    }
    check(executor->status().background.groups == 1 && !executor->status().stopping &&
          !blocker.poll().terminal() && fs::is_empty(directory),
          "Material teardown stopped an unrelated consumer or leaked queued work.");
    release = true;
    check(executor->wait(blocker).snapshot.state == jobs::State::succeeded && !cancelled_external,
          "Material teardown cancelled another consumer's group.");
}

void reserved_frame_and_running_teardown(const Directory& scratch) {
    const auto directory = scratch.child("frame");
    jobs::Config config;
    config.workers = 2;
    auto executor = std::make_shared<jobs::Executor>(config);
    profiling::Recorder recorder;
    recorder.start(64);
    profiling::Binding binding(&recorder, profiling::Source::request);
    bool running_teardown = false, overlapping_frame = false;
    unsigned attempts = 0;
    // A real bake has no test gates. Observe and qualify its actual intervals
    // instead of assuming any particular CPU speed or duration. Retry only a
    // bounded number of times if this machine completes between observations.
    while (attempts < 8 && !(running_teardown && overlapping_frame)) {
        ++attempts;
        const auto first_event = recorder.events().size();
        auto service = std::make_unique<materials::Service>(executor);
        service->dispatch("asset.material.generate", {{"recipe", recipe(40 + attempts, 512)}}, directory);
        const auto deadline = Clock::now() + std::chrono::seconds(20);
        for (;;) {
            const auto state = executor->status();
            if (state.executing == 1 || state.background.tasks == 0) break;
            check(Clock::now() < deadline, "Material callback did not start or complete.");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto frame_ran = std::make_shared<std::atomic_bool>(false);
        auto frame = executor->submit({{[frame_ran](jobs::Context&) { frame_ran->store(true); }, {}, "material.test.frame"}}, jobs::Lane::frame,
                                      profiling::job_attribution());
        // Do not help the frame on the owner; require actual worker progress.
        while (!frame.poll().terminal()) {
            check(Clock::now() < deadline, "Material job prevented reserved frame progress.");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(frame_ran->load() && frame.result().snapshot.state == jobs::State::succeeded,
              "Reserved frame task failed during material work.");
        {
            // This scope marks owner entry into service destruction in the
            // same recording clock as the native material execution interval.
            profiling::Scope teardown("material.test.teardown");
            service.reset();
        }
        profiling::collect_jobs(*executor);
        check(executor->status().background.groups == 0 && executor->status().executing == 0 && fs::is_empty(directory),
              "Material destructor left running work or published abandoned output.");
        const profiling::Event* bake = nullptr;
        const profiling::Event* frame_event = nullptr;
        const profiling::Event* teardown = nullptr;
        const auto events = recorder.events();
        for (std::size_t index = first_event; index < events.size(); ++index) {
            const auto& event = events[index];
            const std::string_view name(event.name.data());
            if (name == "material.bake") bake = &event;
            if (name == "material.test.frame") frame_event = &event;
            if (name == "material.test.teardown") teardown = &event;
        }
        check(frame_event && teardown && frame_event->complete && teardown->complete &&
              !frame_event->failed && frame_event->background == false,
              "Real frame/destructor observations were not collected.");
        if (bake) {
            check(bake->complete && bake->background, "Material observation did not describe actual completed background work.");
            const auto end = bake->start_ns + bake->duration_ns;
            running_teardown |= bake->start_ns <= teardown->start_ns && teardown->start_ns < end;
            const bool overlaps = std::max(bake->start_ns, frame_event->start_ns) <
                                  std::min(end, frame_event->start_ns + frame_event->duration_ns);
            if (overlaps) check(bake->thread != frame_event->thread,
                                "Simultaneous material/frame spans used the same physical thread.");
            overlapping_frame |= overlaps;
        }
    }
    recorder.stop();
    check(!recorder.status().full && !recorder.status().dropped && !executor->status().trace_dropped,
          "Material timing qualification lost native observations.");
    std::cout << "Material real-bake observations: running-at-teardown=" << running_teardown
              << ", frame-overlap=" << overlapping_frame << ", attempts=" << attempts << ".\n";
    if (!running_teardown || !overlapping_frame)
        std::cout << "Unobserved overlap remains unqualified; completion and abandoned-output cleanup passed.\n";
}

void lazy_shared_factory(const Directory& scratch) {
    const auto directory = scratch.child("factory");
    check(!jobs::peek_owner_executor(), "Injected material consumers created an unexpected factory pool.");
    {
        materials::Service service;
        check(!jobs::peek_owner_executor(), "Default material constructor eagerly created workers.");
        check(service.dispatch("asset.material.jobs", Json::object(), directory).at("jobs").empty() &&
              !jobs::peek_owner_executor(), "Read-only material inspection created workers.");
        auto generated = service.dispatch("asset.material.generate", {{"recipe", recipe()}}, directory);
        auto executor = jobs::peek_owner_executor();
        check(executor && executor == jobs::owner_executor(), "Material service did not share the owner factory pool.");
        service.dispatch("asset.material.cancel", {{"id", generated.at("id")}}, directory);
        check(terminal(service, generated.at("id"), directory).at("state") == "cancelled", "Factory material cancellation failed.");
    }
    check(!jobs::peek_owner_executor() && fs::is_empty(directory), "Weak factory retained material workers or abandoned output.");
}
} // namespace

int main() {
    try {
        Directory scratch;
        worker_modes(scratch);
        bounded_shared_admission(scratch);
        retained_records_and_teardown(scratch);
        reserved_frame_and_running_teardown(scratch);
        lazy_shared_factory(scratch);
        std::cout << "Material jobs: 5 consumer contract groups passed (0/1/2/4/8 workers).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Material jobs failure: " << error.what() << '\n';
        return 1;
    }
}
