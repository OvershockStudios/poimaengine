// SPDX-License-Identifier: Apache-2.0
namespace {
constexpr std::array draft_world_sidecars{".lock", ".pending", ".previous", ".previous.pending"};

bool draft_paths_overlap(const fs::path& first, const fs::path& second) {
    return within(first, second) || within(second, first);
}

std::string draft_digest(const Json& value) {
    require(value.is_string() && hexadecimal(value.get<std::string>(), 64),
        "Expected 64 lowercase hexadecimal characters for SHA-256.");
    return value.get<std::string>();
}

std::pair<fs::path, Json> read_draft_marker(const fs::path& requested) {
    adoption_safe_path(requested);
    regular(requested);
    const auto manifest = canonical_project_path(requested);
    const auto root = manifest.parent_path();
    plain_directory(root);
    require(fs::symlink_status(project_io_path(root / ".poima-incomplete")).type() == fs::file_type::not_found,
        "Draft project publication is incomplete.");
    const auto marker_path = contained(root, project_draft_marker);
    adoption_safe_path(marker_path);
    auto marker = parse(read(marker_path, 65536));
    fields(marker, {"format", "version", "base_sha256", "base_project_id", "base_revision", "project_id", "project"},
        {"format", "version", "base_sha256", "base_project_id", "base_revision", "project_id", "project"});
    require(marker.at("format") == "poima.project-draft" && integer(marker.at("version")) == 1 &&
        id(marker.at("project_id")) != id(marker.at("base_project_id")) &&
        relative_path(marker.at("project")) == text(manifest.filename()),
        "Invalid project draft lineage marker.");
    (void)draft_digest(marker.at("base_sha256"));
    (void)integer(marker.at("base_revision"));
    return {manifest, std::move(marker)};
}

Project draft_input(const Json& manifest, const SourceCheckpoint& base, const std::string& project_name) {
    require(manifest.is_string(), "Draft manifest path must be text.");
    const auto requested = path_of(manifest.get<std::string>()).lexically_normal();
    require(requested.is_absolute() && requested.filename() == path_of(project_name),
        "Expected an absolute draft manifest path with the base project filename.");
    const auto [canonical, marker] = read_draft_marker(requested);
    Project draft;
    draft.manifest = canonical;
    draft.root = canonical.parent_path();
    require(marker.at("base_sha256") == base.source_sha256 && marker.at("base_project_id") == base.source.spec.at("project_id") &&
        integer(marker.at("base_revision")) == base.source.content.revision && marker.at("project") == project_name,
        "Project draft lineage does not match the selected base checkpoint.");
    draft.spec = parse(read(draft.manifest, 65536));
    fields(draft.spec, {"format", "version", "project_id", "name", "entry", "input_profile", "audio", "gameplay", "startup_splash"},
        {"format", "version", "project_id", "name", "entry"});
    const auto version = integer(draft.spec.at("version"));
    require(draft.spec.at("format") == "poima.project" && (version == 1 || version == 2) &&
        id(draft.spec.at("project_id")) == id(marker.at("project_id")),
        "Project draft lineage does not match the draft manifest identity.");
    const auto& entry = draft.spec.at("entry");
    fields(entry, {"world", "camera", "controller"}, {"world", "camera", "controller"});
    draft.world = draft.root / path_of(relative_path(entry.at("world")));
    if (draft.spec.contains("input_profile"))
        draft.profile = draft.root / path_of(relative_path(draft.spec.at("input_profile")));
    return draft;
}

void protect_draft_files(const Project& project, std::set<std::string>& protected_paths) {
    protected_paths.insert(folded(text(project.manifest.filename())));
    const auto world = folded(text(project.world.lexically_relative(project.root)));
    protected_paths.insert(world);
    for (const auto* suffix : draft_world_sidecars) {
        protected_paths.insert(world + suffix);
        if (!project.profile.empty())
            protected_paths.insert(folded(text(project.profile.lexically_relative(project.root))) + suffix);
    }
}

void validate_draft_selection(const std::string& name, const std::set<std::string>& protected_paths) {
    auto path = path_of(name);
    while (!path.empty()) {
        const auto component = folded(text(path.filename()));
        require(!component.starts_with(".poima-") && component != ".git" && component != ".vs" &&
            !protected_paths.contains(folded(text(path))), "Draft selection targets a protected project path: " + name);
        path = path.parent_path();
    }
}

struct DraftFile {
    fs::path root;
    std::string sha256;
    std::uint64_t size;
};

// Register ancestors as well as files so Linux and Windows prepare the same tree.
void register_draft_path(std::map<std::string, std::pair<std::string, bool>>& tree, const std::string& name) {
    for (auto end = name.find('/');; end = name.find('/', end + 1)) {
        const auto part = name.substr(0, end);
        const bool directory = end != std::string::npos;
        const auto [match, inserted] = tree.try_emplace(folded(part), part, directory);
        require(inserted || match->second.first == part, "Case-insensitive draft path collision: " + name);
        require(inserted || match->second.second == directory, "Draft path conflicts with a file or directory: " + name);
        require(tree.size() <= 8191, "Draft integration exceeds its directory entry budget.");
        if (!directory) break;
    }
}

Json apply_draft_transaction(const fs::path& world, const Json& transaction, std::uint64_t base_revision) {
    std::vector<fs::path> sidecars;
    for (const auto* suffix : draft_world_sidecars) {
        sidecars.push_back(project_io_path(fs::path(world).concat(suffix)));
        require(fs::symlink_status(sidecars.back()).type() == fs::file_type::not_found,
            "Base checkpoint contains an authoritative world transaction sidecar.");
    }
    const auto world_io = project_io_path(world);
    const auto mode = fs::status(world_io).permissions();
    fs::permissions(world_io, fs::perms::owner_write, fs::perm_options::add);
    Json result;
    {
        WorldSession session(text(world));
        result = call(session, "world.transact", transaction);
    }
    require(result.at("committed") == true && result.at("replayed") == false &&
        integer(result.at("revision")) == base_revision + 1, "Expected a fresh committed draft transaction.");
    for (const auto& sidecar : sidecars) fs::remove(sidecar);
    fs::permissions(world_io, mode & fs::perms::all, fs::perm_options::replace);
    return result;
}
}

std::string project_draft_world_path(const std::string& manifest) {
    const auto [canonical, marker] = read_draft_marker(fs::absolute(path_of(manifest)).lexically_normal());
    const auto draft = project(text(canonical));
    require(draft.spec.at("project_id") == marker.at("project_id") &&
        draft.content.revision >= integer(marker.at("base_revision")),
        "Project draft lineage does not match the draft manifest identity or revision.");
    adoption_safe_path(draft.world);
    for (const auto* suffix : draft_world_sidecars)
        adoption_safe_path(fs::path(draft.world).concat(suffix), true);
    return text(draft.world);
}

namespace project_detail {
void validate_draft_asset_source(const std::string& world, const std::string& source) {
    const auto path = contained(path_of(world).parent_path(), relative_path(Json(source)));
    adoption_safe_path(path);
}
}

Reply integrate_project_drafts(const std::string& checkpoint, const std::string& output, const std::string& request_json) {
    return operation("project.integrate", [&] {
        require(request_json.size() <= 1024 * 1024, "Draft integration request exceeds 1 MiB.");
        const auto request = parse(request_json);
        fields(request, {"base_sha256", "drafts", "transaction"}, {"base_sha256", "drafts"});
        const auto base_sha256 = draft_digest(request.at("base_sha256"));
        const auto& drafts = request.at("drafts");
        require(drafts.is_array() && !drafts.empty() && drafts.size() <= 10, "Integration requires 1..10 distinct drafts.");
        const auto base = read_source_checkpoint(path_of(checkpoint), base_sha256);
        const auto base_id = id(base.source.spec.at("project_id"));
        const auto base_revision = base.source.content.revision;
        const auto project_name = text(base.source.manifest.filename());
        const auto world_name = relative_path(base.source.spec.at("entry").at("world"));
        const auto destination = destination_path(output);
        require(!draft_paths_overlap(destination, base.root), "Integration output cannot overlap the base checkpoint.");
        const bool transact = request.contains("transaction");
        if (transact) {
            const auto& transaction = request.at("transaction");
            fields(transaction, {"request_id", "base_revision", "ops"}, {"request_id", "base_revision", "ops"});
            id(transaction.at("request_id"));
            require(integer(transaction.at("base_revision")) == base_revision,
                "Integration transaction base revision differs from the selected checkpoint.");
        }

        std::vector<fs::path> roots{base.root};
        std::vector<Project> projects;
        std::set<std::string> project_ids{base_id}, protected_paths;
        protect_draft_files(base.source, protected_paths);
        for (const auto& input : drafts) {
            fields(input, {"manifest", "files"}, {"manifest", "files"});
            require(input.at("files").is_array(), "Draft files must be an array.");
            auto draft = draft_input(input.at("manifest"), base, project_name);
            require(!draft_paths_overlap(destination, draft.root), "Integration output cannot overlap a draft project.");
            for (const auto& root : roots)
                require(!draft_paths_overlap(draft.root, root), "Draft project directories cannot overlap each other or the base checkpoint.");
            require(project_ids.insert(id(draft.spec.at("project_id"))).second,
                "Integration requires distinct draft project identities.");
            roots.push_back(draft.root);
            protect_draft_files(draft, protected_paths);
            projects.push_back(std::move(draft));
        }

        std::map<std::string, std::pair<std::string, bool>> tree;
        for (const auto& entry : base.files) {
            const auto path = entry.at("path").get<std::string>().substr(7);
            const auto name = folded(path.substr(0, path.find('/')));
            require(name != project_draft_marker, "Base checkpoint contains a reserved draft marker.");
            register_draft_path(tree, path);
        }
        std::map<std::string, DraftFile> selected;
        std::uint64_t selected_bytes = 0;
        std::size_t selections = 0;
        for (std::size_t i = 0; i < projects.size(); ++i) {
            const auto& files = drafts.at(i).at("files");
            require(files.size() <= count_limit - selections, "Integration exceeds 4096 selected files.");
            selections += files.size();
            if (files.empty()) continue;
            const auto& draft = projects[i];
            const auto editable = editable_files(draft);
            std::set<std::string> seen;
            for (const auto& file : files) {
                fields(file, {"path", "sha256"}, {"path", "sha256"});
                const auto name = relative_path(file.at("path"));
                const auto sha256 = draft_digest(file.at("sha256"));
                validate_draft_selection(name, protected_paths);
                require(seen.insert(name).second, "Duplicate selected draft file: " + name);
                register_draft_path(tree, name);
                require(std::binary_search(editable.begin(), editable.end(), name),
                    "Selected file is missing or outside the editable draft files: " + name);
                const auto source = contained(draft.root, name);
                adoption_safe_path(source);
                const auto bytes = read(source, std::min(file_limit, total_limit - selected_bytes));
                require(hash(bytes) == sha256, "Draft file hash mismatch: " + name);
                selected_bytes += bytes.size();
                const auto [existing, inserted] = selected.try_emplace(name, DraftFile{draft.root, sha256, bytes.size()});
                require(inserted || existing->second.sha256 == sha256, "Divergent draft files target the same path: " + name);
            }
        }
        require(transact || !selected.empty(), "Integration requires selected draft files or a world transaction.");

        auto spec = base.source.spec;
        auto candidate_id = new_id();
        while (project_ids.contains(candidate_id)) candidate_id = new_id();
        spec["project_id"] = candidate_id;
        Stage stage(text(destination));
        require(stage.destination == destination, "Integration output parent changed before staging.");
        Inventory inventory{stage};
        const auto overlay = [&](const std::string& name, const DraftFile& file) {
            const auto source = contained(file.root, name);
            adoption_safe_path(source);
            const auto bytes = read(source, file.size);
            require(bytes.size() == file.size && hash(bytes) == file.sha256, "Draft file hash mismatch during staging: " + name);
            inventory.add(name, bytes, "source", fs::status(project_io_path(source)).permissions());
        };
        std::vector<std::string> base_paths;
        for (const auto& entry : base.files) {
            const auto name = entry.at("path").get<std::string>().substr(7);
            base_paths.push_back(name);
            const auto original = contained(base.source.root, name);
            const auto size = integer(entry.at("size"), file_limit);
            const auto bytes = read(original, size);
            require(bytes.size() == size && hash(bytes) == entry.at("sha256").get<std::string>(),
                "Base source changed during integration: " + name);
            if (const auto file = selected.find(name); file != selected.end()) overlay(name, file->second);
            else inventory.add(name, name == project_name ? spec.dump(2) + "\n" : bytes, "source",
                fs::status(project_io_path(original)).permissions());
        }
        require(file_tree(base.source.root) == base_paths &&
            hash(read(contained(base.root, checkpoint_name), 4 * 1024 * 1024)) == base_sha256,
            "Base checkpoint changed during integration.");
        Json merged = Json::array();
        for (const auto& [name, file] : selected) {
            if (!inventory.names.contains(folded(name))) overlay(name, file);
            merged.push_back({{"path", name}, {"sha256", file.sha256}});
        }

        Json result = {{"manifest", text(stage.destination / project_name)}, {"project_id", candidate_id},
            {"base_project_id", base_id}, {"base_sha256", base_sha256}, {"base_revision", base_revision},
            {"merged_files", std::move(merged)}};
        std::uint64_t old_world_size = 0;
        if (transact) {
            const auto world = contained(stage.path, world_name);
            old_world_size = fs::file_size(project_io_path(world));
            result["transaction"] = apply_draft_transaction(world, request.at("transaction"), base_revision);
        }
        const auto candidate = project(text(stage.path / project_name));
        require(candidate.spec == spec && candidate.content.revision == base_revision + (transact ? 1 : 0),
            "Integrated project identity or revision differs from the staged result.");
        if (transact)
            require(candidate.world_bytes.size() <= total_limit - (inventory.total - old_world_size),
                "Integrated project exceeds its 1 GiB source budget.");
        inventory.sort();
        std::vector<std::string> expected_paths;
        for (const auto& entry : inventory.files) expected_paths.push_back(entry.at("path").get<std::string>());
        require(file_tree(stage.path) == expected_paths, "Integrated project file set changed during staging.");
        result["revision"] = candidate.content.revision;
        stage.publish(project_name);
        return result;
    });
}
