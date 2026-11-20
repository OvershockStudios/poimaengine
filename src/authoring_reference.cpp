// SPDX-License-Identifier: Apache-2.0
#include "authoring_reference.hpp"
#include "poima/assets.hpp"
#include "poima/build_info.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace poima::authoring_reference {
namespace {
using Json = nlohmann::json;
constexpr std::size_t page_bytes = 8192;
constexpr std::size_t default_lines = 120;
constexpr std::size_t max_lines = 180;

#include "poima/authoring_reference_data.inc"

struct Guide {
    template<std::size_t N>
    Guide(std::string_view identifier, std::string_view title, const unsigned char (&data)[N])
        : id(identifier), name(title), text(reinterpret_cast<const char*>(data), N),
          digest(sha256(std::as_bytes(std::span(data)))) {
        offsets.push_back(0);
        for (std::size_t i = 0; i < text.size(); ++i)
            if (text[i] == '\n' && i + 1 < text.size()) offsets.push_back(i + 1);
        offsets.push_back(text.size());
    }

    std::string_view id, name, text;
    std::string digest;
    std::vector<std::size_t> offsets;

    std::size_t lines() const { return offsets.size() - 1; }

    Json metadata() const {
        return {{"guide", id}, {"title", name}, {"sha256", digest}, {"line_count", lines()}, {"bytes", text.size()}};
    }

    Json headings() const {
        auto index = Json::array();
        bool inside_fence = false;
        for (std::size_t i = 0; i < lines(); ++i) {
            const auto line = text.substr(offsets[i], offsets[i + 1] - offsets[i]);
            if (line.starts_with("```")) inside_fence = !inside_fence;
            if (inside_fence) continue;
            const auto level = line.find_first_not_of('#');
            if (level == 0 || level > 3 || line[level] != ' ') continue;
            const auto last = line.find_last_not_of(" \r\n");
            if (last > level) index.push_back({{"line", i + 1}, {"level", level},
                {"title", line.substr(level + 1, last - level)}});
        }
        return index;
    }

    Json page(std::size_t line, std::size_t limit) const {
        const auto first = line - 1;
        auto end = first;
        while (end < lines() && end - first < limit && offsets[end + 1] - offsets[first] <= page_bytes) ++end;
        if (end == first) throw std::invalid_argument("A reference line exceeds the page byte limit.");
        const auto content = text.substr(offsets[first], offsets[end] - offsets[first]);
        return {{"guide", id}, {"sha256", digest}, {"line", line}, {"line_count", end - first},
            {"next_line", end < lines() ? Json(end + 1) : Json(nullptr)}, {"text", content}};
    }
};

const auto& guides() {
    static const std::array<Guide, 4> entries{{
        {"creator-gameplay", "Build and verify gameplay from the creator", creator_gameplay},
        {"managed-gameplay", "C# gameplay in the native runtime", managed_gameplay},
        {"game-ui", "Native game UI foundation", game_ui},
        {"game-verification", "Verify gameplay in an exported game", game_verification},
    }};
    return entries;
}

std::size_t integer(const Json& value, std::size_t maximum, const char* diagnostic) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 1))
        throw std::invalid_argument(diagnostic);
    const auto number = value.get<std::uint64_t>();
    if (number < 1 || number > maximum) throw std::invalid_argument(diagnostic);
    return static_cast<std::size_t>(number);
}
}

Json schema() {
    auto names = Json::array();
    for (const auto& guide : guides()) names.push_back(guide.id);
    return {{"type", "object"}, {"additionalProperties", false},
        {"description", "Read public references bundled in this Poima build. No arguments list guides. A guide alone lists its headings. Specify a 1-based line to read up to 8192 UTF-8 bytes, then follow next_line. Start with creator-gameplay and game-verification for a first game."},
        {"properties", {{"guide", {{"type", "string"}, {"enum", std::move(names)}}},
            {"line", {{"type", "integer"}, {"minimum", 1}}},
            {"lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", max_lines}, {"default", default_lines}}}}},
        {"dependentRequired", {{"line", {"guide"}}, {"lines", {"guide", "line"}}}}};
}

Json read(const Json& parameters) {
    if (!parameters.is_object())
        throw std::invalid_argument("Expected a reference parameter object.");
    for (auto field = parameters.begin(); field != parameters.end(); ++field)
        if (field.key() != "guide" && field.key() != "line" && field.key() != "lines")
            throw std::invalid_argument("Reference parameters accept only guide, line and lines.");
    if ((parameters.contains("lines") && !parameters.contains("line")) ||
        (parameters.contains("line") && !parameters.contains("guide")))
        throw std::invalid_argument("Select a guide and starting line before choosing a page length.");

    if (parameters.empty()) {
        Json entries = Json::array();
        for (const auto& guide : guides()) entries.push_back(guide.metadata());
        return {{"engine_version", POIMA_VERSION}, {"page_bytes", page_bytes}, {"guides", std::move(entries)}};
    }
    if (!parameters.at("guide").is_string()) throw std::invalid_argument("Reference guide must be a published name.");
    const auto& id = parameters.at("guide").get_ref<const std::string&>();
    const auto& catalog = guides();
    const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const Guide& guide) { return guide.id == id; });
    if (found == catalog.end()) throw std::invalid_argument("Unknown reference guide.");
    if (!parameters.contains("line")) {
        auto result = found->metadata();
        result["engine_version"] = POIMA_VERSION;
        result["headings"] = found->headings();
        return result;
    }
    const auto first = integer(parameters.at("line"), found->lines(), "Reference start line is out of range.");
    const auto limit = parameters.contains("lines") ?
        integer(parameters.at("lines"), max_lines, "Reference page length must be 1..180 lines.") : default_lines;
    auto result = found->page(first, limit);
    result["engine_version"] = POIMA_VERSION;
    return result;
}
}
