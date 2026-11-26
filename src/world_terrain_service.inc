// SPDX-License-Identifier: Apache-2.0
// Included in World. Publishing does not author entities.
    std::string terrain_recipe_file(const Json& source) const {
        require(source.is_string(), "Terrain recipe source must be a file path.");
        const auto& name = source.get_ref<const std::string&>();
        require(!name.empty() && name.find('\0') == std::string::npos, "Invalid terrain recipe source path.");
        auto path = fs::path(std::u8string(name.begin(), name.end()));
        require(path.extension() == ".json", "Terrain recipe source must have a .json extension.");
        if (path.is_relative()) path = path_.parent_path() / path;
        const auto input_path = project_detail::project_io_path(path);
        require(fs::is_regular_file(fs::symlink_status(input_path)) && fs::hard_link_count(input_path) == 1,
            "Terrain recipe sources must be ordinary files without aliases.", -32050);
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(input_path.c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "Terrain recipe sources cannot be reparse points.", -32050);
#endif
        const auto size = fs::file_size(input_path);
        require(size > 0 && size <= terrain::max_cook_source_bytes, "Terrain recipe source must be within 4 MiB.", -32050);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        std::ifstream input(input_path, std::ios::binary);
        require(bool(input.read(bytes.data(), static_cast<std::streamsize>(size))) && input.peek() == std::char_traits<char>::eof(),
            "Terrain recipe read failed or changed size.", -32050);
        return bytes;
    }
    std::string terrain_recipe_input(const Json& params) const {
        const bool inline_recipe = params.contains("recipe");
        require(inline_recipe != params.contains("source"), "Provide exactly one of recipe or source.");
        if (!inline_recipe) return terrain_recipe_file(params.at("source"));
        require(params.at("recipe").is_string(), "Terrain recipe must be JSON text.");
        return params.at("recipe").get<std::string>();
    }
    Json terrain_dispatch(const std::string& method, const Json& params) const {
        const bool publish = method == "world.terrain.cook";
        require(publish || method == "world.terrain.inspect", "Unknown terrain method.", -32601);
        if (publish) fields(params, {"recipe", "source", "expected_recipe_sha256"}, {"expected_recipe_sha256"});
        else fields(params, {"recipe", "source"});
        std::string expected;
        if (publish) {
            require(params.at("expected_recipe_sha256").is_string(), "Expected terrain recipe SHA-256 must be text.");
            expected = params.at("expected_recipe_sha256").get<std::string>();
            require(valid_asset_id(expected), "Expected terrain recipe SHA-256 must be 64 lowercase hexadecimal digits.");
        }
        try {
            const auto cooked = terrain::cook_heightfield_json(terrain_recipe_input(params));
            if (publish && expected != cooked.recipe_sha256)
                throw Error(-32009, "Terrain recipe changed since inspection.",
                    {{"expected_recipe_sha256", expected}, {"recipe_sha256", cooked.recipe_sha256}});

            Json tiles = Json::array();
            for (const auto& tile : cooked.tiles) {
                Json row = {{"x", tile.x}, {"z", tile.z}, {"asset", tile.asset}, {"primitive", 0},
                    {"bytes", tile.bytes.size()}, {"vertices", tile.vertex_count}, {"triangles", tile.triangle_count},
                    {"bounds", {{"min", tile.bounds.minimum}, {"max", tile.bounds.maximum}}}};
                if (cooked.lod_recipe) {
                    Json mesh = {{"asset", tile.asset}, {"primitive", 0}, {"visible", true}};
                    Json tiers = Json::array();
                    if (!tile.lods.empty()) {
                        std::array<double,3> center{};
                        for (std::size_t axis = 0; axis < center.size(); ++axis)
                            center[axis] = (tile.bounds.minimum[axis] + tile.bounds.maximum[axis]) * .5;
                        mesh["lod_anchor"] = center;
                        mesh["lods"] = Json::array();
                        for (const auto& lod : tile.lods) {
                            mesh["lods"].push_back({{"asset", tile.asset}, {"primitive", lod.primitive},
                                {"min_distance_m", lod.min_distance_m}});
                            tiers.push_back({{"primitive", lod.primitive}, {"stride", lod.stride},
                                {"vertices", lod.vertex_count}, {"triangles", lod.triangle_count}});
                        }
                    }
                    row["static_mesh"] = std::move(mesh);
                    row["lod_tiers"] = std::move(tiers);
                }
                tiles.push_back(std::move(row));
            }
            Json result = {{"recipe_sha256", cooked.recipe_sha256}, {"tiles_x", cooked.tiles_x},
                {"tiles_z", cooked.tiles_z}, {"tiles", std::move(tiles)}};
            if (publish) {
                const auto directory = asset_directory();
                for (const auto& tile : cooked.tiles) {
                    const auto stored = store_cooked_model_bytes(directory, tile.bytes);
                    require(stored.id == tile.asset && stored.bytes == tile.bytes.size(),
                        "Stored terrain model differs from the planned tile.", -32050);
                }
            }
            return result;
        } catch (const Error&) { throw; }
        catch (const std::invalid_argument& error) { throw Error(-32602, error.what()); }
        catch (const fs::filesystem_error&) { throw Error(-32050, "Terrain asset store filesystem operation failed."); }
        catch (const std::exception& error) { throw Error(-32050, error.what()); }
    }
