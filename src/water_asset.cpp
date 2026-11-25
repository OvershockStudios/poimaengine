// SPDX-License-Identifier: Apache-2.0
#include "water_asset.hpp"
#include "poima/local_session.hpp"
#include "poima/terrain_asset.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace poima::water {
namespace {
using Json = nlohmann::json;
constexpr auto artifact_format = "poima.water.region";

std::uint32_t artifact_version(const RegionDefinition& definition) noexcept {
    if (definition.storage) return 19;
    if (definition.body_displacement) return 18;
    if (definition.surface_wakes) return 17;
    if (definition.currents) return 16;
    if (definition.optics && definition.optics->solar_scattering) return 15;
    if (definition.optics && definition.optics->reflection_roughness) return 14;
    if (definition.swell_variation) return 13;
    if (definition.optics && definition.optics->offshore) return 12;
    if (!definition.surface_swell.empty()) return 11;
    if (definition.optics) {
        if (definition.optics->follow_terrain) return 10;
        if (definition.optics->shore_foam) return 9;
        if (definition.optics->caustics_strength>0) return 8;
        return definition.optics->refraction ? 7 : 6;
    }
    if (definition.listener_audio) return 5;
    if (!definition.surface_wave_layers.empty()) return 4;
    if (definition.surface_ripples) return 3;
    return definition.rain_cells.empty() ? 1 : 2;
}

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

SurfaceRipples parse_surface_ripples(const Json& ripples) {
    fields(ripples, {"wavelength_m", "slope", "period_ticks", "heading_degrees"});
    require(ripples.at("period_ticks").is_number_integer(),
        "Water surface ripple period_ticks must be an integer.");
    return {number(ripples.at("wavelength_m")), number(ripples.at("slope")),
        integer(ripples.at("period_ticks"), 8, 3600), number(ripples.at("heading_degrees"))};
}

StorageDefinition parse_storage(const Json& value, const GridConfig& grid) {
    fields(value, {"reservoirs", "ports"});
    const auto& tanks = value.at("reservoirs");
    const auto& ports = value.at("ports");
    require(tanks.is_array() && !tanks.empty() && tanks.size() <= max_water_reservoirs &&
        ports.is_array() && !ports.empty() && ports.size() <= max_water_ports,
        "The water storage reservoir or port count is outside its bounds.");
    StorageDefinition result;
    for (const auto& tank : tanks) {
        fields(tank, {"id", "floor_height_m", "area_m2", "capacity_m3", "initial_volume_m3"});
        require(tank.at("id").is_string(), "Water reservoirs need a named ID.");
        result.reservoirs.push_back({tank.at("id"), number(tank.at("floor_height_m")),
            number(tank.at("area_m2")), number(tank.at("capacity_m3")), number(tank.at("initial_volume_m3"))});
    }
    const auto endpoint = [&](const Json& raw) -> StorageEndpoint {
        fields(raw, {}, {"cell", "reservoir"});
        require(raw.size() == 1, "Water storage endpoints require exactly one cell or reservoir.");
        if (raw.contains("cell")) return cell(raw.at("cell"), grid);
        require(raw.at("reservoir").is_string(), "A water reservoir reference must be a named ID.");
        return raw.at("reservoir").get<std::string>();
    };
    for (const auto& port : ports) {
        fields(port, {"id", "from", "to", "sill_height_m", "conductance_m2_s", "max_flow_m3_s", "openness"},
            {"action", "action_openness"});
        require(port.at("id").is_string(), "Water storage ports need a named ID.");
        StoragePortDefinition rule{port.at("id"), endpoint(port.at("from")), endpoint(port.at("to")),
            number(port.at("sill_height_m")), number(port.at("conductance_m2_s")), number(port.at("max_flow_m3_s")),
            number(port.at("openness")), {}};
        if (port.contains("action")) {
            require(port.at("action").is_string() && !port.at("action").get_ref<const std::string&>().empty(),
                "Water storage gate action must be a named input string.");
            rule.action = port.at("action");
        }
        if (port.contains("action_openness")) {
            require(!rule.action.empty(), "Water storage action_openness needs an action.");
            rule.action_openness = number(port.at("action_openness"));
        }
        result.ports.push_back(std::move(rule));
    }
    std::ranges::sort(result.reservoirs, {}, &ReservoirDefinition::id);
    std::ranges::sort(result.ports, {}, &StoragePortDefinition::id);
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
    if (object.contains("surface_ripples"))
        definition.surface_ripples = parse_surface_ripples(object.at("surface_ripples"));
    if (object.contains("surface_wave_layers")) {
        const auto& layers = object.at("surface_wave_layers");
        require(layers.is_array() && !layers.empty() && layers.size() <= max_surface_wave_layers,
            "Water expects one to four surface wave layers.");
        for (const auto& layer : layers)
            definition.surface_wave_layers.push_back(parse_surface_ripples(layer));
    }
    if (object.contains("surface_swell")) {
        const auto& layers=object.at("surface_swell");
        require(layers.is_array() && !layers.empty() && layers.size()<=max_surface_wave_layers,
            "Water expects one to four swell layers.");
        for (const auto& layer : layers)
            definition.surface_swell.push_back(parse_surface_ripples(layer));
    }
    if (object.contains("swell_variation")) {
        const auto& profile=object.at("swell_variation");
        fields(profile,{"scale_m","strength","seed"});
        definition.swell_variation=WaterSwellVariation{number(profile.at("scale_m")),number(profile.at("strength")),
            integer(profile.at("seed"),0,std::numeric_limits<std::uint32_t>::max())};
    }
    if (object.contains("listener_audio")) {
        const auto& value = object.at("listener_audio");
        fields(value, {"cutoff_hz", "gain", "full_depth_m"});
        definition.listener_audio = ListenerAudio{number(value.at("cutoff_hz")),
            number(value.at("gain")), number(value.at("full_depth_m"))};
    }
    if (object.contains("currents")) {
        const auto& profile=object.at("currents");
        fields(profile,{"max_speed_m_s","min_depth_m"});
        definition.currents=WaterCurrents{number(profile.at("max_speed_m_s")),number(profile.at("min_depth_m"))};
    }
    if (object.contains("surface_wakes")) {
        const auto& profile=object.at("surface_wakes");
        fields(profile,{"speed_m_s","damping_per_second","max_amplitude_m","body_gain"});
        definition.surface_wakes=SurfaceWakes{number(profile.at("speed_m_s")),number(profile.at("damping_per_second")),
            number(profile.at("max_amplitude_m")),number(profile.at("body_gain"))};
    }
    if (object.contains("body_displacement")) {
        const auto& profile=object.at("body_displacement");
        fields(profile,{"max_rise_m"});
        definition.body_displacement=BodyDisplacement{number(profile.at("max_rise_m"))};
    }
    if (object.contains("storage")) definition.storage = parse_storage(object.at("storage"), definition.config);
    if (object.contains("optics")) {
        const auto& value=object.at("optics");
        fields(value, {"extinction_per_m","scattered_radiance"},
            {"refraction","caustics_strength","shore_foam","follow_terrain","offshore","reflection_roughness","solar_scattering"});
        if (value.contains("refraction"))
            require(value.at("refraction").is_boolean(),"Water optics refraction must be Boolean.");
        if (value.contains("follow_terrain"))
            require(value.at("follow_terrain").is_boolean(),"Water optics follow_terrain must be Boolean.");
        const auto color=[&](const char* name, double maximum) {
            const auto& array=value.at(name);
            require(array.is_array() && array.size()==3,"Water optics use three RGB coefficients.");
            std::array<double,3> result{};
            for (std::size_t channel=0;channel<result.size();++channel) {
                const double sample=number(array[channel]);
                require(sample>=0 && sample<=maximum,"Water optics coefficient is out of bounds.");
                result[channel]=sample;
            }
            return result;
        };
        definition.optics=WaterOptics{color("extinction_per_m",8),color("scattered_radiance",16),
            value.value("refraction",false),value.contains("caustics_strength") ? number(value.at("caustics_strength")) : 0};
        definition.optics->follow_terrain=value.value("follow_terrain",false);
        if (value.contains("reflection_roughness"))
            definition.optics->reflection_roughness=number(value.at("reflection_roughness"));
        if (value.contains("solar_scattering")) {
            const auto strength=number(value.at("solar_scattering"));
            require(strength>=0 && strength<=1,"Water solar_scattering must be in [0,1].");
            if (strength>0) definition.optics->solar_scattering=strength;
        }
        if (value.contains("shore_foam")) {
            const auto& foam = value.at("shore_foam");
            fields(foam, {"depth_m","opacity"});
            definition.optics->shore_foam = WaterShoreFoam{number(foam.at("depth_m")),number(foam.at("opacity"))};
        }
        if (value.contains("offshore")) {
            const auto& sea=value.at("offshore");
            fields(sea,{"edge","surface_height_m","depth_m"});
            constexpr std::array<std::string_view,4> names{"min_z","max_x","max_z","min_x"};
            require(sea.at("edge").is_string(),"Offshore edge must be a string.");
            const auto edge=std::ranges::find(names,sea.at("edge").get_ref<const std::string&>());
            require(edge!=names.end(),"Offshore edge must be min_z, max_x, max_z or min_x.");
            definition.optics->offshore=WaterOffshore{static_cast<WaterEdge>(edge-names.begin()),
                number(sea.at("surface_height_m")),number(sea.at("depth_m"))};
        }
    }
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
    Json document{{"format", artifact_format},
        {"version", artifact_version(definition)},
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
    if (definition.surface_ripples)
        document["surface_ripples"] = surface_ripples_json(*definition.surface_ripples);
    if (!definition.surface_wave_layers.empty())
        document["surface_wave_layers"] = surface_wave_layers_json(definition.surface_wave_layers);
    if (!definition.surface_swell.empty())
        document["surface_swell"] = surface_wave_layers_json(definition.surface_swell);
    if (definition.swell_variation)
        document["swell_variation"] = swell_variation_json(*definition.swell_variation);
    if (definition.listener_audio)
        document["listener_audio"] = listener_audio_json(*definition.listener_audio);
    if (!definition.shore_corner_heights_m.empty())
        document["shore_corner_heights_m"] = definition.shore_corner_heights_m;
    if (definition.optics)
        document["optics"] = optics_json(*definition.optics);
    if (definition.currents)
        document["currents"] = currents_json(*definition.currents);
    if (definition.surface_wakes)
        document["surface_wakes"] = surface_wakes_json(*definition.surface_wakes);
    if (definition.body_displacement)
        document["body_displacement"] = body_displacement_json(*definition.body_displacement);
    if (definition.storage) document["storage"] = storage_definition_json(*definition.storage);
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
std::vector<double> sample_corners(const GridConfig& grid,const terrain::Heightfield& terrain) {
    std::vector<double> heights;
    heights.reserve(static_cast<std::size_t>(grid.cells_x+1)*(grid.cells_z+1));
    for (std::uint32_t z=0;z<=grid.cells_z;++z) for (std::uint32_t x=0;x<=grid.cells_x;++x) {
        const auto sample=terrain.sample(grid.origin_x_m+x*grid.cell_size_m,grid.origin_z_m+z*grid.cell_size_m);
        require(sample.has_value(),"Water shoreline corner is outside the rounded terrain.");
        heights.push_back(sample->height==0 ? 0.0 : sample->height);
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
    if (definition.optics && definition.optics->follow_terrain)
        definition.shore_corner_heights_m=sample_corners(definition.config,terrain);
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
        "initial_depths_m", "faces", "exchanges"}, {"rain_cells", "surface_ripples", "surface_wave_layers", "surface_swell",
        "swell_variation", "listener_audio", "optics", "shore_corner_heights_m", "currents", "surface_wakes", "body_displacement", "storage"});
    require(object.at("format") == artifact_format, "Unsupported water asset format.");
    const auto version=integer(object.at("version"),1,19);
    RegionAsset asset;
    asset.terrain_recipe_sha256 = hex(object.at("terrain_recipe_sha256"), 64);
    auto& definition = asset.definition;
    definition.config = parse_grid(object.at("grid"));
    const auto count = static_cast<std::size_t>(definition.config.cells_x) * definition.config.cells_z;
    definition.floor_heights_m = samples(object.at("floor_heights_m"), count);
    require(definition.floor_heights_m.size() == count, "Water asset is missing floor samples.");
    definition.initial_depths_m = samples(object.at("initial_depths_m"), count);
    parse_rules(object, definition);
    if (object.contains("shore_corner_heights_m"))
        definition.shore_corner_heights_m=samples(object.at("shore_corner_heights_m"),
            static_cast<std::size_t>(definition.config.cells_x+1)*(definition.config.cells_z+1));
    require(version==artifact_version(definition),"Unsupported water asset format.");
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
    auto ripples = object({{"wavelength_m", range("number", .10, 8.0)}, {"slope", range("number", 0, .4)},
        {"period_ticks", range("integer", 8, 3600)}, {"heading_degrees", range("number", -180, 180)}},
        {"wavelength_m", "slope", "period_ticks", "heading_degrees"});
    ripples["description"] = "Optional water surface normal ripples driven by committed simulation ticks. Heading zero follows world +X and positive headings rotate toward +Z.";
    auto layers = array(ripples, 1, max_surface_wave_layers);
    layers["description"] = "Ordered water shading waves with independent headings and periods. Combined slope must be at most 0.4. Choose either surface_wave_layers or surface_ripples.";
    auto swell_wave=ripples;
    swell_wave.erase("description");
    swell_wave["properties"]["wavelength_m"]["maximum"]=1024;
    swell_wave["properties"]["slope"].erase("minimum");
    swell_wave["properties"]["slope"]["exclusiveMinimum"]=0;
    auto swell=array(std::move(swell_wave),1,max_surface_wave_layers);
    swell["description"]="Displace the fitted water surface at committed ticks. Wavelength must span at least four cells. Total slope is at most 0.4 and total amplitude at most half a cell or eight metres. Existing normal-only waves can be layered over it.";
    auto variation=object({{"scale_m",range("number",.1,65'536)},
        {"strength",{{"type","number"},{"exclusiveMinimum",0},{"maximum",1}}},
        {"seed",range("integer",0,std::numeric_limits<std::uint32_t>::max())}},{"scale_m","strength","seed"});
    variation["description"]="Vary swell amplitude and crest position using seeded spatial patches. Requires surface_swell, a scale of at least eight cells, and at least four cells per resulting local wavelength. The warped slope bound remains 0.4.";
    auto listener_audio = object({{"cutoff_hz", range("number", 80, 8000)}, {"gain", unit},
        {"full_depth_m", range("number", .01, 10)}}, {"cutoff_hz", "gain", "full_depth_m"});
    listener_audio["description"] = "Low-pass and gain applied to the spatial mix when the listener enters the committed water column. Blend reaches full strength at full_depth_m.";
    auto optics = object({{"extinction_per_m",array(range("number",0,8),3,3)},
        {"scattered_radiance",array(range("number",0,16),3,3)},
        {"refraction",{{"type","boolean"},{"default",false},
            {"description","Bend the sightline at the first water interface and sample a matching opaque depth hit."}}},
        {"follow_terrain",{{"type","boolean"},{"default",false},
            {"description","Fit the optical bed and listener submersion to sampled terrain, respecting dry cells and gate controls."}}},
        {"caustics_strength",range("number",0,1)},
        {"reflection_roughness",range("number",0,1)},
        {"solar_scattering",range("number",0,1)},
        {"offshore",object({{"edge",{{"enum",{"min_z","max_x","max_z","min_x"}}}},
            {"surface_height_m",coordinate},{"depth_m",range("number",.1,10'000)}},
            {"edge","surface_height_m","depth_m"})},
        {"shore_foam",object({{"depth_m",range("number",.02,3)},{"opacity",range("number",0,1)}},
            {"depth_m","opacity"})}},
        {"extinction_per_m","scattered_radiance"});
    optics["description"] = "Optional transparent surface and depth-dependent RGB extinction and in-scattering. Requires single-sample Vulkan color for optical composition.";
    optics["properties"]["caustics_strength"]["description"] =
        "Focus light from the linked sky sun below the surface using the authored wave normals. Zero disables the effect.";
    optics["properties"]["reflection_roughness"]["description"] =
        "Perceptual roughness for above-water sky reflections when environment IBL is enabled. A selected value adds unresolved wave slope variance at distance.";
    optics["properties"]["solar_scattering"]["description"] =
        "Blend the water's scattered radiance between its authored value and that value scaled by the committed solar sky gain. Zero and omission cook the same bytes.";
    optics["properties"]["shore_foam"]["description"] =
        "Add surface foam to shallow committed water. Depth is measured vertically in metres. Opacity zero keeps the selected profile with a clear surface.";
    optics["properties"]["offshore"]["description"] =
        "Extend water across the half-plane beyond one fitted grid side. Its surface and bed are authored elevations independent of the conserved nearshore grid.";
    const Json fitted={{"required",{"follow_terrain"}},{"properties",{{"follow_terrain",{{"const",true}}}}}};
    optics["if"]={{"required",{"offshore"}}};
    optics["then"]=fitted;
    const auto currents=object({{"max_speed_m_s",range("number",.1,30)},{"min_depth_m",range("number",.001,1)}},
        {"max_speed_m_s","min_depth_m"});
    const auto wakes=object({{"speed_m_s",range("number",.1,30)},{"damping_per_second",range("number",.5,20)},
        {"max_amplitude_m",range("number",.001,.25)},{"body_gain",{{"type","number"},{"exclusiveMinimum",0},{"maximum",1}}}},
        {"speed_m_s","damping_per_second","max_amplitude_m","body_gain"});
    const auto displacement=object({{"max_rise_m",range("number",.01,4)}},{"max_rise_m"});
    const Json storage_id{{"type", "string"}, {"pattern", "^[a-z][a-z0-9_.-]{0,63}$"}};
    const auto tank = object({{"id", storage_id}, {"floor_height_m", coordinate},
        {"area_m2", range("number", .01, max_reservoir_area_m2)},
        {"capacity_m3", range("number", .001, max_reservoir_capacity_m3)},
        {"initial_volume_m3", range("number", 0, max_reservoir_capacity_m3)}},
        {"id", "floor_height_m", "area_m2", "capacity_m3", "initial_volume_m3"});
    const auto tank_endpoint = object({{"reservoir", storage_id}}, {"reservoir"});
    const Json storage_endpoint{{"oneOf", Json::array({object({{"cell", cell}}, {"cell"}), tank_endpoint})}};
    const Json storage_rate{{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", max_storage_rate}};
    auto port = object({{"id", storage_id}, {"from", storage_endpoint}, {"to", storage_endpoint},
        {"sill_height_m", coordinate}, {"conductance_m2_s", storage_rate}, {"max_flow_m3_s", storage_rate},
        {"openness", unit}, {"action", storage_id}, {"action_openness", unit}},
        {"id", "from", "to", "sill_height_m", "conductance_m2_s", "max_flow_m3_s", "openness"});
    port["anyOf"] = Json::array({Json{{"properties", {{"from", tank_endpoint}}}},
        Json{{"properties", {{"to", tank_endpoint}}}}});
    port["dependentRequired"] = {{"action_openness", {"action"}}};
    auto storage = object({{"reservoirs", array(tank, 1, max_water_reservoirs)},
        {"ports", array(port, 1, max_water_ports)}}, {"reservoirs", "ports"});
    storage["description"] = "Finite fixed-area reservoirs and gated, bidirectional transfers to reservoirs or terrain cells.";
    auto plan = object({{"revision", tick_schema}, {"terrain", std::move(source)}, {"expected_terrain_recipe_sha256", sha_schema},
        {"tile_entities", array(id_schema, 1, terrain::max_cook_tiles)}, {"grid", grid},
        {"initial_depths_m", array(range("number", 0, 1'000'000), 0, max_region_side * max_region_side)},
        {"faces", array(std::move(faces), 0, max_region_faces)}, {"exchanges", array(exchange, 0, max_region_exchanges)},
        {"rain_cells", array(cell, 0, max_region_side * max_region_side)}, {"surface_ripples", std::move(ripples)},
        {"surface_wave_layers", std::move(layers)}, {"surface_swell",std::move(swell)}, {"swell_variation",std::move(variation)},
        {"listener_audio", std::move(listener_audio)}, {"optics",std::move(optics)},{"currents",currents},
        {"surface_wakes",wakes},{"body_displacement",displacement},{"storage",std::move(storage)}},
        {"revision", "terrain", "expected_terrain_recipe_sha256", "tile_entities", "grid"});
    plan["not"] = {{"required", {"surface_ripples", "surface_wave_layers"}}};
    plan["if"]={{"anyOf",{Json{{"required",{"surface_swell"}}},Json{{"required",{"surface_wakes"}}}}}};
    plan["then"]={{"required",{"optics"}},{"properties",{{"optics",fitted}}}};
    plan["dependentRequired"]={{"swell_variation",{"surface_swell"}}};
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
    const auto endpoint=array(range("number",-1e6,1e6),3,3);
    const auto cell=array(range("integer",0,max_region_side-1),2,2);
    const auto current_face=object({{"cell",cell},{"axis",{{"enum",{"x","z"}}}}},{"cell","axis"});
    auto result = object({{"session_id", id_schema}, {"tick", tick_schema},
        {"cells", array(cell, 0, 64)},
        {"current_faces",array(current_face,0,64)},
        {"points", array(array(Json{{"type", "number"}}, 3, 3), 0, 64)},
        {"segments", array(object({{"from_world",endpoint},{"to_world",endpoint}},
            {"from_world","to_world"}),0,64)},
        {"bodies",array(id_schema,0,max_buoyant_bodies)},{"structure_revision",tick_schema}}, {"session_id", "tick"});
    result["dependentRequired"]={{"structure_revision",{"bodies"}}};
    result["description"] = "Read the region identity, latest volume receipt, authored face controls and up to 64 addressed cells at a committed tick."
        " Rain-enabled regions also report catchment cells and per-step and cumulative rain supply."
        " A selected surface_ripples profile is reported even at slope zero."
        " Selected surface_wave_layers, surface_swell, swell_variation and optics are reported when authored."
        " Optional points in world [x,y,z] order report the containing column and the submersion depth."
        " Wet point columns in a swell region include the geometric surface_normal. Cell volumes follow the conserved grid."
        " Optional segments return the first top-surface entry for each world-space sweep, in request order."
        " Optional bodies report current displaced volume, center and lift. Guard changed membership with structure_revision."
        " Current-enabled regions expose velocity at sampled wet points and buoyant bodies. Use current_faces for signed face discharge."
        " Regions with surface_wakes also report each selected cell's height offset and vertical increment."
        " Regions with body_displacement report saved occupancy and its contribution to the water stage."
        " Storage reports current reservoir volumes, surface elevations, gate openings and signed discharges."
        " The total volume includes both stored and terrain water.";
    return result;
}
} // namespace poima::water
