// SPDX-License-Identifier: Apache-2.0
#include "foliage_authoring.hpp"
#include "poima/foliage.hpp"
#include "poima/local_session.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace poima::foliage::authoring {
namespace {
using Json = nlohmann::json;
using Variant = std::pair<std::string, std::uint32_t>;
using Rectangle = std::array<double, 4>;
using Cell = std::array<std::uint32_t, 2>;

constexpr auto semantics = "poima.foliage.plan.v1";
constexpr auto max_coordinate = terrain::max_world_coordinate_m;
constexpr double min_cell_size = 0.01;
constexpr double max_cell_size = 10'000;
constexpr double min_spacing = 0.001;
constexpr double max_spacing = 2'000'000;
constexpr double min_scale = 0.001;
constexpr double max_scale = 100;
constexpr std::uint32_t max_primitive = 9999;
constexpr std::uint32_t max_weight = 1'000'000;
constexpr std::uint32_t max_species_id = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t default_page_size = 32;

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

void fields(const Json& object, std::initializer_list<std::string_view> required,
            std::initializer_list<std::string_view> optional = {}) {
    require(object.is_object(), "Foliage plan requires JSON objects.");
    for (const auto field : required) require(object.contains(field), "Missing required foliage plan field.");
    for (auto it = object.begin(); it != object.end(); ++it) {
        require(std::find(required.begin(), required.end(), it.key()) != required.end() ||
                    std::find(optional.begin(), optional.end(), it.key()) != optional.end(),
                "Unknown foliage plan field.");
    }
}

std::string hex(const Json& value, std::size_t digits) {
    require(value.is_string(), "Foliage keys and digests must be hexadecimal strings.");
    const auto& text = value.get_ref<const std::string&>();
    require(text.size() == digits && text.find_first_not_of("0123456789abcdef") == std::string::npos,
            "Foliage keys and digests require fixed-width lowercase hexadecimal strings.");
    return text;
}

std::uint64_t hex_word(const std::string& value) {
    std::uint64_t result = 0;
    const auto decoded = std::from_chars(value.data(), value.data() + value.size(), result, 16);
    require(decoded.ec == std::errc{} && decoded.ptr == value.data() + value.size(), "Invalid foliage hex word.");
    return result;
}

std::uint32_t integer(const Json& value, std::uint32_t low, std::uint32_t high) {
    require(value.is_number_integer(), "Foliage count, cell, species and primitive fields must be integers.");
    const double count = value.get<double>();
    require(count >= low && count <= high, "Foliage integer out of range.");
    return static_cast<std::uint32_t>(count);
}

double number(const Json& value, double low, double high) {
    require(value.is_number(), "Foliage coordinates, density, spacing, slope and scale must be numbers.");
    const double result = value.get<double>();
    require(std::isfinite(result) && result >= low && result <= high, "Foliage number out of range.");
    return result == 0 ? 0. : result;
}

double optional_number(const Json& value, const char* field, double fallback, double low, double high) {
    return value.contains(field) ? number(value.at(field), low, high) : fallback;
}

void array_size(const Json& array, std::size_t low, std::size_t high) {
    require(array.is_array() && array.size() >= low && array.size() <= high, "Foliage array length out of range.");
}

std::string digest(std::string_view bytes) {
    return sha256(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

struct Recipe {
    ScatterArea area;
    ScatterRecipe scatter;
    std::vector<Species> species;
    std::map<std::uint64_t, std::vector<Variant>> variants;
    std::vector<Rectangle> exclusions;
    std::set<Cell> suppressed;
    Json canonical;
};

Recipe read_recipe(const Json& input) {
    fields(input, {"patch_key", "seed", "origin_m", "cell_size_m", "grid", "species"},
           {"density", "minimum_spacing_m", "exclude_rectangles_m", "suppressed_cells"});
    Recipe recipe;
    const auto patch = hex(input.at("patch_key"), 16);
    const auto seed = hex(input.at("seed"), 16);
    const auto& origin = input.at("origin_m");
    const auto& grid = input.at("grid");
    array_size(origin, 2, 2);
    array_size(grid, 2, 2);
    auto& area = recipe.area;
    area.id = hex_word(patch);
    require(area.id != 0, "Foliage patch key must be nonzero.");
    area.origin_x_m = number(origin[0], -max_coordinate, max_coordinate);
    area.origin_z_m = number(origin[1], -max_coordinate, max_coordinate);
    area.cell_size_m = number(input.at("cell_size_m"), min_cell_size, max_cell_size);
    area.columns = integer(grid[0], 1, max_plan_cells);
    area.rows = integer(grid[1], 1, max_plan_cells);
    require(std::uint64_t{area.columns} * area.rows <= max_plan_cells, "Foliage plan exceeds 4096 cells.");
    recipe.scatter = {hex_word(seed), optional_number(input, "density", 1, 0, 1),
        optional_number(input, "minimum_spacing_m", 0, 0, max_spacing)};
    require(recipe.scatter.minimum_spacing_m == 0 || recipe.scatter.minimum_spacing_m >= min_spacing,
            "Foliage spacing must be zero or at least 0.001 m.");

    const auto& species = input.at("species");
    array_size(species, 1, max_plan_species);
    std::size_t variant_count = 0;
    for (const auto& entry : species) {
        fields(entry, {"id", "variants"}, {"weight", "slope_degrees", "uniform_scale"});
        Species definition;
        definition.id = integer(entry.at("id"), 1, max_species_id);
        require(!recipe.variants.contains(definition.id), "Foliage species IDs must be unique.");
        if (entry.contains("weight")) definition.weight = integer(entry.at("weight"), 1, max_weight);
        if (entry.contains("slope_degrees")) {
            const auto& slopes = entry.at("slope_degrees");
            array_size(slopes, 2, 2);
            definition.minimum_slope_degrees = number(slopes[0], 0, 90);
            definition.maximum_slope_degrees = number(slopes[1], definition.minimum_slope_degrees, 90);
        }
        if (entry.contains("uniform_scale")) {
            const auto& scales = entry.at("uniform_scale");
            array_size(scales, 2, 2);
            definition.minimum_scale = number(scales[0], min_scale, max_scale);
            definition.maximum_scale = number(scales[1], definition.minimum_scale, max_scale);
        }
        const auto& variants = entry.at("variants");
        array_size(variants, 1, max_plan_variants - variant_count);
        variant_count += variants.size();
        definition.variation_count = static_cast<std::uint32_t>(variants.size());
        auto& definitions = recipe.variants[definition.id];
        for (const auto& asset : variants) {
            fields(asset, {"asset", "primitive"});
            definitions.emplace_back(hex(asset.at("asset"), 64), integer(asset.at("primitive"), 0, max_primitive));
        }
        recipe.species.push_back(definition);
    }
    std::sort(recipe.species.begin(), recipe.species.end(), [](const auto& a, const auto& b) { return a.id < b.id; });

    if (input.contains("exclude_rectangles_m")) {
        const auto& rectangles = input.at("exclude_rectangles_m");
        array_size(rectangles, 0, max_exclusions);
        for (const auto& rectangle : rectangles) {
            array_size(rectangle, 4, 4);
            Rectangle rect{};
            for (std::size_t i = 0; i < rect.size(); ++i) rect[i] = number(rectangle[i], -max_coordinate, max_coordinate);
            require(rect[0] < rect[2] && rect[1] < rect[3], "Foliage exclusion needs increasing rectangle bounds.");
            recipe.exclusions.push_back(rect);
        }
        std::sort(recipe.exclusions.begin(), recipe.exclusions.end());
        recipe.exclusions.erase(std::unique(recipe.exclusions.begin(), recipe.exclusions.end()), recipe.exclusions.end());
    }
    if (input.contains("suppressed_cells")) {
        const auto& suppressed = input.at("suppressed_cells");
        array_size(suppressed, 0, max_suppressions);
        for (const auto& entry : suppressed) {
            array_size(entry, 2, 2);
            const Cell cell{integer(entry[0], 0, area.columns - 1), integer(entry[1], 0, area.rows - 1)};
            require(recipe.suppressed.insert(cell).second, "Duplicate foliage cell suppression.");
        }
    }

    Json normalized_species = Json::array();
    for (const auto& definition : recipe.species) {
        Json variants = Json::array();
        for (const auto& [asset, primitive] : recipe.variants.at(definition.id))
            variants.push_back({{"asset", asset}, {"primitive", primitive}});
        normalized_species.push_back({{"id", definition.id}, {"weight", definition.weight},
            {"slope_degrees", {definition.minimum_slope_degrees, definition.maximum_slope_degrees}},
            {"uniform_scale", {definition.minimum_scale, definition.maximum_scale}}, {"variants", std::move(variants)}});
    }
    recipe.canonical = {{"patch_key", patch}, {"seed", seed}, {"origin_m", {area.origin_x_m, area.origin_z_m}},
        {"cell_size_m", area.cell_size_m}, {"grid", {area.columns, area.rows}}, {"density", recipe.scatter.density},
        {"minimum_spacing_m", recipe.scatter.minimum_spacing_m}, {"species", std::move(normalized_species)},
        {"exclude_rectangles_m", recipe.exclusions}, {"suppressed_cells", recipe.suppressed}};
    return recipe;
}

struct SurfaceCounts {
    std::size_t manual = 0;
    std::size_t rectangle = 0;
    std::size_t outside = 0;
};

std::optional<SurfaceSample> sample(const terrain::Heightfield& terrain, const Recipe& recipe,
                                   double x, double z, SurfaceCounts& counts) {
    // At the maximum world coordinate and minimum cell size, double precision
    // keeps the jittered point inside its generating cell even after rounding.
    const auto& area = recipe.area;
    const auto column = static_cast<std::uint32_t>(std::floor((x - area.origin_x_m) / area.cell_size_m));
    const auto row = static_cast<std::uint32_t>(std::floor((z - area.origin_z_m) / area.cell_size_m));
    if (recipe.suppressed.contains({column, row})) {
        ++counts.manual;
        return std::nullopt;
    }
    for (const auto& rect : recipe.exclusions) {
        if (x >= rect[0] && z >= rect[1] && x < rect[2] && z < rect[3]) {
            ++counts.rectangle;
            return std::nullopt;
        }
    }
    const auto hit = terrain.sample(x, z);
    if (!hit) {
        ++counts.outside;
        return std::nullopt;
    }
    return SurfaceSample{hit->height, std::clamp(hit->geometric_normal[1], 0., 1.)};
}
} // namespace

PinConflict::PinConflict(std::string pin_name, std::string expected_digest, std::string actual_digest)
    : std::runtime_error("Foliage plan pin changed."), pin(std::move(pin_name)),
      expected(std::move(expected_digest)), actual(std::move(actual_digest)) {}

nlohmann::json plan(const terrain::Heightfield& terrain, std::string_view terrain_sha256,
                    const nlohmann::json& params, const ValidateVariant& validate_variant) {
    fields(params, {"expected_terrain_recipe_sha256", "scatter"}, {"offset", "limit", "expected_plan_sha256"});
    const auto terrain_pin = hex(params.at("expected_terrain_recipe_sha256"), 64);
    if (terrain_pin != terrain_sha256)
        throw PinConflict("terrain_recipe_sha256", terrain_pin, std::string(terrain_sha256));
    const auto offset = params.contains("offset") ? integer(params.at("offset"), 0, max_plan_cells) : 0u;
    const auto limit = params.contains("limit") ? integer(params.at("limit"), 1, max_page_size) : default_page_size;
    const bool has_pin = params.contains("expected_plan_sha256");
    require(offset == 0 || has_pin, "A foliage continuation needs expected_plan_sha256.");
    const auto expected = has_pin ? hex(params.at("expected_plan_sha256"), 64) : std::string{};
    require(bool(validate_variant), "Foliage plan requires a cooked model admission callback.");

    const auto recipe = read_recipe(params.at("scatter"));
    SurfaceCounts surface{};
    const auto result = scatter(recipe.area, recipe.scatter, recipe.species,
        [&](double x, double z) { return sample(terrain, recipe, x, z, surface); });

    Json rows = Json::array();
    for (const auto& placement : result.instances) {
        const auto& [asset, primitive] = recipe.variants.at(placement.species).at(placement.variation);
        const auto half_yaw = placement.yaw_radians / 2;
        const auto scale = placement.uniform_scale;
        // Reuse the cell encoding as a logical key. Entity allocation is independent.
        rows.push_back({{"key", entity_id(placement.id)}, {"cell", {placement.id.column, placement.id.row}},
            {"species", placement.species}, {"variant", placement.variation},
            {"transform", {{"position", placement.position_m},
                {"rotation", {0, std::sin(half_yaw), 0, std::cos(half_yaw)}}, {"scale", {scale, scale, scale}}}},
            {"static_mesh", {{"asset", asset}, {"primitive", primitive}, {"visible", true}}}});
    }
    const Json counts = {{"cells", result.counts.cells}, {"placed", rows.size()},
        {"density_rejected", result.counts.density_rejected}, {"manual_suppressed", surface.manual},
        {"rectangle_excluded", surface.rectangle}, {"outside_terrain", surface.outside},
        {"slope_rejected", result.counts.slope_rejected}, {"spacing_rejected", result.counts.spacing_rejected}};
    const auto plan_hash = digest(Json{{"semantics", semantics}, {"terrain_recipe_sha256", terrain_pin},
        {"recipe", recipe.canonical}, {"counts", counts}, {"placements", rows}}.dump());
    if (has_pin && expected != plan_hash) throw PinConflict("plan_sha256", expected, plan_hash);
    require(offset <= rows.size(), "Foliage page offset exceeds placement count.");

    std::set<Variant> unique;
    for (const auto& [species, variants] : recipe.variants) {
        (void)species;
        unique.insert(variants.begin(), variants.end());
    }
    for (const auto& [asset, primitive] : unique) validate_variant(asset, primitive);

    const auto end = std::min(rows.size(), static_cast<std::size_t>(offset) + limit);
    Json page = Json::array();
    for (std::size_t i = offset; i < end; ++i) page.push_back(std::move(rows[i]));
    return {{"semantics", semantics}, {"terrain_recipe_sha256", terrain_pin}, {"plan_sha256", plan_hash},
        {"counts", counts}, {"placements", std::move(page)}, {"offset", offset},
        {"next_offset", end < rows.size() ? Json(end) : Json(nullptr)}};
}

Json authoring_schemas() {
    const auto object = [](Json properties, Json required) {
        return Json{{"type", "object"}, {"additionalProperties", false},
            {"properties", std::move(properties)}, {"required", std::move(required)}};
    };
    const auto array = [](Json item, std::size_t minimum, std::size_t maximum) {
        return Json{{"type", "array"}, {"items", std::move(item)}, {"minItems", minimum}, {"maxItems", maximum}};
    };
    const auto bounded = [](const char* type, auto minimum, auto maximum) {
        return Json{{"type", type}, {"minimum", minimum}, {"maximum", maximum}};
    };
    const Json digest = {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}};
    const Json word = {{"type", "string"}, {"pattern", "^[0-9a-f]{16}$"}};
    auto patch_key = word;
    patch_key["not"] = {{"const", "0000000000000000"}};
    patch_key["description"] = "Unique logical patch namespace. Placement keys are separate from authored entity IDs.";

    auto terrain = object({
        {"recipe", {{"type", "string"}, {"minLength", 1}, {"maxLength", local_session_request_limit},
            {"description", "Inline v1 heightfield JSON text. The entire request must fit the session message limit."}}},
        {"source", {{"type", "string"}, {"minLength", 1}, {"pattern", "\\.json$"},
            {"description", "Local heightfield recipe file. Draft paths are relative to the entry world directory."}}},
    }, Json::array());
    terrain["oneOf"] = Json::array({Json{{"required", {"recipe"}}}, Json{{"required", {"source"}}}});

    const auto variant = object({{"asset", digest}, {"primitive", bounded("integer", 0, max_primitive)}},
        {"asset", "primitive"});
    auto variants = array(variant, 1, max_plan_variants);
    variants["description"] = "Ordered unweighted cooked primitives. At most 32 variant entries across all species.";
    auto weight = bounded("integer", 1, max_weight);
    weight["default"] = 1;
    auto slope = array(bounded("number", 0, 90), 2, 2);
    slope["default"] = {0, 90};
    slope["description"] = "Inclusive [minimum, maximum] angle in degrees from horizontal.";
    auto scale = array(bounded("number", min_scale, max_scale), 2, 2);
    scale["default"] = {1, 1};
    scale["description"] = "Inclusive [minimum, maximum] uniform scale.";
    const auto species = object({{"id", bounded("integer", 1, max_species_id)}, {"weight", std::move(weight)},
            {"slope_degrees", std::move(slope)}, {"uniform_scale", std::move(scale)}, {"variants", std::move(variants)}},
        {"id", "variants"});
    auto species_list = array(species, 1, max_plan_species);
    species_list["description"] = "Species IDs must be unique within the patch.";

    const auto coordinate = bounded("number", -max_coordinate, max_coordinate);
    auto origin = array(coordinate, 2, 2);
    origin["description"] = "[x, z] origin in world metres. The entire grid must fit the coordinate bounds.";
    auto grid = array(bounded("integer", 1, max_plan_cells), 2, 2);
    grid["description"] = "[columns, rows]. The product may be at most 4096.";
    auto density = bounded("number", 0, 1);
    density["default"] = 1;
    const Json spacing = {{"anyOf", Json::array({Json{{"const", 0}}, bounded("number", min_spacing, max_spacing)})},
        {"default", 0}, {"description", "Horizontal minimum spacing in metres. Zero disables spacing."}};
    auto rectangles = array(array(coordinate, 4, 4), 0, max_exclusions);
    rectangles["default"] = Json::array();
    rectangles["description"] = "Rectangles [xmin, zmin, xmax, zmax] in world metres. Minimum edges inclusive, maximum edges exclusive.";
    const auto cell = array(bounded("integer", 0, max_plan_cells - 1), 2, 2);
    auto suppressed = array(cell, 0, max_suppressions);
    suppressed["default"] = Json::array();
    suppressed["uniqueItems"] = true;
    suppressed["description"] = "Zero-based [column, row] pairs inside the declared grid.";
    auto scatter = object({{"patch_key", std::move(patch_key)}, {"seed", word}, {"origin_m", std::move(origin)},
        {"cell_size_m", bounded("number", min_cell_size, max_cell_size)}, {"grid", std::move(grid)},
        {"density", std::move(density)}, {"minimum_spacing_m", spacing}, {"species", std::move(species_list)},
        {"exclude_rectangles_m", std::move(rectangles)}, {"suppressed_cells", std::move(suppressed)}},
        {"patch_key", "seed", "origin_m", "cell_size_m", "grid", "species"});

    auto offset = bounded("integer", 0, max_plan_cells);
    offset["default"] = 0;
    auto limit = bounded("integer", 1, max_page_size);
    limit["default"] = default_page_size;
    auto method = object({{"terrain", std::move(terrain)}, {"expected_terrain_recipe_sha256", digest},
        {"scatter", std::move(scatter)}, {"offset", std::move(offset)}, {"limit", std::move(limit)},
        {"expected_plan_sha256", digest}}, {"terrain", "expected_terrain_recipe_sha256", "scatter"});
    method["if"] = {{"required", {"offset"}}, {"properties", {{"offset", {{"minimum", 1}}}}}};
    method["then"] = {{"required", {"expected_plan_sha256"}}};
    method["description"] = "Plan revision-neutral foliage placements on a pinned heightfield. Returns row-major logical keys and standard Transform and StaticMesh values. Pin plan_sha256 on continuation.";
    return {{"world.foliage.plan", std::move(method)}};
}

} // namespace poima::foliage::authoring
