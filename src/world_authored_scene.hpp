// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/scene.hpp"
#include "world_json.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>

namespace poima {
struct EditorCamera;
struct ModelCache;
}

namespace poima::world_detail {

// Begin a candidate without mutating the last published presentation lineage.
// The world owner publishes it only after the authored file is replaced.
class AuthoredPresentation {
    struct Candidate {
        std::map<std::string, std::pair<Json, std::uint64_t>> shapes;
        std::uint64_t cursor = 0;
    };

public:
    Candidate stage(const Json& document) const;
    void publish(Candidate&& candidate) noexcept;
    void require_next_generation() const;
    void advance_generation() noexcept;
    void stamp(SceneSnapshot& snapshot) const;

private:
    const std::string source_ = new_presentation_source_id();
    std::uint64_t generation_ = 0;
    Candidate current_;
};

void validate_editor_camera(const EditorCamera& camera);
SceneLighting authored_lighting(const Json& document, const std::map<std::string, Matrix4>& matrices);
void resolve_scene_post_effect(SceneSnapshot& snapshot, const std::filesystem::path& assets);
SceneSnapshot build_editor_authored_scene(const Json& published, const Json* preview,
    const std::filesystem::path& assets, ModelCache& model_cache, const AuthoredPresentation& presentation);
void append_captured_authored_geometry(SceneSnapshot& snapshot, const Json& document,
    const std::map<std::string, Matrix4>& matrices, const std::filesystem::path& assets,
    ModelCache& model_cache, const AuthoredPresentation& presentation);

} // namespace poima::world_detail
