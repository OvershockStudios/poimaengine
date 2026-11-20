// SPDX-License-Identifier: Apache-2.0
namespace {
constexpr auto checkpoint_name = "checkpoint.json";
constexpr auto suite_name = "game.verify.json";
constexpr auto check_report_name = "checks.json";

void plain_directory(const fs::path& path) {
    const auto io = project_io_path(path);
    require(fs::is_directory(fs::symlink_status(io)), "Expected an ordinary checkpoint directory.");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(io.c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
        "Checkpoint directories cannot be reparse points.");
#endif
}

std::vector<std::string> editable_files(const Project& project) {
    std::set<std::string> sidecars;
    for (const auto& path : {project.world, project.profile}) {
        if (path.empty()) continue;
        const auto name = folded(text(path.lexically_relative(project.root)));
        for (const auto* suffix : {".lock", ".pending", ".previous"}) sidecars.insert(name + suffix);
    }
    const auto root = project_io_path(project.root);
    std::vector<std::string> files;
    std::set<std::string> names;
    std::size_t entries = 0;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        require(++entries <= 8191, "Project checkpoint exceeds its directory entry budget.");
        const auto leaf = folded(text(it->path().filename()));
        const auto relative = text(it->path().lexically_relative(root));
        const auto status = it->symlink_status();
        const bool managed_cache = fs::is_directory(status) && (leaf == "bin" || leaf == "obj") &&
            fs::is_regular_file(it->path().parent_path() / "obj" / "project.assets.json");
        if (leaf == ".git" || leaf == ".vs" || leaf == ".poima-gameplay-adoptions" ||
            managed_cache || sidecars.contains(folded(relative))) {
            it.disable_recursion_pending();
            continue;
        }
        require(!fs::is_symlink(status), "Project checkpoints cannot include symlinks.");
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(it->path().c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "Project checkpoints cannot include reparse points.");
#endif
        relative_path(relative);
        require(names.insert(folded(relative)).second, "Case-insensitive project checkpoint path collision.");
        if (fs::is_directory(status)) continue;
        require(fs::is_regular_file(status), "Project checkpoints accept regular files only.");
        require(files.size() < count_limit, "Project checkpoint exceeds 4096 source files.");
        files.push_back(relative);
    }
    std::sort(files.begin(), files.end());
    return files;
}

struct Checkpoint {
    fs::path root;
    Project source;
    Json files;
    Json report;
    VerifiedGame game;
};

Checkpoint verified_checkpoint(const fs::path& checkpoint) {
    const auto root = fs::absolute(checkpoint).lexically_normal().parent_path();
    plain_directory(root);
    require(checkpoint.filename() == checkpoint_name, "Expected a checkpoint.json file.");
    require(!fs::exists(root / ".poima-incomplete"), "Checkpoint publication is incomplete.");
    const auto info = parse(read(contained(root, checkpoint_name), 4 * 1024 * 1024));
    fields(info, {"format", "version", "project_id", "revision", "project", "files"},
        {"format", "version", "project_id", "revision", "project", "files"});
    require(info.at("format") == "poima.project-checkpoint" && integer(info.at("version")) == 1,
        "Unsupported project checkpoint format or version.");
    const auto project_id = id(info.at("project_id"));
    const auto revision = integer(info.at("revision"));
    const auto project_name = relative_path(info.at("project"));
    require(project_name.find('/') == std::string::npos, "Checkpoint project must be a root manifest.");
    const auto report = parse(read(contained(root, check_report_name), 4 * 1024 * 1024));
    require(report.at("format") == "poima.game-verification-result" && integer(report.at("version")) == 1 &&
        report.at("passed") == true && integer(report.at("groups_total"), 32) > 0 &&
        report.at("groups_passed") == report.at("groups_total"), "Checkpoint requires passing gameplay checks.");
    require(report.at("project_id") == project_id && report.at("revision") == revision,
        "Verification report belongs to a different project or revision.");
    const auto& groups = report.at("groups");
    require(groups.is_array() && groups.size() == report.at("groups_total").get<std::size_t>(),
        "Verification group counts differ.");
    for (const auto& group : groups) {
        require(group.at("passed") == true && !group.contains("issue") && group.at("checks").is_array() &&
            !group.at("checks").empty(), "Verification report has an incomplete group.");
        for (const auto& check : group.at("checks")) require(check.at("passed") == true, "Verification report contains a failed check.");
    }
    const auto source_root = root / "source";
    plain_directory(source_root);
    require(hash(read(contained(source_root, suite_name), 1024 * 1024)) == report.at("suite_sha256").get<std::string>(),
        "Checkpoint verification suite bytes changed.");
    const auto& files = info.at("files");
    require(files.is_array() && !files.empty() && files.size() <= count_limit, "Invalid checkpoint source inventory.");
    const auto actual = file_tree(source_root);
    require(actual.size() == files.size(), "Checkpoint source file set changed.");
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto& entry = files[i];
        fields(entry, {"path", "size", "sha256", "role"}, {"path", "size", "sha256", "role"});
        require(entry.at("path") == "source/" + actual[i] && entry.at("role") == "source",
            "Checkpoint source inventory path changed.");
        const auto size = integer(entry.at("size"), file_limit);
        require(size <= total_limit - total, "Checkpoint source exceeds its byte budget.");
        total += size;
        const auto bytes = read(contained(source_root, actual[i]));
        require(bytes.size() == size && hash(bytes) == entry.at("sha256").get<std::string>(), "Checkpoint source changed: " + actual[i]);
    }
    auto source = project(text(contained(source_root, project_name)));
    require(source.spec.at("project_id") == project_id && source.content.revision == revision,
        "Checkpoint source project identity differs.");
    plain_directory(root / "game");
    const auto verified_version = report.at("engine_version").get<std::string>();
    require(!verified_version.empty() && verified_version.size() <= 64 &&
        verified_version.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.+-") == std::string::npos,
        "Invalid verification report engine version.");
    auto game = verify_game(text(contained(root, "game/game.json")), verified_version);
    require(game.definition.manifest_sha256 == report.at("bundle_sha256").get<std::string>() && game.definition.project_id == project_id &&
        game.definition.revision == revision, "Verified game bundle identity changed.");
    require(game.spec.at("source").at("world_sha256") == hash(source.world_bytes) &&
        game.definition.camera == source.spec.at("entry").at("camera").get<std::string>() &&
        game.definition.controller == source.spec.at("entry").at("controller").get<std::string>() &&
        game.definition.audio == source.spec.value("audio", false), "Verified game source differs from the checkpoint.");
    require(source.profile.empty() ? game.definition.input_profile.empty() :
        !game.definition.input_profile.empty() && read(path_of(game.definition.input_profile), 16 * 1024 * 1024) == source.profile_bytes,
        "Verified game input profile differs from the checkpoint.");
    require(source.gameplay ? game.definition.gameplay_descriptor_sha256 == source.gameplay->descriptor_sha256 &&
        parse(game.definition.gameplay_values) == source.gameplay_values : game.definition.gameplay_descriptor.empty(),
        "Verified gameplay differs from the checkpoint.");
    return {root, std::move(source), files, report, std::move(game)};
}

Json checkpoint_summary(const Checkpoint& value) {
    return {{"checkpoint", text(value.root / checkpoint_name)}, {"game_manifest", text(value.game.manifest)},
        {"report", text(value.root / check_report_name)}, {"project_id", value.source.spec.at("project_id")},
        {"revision", value.source.content.revision}, {"groups_passed", value.report.at("groups_passed")},
        {"bundle_sha256", value.game.definition.manifest_sha256}};
}
}

Reply checkpoint_project(const std::string& manifest, const std::string& output) {
    return operation("project.checkpoint", [&] {
        const auto current = project(manifest);
        const auto destination = destination_path(output);
        require(!within(destination, current.root) && !within(current.root, destination),
            "Checkpoint destination must be outside the project.");
        const auto files = editable_files(current);
        Stage stage(output);
        Inventory inventory{stage};
        for (const auto& path : files) {
            const auto source = contained(current.root, path);
            inventory.add("source/" + path, read(source), "source", fs::status(project_io_path(source)).permissions());
        }
        const auto project_name = relative_path(text(current.manifest.filename()));
        const auto local = project(text(stage.path / "source" / project_name));
        require(local.manifest_bytes == current.manifest_bytes && local.world_bytes == current.world_bytes &&
            local.profile_bytes == current.profile_bytes && local.gameplay_bytes == current.gameplay_bytes,
            "Project changed while capturing the checkpoint.");
        for (const auto& item : inventory.files) {
            const auto path = item.at("path").get<std::string>().substr(7);
            require(hash(read(contained(current.root, path))) == item.at("sha256").get<std::string>(), "Project file changed during checkpoint: " + path);
        }
        require(editable_files(current) == files, "Project file set changed during checkpoint.");
        const auto revision = current.content.revision;
        const Json info = {{"format", "poima.project-checkpoint"}, {"version", 1}, {"project_id", current.spec.at("project_id")},
            {"revision", revision}, {"project", project_name}, {"files", inventory.files}};
        const auto bytes = info.dump(2) + "\n";
        require(bytes.size() <= 4 * 1024 * 1024, "Checkpoint metadata exceeds 4 MiB.");
        exclusive_write(stage.path / checkpoint_name, bytes);
        stage.publish(checkpoint_name);
        return Json{{"checkpoint", text(stage.destination / checkpoint_name)},
            {"source_manifest", text(stage.destination / "source" / project_name)}, {"revision", revision}, {"files", files.size()}};
    });
}

Reply project_history(const std::string& directory, const std::string& project_id_arg) {
    return operation("project.history", [&] {
        const auto project_id = id(project_id_arg);
        const auto root = fs::absolute(path_of(directory)).lexically_normal();
        if (!fs::exists(root)) return Json{{"latest", nullptr}, {"skipped", 0}};
        plain_directory(root);
        const auto io = project_io_path(root);
        std::vector<fs::path> candidates;
        for (const auto& item : fs::directory_iterator(io)) {
            if (!fs::is_directory(item.symlink_status())) continue;
            require(candidates.size() < count_limit, "Checkpoint history exceeds 4096 directories.");
            const auto name = text(item.path().filename());
            if (!name.starts_with('.')) candidates.push_back(root / item.path().filename());
        }
        std::sort(candidates.rbegin(), candidates.rend());
        std::size_t skipped = 0;
        for (const auto& candidate : candidates) {
            try {
                const auto checkpoint = verified_checkpoint(candidate / checkpoint_name);
                require(checkpoint.source.spec.at("project_id") == project_id, "Checkpoint belongs to another project.");
                return Json{{"latest", checkpoint_summary(checkpoint)}, {"skipped", skipped}};
            } catch (const std::exception&) { ++skipped; }
        }
        return Json{{"latest", nullptr}, {"skipped", skipped}};
    });
}

Reply recover_project(const std::string& manifest, const std::string& output) {
    return operation("project.recover", [&] {
        const auto checkpoint = verified_checkpoint(path_of(manifest));
        const auto destination = destination_path(output);
        require(!within(destination, checkpoint.root) && !within(checkpoint.root, destination),
            "Recovery destination must be outside the checkpoint.");
        const auto& source = checkpoint.source;
        const auto name = text(source.manifest.filename());
        auto fork = source.spec;
        fork["project_id"] = new_id();
        Stage stage(output);
        Inventory restored{stage};
        for (const auto& entry : checkpoint.files) {
            const auto path = entry.at("path").get<std::string>().substr(7);
            const auto original = contained(source.root, path);
            const auto bytes = read(original);
            require(bytes.size() == entry.at("size") && hash(bytes) == entry.at("sha256").get<std::string>(), "Checkpoint source changed during recovery: " + path);
            restored.add(path, path == name ? fork.dump(2) + "\n" : bytes, "source", fs::status(project_io_path(original)).permissions());
        }
        const auto copy = project(text(stage.path / name));
        require(copy.spec == fork && copy.world_bytes == source.world_bytes && copy.profile_bytes == source.profile_bytes &&
            copy.gameplay_bytes == source.gameplay_bytes, "Recovered project dependency validation failed.");
        stage.publish(name);
        auto result = checkpoint_summary(checkpoint);
        result["restored_manifest"] = text(stage.destination / name);
        result["restored_project_id"] = fork["project_id"];
        result["restored_files"] = restored.files.size();
        return result;
    });
}
