// SPDX-License-Identifier: Apache-2.0
#include "poima/ui_presenter.hpp"
#include "poima/ui_document.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <iomanip>
#include <locale>
#include <sstream>
#include <span>
#include <stdexcept>
namespace poima {
std::span<const std::uint8_t> embedded_ui_font();
namespace {
void check(bool value,const char* message) {if(!value)throw std::invalid_argument(message);}
bool identity(const std::string& value) {
    return value.size()==32 && value!=std::string(32,'0') && value.find_first_not_of("0123456789abcdef")==std::string::npos;
}
std::string number(double value) {std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<value;return out.str();}
std::string length(const ui::Length& value) {return number(value.value)+(value.unit==ui::LengthUnit::dp?"dp":"%");}
std::string colors(const ui::ColorState& value) {
    std::string out;if(value.color)out+="color:"+*value.color+";";if(value.background_color)out+="background-color:"+*value.background_color+";";if(value.border_color)out+="border-color:"+*value.border_color+";";return out;
}
std::string authored_css(const ui::Element& e) {
    std::string rules,base;
    if(e.layout) {
        const auto& l=*e.layout;base+="margin:0dp;box-sizing:border-box;";
        if(e.kind==ui::Kind::panel)base+="display:flex;flex-direction:"+std::string(l.direction.value_or(ui::Direction::column)==ui::Direction::row?"row":"column")+";";
        if(l.position)base+="position:"+std::string(*l.position==ui::Position::absolute?"absolute":"relative")+";";
        const auto emit_length=[&](const char* key,const std::optional<ui::Length>& v){if(v)base+=std::string(key)+":"+length(*v)+";";};
        emit_length("width",l.width);emit_length("height",l.height);emit_length("min-width",l.min_width);emit_length("max-width",l.max_width);emit_length("min-height",l.min_height);emit_length("max-height",l.max_height);emit_length("left",l.left);emit_length("top",l.top);emit_length("right",l.right);emit_length("bottom",l.bottom);
        if(l.align) {const char* v=*l.align==ui::Align::start?"flex-start":*l.align==ui::Align::end?"flex-end":*l.align==ui::Align::center?"center":"stretch";base+="align-items:"+std::string(v)+";";}
        if(l.justify) {const char* v=*l.justify==ui::Justify::start?"flex-start":*l.justify==ui::Justify::end?"flex-end":*l.justify==ui::Justify::center?"center":"space-between";base+="justify-content:"+std::string(v)+";";}
        if(l.padding) {base+="padding:";for(const auto v:*l.padding)base+=number(v)+"dp ";base+=";";}
        if(l.gap)base+="gap:"+number(*l.gap)+"dp;";
        if(l.grow)base+="flex-grow:"+number(*l.grow)+";";
        if(l.shrink)base+="flex-shrink:"+number(*l.shrink)+";";
        if(l.overflow)base+="overflow:"+std::string(*l.overflow==ui::Overflow::visible?"visible":*l.overflow==ui::Overflow::hidden?"hidden":"auto")+";clip:"+(*l.overflow==ui::Overflow::visible?std::string("auto"):std::string("always"))+";";
    }
    if(e.style) {
        const auto& s=*e.style;base+=colors({s.color,s.background_color,s.border_color});
        if(s.font_size)base+="font-size:"+number(*s.font_size)+"dp;";
        if(s.border_width)base+="border-width:"+number(*s.border_width)+"dp;";
        if(s.border_radius)base+="border-radius:"+number(*s.border_radius)+"dp;";
        if(s.text_align)base+="text-align:"+std::string(*s.text_align==ui::TextAlign::left?"left":*s.text_align==ui::TextAlign::right?"right":"center")+";";
        const auto state=[&](const char* selector,const std::optional<ui::ColorState>& v){if(v)rules+="#"+e.id+selector+"{"+colors(*v)+"}";};
        state(":hover",s.hover);state(":focus",s.focus);state(":active",s.pressed);
        if(s.disabled)rules+="#"+e.id+":disabled,#"+e.id+".disabled{"+colors(*s.disabled)+"}";
    }
    return base.empty()?rules:"#"+e.id+"{"+base+"}"+rules;
}
std::uint32_t hit_signature(UiDocument& document,const UiElementInspection& row) {
    const auto left=std::max(row.bounds[0],row.clip[0]),top=std::max(row.bounds[1],row.clip[1]);
    const auto right=std::min(row.bounds[2],row.clip[2]),bottom=std::min(row.bounds[3],row.clip[3]);
    if(left>=right || top>=bottom)return 0;
    std::uint32_t mask=0;
    for(int y=0;y<5;++y)for(int x=0;x<5;++x)if(document.pointer_target(left+(right-left)*(float(x)+.5f)/5,top+(bottom-top)*(float(y)+.5f)/5).button==row.id)mask|=std::uint32_t{1}<<(y*5+x);
    return mask;
}
UiDocumentSource document_source(const ui::Presentation& projection) {
    check(projection.elements.size()<=256,"UI presentation element budget exceeded.");
    std::map<std::string,const ui::PresentationRow*> rows;
    std::map<std::string,std::vector<const ui::PresentationRow*>> children;
    std::string previous;std::size_t text_bytes=0;
    ui::Definition definition;definition.reserve(projection.elements.size());for(const auto& row:projection.elements)definition.push_back(row.element);ui::validate_definition(definition);
    for(const auto& row:projection.elements) {
        const auto& e=row.element;
        check(identity(e.id) && (previous.empty() || previous<e.id),"UI presentation IDs must be canonical, sorted and unique.");previous=e.id;
        check(e.parent.empty() || identity(e.parent),"UI presentation parent ID is invalid.");
        check(e.kind==ui::Kind::panel || e.kind==ui::Kind::label || e.kind==ui::Kind::button,"UI presentation kind is invalid.");
        check(e.text.size()<=16384,"UI presentation text exceeds its bound.");text_bytes+=e.text.size();check(text_bytes<=1024*1024,"UI presentation text budget exceeded.");rows.emplace(e.id,&row);children[e.parent].push_back(&row);
    }
    for(auto& [parent,siblings]:children) {(void)parent;std::sort(siblings.begin(),siblings.end(),[](const auto* a,const auto* b){const auto order=[](const auto* r){return r->element.layout?r->element.layout->order.value_or(0):0;};return order(a)!=order(b)?order(a)<order(b):a->element.id<b->element.id;});}
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
    const bool authored=std::any_of(projection.elements.begin(),projection.elements.end(),[](const auto& row){return row.element.layout || row.element.style;});
    if(authored) {
        std::string css="#ui-canvas{position:absolute;left:0dp;top:0dp;width:100%;height:100%;pointer-events:none;}";
        for(const auto& row:projection.elements) {
            const auto& e=row.element;css+=authored_css(e);css+="#"+e.id+".hidden{display:none;}";
            if(e.kind==ui::Kind::button || e.kind==ui::Kind::panel)css+="#"+e.id+"{pointer-events:"+(e.kind==ui::Kind::panel && e.layout && e.layout->hit_test==ui::HitTest::pass_through?std::string("none"):std::string("auto"))+";}";
        }
        source.rml.insert(source.rml.find("</style>"),css);
    }
    std::function<void(const ui::PresentationRow*)> append_row;
    append_row=[&](const ui::PresentationRow* row) {
            const auto& e=row->element;
            const std::string tag=e.kind==ui::Kind::button ? "button" : "div";
            std::string cls=e.kind==ui::Kind::panel ? "panel" : e.kind==ui::Kind::label ? "label" : "button";
            if(!row->effective_visible)cls+=" hidden";
            if(projection.modal==e.id)cls+=" modal";
            if(authored && !row->effective_enabled)cls+=" disabled";
            source.rml+="<"+tag+" id=\""+e.id+"\" class=\""+cls+"\">";
            if(e.kind==ui::Kind::panel) {if(!e.layout || e.layout->hit_test!=ui::HitTest::pass_through)source.hit_regions.push_back(e.id);for(const auto* child:children[e.id])append_row(child);}
            else source.elements.push_back({e.id,e.kind==ui::Kind::button ? UiElementKind::button : UiElementKind::label,e.action});
            source.rml+="</"+tag+">";
    };
    const auto& roots=children[""];const bool legacy=roots.empty() || std::any_of(roots.begin(),roots.end(),[](const auto* row){return !row->element.layout;});
    if(legacy) {source.hit_regions.push_back("ui-root");for(const auto* row:roots)if(!row->element.layout)append_row(row);source.rml+="</div>";}
    else source.rml.erase(source.rml.rfind("<div id=\"ui-root\">"));
    if(std::any_of(roots.begin(),roots.end(),[](const auto* row){return row->element.layout.has_value();})) {source.rml+="<div id=\"ui-canvas\">";for(const auto* row:roots)if(row->element.layout)append_row(row);source.rml+="</div>";}
    source.rml+="</body></rml>";return source;
}
}
struct UiPresenter::Impl {
    std::shared_ptr<const ui::Presentation> projection;
    std::unique_ptr<UiDocument> document;
    std::shared_ptr<const UiFrame> packet;
    std::uint32_t width=0,height=0;float scale=0;
    std::optional<std::string> focused,pointer_pressed,accept_pressed;
    std::optional<std::array<float,2>> pointer_position;
    std::optional<std::array<float,2>> pointer_origin;
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
        pointer_pressed.reset();accept_pressed.reset();pointer_origin.reset();
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
    const auto old_origin=s.pointer_origin;
    const bool compatible_extent=s.width==width && s.height==height && s.scale==scale;
    const bool compatible_modal=s.projection && s.projection->modal==projection->modal;
    std::vector<UiElementInspection> old_layout;
    if(changed && s.document && (old_pointer || old_accept))old_layout=s.document->inspect();
    std::uint32_t old_accept_hits=0;
    if(old_accept)for(const auto& row:old_layout)if(row.id==*old_accept)old_accept_hits=hit_signature(*s.document,row);
    const bool origin_hit=old_origin && old_pointer && s.document && s.document->pointer_target((*old_origin)[0],(*old_origin)[1]).button==old_pointer;
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
            return row!=projection->elements.end() && before!=old_layout.end() && after!=new_layout.end() && before->bounds==after->bounds && before->clip==after->clip && before->action==after->action;
        };
        if(preserved(old_pointer) && origin_hit && document->pointer_target((*old_origin)[0],(*old_origin)[1]).button==old_pointer) {s.pointer_pressed=old_pointer;s.pointer_origin=old_origin;if(s.pointer_position && document->pointer_target((*s.pointer_position)[0],(*s.pointer_position)[1]).button==old_pointer)document->set_pressed(*old_pointer,true);}
        if(preserved(old_accept))for(const auto& row:new_layout)if(row.id==*old_accept && old_accept_hits!=0 && hit_signature(*document,row)==old_accept_hits) {s.accept_pressed=old_accept;document->set_pressed(*old_accept,true);}
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
std::vector<UiElementInspection> UiPresenter::inspect() {
    auto& s=*impl_;check(s.document && s.width,"UI inspection requires a successful frame.");return s.document->inspect();
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
            if(hit.button && s.eligible(*hit.button)) {s.pointer_pressed=hit.button;s.pointer_origin=std::array<float,2>{event.x,event.y};s.focused=hit.button;s.document->focus(s.focused,false);s.document->set_pressed(*hit.button,true);}
        }break;
    case UiInputKind::pointer_up:
        if(s.pointer_captured)s.document->pointer_button(false);
        if(s.pointer_pressed && hit.button==s.pointer_pressed && s.eligible(*s.pointer_pressed))result.activated=s.pointer_pressed;
        if(s.pointer_pressed)s.document->set_pressed(*s.pointer_pressed,false);
        s.pointer_pressed.reset();s.pointer_origin.reset();s.pointer_captured=false;break;
    case UiInputKind::pointer_leave:
        result.consumed=result.consumed || s.pointer_captured;s.document->pointer_leave();
        s.pointer_position.reset();
        if(s.pointer_pressed)s.document->set_pressed(*s.pointer_pressed,false);
        s.pointer_pressed.reset();s.pointer_origin.reset();break;
    case UiInputKind::pointer_wheel:
        if(result.consumed) {s.unpress();s.document->pointer_wheel(event.delta);}break;
    case UiInputKind::focus_next:case UiInputKind::focus_previous: {
        s.unpress();std::vector<std::string> ids;for(const auto& row:s.document->inspect())if(s.eligible(row.id))ids.push_back(row.id);
        if(!ids.empty()) {
            const auto found=s.focused?std::find(ids.begin(),ids.end(),*s.focused):ids.end();
            const bool reverse=event.kind==UiInputKind::focus_previous;
            const auto index=found==ids.end() ? (reverse?ids.size()-1:0) : (static_cast<std::size_t>(found-ids.begin())+(reverse?ids.size()-1:1))%ids.size();
            if(s.document->focus(ids[index]))s.focused=ids[index];
            result.consumed=true;
        }break;
    }
    case UiInputKind::accept_down:
        if(s.focused && s.eligible(*s.focused)) {result.consumed=true;if(!s.accept_captured) {s.unpress();if(s.document->activate(*s.focused)) {s.accept_pressed=s.focused;s.document->set_pressed(*s.focused,true);}}s.accept_captured=true;}break;
    case UiInputKind::accept_up:
        result.consumed=result.consumed || s.accept_captured;
        if(s.accept_pressed && s.accept_pressed==s.focused && s.eligible(*s.accept_pressed) && s.document->activate(*s.accept_pressed))result.activated=s.accept_pressed;
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
