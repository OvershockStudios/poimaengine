// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/core.hpp"
#include "poima/scene.hpp"
#include <memory>
#include <string>

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
    bool draw_frame(const SceneSnapshot& scene,EditorRect viewport,const ImDrawData* ui,const std::string* capture_path);
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
    // Capture writes the full window to RenderOptions::capture. Ordinary draws
    // do not copy pixels to readback memory. Editor staging is bounded to
    // 128 MiB of RGBA pixels and remains available without a startup path.
    bool draw(const SceneSnapshot& scene,EditorRect viewport,const ImDrawData* ui,bool capture=false);
    // Draw and exclusively create a BMP at this nonempty, NUL-free UTF-8 path.
    // The frontend must preflight destination permissions and protected paths.
    // False publishes nothing; retry with a fresh scene/UI frame. This does not
    // change the configured final-capture path. Existing files are never replaced.
    bool draw_capture(const SceneSnapshot& scene,EditorRect viewport,const ImDrawData* ui,const std::string& capture_path);
    // capture_written means at least one capture completed during this viewport
    // lifetime; failures do not clear it. Dimensions and frames_presented refer
    // to successful presentations, including those whose file publication fails.
    RenderReport report() const;
    EditorViewportResources resources() const;
};
}
