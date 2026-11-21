// SPDX-License-Identifier: Apache-2.0
#include "foliage_authoring.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <map>
#include <utility>

namespace {
using Json = nlohmann::json;
using namespace poima::foliage::authoring;
const std::string terrain_digest(64, 'a');
const std::string asset_a(64, 'b');
const std::string asset_b(64, 'c');

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Exception, typename Test>
void must_throw(Test test, const char* message) {
    try { test(); }
    catch (const Exception&) { return; }
    throw std::runtime_error(message);
}

Json request(unsigned columns, unsigned rows) {
    return {{"expected_terrain_recipe_sha256", terrain_digest}, {"scatter", {
        {"patch_key", "10000000000000ab"}, {"seed", "f00c00000000000a"}, {"origin_m", {0, 0}},
        {"cell_size_m", 1}, {"grid", {columns, rows}}, {"species", Json::array({
            Json{{"id", 4}, {"variants", Json::array({Json{{"asset", asset_a}, {"primitive", 2}}})}},
        })},
    }}};
}

void masks_pins_and_binding() {
    const poima::terrain::Heightfield terrain({.samples_x = 2, .samples_z = 2, .spacing = {5, 6}},
        std::array{0.0f, 2.5f, 1.5f, 4.0f});
    auto input = request(7, 6);
    input["scatter"]["exclude_rectangles_m"] = {{2, 0, 3, 6}};
    input["scatter"]["suppressed_cells"] = {{0, 0}, {2, 2}};
    input["scatter"]["species"].push_back({{"id", 8}, {"weight", 2}, {"variants", Json::array({
        Json{{"asset", asset_a}, {"primitive", 2}}, Json{{"asset", asset_b}, {"primitive", 3}},
    })}});
    auto calls = std::map<std::pair<std::string, std::uint32_t>, int>{};
    const auto verify_asset = [&](std::string_view asset, std::uint32_t primitive) {
        ++calls[{std::string(asset), primitive}];
    };
    const auto whole = plan(terrain, terrain_digest, input, verify_asset);
    require(calls == decltype(calls){{{asset_a, 2}, 1}, {{asset_b, 3}, 1}}, "Variant admission was not deduplicated.");
    const auto counts = whole.at("counts");
    require(counts.at("cells") == 42 && counts.at("placed") == 23 && counts.at("manual_suppressed") == 2 &&
            counts.at("rectangle_excluded") == 5 && counts.at("outside_terrain") == 12,
            "Mask precedence or outside count differs from independently predicted cells.");
    const auto rows = whole.at("placements");
    bool saw_other_variant = false;
    for (const auto& row : rows) {
        const auto pose = row.at("transform");
        const auto p = pose.at("position").get<std::array<double, 3>>();
        const auto q = pose.at("rotation").get<std::array<double, 4>>();
        const auto cell = row.at("cell").get<std::array<int, 2>>();
        require(std::abs(p[1] - (0.5 * p[0] + 0.25 * p[2])) < 1e-13 && std::abs(q[1] * q[1] + q[3] * q[3] - 1) < 1e-13,
                "Transform differs from the analytical plane or normalized yaw.");
        require(cell[0] != 2 && cell[0] < 5 && (cell[0] != 0 || cell[1] != 0), "A masked cell produced an instance.");
        const auto mesh = row.at("static_mesh");
        if (row.at("variant") == 1) {
            require(row.at("species") == 8 && mesh.at("asset") == asset_b && mesh.at("primitive") == 3,
                "Variant one mapped the wrong primitive.");
            saw_other_variant = true;
        } else require(mesh.at("asset") == asset_a && mesh.at("primitive") == 2, "Variant zero mapped the wrong primitive.");
    }
    require(saw_other_variant, "Fixture did not sample the second variant.");

    std::swap(input["scatter"]["species"][0], input["scatter"]["species"][1]);
    std::swap(input["scatter"]["suppressed_cells"][0], input["scatter"]["suppressed_cells"][1]);
    input["scatter"]["density"] = 1.0;
    input["scatter"]["exclude_rectangles_m"].push_back(input["scatter"]["exclude_rectangles_m"][0]);
    input["limit"] = 5;
    Json stitched = Json::array();
    auto page = plan(terrain, terrain_digest, input, verify_asset);
    while (true) {
        require(page.at("plan_sha256") == whole.at("plan_sha256"), "Order or equivalent default normalization changed the plan pin.");
        for (const auto& row : page.at("placements")) stitched.push_back(row);
        const auto next = page.at("next_offset");
        if (next.is_null()) break;
        input["offset"] = next;
        input["expected_plan_sha256"] = page.at("plan_sha256");
        page = plan(terrain, terrain_digest, input, verify_asset);
    }
    require(stitched == rows, "Pinned pages changed the full-row order or values.");

    input["scatter"]["seed"] = "f00c00000000000b";
    must_throw<PinConflict>([&] { (void)plan(terrain, terrain_digest, input, verify_asset); }, "An old plan pin survived re-seeding.");
    must_throw<PinConflict>([&] { (void)plan(terrain, std::string(64, 'd'), input, verify_asset); }, "An old terrain pin was accepted.");
    input.erase("expected_plan_sha256");
    must_throw<std::invalid_argument>([&] { (void)plan(terrain, terrain_digest, input, verify_asset); }, "Unpinned continuation was admitted.");
    input.erase("offset");
    input["scatter"]["grid"] = {4096, 2};
    must_throw<std::invalid_argument>([&] { (void)plan(terrain, terrain_digest, input, verify_asset); }, "Over-capacity grid was admitted.");
}

void geometric_slope_and_spacing() {
    const poima::terrain::Heightfield folded({.samples_x = 2, .samples_z = 2, .spacing = {2, 4}},
        std::array{0.0f, 0.0f, 0.0f, 4.0f});
    const ValidateVariant verify = [](std::string_view, std::uint32_t) {};
    auto input = request(4, 8);
    input["scatter"]["cell_size_m"] = 0.5;
    const auto all = plan(folded, terrain_digest, input, verify);
    input["scatter"]["species"][0]["slope_degrees"] = {0, 54};
    const auto filtered = plan(folded, terrain_digest, input, verify);
    Json expected = Json::array();
    for (const auto& row : all.at("placements")) {
        const auto p = row.at("transform").at("position").get<std::array<double, 3>>();
        if (p[0] / 2 >= p[2] / 4) expected.push_back(row); // 45° on this face, 63.4° on the other.
    }
    require(!expected.empty() && expected.size() < 32 && expected == filtered.at("placements"),
        "Slope filter used a different face or normal from the heightfield mesh.");

    input = request(2, 1);
    input["scatter"]["minimum_spacing_m"] = 3;
    const auto spaced = plan(folded, terrain_digest, input, verify);
    require(spaced.at("counts").at("placed") == 1 && spaced.at("counts").at("spacing_rejected") == 1,
        "Two candidates within one spacing radius did not reduce to one.");
    const auto first = spaced.at("placements")[0];
    input["scatter"]["suppressed_cells"] = Json::array({first.at("cell")});
    const auto alternate = plan(folded, terrain_digest, input, verify);
    require(alternate.at("counts").at("placed") == 1 && alternate.at("counts").at("manual_suppressed") == 1 &&
            alternate.at("counts").at("spacing_rejected") == 0 && alternate.at("placements")[0].at("key") != first.at("key"),
            "Suppression occurred after spacing and did not release the surviving neighbor.");
    input["scatter"].erase("suppressed_cells");
    require(plan(folded, terrain_digest, input, verify) == spaced, "Restoring a logical cell did not recover its exact key.");
}

void rectangle_edges_and_admission() {
    const poima::terrain::Heightfield terrain({.samples_x = 2, .samples_z = 2},
        std::array{0.0f, 0.0f, 0.0f, 0.0f});
    unsigned admits = 0;
    const ValidateVariant verify = [&](std::string_view, std::uint32_t) { ++admits; };
    auto input = request(1, 1);
    const auto initial = plan(terrain, terrain_digest, input, verify);
    require(admits == 1, "Initial plan did not admit its model primitive.");
    const auto first = initial.at("placements")[0];
    require(first.contains("key") && !first.contains("id"), "Planner exposed a logical key as an allocated entity ID.");
    const auto p = first.at("transform").at("position").get<std::array<double, 3>>();
    input["scatter"]["exclude_rectangles_m"] = {{p[0], p[2], 1, 1}};
    require(plan(terrain, terrain_digest, input, verify).at("counts").at("rectangle_excluded") == 1,
        "The minimum rectangle edges were not inclusive.");
    input["scatter"]["exclude_rectangles_m"] = {{0, 0, p[0], 1}};
    require(plan(terrain, terrain_digest, input, verify).at("placements")[0] == first,
        "The maximum rectangle X edge was not exclusive.");
    input["scatter"]["exclude_rectangles_m"] = {{0, 0, 1, p[2]}};
    require(plan(terrain, terrain_digest, input, verify).at("placements")[0] == first,
        "The maximum rectangle Z edge was not exclusive.");

    admits = 0;
    input["expected_plan_sha256"] = initial.at("plan_sha256");
    must_throw<PinConflict>([&] { (void)plan(terrain, terrain_digest, input, verify); }, "Changed mask accepted the old plan pin.");
    require(admits == 0, "Variant admission ran before rejecting a stale plan pin.");
    input.erase("expected_plan_sha256");
    input["scatter"]["density"] = 0;
    const auto empty = plan(terrain, terrain_digest, input, verify);
    require(empty.at("placements").empty() && admits == 1,
        "A zero-density plan did not validate its supplied cooked variant.");
}

void focused_request_schema() {
    const auto schemas = authoring_schemas();
    require(schemas.size() == 1 && schemas.contains("world.foliage.plan"), "Expected one foliage planner discovery method.");
    const auto method = schemas.at("world.foliage.plan");
    const auto properties = method.at("properties");
    require(method.at("required") == Json::array({"terrain", "expected_terrain_recipe_sha256", "scatter"}) &&
            properties.size() == 6 && !properties.contains("revision") && !method.at("additionalProperties").get<bool>(),
        "Wrong top-level required fields or unexpected input revision.");
    const auto terrain_choices = properties.at("terrain").at("oneOf");
    require(terrain_choices.size() == 2 && terrain_choices[0].at("required") == Json::array({"recipe"}) &&
            terrain_choices[1].at("required") == Json::array({"source"}) &&
            method.at("then").at("required") == Json::array({"expected_plan_sha256"}),
        "Terrain input exclusivity or continuation pin missing from discovery.");
    const auto scatter = properties.at("scatter").at("properties");
    require(properties.at("limit").at("maximum") == max_page_size &&
            scatter.at("grid").at("items").at("maximum") == max_plan_cells &&
            scatter.at("species").at("maxItems") == max_plan_species &&
            scatter.at("species").at("items").at("properties").at("variants").at("maxItems") == max_plan_variants &&
            scatter.at("exclude_rectangles_m").at("maxItems") == max_exclusions &&
            scatter.at("suppressed_cells").at("maxItems") == max_suppressions,
        "Discovery limits differ from the planner bounds.");
}
} // namespace

int main() {
    try {
        masks_pins_and_binding();
        geometric_slope_and_spacing();
        rectangle_edges_and_admission();
        focused_request_schema();
        std::cout << "Foliage planner passed masks, slopes, spacing, pins, paging, variant selection and request discovery.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
