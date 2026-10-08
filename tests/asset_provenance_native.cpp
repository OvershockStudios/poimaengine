// SPDX-License-Identifier: Apache-2.0
#include "../src/asset_provenance.hpp"
#include "poima/world.hpp"
#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>

int main() {
    namespace fs = std::filesystem;
    using namespace poima::provenance;
    fs::path directory;
    try {
        auto require = [](bool value) { if (!value) throw std::runtime_error("Provenance test failed."); };
        auto invalid = [&](auto call) {
            bool rejected = false;
            try { call(); } catch (const std::invalid_argument&) { rejected = true; }
            require(rejected);
        };
        const std::string asset(64, 'a');
        Json record{{"format", "poima.asset-provenance"}, {"version", 1}, {"asset", asset}, {"kind", "image"},
                    {"title", "Example"}, {"creator", "Creator"}, {"source", "https://example.org/work"},
                    {"license", {{"identifier", "Custom permission"}, {"notice", "Caller-declared permission."}}},
                    {"inputs", {{{"sha256", std::string(64, 'b')}, {"bytes", 9007199254740991ULL}}}}};
        validate_record(record);
        for (const auto& change : std::vector<Json>{
            {{"unknown", true}}, {{"title", ""}}, {{"creator", std::string(1025, 'x')}},
            {{"version", 1.0}}, {{"asset", std::string(64, 'A')}}, {{"kind", "other"}},
            {{"source", "https://user:secret@example.org/work"}}, {{"source", "https://example.org/\n"}},
            {{"source", "file:///work"}}, {{"source", "https://:80/work"}},
            {{"inputs", {{{"sha256", std::string(64, 'b')}, {"bytes", 9007199254740992ULL}}}}},
            {{"inputs", {{{"sha256", std::string(64, 'b')}, {"bytes", -1}}}}}}) {
            auto bad = record;
            bad.update(change);
            invalid([&] { validate_record(bad); });
        }
        auto nul = record; nul["creator"] = std::string("bad\0name", 8);
        invalid([&] { validate_record(nul); });
        auto bad = record;
        bad["inputs"].push_back(bad["inputs"][0]);
        invalid([&] { validate_record(bad); });
        bad = record; bad["title"] = std::string("\xc0\x80", 2);
        invalid([&] { validate_record(bad); });
        validate_bindings(Json{{asset, {std::string(64, '1'), std::string(64, '2')}}});
        invalid([&] { validate_bindings(Json{{asset, {std::string(64, '2'), std::string(64, '1')}}}); });
        invalid([&] { validate_bindings(Json{{asset, Json::array()}}); });
        auto hash_id = [](std::size_t value) {
            std::ostringstream out;
            out << std::hex << std::setfill('0') << std::setw(64) << value;
            return out.str();
        };
        auto max_inputs = record;
        max_inputs["inputs"] = Json::array();
        for (std::size_t i = 0; i < 32; ++i)
            max_inputs["inputs"].push_back({{"sha256", hash_id(i)}, {"bytes", 0}});
        validate_record(max_inputs);
        max_inputs["inputs"].push_back({{"sha256", hash_id(32)}, {"bytes", 0}});
        invalid([&] { validate_record(max_inputs); });
        Json eight = Json::array();
        for (std::size_t i = 0; i < 8; ++i) eight.push_back(hash_id(i));
        validate_bindings(Json{{asset, eight}});
        auto nine = eight; nine.push_back(hash_id(8));
        invalid([&] { validate_bindings(Json{{asset, nine}}); });
        Json max_bindings = Json::object();
        for (std::size_t i = 0; i < 10000; ++i) max_bindings[hash_id(i)] = Json::array({hash_id(0)});
        validate_bindings(max_bindings);
        max_bindings[hash_id(10000)] = Json::array({hash_id(0)});
        invalid([&] { validate_bindings(max_bindings); });
        auto max_bytes = record;
        max_bytes["license"]["notice"] = "";
        const auto overhead = max_bytes.dump().size();
        require(overhead < max_record_bytes);
        const auto available = max_record_bytes - overhead;
        // Quotes occupy one input byte and two canonical JSON bytes. A plain
        // suffix supplies the odd byte without exceeding the raw notice bound.
        std::string notice(available / 2, '"');
        if (available % 2) notice.push_back('x');
        require(!notice.empty() && notice.size() <= 32768);
        max_bytes["license"]["notice"] = notice;
        require(max_bytes.dump().size() == max_record_bytes);
        validate_record(max_bytes);
        notice.push_back('x');
        require(notice.size() <= 32768);
        max_bytes["license"]["notice"] = notice;
        require(max_bytes.dump().size() == max_record_bytes + 1);
        invalid([&] { validate_record(max_bytes); });
        require(schemas().size() == 3);
        directory = fs::temp_directory_path() / ("poima-provenance-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(fs::create_directory(directory));
        const auto first = store(directory, record), second = store(directory, record);
        require(first.created && !second.created && first.id == second.id && first.record == record && first.bytes == record.dump().size());
        require(read(directory, first.id).record == record);
        auto corrupt = [&](std::string bytes) {
            const auto id = poima::content_hash(bytes);
            const auto path = directory / (id + ".pprov");
            std::ofstream(path, std::ios::binary) << bytes;
            bool rejected = false;
            try { (void)read(directory, id); } catch (const std::runtime_error&) { rejected = true; }
            require(rejected);
        };
        corrupt(record.dump(2)); // Hash-valid but not canonical.
        corrupt("{\"format\":\"poima.asset-provenance\",\"format\":\"poima.asset-provenance\"}");
        corrupt(std::string("{\"title\":\"") + std::string("\xc0\x80", 2) + "\"}");
        const auto original = directory / (first.id + ".pprov");
        std::ofstream(original, std::ios::binary | std::ios::trunc) << "corrupt";
        bool rejected = false;
        try { (void)store(directory, record); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected && fs::file_size(original) == 7); // No replacement of corrupt immutable payload.
#ifndef _WIN32
        fs::remove(original);
        fs::create_symlink(directory / "missing", original);
        rejected = false;
        try { (void)store(directory, record); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected && fs::is_symlink(fs::symlink_status(original)));
        fs::remove(original);
        const auto pending = directory / (first.id + ".pprov.pending");
        fs::create_symlink(directory / "missing", pending);
        rejected = false;
        try { (void)store(directory, record); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected && fs::is_symlink(fs::symlink_status(pending)) && !fs::exists(original));
#endif
        const auto world_path = directory / "readonly.poima";
        auto rpc = [](poima::WorldSession& session, const char* method, Json params) {
            return Json::parse(session.request(Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", std::move(params)}}.dump()));
        };
        {
            poima::WorldSession author(world_path.string());
            const auto created = rpc(author, "world.transact", {{"request_id", std::string(32, '1')}, {"base_revision", 0},
                {"ops", {{{"op", "entity.create"}, {"id", std::string(32, '2')}, {"name", "Read-only fixture"}, {"parent", nullptr}}}}});
            require(created.contains("result") && created["result"]["revision"] == 1);
        }
        const auto world_assets = fs::path(world_path.string() + ".assets");
        require(fs::create_directory(world_assets));
        const auto saved_record = store(world_assets, record);
        auto tree = [&] {
            std::map<std::string, std::string> result;
            for (const auto& entry : fs::recursive_directory_iterator(directory)) {
                if (entry.is_symlink()) continue;
                if (entry.is_regular_file()) {
                    std::ifstream input(entry.path(), std::ios::binary);
                    result.emplace(entry.path().lexically_relative(directory).generic_string(),
                        std::string(std::istreambuf_iterator<char>(input), {}));
                }
            }
            return result;
        };
        const auto before = tree();
        {
            poima::WorldSession runtime(world_path.string(), poima::WorldOpenMode::read_only_runtime);
            require(rpc(runtime, "asset.provenance.create", Json::object())["error"]["code"] == -32081);
            require(rpc(runtime, "asset.provenance.create", {{"record", record}})["error"]["code"] == -32081);
            require(rpc(runtime, "world.transact", {{"request_id", std::string(32, '3')}, {"base_revision", 1},
                {"ops", {{{"op", "asset.provenance.set"}, {"asset", asset}, {"records", {saved_record.id}}}}}})["error"]["code"] == -32081);
            require(rpc(runtime, "world.asset.provenance", Json::object())["error"]["code"] == -32602);
            const auto bindings = rpc(runtime, "world.asset.provenance", {{"revision", 1}});
            require(bindings.contains("result") && bindings["result"]["assets"].empty() && bindings["result"]["revision"] == 1);
            const auto inspected = rpc(runtime, "asset.provenance.inspect", {{"record", saved_record.id}});
            require(inspected.contains("result") && inspected["result"]["metadata"] == record);
        }
        require(tree() == before);
        fs::remove_all(directory);
        std::cout << "Provenance schema, bounds, canonical storage, duplicate rejection and immutable publication passed.\n";
        return 0;
    } catch (const std::exception& error) {
        if (!directory.empty()) { std::error_code ignored; fs::remove_all(directory, ignored); }
        std::cerr << error.what() << '\n';
        return 1;
    }
}
