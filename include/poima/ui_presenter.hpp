// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/ui.hpp"
#include "poima/ui_model.hpp"
#include "poima/ui_input.hpp"
#include "poima/ui_document.hpp"
#include <memory>
namespace poima {
// Immutable observations of the exact RmlUi document which emitted a frame.
// Bounds are untransformed border rectangles, clip is ancestor/viewport scissor;
// neither is glyph ink, transformed coverage nor an occlusion-area estimate.
struct UiLayoutObservation {
    std::uint32_t width=0,height=0;
    float scale=1;
    std::uint64_t generation=0;
    std::shared_ptr<const ui::Presentation> projection;
    // Shares the exact UiDocument frame inspection; no second copy of IDs,
    // actions or text. Immutable storage outlives document/projection replacement.
    std::shared_ptr<const std::vector<UiElementInspection>> controls; // Labels/buttons only.
};
// Default nested vertical layout. All authoritative values arrive in an owned
// projection; generated RML and the embedded font are presentation resources.
class UiPresenter {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    UiPresenter();
    ~UiPresenter();
    UiPresenter(const UiPresenter&)=delete;
    UiPresenter& operator=(const UiPresenter&)=delete;
    std::shared_ptr<const UiFrame> frame(std::shared_ptr<const ui::Presentation>,std::uint32_t width,std::uint32_t height,float scale=1);
    // Requires a successful frame. Returns current viewport-local layout in
    // authored visual traversal order, without executing actions.
    std::vector<UiElementInspection> inspect();
    // Read the last successful frame's immutable observations. Does not update
    // the DOM, advance focus/scroll, regenerate geometry or execute actions.
    std::shared_ptr<const UiLayoutObservation> layout_observation() const;
    // Requires a successful frame. Coordinates are physical viewport-local;
    // positive wheel delta scrolls down, bounded to 100 lines per event.
    // activate=false consumes only the last displayed regions/owned releases.
    // It cancels activation and does not navigate, scroll or mutate layout.
    UiInputResult input(const UiInput&,bool activate=true);
    // The owner must reset on runtime/session replacement or viewport focus loss.
    // Ordinary HUD projections preserve held gestures only while target geometry,
    // eligibility, modal ownership, extent and DPI remain unchanged.
    void reset_input();
};
}
