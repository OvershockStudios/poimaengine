// SPDX-License-Identifier: Apache-2.0
#include "water_asset.hpp"
#include "poima/local_session.hpp"
#include "poima/terrain_asset.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>

namespace poima::water {
namespace {
using Json = nlohmann::json;
constexpr auto artifact_format = "poima.water.region";

void require(bool valid, const char* message) {
    if (!valid) throw std::invalid_argument(message);
}

void fields(const Json& object, std::initializer_list<std::string_view> required,
    std::initializer_list<std::string_view> optional = {}) {
    require(object.is_object(), "Water configuration requires JSON objects.");
    for (auto key : required) require(object.contains(key), "Missing required water field.");
    for (const auto& [key, value] : object.items()) {
        (void)value;
        require(std::find(required.begin(), required.end(), key) != required.end() ||
            std::find(optional.begin(), optional.end(), key) != optional.end(), "Unknown water field.");
    }
}

std::string hex(const Json& value, std::size_t size) {
    require(value.is_string(), "Expected a hexadecimal water identity.");
    const auto& text = value.get_ref<const std::string&>();
    require(text.size() == size && text.find_first_not_of("0123456789abcdef") == std::string::npos,
        "Expected a lowercase hexadecimal water identity.");
    return text;
}

double number(const Json& value) {
    require(value.is_number(), "Water values must be finite numbers.");
    const double parsed = value.get<double>();
    require(std::isfinite(parsed), "Water values must be finite numbers.");
    return parsed == 0 ? 0.0 : parsed;
}

std::uint32_t integer(const Json& value, std::uint32_t low, std::uint32_t high) {
    require(value.is_number_integer(), "Water cell indices and dimensions must be integers.");
    const double parsed = number(value);
    require(parsed >= low && parsed <= high, "Water cell index or dimension out of bounds.");
    return static_cast<std::uint32_t>(parsed);
}

GridConfig parse_grid(const Json& object) {
    fields(object, {"cells_x", "cells_z", "cell_size_m", "origin_x_m", "origin_z_m"}, {"relaxation_per_second"});
    const auto rate = object.contains("relaxation_per_second") ? number(object.at("relaxation_per_second")) : 6.0;
    GridConfig grid{integer(object.at("cells_x"), 1, max_region_side), integer(object.at("cells_z"), 1, max_region_side),
        number(object.at("cell_size_m")), number(object.at("origin_x_m")), number(object.at("origin_z_m")), rate};
    require(grid.cell_size_m >= .01 && grid.cell_size_m <= 256, "Water cell size must be in 0.01..256 metres.");
    require(grid.relaxation_per_second >= 0 && grid.relaxation_per_second <= 12, "Water relaxation must be in 0..12 per second.");
    return grid;
}

Cell cell(const Json& value, const GridConfig& grid) {
    require(value.is_array() && value.size() == 2, "Water cells use [x, z] indices.");
    return {integer(value[0], 0, grid.cells_x - 1), integer(value[1], 0, grid.cells_z - 1)};
}

std::vector<double> samples(const Json& array, std::size_t count) {
    require(array.is_array() && (array.empty() || array.size() == count), "Water sample array must match the grid.");
    std::vector<double> values;
    values.reserve(count);
    for (const auto& item : array) values.push_back(number(item));
    return values;
}

RegionFace face(const Json& value, const GridConfig& grid) {
    fields(value, {"cell", "axis", "openness"}, {"sill_height_m", "action", "action_openness"});
    const auto& axis = value.at("axis");
    require(axis == "x" || axis == "z", "Water face axis must be x or z.");
    RegionFace result{cell(value.at("cell"), grid), axis == "x" ? FaceAxis::x : FaceAxis::z,
        {number(value.at("openness")), {}}, {}, 1};
    if (value.contains("sill_height_m") && !value.at("sill_height_m").is_null())
        result.initial.sill_height_m = number(value.at("sill_height_m"));
    if (value.contains("action")) {
        require(value.at("action").is_string(), "Water gate action must be a named input string.");
        result.action = value.at("action");
        require(!result.action.empty(), "Water gate action is empty.");
    }
    if (value.contains("action_openness")) result.action_openness = number(value.at("action_openness"));
    require(value.contains("action") || !value.contains("action_openness"), "Water action_openness needs an action.");
    return result;
}

void parse_rules(const Json& object, RegionDefinition& definition) {
    const auto read = [&](const char* field, auto limit, auto& result, const auto& convert) {
        if (!object.contains(field)) return;
        const auto& values = object.at(field);
        require(values.is_array() && values.size() <= limit, "Water face or exchange count exceeds its bound.");
        for (const auto& value : values) result.push_back(convert(value, definition.config));
    };
    read("faces", max_region_faces, definition.faces, face);
    read("exchanges", max_region_exchanges, definition.exchanges, [](const Json& value, const GridConfig& grid) {
        fields(value, {"cell", "source_m3_per_second", "drain_m3_per_second"});
        return CellExchange{cell(value.at("cell"), grid), number(value.at("source_m3_per_second")),
            number(value.at("drain_m3_per_second"))};
    });
    read("rain_cells", static_cast<std::size_t>(definition.config.cells_x) * definition.config.cells_z,
        definition.rain_cells, cell);
    std::ranges::sort(definition.rain_cells, {}, [](Cell cell) { return std::pair{cell.z, cell.x}; });
    std::sort(definition.faces.begin(), definition.faces.end(), [](const auto& left, const auto& right) {
        return std::tie(left.cell.z, left.cell.x, left.axis) < std::tie(right.cell.z, right.cell.x, right.axis);
    });
    std::sort(definition.exchanges.begin(), definition.exchanges.end(), [](const auto& left, const auto& right) {
        return std::tie(left.cell.z, left.cell.x) < std::tie(right.cell.z, right.cell.x);
    });
}

Json artifact_document(const RegionAsset& asset) {
    const auto& definition = asset.definition;
    const auto& grid = definition.config;
    auto tiles = Json::array();
    for (const auto& tile : asset.tiles) tiles.push_back({{"entity", tile.entity}, {"asset", tile.asset}});
    auto faces = Json::array();
    for (const auto& face : definition.faces) {
        Json row{{"cell", {face.cell.x, face.cell.z}}, {"axis", face.axis == FaceAxis::x ? "x" : "z"},
            {"openness", face.initial.openness}, {"sill_height_m", face.initial.sill_height_m ? Json(*face.initial.sill_height_m) : Json(nullptr)}};
        if (!face.action.empty()) { row["action"] = face.action;row["action_openness"] = face.action_openness; }
        faces.push_back(std::move(row));
    }
    auto exchanges = Json::array();
    for (const auto& exchange : definition.exchanges) exchanges.push_back({{"cell", {exchange.cell.x, exchange.cell.z}},
        {"source_m3_per_second", exchange.source_m3_per_second}, {"drain_m3_per_second", exchange.drain_m3_per_second}});
    Json document{{"format", artifact_format}, {"version", definition.rain_cells.empty() ? 1 : 2},
        {"terrain_recipe_sha256", asset.terrain_recipe_sha256},
        {"tiles", std::move(tiles)}, {"grid", {{"cells_x", grid.cells_x}, {"cells_z", grid.cells_z}, {"cell_size_m", grid.cell_size_m},
            {"origin_x_m", grid.origin_x_m}, {"origin_z_m", grid.origin_z_m}, {"relaxation_per_second", grid.relaxation_per_second}}},
        {"floor_heights_m", definition.floor_heights_m}, {"initial_depths_m", definition.initial_depths_m},
        {"faces", std::move(faces)}, {"exchanges", std::move(exchanges)}};
    if (!definition.rain_cells.empty()) {
        auto rain_cells = Json::array();
        for (const auto cell : definition.rain_cells)
            rain_cells.push_back({cell.x, cell.z});
        document["rain_cells"] = std::move(rain_cells);
    }
    return document;
}

std::string digest(std::string_view data) {
    return sha256(std::as_bytes(std::span(data.data(), data.size())));
}

std::vector<double> sample_floor(const GridConfig& grid, const terrain::Heightfield& terrain) {
    for (const auto row : {0u, grid.cells_z}) for (const auto column : {0u, grid.cells_x}) {
        const double x = grid.origin_x_m + column * grid.cell_size_m;
        const double z = grid.origin_z_m + row * grid.cell_size_m;
        require(terrain.sample(x, z).has_value(), "Water region extends beyond the rounded terrain surface.");
    }
    std::vector<double> heights;
    heights.reserve(static_cast<std::size_t>(grid.cells_x) * grid.cells_z);
    for (std::uint32_t row = 0; row < grid.cells_z; ++row) for (std::uint32_t column = 0; column < grid.cells_x; ++column) {
        const double x = grid.origin_x_m + (column + 0.5) * grid.cell_size_m;
        const double z = grid.origin_z_m + (row + 0.5) * grid.cell_size_m;
        heights.push_back(terrain.sample(x, z).value().height);
    }
    return heights;
}
} // namespace

std::string compile_region_asset(const Json& parameters, const terrain::Heightfield& terrain,
    std::string_view terrain_sha256, const std::vector<TileBinding>& tiles) {
    RegionAsset asset;
    asset.terrain_recipe_sha256 = terrain_sha256;
    asset.tiles = tiles;
    auto& definition = asset.definition;
    definition.config = parse_grid(parameters.at("grid"));
    definition.floor_heights_m = sample_floor(definition.config, terrain);
    if (parameters.contains("initial_depths_m"))
        definition.initial_depths_m = samples(parameters.at("initial_depths_m"), definition.floor_heights_m.size());
    parse_rules(parameters, definition);
    auto bytes = artifact_document(asset).dump();
    require(bytes.size() <= max_region_asset_bytes, "Water region asset exceeds its byte budget.");
    definition.plan_sha256 = digest(bytes);
    (void)Region(definition);
    return bytes;
}

RegionAsset decode_region_asset(std::string_view bytes) {
    require(!bytes.empty() && bytes.size() <= max_region_asset_bytes, "Water region asset exceeds its byte budget.");
    const auto object = Json::parse(bytes, [](int depth, Json::parse_event_t, const Json&) {
        require(depth <= 8, "Water asset nesting exceeds its bound.");
        return true;
    });
    fields(object, {"format", "version", "terrain_recipe_sha256", "tiles", "grid", "floor_heights_m",
        "initial_depths_m", "faces", "exchanges"}, {"rain_cells"});
    require(object.at("format") == artifact_format &&
        integer(object.at("version"), 1, 2) == (object.contains("rain_cells") ? 2u : 1u),
        "Unsupported water asset format.");
    RegionAsset asset;
    asset.terrain_recipe_sha256 = hex(object.at("terrain_recipe_sha256"), 64);
    auto& definition = asset.definition;
    definition.config = parse_grid(object.at("grid"));
    const auto count = static_cast<std::size_t>(definition.config.cells_x) * definition.config.cells_z;
    definition.floor_heights_m = samples(object.at("floor_heights_m"), count);
    require(definition.floor_heights_m.size() == count, "Water asset is missing floor samples.");
    definition.initial_depths_m = samples(object.at("initial_depths_m"), count);
    parse_rules(object, definition);
    const auto& tiles = object.at("tiles");
    require(tiles.is_array() && !tiles.empty() && tiles.size() <= terrain::max_cook_tiles, "Water asset requires 1..64 terrain tiles.");
    std::set<std::string> tile_entities;
    for (const auto& tile : tiles) {
        fields(tile, {"entity", "asset"});
        auto entity = hex(tile.at("entity"), 32);
        require(tile_entities.insert(entity).second, "Water terrain entity IDs must be unique.");
        asset.tiles.push_back({std::move(entity), hex(tile.at("asset"), 64)});
    }
    require(artifact_document(asset).dump() == bytes, "Water asset is not canonically encoded.");
    definition.plan_sha256 = digest(bytes);
    (void)Region(definition);
    return asset;
}
namespace {
Json object(Json properties, Json required) {
    return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}};
}
Json array(Json item, std::size_t minimum, std::size_t maximum) {
    return {{"type", "array"}, {"items", std::move(item)}, {"minItems", minimum}, {"maxItems", maximum}};
}
Json range(const char* type, double minimum, double maximum) {
    return {{"type", type}, {"minimum", minimum}, {"maximum", maximum}};
}
const Json sha_schema = {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}};
const Json id_schema = {{"type", "string"}, {"pattern", "^[0-9a-f]{32}$"}};
const Json tick_schema = {{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991ULL}};
}

Json region_authoring_schemas() {
    auto source = object({{"recipe", {{"type", "string"}, {"minLength", 1}, {"maxLength", local_session_request_limit}}},
        {"source", {{"type", "string"}, {"pattern", "\\.json$"}, {"description", "Relative to the entry world directory in an MCP draft."}}}},
        Json::array());
    source["oneOf"] = Json::array({Json{{"required", {"recipe"}}}, Json{{"required", {"source"}}}});
    const auto axis_count = range("integer", 1, max_region_side);
    const auto coordinate = range("number", -1'000'000, 1'000'000);
    const auto cell = array(range("integer", 0, max_region_side - 1), 2, 2);
    const auto unit = range("number", 0, 1);
    const auto rate = range("number", 0, 1'000'000);
    const auto grid = object({{"cells_x", axis_count}, {"cells_z", axis_count}, {"cell_size_m", range("number", .01, 256)},
        {"origin_x_m", coordinate}, {"origin_z_m", coordinate}, {"relaxation_per_second", range("number", 0, 12)}},
        {"cells_x", "cells_z", "cell_size_m", "origin_x_m", "origin_z_m"});
    auto faces = object({{"cell", cell}, {"axis", {{"enum", {"x", "z"}}}}, {"openness", unit},
        {"sill_height_m", {{"anyOf", Json::array({coordinate, Json{{"type", "null"}}})}}},
        {"action", {{"type", "string"}, {"pattern", "^[a-z][a-z0-9_.-]{0,63}$"}}}, {"action_openness", unit}},
        {"cell", "axis", "openness"});
    faces["dependentRequired"] = {{"action_openness", Json::array({"action"})}};
    const auto exchange = object({{"cell", cell}, {"source_m3_per_second", rate}, {"drain_m3_per_second", rate}},
        {"cell", "source_m3_per_second", "drain_m3_per_second"});
    auto plan = object({{"revision", tick_schema}, {"terrain", std::move(source)}, {"expected_terrain_recipe_sha256", sha_schema},
        {"tile_entities", array(id_schema, 1, terrain::max_cook_tiles)}, {"grid", grid},
        {"initial_depths_m", array(range("number", 0, 1'000'000), 0, max_region_side * max_region_side)},
        {"faces", array(std::move(faces), 0, max_region_faces)}, {"exchanges", array(exchange, 0, max_region_exchanges)},
        {"rain_cells", array(cell, 0, max_region_side * max_region_side)}},
        {"revision", "terrain", "expected_terrain_recipe_sha256", "tile_entities", "grid"});
    plan["properties"]["rain_cells"]["uniqueItems"] = true;
    plan["properties"]["rain_cells"]["description"] =
        "Cells designated to collect precipitation. Duplicate or out-of-grid cells are invalid. Omit or leave empty for no precipitation.";
    plan["description"] = "Plan one local water region on the ordered root terrain tile entities. Samples the reviewed terrain beds without publishing an asset.";
    auto cook = plan;
    cook["properties"]["expected_plan_sha256"] = sha_schema;
    cook["required"].push_back("expected_plan_sha256");
    cook["description"] = "Publish only the reviewed immutable .pwater region. Bind the resulting plan hash with water.set in a guarded world transaction.";
    return {{"world.water.plan", std::move(plan)}, {"world.water.cook", std::move(cook)}};
}

Json region_runtime_schema() {
    auto result = object({{"session_id", id_schema}, {"tick", tick_schema},
        {"cells", array(array(range("integer", 0, max_region_side - 1), 2, 2), 0, 64)}}, {"session_id", "tick"});
    result["description"] = "Read the region identity, latest volume receipt, authored face controls and up to 64 addressed cells at a committed tick. Rain-enabled regions also report catchment cells and per-step and cumulative rain supply.";
    return result;
}
} // namespace poima::water
