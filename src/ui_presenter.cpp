// SPDX-License-Identifier: Apache-2.0
#include "poima/ui_presenter.hpp"
#include "poima/ui_document.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <span>
#include <stdexcept>
namespace poima {
std::span<const std::uint8_t> embedded_ui_font();
namespace {
void check(bool value,const char* message) {if(!value)throw std::invalid_argument(message);}
bool identity(const std::string& value) {
    return value.size()==32 && value!=std::string(32,'0') && value.find_first_not_of("0123456789abcdef")==std::string::npos;
}
UiDocumentSource document_source(const ui::Presentation& projection) {
    check(projection.elements.size()<=256,"UI presentation element budget exceeded.");
    std::map<std::string,const ui::PresentationRow*> rows;
    std::map<std::string,std::vector<const ui::PresentationRow*>> children;
    std::string previous;std::size_t text_bytes=0;
    for(const auto& row:projection.elements) {
        const auto& e=row.element;
        check(identity(e.id) && (previous.empty() || previous<e.id),"UI presentation IDs must be canonical, sorted and unique.");previous=e.id;
        check(e.parent.empty() || identity(e.parent),"UI presentation parent ID is invalid.");
        check(e.kind==ui::Kind::panel || e.kind==ui::Kind::label || e.kind==ui::Kind::button,"UI presentation kind is invalid.");
        check(e.text.size()<=16384,"UI presentation text exceeds its bound.");text_bytes+=e.text.size();check(text_bytes<=1024*1024,"UI presentation text budget exceeded.");rows.emplace(e.id,&row);children[e.parent].push_back(&row);
    }
    for(const auto& row:projection.elements) {
        std::size_t depth=1;
        for(auto parent=row.element.parent;!parent.empty();) {
            const auto found=rows.find(parent);check(found!=rows.end() && found->second->element.kind==ui::Kind::panel,"UI presentation parent must be a panel.");
            check(++depth<=32,"UI presentation hierarchy depth/cycle is invalid.");parent=found->second->element.parent;
        }
    }
    UiDocumentSource source;const auto font=embedded_ui_font();source.fonts.push_back({"Poima Runtime Inter",{font.begin(),font.end()}});
    source.rml=R"(<rml><head><style>
body { margin: 0px; width: 100%; height: 100%; font-family: Poima Runtime Inter; font-size: 18dp; color: #edf2fa; }
#ui-root { position: absolute; left: 3%; top: 3%; width: 44%; max-height: 94%; overflow: auto; clip: always; }
scrollbarvertical { width: 10dp; background-color: #152132; }
scrollbarvertical sliderbar { width: 10dp; min-height: 20dp; background-color: #51657d; }
scrollbarhorizontal { height: 10dp; }
.panel { display: block; padding: 10dp; margin-bottom: 8dp; background-color: #152132d9; border-width: 1dp; border-color: #51657d; }
.modal { border-color: #89c4ff; background-color: #152132f5; }
.label { display: block; margin-bottom: 8dp; white-space: pre-wrap; word-break: break-all; }
button { display: block; width: 100%; box-sizing: border-box; padding: 9dp; margin-bottom: 8dp; background-color: #356695; color: #ffffff; white-space: pre-wrap; word-break: break-all; tab-index: auto; }
button:disabled { background-color: #34404d; color: #9ea9b8; }
button:focus { background-color: #477fac; }
button:hover { background-color: #477fac; }
button:active { background-color: #214568; }
.hidden { display: none; }
</style></head><body><div id="ui-root">)";
    std::function<void(const std::string&)> append=[&](const std::string& parent) {
        for(const auto* row:children[parent]) {
            const auto& e=row->element;
            const std::string tag=e.kind==ui::Kind::button ? "button" : "div";
            std::string cls=e.kind==ui::Kind::panel ? "panel" : e.kind==ui::Kind::label ? "label" : "button";
            if(!row->effective_visible)cls+=" hidden";
            if(projection.modal==e.id)cls+=" modal";
            source.rml+="<"+tag+" id=\""+e.id+"\" class=\""+cls+"\">";
            if(e.kind==ui::Kind::panel) {source.hit_regions.push_back(e.id);append(e.id);}
            else source.elements.push_back({e.id,e.kind==ui::Kind::button ? UiElementKind::button : UiElementKind::label,e.action});
            source.rml+="</"+tag+">";
        }
    };
    source.hit_regions.push_back("ui-root");append("");source.rml+="</div></body></rml>";return source;
}
}
struct UiPresenter::Impl {
    std::shared_ptr<const ui::Presentation> projection;
    std::unique_ptr<UiDocument> document;
    std::shared_ptr<const UiFrame> packet;
    std::uint32_t width=0,height=0;float scale=0;
    std::optional<std::string> focused,pointer_pressed,accept_pressed;
    std::optional<std::array<float,2>> pointer_position;
    bool pointer_captured=false,accept_captured=false;
    std::uint64_t generation=0;
    bool eligible(const std::string& id) const {
        if(!projection)return false;
        const auto it=std::lower_bound(projection->elements.begin(),projection->elements.end(),id,[](const auto& row,const auto& key){return row.element.id<key;});
        return it!=projection->elements.end() && it->element.id==id && it->eligible && it->element.kind==ui::Kind::button;
    }
    void unpress() {
        if(pointer_pressed)document->set_pressed(*pointer_pressed,false);
        if(accept_pressed)document->set_pressed(*accept_pressed,false);
        pointer_pressed.reset();accept_pressed.reset();
    }
    void dirty() {check(generation<9007199254740991ULL,"UI presentation generation exhausted.");++generation;packet.reset();}
};
UiPresenter::UiPresenter():impl_(std::make_unique<Impl>()) {}
UiPresenter::~UiPresenter()=default;
std::shared_ptr<const UiFrame> UiPresenter::frame(std::shared_ptr<const ui::Presentation> projection,std::uint32_t width,std::uint32_t height,float scale) {
    check(projection!=nullptr,"UI presentation is absent.");
    check(width>=1 && width<=8192 && height>=1 && height<=8192 && std::isfinite(scale) && scale>=.25f && scale<=8,"Invalid UI presentation extent/scale.");
    auto& s=*impl_;
    if(s.projection==projection && s.width==width && s.height==height && s.scale==scale && s.packet)return s.packet;
    const bool changed=s.projection!=projection || s.width!=width || s.height!=height || s.scale!=scale;
    const auto old_pointer=s.pointer_pressed,old_accept=s.accept_pressed;
    const bool compatible_extent=s.width==width && s.height==height && s.scale==scale;
    const bool compatible_modal=s.projection && s.projection->modal==projection->modal;
    std::vector<UiElementInspection> old_layout;
    if(changed && s.document && (old_pointer || old_accept))old_layout=s.document->inspect();
    std::unique_ptr<UiDocument> replacement;
    auto* document=s.document.get();
    if(s.projection!=projection || !document) {
        replacement=std::make_unique<UiDocument>(document_source(*projection));document=replacement.get();
        for(const auto& row:projection->elements)if(row.element.kind!=ui::Kind::panel) {
            document->set_text(row.element.id,row.element.text);
            document->set_enabled(row.element.id,row.element.kind==ui::Kind::button ? row.eligible : row.effective_enabled);
        }
    }
    if(changed && s.document)s.unpress();
    auto result=document->frame(width,height,scale,0);
    if(changed && s.focused) {
        const auto it=std::find_if(projection->elements.begin(),projection->elements.end(),[&](const auto& row){return row.element.id==*s.focused && row.eligible;});
        if(it!=projection->elements.end()) {document->focus(s.focused);result=document->frame(width,height,scale,0);}
    }
    if(changed && s.pointer_position) {document->pointer_move((*s.pointer_position)[0],(*s.pointer_position)[1]);result=document->frame(width,height,scale,0);}
    if(changed && compatible_extent && compatible_modal && !old_layout.empty()) {
        const auto new_layout=document->inspect();
        auto preserved=[&](const std::optional<std::string>& id) {
            if(!id)return false;
            const auto row=std::find_if(projection->elements.begin(),projection->elements.end(),[&](const auto& r){return r.element.id==*id && r.eligible;});
            const auto before=std::find_if(old_layout.begin(),old_layout.end(),[&](const auto& r){return r.id==*id;});
            const auto after=std::find_if(new_layout.begin(),new_layout.end(),[&](const auto& r){return r.id==*id;});
            return row!=projection->elements.end() && before!=old_layout.end() && after!=new_layout.end() && before->bounds==after->bounds && before->clip==after->clip;
        };
        if(preserved(old_pointer)) {s.pointer_pressed=old_pointer;if(s.pointer_position && document->pointer_target((*s.pointer_position)[0],(*s.pointer_position)[1]).button==old_pointer)document->set_pressed(*old_pointer,true);}
        if(preserved(old_accept)) {s.accept_pressed=old_accept;document->set_pressed(*old_accept,true);}
        result=document->frame(width,height,scale,0);
    }
    check(!changed || s.generation<9007199254740991ULL,"UI presentation generation exhausted.");
    auto packet=*result;packet.revision=s.generation+(changed?1:0);auto frozen=freeze_ui_frame(std::move(packet));
    if(replacement)s.document=std::move(replacement);
    s.projection=std::move(projection);s.packet=std::move(frozen);s.width=width;s.height=height;s.scale=scale;
    if(changed)++s.generation;
    if(s.focused && !s.eligible(*s.focused)) {s.focused.reset();s.document->focus({});}
    return s.packet;
}
UiInputResult UiPresenter::input(const UiInput& event,bool activate) {
    auto& s=*impl_;check(s.document && s.width,"UI input requires a successful frame.");
    check(std::isfinite(event.x) && std::isfinite(event.y) && std::abs(event.x)<=1000000 && std::abs(event.y)<=1000000 &&
        std::isfinite(event.delta) && std::abs(event.delta)<=100,"Invalid UI input coordinates/wheel.");
    check(event.kind>=UiInputKind::pointer_move && event.kind<=UiInputKind::pointer_wheel,"Invalid UI input kind.");
    UiInputResult result;result.consumed=!s.projection->modal.empty();
    const auto pointer=event.kind==UiInputKind::pointer_move || event.kind==UiInputKind::pointer_down || event.kind==UiInputKind::pointer_up || event.kind==UiInputKind::pointer_wheel;
    if(!activate) {
        // The owner has newer authoritative state than the displayed layout.
        // Never act on old eligibility, but do not turn an old visible button
        // or its owned release into a gameplay/capture event either.
        if(s.pointer_captured)s.document->pointer_button(false);
        s.unpress();
        if(pointer) {
            const auto hit=s.document->pointer_target(event.x,event.y);
            result.consumed=result.consumed || s.pointer_captured || (event.kind!=UiInputKind::pointer_up && (hit.region || hit.button.has_value()));
        }
        switch(event.kind) {
        case UiInputKind::pointer_down:s.pointer_captured=result.consumed;break;
        case UiInputKind::pointer_up:s.pointer_captured=false;break;
        case UiInputKind::pointer_leave:result.consumed=result.consumed || s.pointer_captured;break;
        case UiInputKind::focus_next:case UiInputKind::focus_previous:
            result.consumed=result.consumed || std::any_of(s.projection->elements.begin(),s.projection->elements.end(),[&](const auto& row){return s.eligible(row.element.id);});break;
        case UiInputKind::accept_down:
            result.consumed=result.consumed || s.accept_captured || s.focused.has_value();s.accept_captured=result.consumed;break;
        case UiInputKind::accept_up:result.consumed=result.consumed || s.accept_captured;s.accept_captured=false;break;
        case UiInputKind::cancel:
            result.consumed=result.consumed || s.pointer_captured || s.accept_captured || s.focused.has_value();reset_input();result.presentation_revision=s.generation;return result;
        default:break;
        }
        s.dirty();result.focused=s.focused;result.presentation_revision=s.generation;return result;
    }
    UiPointerTarget hit;
    if(pointer) {s.pointer_position=std::array<float,2>{event.x,event.y};s.document->pointer_move(event.x,event.y);hit=s.document->pointer_target(event.x,event.y);result.consumed=result.consumed || s.pointer_captured || (event.kind!=UiInputKind::pointer_up && (hit.region || hit.button.has_value()));}
    switch(event.kind) {
    case UiInputKind::pointer_move:
        if(s.pointer_pressed)s.document->set_pressed(*s.pointer_pressed,hit.button==s.pointer_pressed);
        break;
    case UiInputKind::pointer_down:
        if(!s.pointer_captured) {
            s.unpress();
            s.pointer_captured=result.consumed;
            if(s.pointer_captured)s.document->pointer_button(true);
            if(hit.button && s.eligible(*hit.button)) {s.pointer_pressed=hit.button;s.focused=hit.button;s.document->focus(s.focused,false);s.document->set_pressed(*hit.button,true);}
        }break;
    case UiInputKind::pointer_up:
        if(s.pointer_captured)s.document->pointer_button(false);
        if(s.pointer_pressed && hit.button==s.pointer_pressed && s.eligible(*s.pointer_pressed))result.activated=s.pointer_pressed;
        if(s.pointer_pressed)s.document->set_pressed(*s.pointer_pressed,false);
        s.pointer_pressed.reset();s.pointer_captured=false;break;
    case UiInputKind::pointer_leave:
        result.consumed=result.consumed || s.pointer_captured;s.document->pointer_leave();
        s.pointer_position.reset();
        if(s.pointer_pressed)s.document->set_pressed(*s.pointer_pressed,false);
        s.pointer_pressed.reset();break;
    case UiInputKind::pointer_wheel:
        if(result.consumed) {s.unpress();s.document->pointer_wheel(event.delta);}break;
    case UiInputKind::focus_next:case UiInputKind::focus_previous: {
        s.unpress();std::vector<std::string> ids;for(const auto& row:s.projection->elements)if(s.eligible(row.element.id))ids.push_back(row.element.id);
        if(!ids.empty()) {
            const auto found=s.focused?std::find(ids.begin(),ids.end(),*s.focused):ids.end();
            const bool reverse=event.kind==UiInputKind::focus_previous;
            const auto index=found==ids.end() ? (reverse?ids.size()-1:0) : (static_cast<std::size_t>(found-ids.begin())+(reverse?ids.size()-1:1))%ids.size();
            if(s.document->focus(ids[index]))s.focused=ids[index];
            result.consumed=true;
        }break;
    }
    case UiInputKind::accept_down:
        if(s.focused && s.eligible(*s.focused)) {result.consumed=true;if(!s.accept_captured) {s.unpress();s.accept_pressed=s.focused;s.document->set_pressed(*s.focused,true);}s.accept_captured=true;}break;
    case UiInputKind::accept_up:
        result.consumed=result.consumed || s.accept_captured;
        if(s.accept_pressed && s.accept_pressed==s.focused && s.eligible(*s.accept_pressed))result.activated=s.accept_pressed;
        if(s.accept_pressed)s.document->set_pressed(*s.accept_pressed,false);
        s.accept_pressed.reset();s.accept_captured=false;break;
    case UiInputKind::cancel:
        result.consumed=result.consumed || s.pointer_captured || s.accept_captured || s.focused.has_value();reset_input();result.presentation_revision=s.generation;return result;
    }
    s.dirty();result.focused=s.focused;result.presentation_revision=s.generation;return result;
}
void UiPresenter::reset_input() {
    auto& s=*impl_;if(!s.document)return;if(s.pointer_captured)s.document->pointer_button(false);s.unpress();s.document->pointer_leave();s.document->focus({});s.focused.reset();s.pointer_position.reset();s.pointer_captured=s.accept_captured=false;s.dirty();
}
}
