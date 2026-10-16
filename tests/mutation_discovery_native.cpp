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

    // Standard conditionals inside an unrelated value schema do not make
    // its explicit operation/type constants overlap the selected Transform.
    // Keep the selected rig's entire conditional subtree byte-for-byte.
    auto rig = branch("component.set", {{"const", "AnimationRig"}});
    Json conditional_value = {{"type", "object"}, {"properties", {
        {"mode", {{"enum", {"extract", "character"}}}},
        {"actor", {{"anyOf", Json::array({Json{{"type", "string"}}, Json{{"type", "null"}}})}}}}}};
    conditional_value["if"] = {{"properties", {{"mode", {{"const", "character"}}}}}, {"required", {"mode"}}};
    conditional_value["then"] = {{"required", {"actor"}}, {"properties", {{"actor", {{"type", "string"}}}}}};
    conditional_value["else"] = {{"properties", {{"actor", {{"type", "null"}}}}}};
    rig["properties"]["value"] = conditional_value;
    rig["required"].push_back("value");
    const auto conditional_source = envelope(Json::array({rename, rig, transform, camera}));
    const auto original_conditional = conditional_source.dump();
    check(items(mutation_schema(conditional_source, "component.set", "Transform")) == Json::array({transform}),
          "Nested standard value conditional disabled disjoint type pruning.");
    check(items(mutation_schema(conditional_source, "component.set", "AnimationRig")) == Json::array({rig}),
          "Selected conditional component schema was lost or rewritten.");
    check(conditional_source.dump() == original_conditional, "Conditional source was mutated.");

    // Recurse into all three conditional schema positions. A reference,
    // unfamiliar dialect or unknown nested keyword still preserves the union.
    for (const auto* position : {"if", "then", "else"}) {
        for (const auto* keyword : {"$ref", "$dynamicRef", "$recursiveRef", "$schema", "$vocabulary", "futureSemantics"}) {
            auto guarded = rig;
            guarded["properties"]["value"][position][keyword] = "opaque";
            const auto value = envelope(Json::array({rename, guarded, transform, camera}));
            check(mutation_schema(value, "component.set", "Transform") == value,
                  "Opaque conditional descendant failed to preserve its whole source union.");
        }
        auto malformed = rig;
        malformed["properties"]["value"][position] = Json::array({true});
        const auto value = envelope(Json::array({rename, malformed, transform, camera}));
        check(mutation_schema(value, "component.set", "Transform") == value,
              "Nonschema conditional value was treated as a known schema.");
    }
    auto boolean_condition = rig;
    boolean_condition["properties"]["value"]["if"] = true;
    boolean_condition["properties"]["value"]["then"] = true;
    boolean_condition["properties"]["value"]["else"] = false;
    check(items(mutation_schema(envelope(Json::array({boolean_condition, transform, camera})),
                                "component.set", "Transform")) == Json::array({transform}),
          "Boolean conditional schemas unnecessarily disabled proven disjoint pruning.");

    // A conditional at the branch itself may alter applicability. Preserve it
    // conservatively and require an independent plain supported witness.
    for (const auto* position : {"if", "then", "else"}) {
        auto conditional_branch = camera; conditional_branch[position] = Json::object();
        const auto value = envelope(Json::array({conditional_branch, transform, rename}));
        check(items(mutation_schema(value, "component.set", "Transform")) == Json::array({conditional_branch, transform}),
              "Branch-level conditional was pruned instead of conservatively retained.");
        auto no_plain_witness = transform; no_plain_witness[position] = Json::object();
        rejects([&] { mutation_schema(envelope(Json::array({no_plain_witness, camera})), "component.set", "Transform"); });
    }

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
