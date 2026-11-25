// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/core.hpp"
#include <memory>

namespace poima {
struct SceneSnapshot;

// Owned by a world service on its request thread. The caller keeps render
// configuration fixed for this lifetime. Each sample supplies a new scene,
// destination and presentation count.
class SceneCaptureSession {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    SceneCaptureSession();
    ~SceneCaptureSession();
    RenderReport capture(const RenderOptions&, const SceneSnapshot&);
};
}
