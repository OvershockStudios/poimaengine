// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "poima/ui.hpp"
#include <optional>
#include <string>
namespace poima {
enum class UiElementKind { label,button };
struct UiFontSource { std::string family;std::vector<std::uint8_t> bytes; };
struct UiElementBinding { std::string id;UiElementKind kind=UiElementKind::label;std::string action; };
struct UiDocumentSource { std::string rml;std::vector<UiFontSource> fonts;std::vector<UiElementBinding> elements;std::vector<std::string> hit_regions; };
struct UiPointerTarget {std::optional<std::string> button;bool region=false;};
struct UiElementInspection {
    std::string id,action,text;UiElementKind kind=UiElementKind::label;
    bool visible=false,enabled=false,hittable=false,focused=false;
    std::array<float,4> bounds{}; // Untransformed border bounds in physical pixels.
    std::array<float,4> clip{}; // Ancestor scissor intersected with viewport.
};
// Native presentation only: semantic actions are returned to the owner, never
// executed as gameplay. No script host, disk resources, external images or URLs.
// Calls across documents serialize RmlUi's global state. Published frames own
// their atlas/geometry bytes and outlive this document. Time is caller supplied.
class UiDocument {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    explicit UiDocument(UiDocumentSource);
    ~UiDocument();
    UiDocument(const UiDocument&)=delete;UiDocument& operator=(const UiDocument&)=delete;
    std::shared_ptr<const UiFrame> frame(std::uint32_t width,std::uint32_t height,float scale,double time);
    std::vector<UiElementInspection> inspect();
    void set_text(const std::string& id,const std::string& plain_text);
    void set_enabled(const std::string& id,bool);
    void set_visible(const std::string& id,bool);
    // Actual clipped/transformed hit; disabled buttons still capture gestures.
    UiPointerTarget pointer_target(float x,float y);
    void pointer_move(float x,float y);
    void pointer_leave();
    void pointer_button(bool down);
    void pointer_wheel(float delta);
    bool focus(const std::optional<std::string>& id,bool scroll=true);
    void set_pressed(const std::string& id,bool);
    std::optional<std::string> pointer_activate(float x,float y);
    // Semantic/focus activation samples a deterministic 5x5 grid in the clipped
    // layout rectangle using actual RmlUi hit tests. Fully occluded/clipped
    // elements cannot activate. Transforms render normally; transformed or very
    // thin/partly occluded controls may lack a sampled point (hittable=false).
    std::optional<std::string> activate(const std::string& id);
    std::optional<std::string> focus_next(bool reverse=false);
    std::optional<std::string> activate_focused();
};
}
