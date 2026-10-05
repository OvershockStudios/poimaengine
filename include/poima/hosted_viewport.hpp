// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/scene.hpp"
#include "poima/editor_overlay.hpp"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace poima {
// Windows-first native child-window surface, independent of ImGui. The caller
// owns the HWND and must destroy this viewport before destroying that window.
// Construct, draw, resize, inspect and destroy on the HWND's owning thread.
// Only one graphics lifetime may be active: Vulkan dispatch is process-global.
class HostedViewport {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool draw_frame(const SceneSnapshot&,const std::string* capture_path);
public:
    HostedViewport(const RenderOptions&,const SceneSnapshot&,void* hwnd);
    ~HostedViewport();
    HostedViewport(const HostedViewport&)=delete;
    HostedViewport& operator=(const HostedViewport&)=delete;
    // Current HWND client extent, including before graphics initialization or
    // while hidden. report() retains the extent of the last presentation.
    std::array<std::uint32_t,2> extent() const;
    void resize();
    // Retains a bounded copy without initializing graphics. Empty clears;
    // otherwise count must be divisible by three and <=65536. Coordinates
    // and linear RGBA must be finite and in [0,1]. Invalid input preserves the
    // prior overlay. Drawn after scene resolve, included in viewport captures.
    void set_overlay(const std::vector<EditorOverlayVertex>& triangles);
    // False means hidden, minimized, zero-size or temporarily unavailable.
    // Calls retain their own snapshot copy. Graphics failures throw and require
    // recreation; failure details remain available through report().
    bool draw(const SceneSnapshot&,bool capture=false);
    // Exclusively creates a BMP. The host must validate protected paths before
    // calling; existing output files are never replaced. False writes nothing.
    bool draw_capture(const SceneSnapshot&,const std::string& path);
    // capture_written is cumulative; extent/frame count refer to presentation.
    // One readback image is bounded to 128 MiB of RGBA pixels.
    RenderReport report() const;
};
}
