// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/scene.hpp"
#include "poima/editor_overlay.hpp"
#include "poima/ui_input.hpp"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace poima {
// Windows-first native child-window surface, independent of ImGui. The caller
// owns the HWND and must destroy this viewport before destroying that window.
// Construct, draw, resize, inspect and destroy on the HWND's owning thread.
// Multiple viewports may coexist and interleave draws on the same UI thread.
// They share an instance/loader lifetime but own separate Vulkan devices,
// swapchains and resources; destroying one does not invalidate the others.
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
    // Routes to the layout used by the last successful presentation. The
    // caller supplies the current authoritative projection; stale/hidden or
    // resized presentations cancel gestures and return an unconsumed result.
    // Coordinates are physical client pixels. Activation identifies a target
    // only: the owner must execute it through the native control service.
    UiInputResult ui_input(const std::shared_ptr<const ui::Presentation>& current,const UiInput&);
    void reset_ui_input();
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
