// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/scene.hpp"
#include <memory>

struct ImDrawData;

namespace poima {
// Logical ImGui screen coordinates; draw data supplies DisplayPos and DPI scale.
struct EditorRect { float x=0,y=0,width=0,height=0; };
struct EditorViewportResources {
    std::size_t meshes=0,materials=0,images=0,skin_instances=0,texture_bytes=0,skin_bytes=0;
};
// One main-thread SDL/Vulkan lifetime. An ImGui context must already exist
// and remain current/alive through viewport destruction.
// The frontend owns event polling and ImGui's SDL input backend.
class EditorViewport {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    EditorViewport(const RenderOptions& options,const SceneSnapshot& scene);
    ~EditorViewport();
    EditorViewport(const EditorViewport&)=delete;
    EditorViewport& operator=(const EditorViewport&)=delete;
    void* native_window() const;
    std::array<std::uint32_t,2> extent() const;
    void resize();
    // False means minimized/out-of-date/temporarily unavailable presentation.
    // Draw data is borrowed only through this call. Errors throw, with details
    // retained in report(). GPU synchronization failures require recreation.
    // Capture writes the full
    // window to the nonempty path supplied in RenderOptions::capture.
    bool draw(const SceneSnapshot& scene,EditorRect viewport,const ImDrawData* ui,bool capture=false);
    RenderReport report() const;
    EditorViewportResources resources() const;
};
}
