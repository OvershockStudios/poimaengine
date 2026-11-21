// SPDX-License-Identifier: Apache-2.0
#include "poima/terrain_asset.hpp"
#include "model_import.hpp"
#include "navigation_geometry.hpp"
#include "poima/runtime.hpp"
#include <bit>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
using namespace poima::terrain;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Operation>
void rejects(Operation operation, std::string_view reason) {
    try { operation(); }
    catch (const std::invalid_argument& error) {
        if (std::string_view(error.what()).find(reason) != std::string_view::npos) return;
        throw std::runtime_error(std::string("Unexpected terrain rejection: ") + error.what());
    }
    throw std::runtime_error("Terrain cook accepted an invalid recipe.");
}

Json recipe(std::uint32_t x, std::uint32_t z, std::uint32_t tile_cells) {
    std::vector<float> heights(static_cast<std::size_t>(x) * z);
    for (std::uint32_t row = 0; row < z; ++row) {
        for (std::uint32_t column = 0; column < x; ++column)
            heights[static_cast<std::size_t>(row) * x + column] = static_cast<float>(column*column + row*column + row) / 8;
    }
    return {{"format", "poima.terrain.heightfield"}, {"version", 1}, {"samples_x", x}, {"samples_z", z},
        {"tile_cells", tile_cells}, {"origin_m", {17, 3, -50}}, {"spacing_m", {2, .5}}, {"heights_m", heights}};
}

template<std::size_t Size>
bool same_bits(const std::array<float, Size>& a, const std::array<float, Size>& b) {
    for (std::size_t i = 0; i < Size; ++i) {
        if (std::bit_cast<std::uint32_t>(a[i]) != std::bit_cast<std::uint32_t>(b[i])) return false;
    }
    return true;
}

Json model_metadata(const std::string& bytes) {
    check(bytes.starts_with("POIMAM03") && bytes.size() >= 16, "Terrain did not emit the standard v3 model format.");
    std::size_t length = 0;
    for (std::size_t i = 0; i < 4; ++i)
        length |= static_cast<std::size_t>(static_cast<unsigned char>(bytes[8+i])) << (8*i);
    check(length <= bytes.size()-16, "Terrain model header length exceeds package size.");
    return Json::parse(std::string_view(bytes).substr(16, length));
}

void verify_cook() {
    const auto source = recipe(6, 5, 2);
    const auto original = cook_heightfield(source);
    const auto raw = cook_heightfield_json(" \n" + source.dump(2) + "\n ");
    check(original.tiles_x == 3 && original.tiles_z == 2 && original.tiles.size() == 6,
        "Terrain cook did not retain the intended tile grid.");
    check(original.recipe_sha256 == raw.recipe_sha256 && original.recipe_sha256.size() == 64,
        "Terrain recipe hash depends on its JSON presentation.");
    auto equivalent = source;
    equivalent["heights_m"][0] = -0.0;
    equivalent["origin_m"][0] = 17.0;
    const auto normalized = cook_heightfield(equivalent);
    check(normalized.recipe_sha256 == original.recipe_sha256, "Equivalent source values changed the canonical terrain identity.");

    const auto heights = source["heights_m"].get<std::vector<float>>();
    const Heightfield field({6, 5, 2, {17, 3, -50}, {2, .5}}, heights);
    std::set<std::string> ids;
    std::map<std::string, std::shared_ptr<const poima::MeshAsset>> decoded;
    std::map<std::string, poima::Matrix4> matrices;
    Json entities = Json::object();
    for (std::size_t i = 0; i < original.tiles.size(); ++i) {
        const auto& tile = original.tiles[i];
        check(tile.x == i%3 && tile.z == i/3, "Terrain cook order differs from Z, then X.");
        const auto owner = std::to_string(i);
        check(tile.asset == raw.tiles[i].asset && tile.asset == normalized.tiles[i].asset &&
            tile.bytes == raw.tiles[i].bytes && tile.bytes == normalized.tiles[i].bytes,
            "Terrain cook did not reproduce package bytes and asset IDs.");
        const auto binary = std::as_bytes(std::span(tile.bytes.data(), tile.bytes.size()));
        check(tile.asset == poima::sha256(binary) && ids.insert(tile.asset).second, "Terrain tile content addressing failed.");
        const auto metadata = model_metadata(tile.bytes);
        check(metadata.at("importer") == "poima-terrain-heightfield-1", "Terrain package lost its generating codec identity.");

        const auto model = poima::decode_model(tile.bytes);
        check(model->nodes.size() == 1 && model->roots == std::vector<std::uint32_t>{0} && model->primitives.size() == 1 &&
            model->nodes[0].primitives == std::vector<std::uint32_t>{0} && model->package_version == 3,
            "Terrain tile does not expose exactly primitive zero.");
        const auto& node = model->nodes[0];
        check(node.parent == -1 && node.position == std::array<double,3>{0,0,0} && node.scale == std::array<double,3>{1,1,1} &&
            node.rotation == std::array<double,4>{0,0,0,1}, "Terrain package added a transform to world-space vertices.");
        check(poima::encode_model_with_importer(*model, heightfield_model_importer) == tile.bytes, "Terrain did not survive the public model codec round trip.");
        const auto& mesh = model->primitives[0];
        const auto source_mesh = field.build_tile(tile.x, tile.z);
        check(mesh->vertices.size() == source_mesh.vertices.size() && mesh->indices == source_mesh.indices && mesh->influences.empty(),
            "Cooked terrain geometry differs from the heightfield mesh.");
        const poima::Bounds bounds = poima::mesh_bounds(mesh.get());
        check(tile.bounds.minimum == bounds.minimum && tile.bounds.maximum == bounds.maximum &&
            tile.vertex_count == mesh->vertices.size() && tile.triangle_count == mesh->indices.size()/3,
            "Terrain package metadata differs from the decoded geometry.");
        for (std::size_t j = 0; j < mesh->vertices.size(); ++j) {
            const auto& a = mesh->vertices[j];
            const auto& b = source_mesh.vertices[j];
            check(same_bits(a.position, b.position) && same_bits(a.normal, b.normal) && same_bits(a.uv, b.uv) &&
                same_bits(a.tangent, b.tangent), "The model codec changed terrain vertex bits.");
        }
        decoded.emplace(tile.asset, mesh);
        matrices.emplace(owner, poima::identity_matrix());
        const Json collider{{"asset", tile.asset}, {"primitive", 0}, {"friction", .5}, {"restitution", 0}};
        entities[owner] = {{"parent", nullptr}, {"components", {{"MeshCollider", collider}}}};
    }

    const auto nav_source = poima::navigation::collision_source(entities, matrices);
    const auto nav_input = poima::navigation::collision_geometry(nav_source, [&](const std::string& asset, std::uint32_t primitive) {
        check(primitive == 0, "Terrain navigation asked for a different primitive.");
        return decoded.at(asset);
    });
    std::size_t vertex = 0, index = 0;
    for (const auto& tile : original.tiles) {
        const auto& mesh = decoded.at(tile.asset);
        for (std::size_t j = 0; j < mesh->vertices.size(); ++j)
            check(same_bits(nav_input.vertices.at(vertex+j), mesh->vertices[j].position), "Navigation altered the terrain vertex position.");
        for (std::size_t j = 0; j < mesh->indices.size(); ++j)
            check(nav_input.triangles.at(index+j) == static_cast<int>(vertex+mesh->indices[j]), "Navigation altered terrain triangle topology.");
        vertex += mesh->vertices.size();
        index += mesh->indices.size();
    }
    check(vertex == nav_input.vertices.size() && index == nav_input.triangles.size(), "Terrain navigation geometry size differs.");

    auto edited = source;
    edited["heights_m"][7] = 3.0;
    const auto recooked = cook_heightfield(edited);
    check(recooked.recipe_sha256 != original.recipe_sha256 && recooked.tiles[0].asset != original.tiles[0].asset &&
        recooked.tiles[1].asset != original.tiles[1].asset && recooked.tiles[5].asset == original.tiles[5].asset,
        "Terrain cook failed to propagate a seam-normal change or changed an unrelated tile.");
}

void verify_errors() {
    const auto source = recipe(2, 2, 1);
    auto invalid_field = [&](const char* field, Json value, std::string_view reason) {
        auto altered = source;
        altered[field] = std::move(value);
        rejects([&] { (void)cook_heightfield(altered); }, reason);
    };
    invalid_field("format", "poima.unknown", "format");
    invalid_field("version", 1.0, "version");
    invalid_field("version", 2, "version");
    invalid_field("samples_x", -2, "samples_x");
    invalid_field("samples_z", std::numeric_limits<std::uint64_t>::max(), "samples_z");
    invalid_field("tile_cells", true, "tile_cells");
    invalid_field("tile_cells", 129, "tile_cells");
    invalid_field("origin_m", {0, 0}, "origin_m");
    invalid_field("spacing_m", {.009, 1}, "spacing");
    invalid_field("heights_m", {0, 0, 0}, "count");
    invalid_field("heights_m", {0, 0, 0, false}, "numbers");
    invalid_field("heights_m", {0, 0, 0, std::numeric_limits<double>::infinity()}, "finite");
    invalid_field("heights_m", {0, 0, 0, 2e307}, "height offset");
    invalid_field("heights_m", {0, 0, 0, 1e5}, "MeshCollider");
    invalid_field("unknown", nullptr, "v1 fields");

    auto missing = source;
    missing.erase("tile_cells");
    rejects([&] { (void)cook_heightfield(missing); }, "v1 fields");
    rejects([&] { (void)cook_heightfield(recipe(66, 2, 1)); }, "64 tiles");
    auto oversized = source;
    oversized["samples_x"] = 400;
    oversized["samples_z"] = 330;
    rejects([&] { (void)cook_heightfield(oversized); }, "131072 height samples");
    oversized["samples_z"] = 320;
    rejects([&] { (void)cook_heightfield(oversized); }, "250000 triangles");

    auto raw = source.dump();
    raw.insert(1, "\"version\":1,");
    rejects([&] { (void)cook_heightfield_json(raw); }, "Duplicate");
    raw = source.dump() + '\0' + "trailing";
    rejects([&] { (void)cook_heightfield_json(raw); }, "null byte");
    rejects([] { (void)cook_heightfield_json("{"); }, "Invalid terrain recipe JSON");
    rejects([] { (void)cook_heightfield_json("[[[[[[[[[[[0]]]]]]]]]]]"); }, "complexity");
    const std::string oversized_bytes(max_cook_source_bytes+1, ' ');
    rejects([&] { (void)cook_heightfield_json(oversized_bytes); }, "4 MiB");
}
} // namespace

int main() {
    try {
        verify_cook();
        verify_errors();
        std::cout << "Terrain asset checks passed: strict recipes, canonical packages, exact geometry and static navigation input.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
