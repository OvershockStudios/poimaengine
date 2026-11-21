// SPDX-License-Identifier: Apache-2.0
#include "world_region_assets.hpp"

#include "asset_store.hpp"
#include "navigation_geometry.hpp"
#include "world_document.hpp"
#include "world_model_assets.hpp"

#include <exception>
#include <fstream>

namespace poima::world_detail {

CheckedNavigation read_navigation_package(const fs::path& directory, const std::string& id) {
    require(navigation::available(),"Navigation binding requires POIMA_ENABLE_NAVIGATION=ON.",-32003);
    try {
        const auto path=asset_package_path(directory,id,".pnav");
        const auto length=fs::file_size(path);
        require(length>0 && length<=navigation::max_package_bytes,"Navigation package must contain1..16MiB.",-32050);
        std::string bytes(static_cast<std::size_t>(length),'\0');
        std::ifstream file(path,std::ios::binary);
        require(bool(file.read(bytes.data(),static_cast<std::streamsize>(length))) && content_hash(bytes)==id,
            "Navigation package hash mismatch/read failure.",-32050);
        auto mesh=navigation::Mesh::decode(bytes);
        require(mesh->encode()==bytes,"Navigation package must use canonical checked encoding.",-32050);
        const auto metadata=Json::parse(mesh->metadata());
        RuntimeNavigationDefinition definition{
            id,mesh->source_fingerprint(),navigation::parse_profile(metadata.at("profile").dump()),std::move(mesh)};
        return {std::move(definition),static_cast<std::uint64_t>(length)};
    } catch(const Error& error){throw Error(error.code,asset_diagnostic(error.what()));}
    catch(const std::exception& error){throw Error(-32050,asset_diagnostic(error.what()));}
}

CheckedNavigation resolve_navigation_binding(const Json& document, const fs::path& directory) {
    const auto& binding=document.at("navigation");
    fields(binding,{"asset"},{"asset"});
    require(binding.at("asset").is_string() && valid_asset_id(binding.at("asset")),"Navigation binding asset must be64 lowercase hexadecimal digits.");
    auto checked=read_navigation_package(directory, binding.at("asset").get<std::string>());
    std::string fingerprint;
    try {
        const auto& entities=document.at("entities");
        fingerprint=navigation::source_fingerprint(navigation::collision_source(entities,world_matrices(entities)));
    } catch(const std::exception& error){throw Error(-32602,asset_diagnostic(error.what()));}
    require(checked.definition.source_fingerprint==fingerprint,
        "Navigation binding topology is stale; bake and explicitly bind the current static collision source.",-32009);
    return checked;
}

void validate_water_tiles(const Json& document, const std::vector<water::TileBinding>& tiles) {
    const auto& entities = document.at("entities");
    const auto* snow_tiles = document.contains("snow") ? &document.at("snow").at("tiles") : nullptr;
    for (const auto& tile : tiles) {
        require(!snow_tiles || (snow_tiles->at(0) != tile.entity && snow_tiles->at(1) != tile.entity),
            "Water terrain tiles cannot also be bound to snow.", -32009);
        const auto found = entities.find(tile.entity);
        require(found != entities.end() && found->at("parent").is_null(), "Water terrain binding is missing a root tile.", -32009);
        const auto& components = found->at("components");
        const auto& transform = components.at("Transform");
        require(transform.at("position") == Json::array({0, 0, 0}) && transform.at("scale") == Json::array({1, 1, 1}) &&
            (transform.at("rotation") == Json::array({0, 0, 0, 1}) || transform.at("rotation") == Json::array({0, 0, 0, -1})),
            "Water terrain tile transform is stale. Restore identity transforms or recook the water region.", -32009);
        for (const auto* kind : {"StaticMesh", "MeshCollider"}) {
            const auto binding = components.find(kind);
            require(binding != components.end() && binding->at("asset") == tile.asset && binding->at("primitive") == 0,
                "Water terrain model binding is stale. Recook the water region for the current terrain.", -32009);
        }
        require(!components.at("StaticMesh").contains("foliage_motion"),
            "Water terrain tiles cannot use foliage motion.",-32009);
    }
}

std::string read_water_package(const fs::path& directory, const std::string& id) {
    const auto path = asset_package_path(directory, id, ".pwater");
    const auto length = fs::file_size(path);
    require(length > 0 && length <= water::max_region_asset_bytes && fs::hard_link_count(path) == 1,
        "Water asset must be a bounded ordinary package.", -32050);
    std::ifstream input(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(length), '\0');
    require(bool(input.read(bytes.data(), static_cast<std::streamsize>(length))) &&
        input.peek() == std::char_traits<char>::eof() && content_hash(bytes) == id,
        "Water asset hash or size differs from the binding.", -32050);
    return bytes;
}

std::pair<water::RegionAsset, std::size_t> resolve_water_binding(const Json& document, const fs::path& directory) {
    try {
        const auto id = document.at("water").at("asset").get<std::string>();
        const auto bytes = read_water_package(directory, id);
        auto asset = water::decode_region_asset(bytes);
        validate_water_tiles(document, asset.tiles);
        return {std::move(asset), bytes.size()};
    } catch (const Error&) { throw; }
    catch (const std::exception& error) { throw Error(-32050, asset_diagnostic(error.what())); }
}

} // namespace poima::world_detail
