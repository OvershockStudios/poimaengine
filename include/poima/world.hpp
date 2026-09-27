// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/scene.hpp"
#include <memory>
#include <string>
#include <string_view>

namespace poima {
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
    explicit WorldSession(const std::string& utf8_path);
    ~WorldSession();
    WorldSession(const WorldSession&)=delete;
    WorldSession& operator=(const WorldSession&)=delete;
    std::string request(std::string_view json_rpc);
    bool closed() const;
    SceneSnapshot authored_snapshot(const EditorCamera& camera) const;
    SceneSnapshot runtime_snapshot(const EditorCamera& camera) const;
};
// Persistent authored-world service over newline-delimited JSON-RPC 2.0.
// Simulation/renderer state is deliberately not stored in this document.
int run_world_session(const std::string& utf8_path);
}
