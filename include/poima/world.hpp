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
struct WorldGameplayStatus {
    bool active=false;
    std::string session_id;
    std::uint64_t tick=0,revision=0;
};
struct WorldSaveStatus {
    std::uint64_t generation=0;
    std::string root;
};
struct EditorCamera {
    Matrix4 world=identity_matrix();
    double vertical_fov=60,near_plane=.1,far_plane=1000;
};
struct WorldCameraInfo {
    std::string id;
    double vertical_fov=60,near_plane=.1,far_plane=1000;
};
struct WorldControllerInfo { std::string id,camera; };
// One synchronous shared authoring/runtime session. Owns the cooperative writer
// lock; callers serialize access. Snapshots own presentation data, not GUI state.
class WorldSession {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    // Packaged hosts supply their verified bundle root so save output cannot
    // write anywhere inside it. The default protects the world's parent folder.
    explicit WorldSession(const std::string& utf8_path,WorldOpenMode mode=WorldOpenMode::authoring,
        const std::string& protected_root={});
    ~WorldSession();
    WorldSession(const WorldSession&)=delete;
    WorldSession& operator=(const WorldSession&)=delete;
    std::string request(std::string_view json_rpc,WorldRequestScope scope=WorldRequestScope::standalone);
    bool closed() const;
    WorldRuntimeStatus runtime_status() const;
    // Counters only: does not serialize gameplay schema or field values.
    WorldGameplayStatus gameplay_status() const;
    // Session configuration only; does not inspect or create storage files.
    WorldSaveStatus save_status() const;
    std::vector<std::pair<std::string,std::string>> runtime_hierarchy() const;
    // Validates the complete typed asset closure; no storage writes.
    WorldPackageContent package_content() const;
    SceneSnapshot authored_snapshot(const EditorCamera& camera) const;
    SceneSnapshot runtime_snapshot(const EditorCamera& camera) const;
    // Camera metadata is sorted by ID. Live enumeration/snapshots require an
    // active runtime and use its frozen camera components even after authoring
    // edits or deletion. These queries never advance simulation or write storage.
    std::vector<WorldCameraInfo> cameras(bool live) const;
    std::vector<WorldControllerInfo> controllers(bool live) const;
    SceneSnapshot authored_camera_snapshot(const std::string& camera_id) const;
    SceneSnapshot runtime_camera_snapshot(const std::string& camera_id) const;
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
