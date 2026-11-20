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
constexpr std::size_t section_name_bytes = 128;

#include "poima/authoring_reference_data.inc"

std::size_t integer(const Json& value, std::size_t maximum, const char* diagnostic) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 1))
        throw std::invalid_argument(diagnostic);
    const auto number = value.get<std::uint64_t>();
    if (number < 1 || number > maximum) throw std::invalid_argument(diagnostic);
    return static_cast<std::size_t>(number);
}

struct Heading {
    std::size_t line, level;
    std::string_view title;
};

struct Guide {
    template<std::size_t N>
    Guide(std::string_view identifier, std::string_view title, const unsigned char (&data)[N])
        : id(identifier), name(title), text(reinterpret_cast<const char*>(data), N),
          digest(sha256(std::as_bytes(std::span(data)))) {
        offsets.push_back(0);
        for (std::size_t i = 0; i < text.size(); ++i)
            if (text[i] == '\n' && i + 1 < text.size()) offsets.push_back(i + 1);
        offsets.push_back(text.size());

        bool inside_fence = false;
        for (std::size_t i = 0; i < lines(); ++i) {
            const auto line = text.substr(offsets[i], offsets[i + 1] - offsets[i]);
            if (line.starts_with("```")) inside_fence = !inside_fence;
            if (inside_fence) continue;
            const auto level = line.find_first_not_of('#');
            if (level == 0 || level > 3 || line[level] != ' ') continue;
            const auto last = line.find_last_not_of(" \r\n");
            if (last > level) index.push_back({i + 1, level, line.substr(level + 1, last - level)});
        }
    }

    std::string_view id, name, text;
    std::string digest;
    std::vector<std::size_t> offsets;
    std::vector<Heading> index;

    std::size_t lines() const { return offsets.size() - 1; }

    Json metadata() const {
        return {{"guide", id}, {"title", name}, {"sha256", digest}, {"line_count", lines()}, {"bytes", text.size()}};
    }

    Json headings() const {
        auto result = Json::array();
        for (const auto& heading : index)
            result.push_back({{"line", heading.line}, {"level", heading.level}, {"title", heading.title}});
        return result;
    }

    Json page(std::size_t line, std::size_t limit, std::size_t last) const {
        const auto first = line - 1;
        auto end = first;
        while (end < last && end - first < limit && offsets[end + 1] - offsets[first] <= page_bytes) ++end;
        if (end == first) throw std::invalid_argument("A reference line exceeds the page byte limit.");
        const auto content = text.substr(offsets[first], offsets[end] - offsets[first]);
        return {{"guide", id}, {"sha256", digest}, {"line", line}, {"line_count", end - first},
            {"next_line", end < last ? Json(end + 1) : Json(nullptr)}, {"text", content}};
    }

    Json section(const Json& parameters, std::size_t limit) const {
        const auto& selector = parameters.at("section");
        if (!selector.is_string())
            throw std::invalid_argument("Reference section must be a heading title.");
        const auto& title = selector.get_ref<const std::string&>();
        if (title.empty() || title.size() > section_name_bytes)
            throw std::invalid_argument("Reference section title must contain 1..128 UTF-8 bytes.");
        const auto matches = [&](const Heading& heading) { return heading.title == title; };
        const auto found = std::find_if(index.begin(), index.end(), matches);
        if (found == index.end())
            throw std::invalid_argument("Unknown section in this guide. Read the guide's heading index.");
        if (std::any_of(found + 1, index.end(), matches))
            throw std::invalid_argument("Section title is ambiguous. Use the heading index and line ranges.");
        const auto following = std::find_if(found + 1, index.end(),
            [&](const Heading& heading) { return heading.level <= found->level; });
        const auto last = following == index.end() ? lines() : following->line - 1;
        const auto line = parameters.contains("line") ?
            integer(parameters.at("line"), last, "Reference start line is outside the section.") : found->line;
        if (line < found->line)
            throw std::invalid_argument("Reference start line is outside the section.");
        auto result = page(line, limit, last);
        result["section"] = found->title;
        result["section_start_line"] = found->line;
        result["section_end_line"] = last;
        return result;
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

}

Json schema() {
    auto names = Json::array();
    for (const auto& guide : guides()) names.push_back(guide.id);
    return {{"type", "object"}, {"additionalProperties", false},
        {"description", "Read guides bundled in this Poima build. Empty parameters list guides. A guide alone lists its headings. "
            "Use section with an exact heading title to read only that section, including its subheadings. Use a 1-based line "
            "to continue within the same section or to read the full guide by pages. Text is limited to 8192 UTF-8 bytes per page."},
        {"properties", {{"guide", {{"type", "string"}, {"enum", std::move(names)}}},
            {"section", {{"type", "string"}, {"minLength", 1}, {"maxLength", section_name_bytes}}},
            {"line", {{"type", "integer"}, {"minimum", 1}}},
            {"lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", max_lines}, {"default", default_lines}}}}},
        {"dependentRequired", {{"section", {"guide"}}, {"line", {"guide"}}, {"lines", {"guide"}}}},
        {"if", {{"required", {"lines"}}}},
        {"then", {{"anyOf", Json::array({Json{{"required", {"section"}}}, Json{{"required", {"line"}}}})}}}};
}

Json read(const Json& parameters) {
    if (!parameters.is_object())
        throw std::invalid_argument("Expected a reference parameter object.");
    for (auto field = parameters.begin(); field != parameters.end(); ++field)
        if (field.key() != "guide" && field.key() != "section" && field.key() != "line" && field.key() != "lines")
            throw std::invalid_argument("Reference parameters accept only guide, section, line and lines.");

    if (parameters.empty()) {
        Json entries = Json::array();
        for (const auto& guide : guides()) entries.push_back(guide.metadata());
        return {{"engine_version", POIMA_VERSION}, {"page_bytes", page_bytes}, {"guides", std::move(entries)}};
    }
    const bool select_section = parameters.contains("section");
    const bool select_line = parameters.contains("line");
    if (!parameters.contains("guide") || (parameters.contains("lines") && !select_line && !select_section))
        throw std::invalid_argument("Select a guide and use a section or starting line for paged reads.");
    if (!parameters.at("guide").is_string()) throw std::invalid_argument("Reference guide must be a published name.");
    const auto& id = parameters.at("guide").get_ref<const std::string&>();
    const auto& catalog = guides();
    const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const Guide& guide) { return guide.id == id; });
    if (found == catalog.end()) throw std::invalid_argument("Unknown reference guide.");
    if (!select_line && !select_section) {
        auto result = found->metadata();
        result["engine_version"] = POIMA_VERSION;
        result["headings"] = found->headings();
        return result;
    }
    const auto limit = parameters.contains("lines") ?
        integer(parameters.at("lines"), max_lines, "Reference page length must be 1..180 lines.") : default_lines;
    auto result = select_section ? found->section(parameters, limit) :
        found->page(integer(parameters.at("line"), found->lines(), "Reference start line is out of range."), limit, found->lines());
    result["engine_version"] = POIMA_VERSION;
    return result;
}
}
