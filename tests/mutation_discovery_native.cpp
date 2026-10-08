// SPDX-License-Identifier: Apache-2.0
#include "mutation_discovery.hpp"
#include <iostream>

using Json = nlohmann::json;
using poima::discovery_detail::mutation_schema;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Json branch(const char* operation, Json type = nullptr) {
    Json properties = {{"op", {{"const", operation}}}, {"id", {{"type", "string"}}}};
    Json required = {"op", "id"};
    if (!type.is_null()) { properties["type"] = std::move(type); required.push_back("type"); }
    return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}
Json envelope(Json branches) {
    return {{"type", "object"}, {"properties", {
        {"request_id", {{"type", "string"}, {"pattern", "^[0-9a-f]{32}$"}}},
        {"base_revision", {{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991ULL}}},
        {"preview", {{"type", "boolean"}, {"default", false}}},
        {"ops", {{"type", "array"}, {"minItems", 1}, {"maxItems", 256}, {"items", {{"oneOf", branches}}}}}}},
        {"required", {"request_id", "base_revision", "ops"}}, {"additionalProperties", false}};
}
const Json& items(const Json& value) { return value.at("properties").at("ops").at("items").at("oneOf"); }
template<class F> void rejects(F run) {
    bool rejected = false; try { run(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Unsupported selection invented from an opaque branch.");
}
}
int main() { try {
    const auto transform = branch("component.set", {{"const", "Transform"}});
    const auto camera = branch("component.set", {{"const", "Camera"}});
    const auto remove = branch("component.remove", {{"enum", {"Camera", "Light"}}});
    const auto custom = branch("component.set", {{"type", "string"}, {"pattern", "^game:[0-9a-f]{32}$"}});
    const auto rename = branch("entity.rename");
    const auto source = envelope(Json::array({transform, camera, remove, custom, rename}));
    const auto original = source.dump();
    const auto focused = mutation_schema(source, "component.set", "Transform");
    check(items(focused) == Json::array({transform}), "Exact component projection did not preserve the source branch.");
    auto restored = focused; restored["properties"]["ops"]["items"]["oneOf"] = items(source);
    check(restored == source && source.dump() == original, "Projection changed the envelope or source.");
    check(items(mutation_schema(source, "component.set")) == Json::array({transform, camera, custom}), "Operation-only projection lost supported variants.");
    check(items(mutation_schema(source, "component.remove", "Camera")) == Json::array({remove}), "Remove enum was synthesized or narrowed.");
    check(items(mutation_schema(source, "component.set", "game:00000000000000000000000000000001")) == Json::array({custom}), "Registered custom pattern projection differs.");
    rejects([&] { mutation_schema(source, "component.remove", "Transform"); });
    rejects([&] { mutation_schema(source, "unknown"); });
    rejects([&] { mutation_schema(source, "component.set", "Missing"); });

    auto ref = rename; ref["properties"]["id"]["$ref"] = "#/$defs/id";
    auto dynamic_ref = rename; dynamic_ref["$dynamicRef"] = "#node";
    auto recursive_ref = rename; recursive_ref["properties"]["op"]["$recursiveRef"] = "#";
    auto dialect = rename; dialect["$schema"] = "http://json-schema.org/draft-04/schema#";
    auto unknown = rename; unknown["futureSemantics"] = true;
    auto composition = rename; composition["allOf"] = Json::array({Json::object()});
    auto broad = branch("component.set"); broad["properties"]["op"] = {{"type", "string"}};
    auto pattern = rename; pattern["properties"]["op"]["pattern"] = ".*";
    auto overlap = branch("unused"); overlap["properties"]["op"] = {{"enum", {"component.set", "other"}}};
    auto opaque = Json::array({ref, dynamic_ref, recursive_ref, dialect, unknown, composition, broad, pattern, overlap});
    auto mixed = opaque; mixed.push_back(transform); mixed.push_back(camera);
    check(mutation_schema(envelope(mixed), "component.set", "Transform") == envelope(mixed),
          "Reference/dialect/unknown descriptor did not preserve its complete union.");
    auto plain_broad = Json::array({broad, pattern, overlap, composition, transform, camera});
    auto expected = Json::array({broad, pattern, overlap, composition, transform});
    check(items(mutation_schema(envelope(plain_broad), "component.set", "Transform")) == expected,
          "Broad or overlapping branches were pruned despite a positive witness.");
    // Opaque survivors do not independently prove that a selector is supported.
    rejects([&] { mutation_schema(envelope(opaque), "not.declared"); });
    rejects([&] { mutation_schema(envelope(Json::array({ref, dialect, unknown})), "entity.rename"); });
    for (const auto* keyword : {"$ref", "$dynamicRef", "$recursiveRef"}) {
        // Removing rename would move the referenced Camera branch from index 1
        // to index 0, changing both reference resolution and union exclusivity.
        auto pointer = Json{{keyword, "#/properties/ops/items/oneOf/1"}};
        const auto referenced = envelope(Json::array({rename, camera, pointer, transform}));
        check(mutation_schema(referenced, "component.set", "Transform") == referenced,
              "Sibling pointer target moved after projection.");
        auto ancestor = source; ancestor["properties"]["ops"]["items"][keyword] = "#/definitions/ops";
        check(mutation_schema(ancestor, "component.set", "Transform") == ancestor,
              "Ancestor reference was changed by union pruning.");
        ancestor = source; ancestor[keyword] = "#/definitions/request";
        check(mutation_schema(ancestor, "component.set", "Transform") == ancestor,
              "Envelope reference was changed by union pruning.");
    }
    for (const auto* keyword : {"$schema", "$vocabulary", "futureSemantics"}) {
        auto ancestor = source; ancestor[keyword] = "unfamiliar";
        check(mutation_schema(ancestor, "component.set", "Transform") == ancestor,
              "Unknown envelope/dialect semantics were pruned.");
        ancestor = source; ancestor["properties"]["ops"]["items"][keyword] = "unfamiliar";
        check(mutation_schema(ancestor, "component.set", "Transform") == ancestor,
              "Unknown union/dialect semantics were pruned.");
    }
    auto impossible = transform; impossible["properties"]["type"]["enum"] = {"Camera"};
    rejects([&] { mutation_schema(envelope(Json::array({impossible})), "component.set", "Transform"); });
    auto op_enum = rename; op_enum["properties"]["op"] = {{"enum", {"entity.rename", "entity.delete"}}};
    check(items(mutation_schema(envelope(Json::array({op_enum, camera})), "entity.rename")) == Json::array({op_enum}),
          "Operation enum was narrowed rather than retained.");
    std::cout << "Mutation projection envelope, availability and conservative union checks passed.\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
