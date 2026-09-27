// SPDX-License-Identifier: Apache-2.0
#include "poima/editor_viewport.hpp"
#include <stdexcept>
namespace poima {
struct EditorViewport::Impl {};
EditorViewport::EditorViewport(const RenderOptions&,const SceneSnapshot&) {
    throw std::runtime_error("This build has no native editor viewport.");
}
EditorViewport::~EditorViewport()=default;
void* EditorViewport::native_window() const { return nullptr; }
std::array<std::uint32_t,2> EditorViewport::extent() const { return {}; }
void EditorViewport::resize() {}
bool EditorViewport::draw(const SceneSnapshot&,EditorRect,const ImDrawData*,bool) { return false; }
bool EditorViewport::draw_capture(const SceneSnapshot&,EditorRect,const ImDrawData*,const std::string&) { return false; }
bool EditorViewport::draw_frame(const SceneSnapshot&,EditorRect,const ImDrawData*,const std::string*) { return false; }
RenderReport EditorViewport::report() const { RenderReport r;r.available=false;r.detail="This build has no native editor viewport.";return r; }
EditorViewportResources EditorViewport::resources() const { return {}; }
}
