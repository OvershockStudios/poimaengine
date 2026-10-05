// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/scene.hpp"
#include <memory>
#include <string>
#include <string_view>

namespace poima {
enum class WorldOpenMode { authoring, read_only_runtime };
struct WorldPackageAsset { std::string filename,sha256;std::uint64_t bytes=0; };
struct WorldPackageContent {
    std::string document;
    std::vector<WorldPackageAsset> assets;
    std::uint64_t revision=0;
    bool needs_audio=false;
};
enum class WorldRequestScope { standalone, shared_headless, shared_editor };
struct WorldRuntimeStatus {
    bool available=false,active=false;
    std::string session_id;
    std::uint64_t tick=0,authored_revision=0;
};
struct EditorCamera {
    Matrix4 world=identity_matrix();
    double vertical_fov=60,near_plane=.1,far_plane=1000;
};
// One synchronous shared authoring/runtime session. Owns the cooperative writer
// lock; callers serialize access. Snapshots own presentation data, not GUI state.
class WorldSession {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit WorldSession(const std::string& utf8_path,WorldOpenMode mode=WorldOpenMode::authoring);
    ~WorldSession();
    WorldSession(const WorldSession&)=delete;
    WorldSession& operator=(const WorldSession&)=delete;
    std::string request(std::string_view json_rpc,WorldRequestScope scope=WorldRequestScope::standalone);
    bool closed() const;
    WorldRuntimeStatus runtime_status() const;
    // Validates the complete typed asset closure; no storage writes.
    WorldPackageContent package_content() const;
    SceneSnapshot authored_snapshot(const EditorCamera& camera) const;
    SceneSnapshot runtime_snapshot(const EditorCamera& camera) const;
    // One transient local transform; rebuilds hierarchy, lights and skin palettes
    // without changing authored state, history, receipts or the normal cache.
    SceneSnapshot authored_preview(const EditorCamera& camera,const std::string& entity,
        const std::array<double,3>& position,const std::array<double,4>& rotation,
        const std::array<double,3>& scale) const;
};
// Persistent authored-world service over newline-delimited JSON-RPC 2.0.
// Simulation/renderer state is deliberately not stored in this document.
int run_world_session(const std::string& utf8_path);
}
