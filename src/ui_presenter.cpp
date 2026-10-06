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
#ui-root { position: absolute; left: 3%; top: 3%; width: 44%; }
.panel { display: block; padding: 10dp; margin-bottom: 8dp; background-color: #152132d9; border-width: 1dp; border-color: #51657d; }
.modal { border-color: #89c4ff; background-color: #152132f5; }
.label { display: block; margin-bottom: 8dp; white-space: pre-wrap; word-break: break-all; }
button { display: block; width: 100%; box-sizing: border-box; padding: 9dp; margin-bottom: 8dp; background-color: #356695; color: #ffffff; white-space: pre-wrap; word-break: break-all; tab-index: auto; }
button:disabled { background-color: #34404d; color: #9ea9b8; }
button:focus { background-color: #477fac; }
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
            if(e.kind==ui::Kind::panel)append(e.id);
            else source.elements.push_back({e.id,e.kind==ui::Kind::button ? UiElementKind::button : UiElementKind::label,e.action});
            source.rml+="</"+tag+">";
        }
    };
    append("");source.rml+="</div></body></rml>";return source;
}
}
struct UiPresenter::Impl {
    std::shared_ptr<const ui::Presentation> projection;
    std::unique_ptr<UiDocument> document;
    std::shared_ptr<const UiFrame> packet;
    std::uint32_t width=0,height=0;float scale=0;
};
UiPresenter::UiPresenter():impl_(std::make_unique<Impl>()) {}
UiPresenter::~UiPresenter()=default;
std::shared_ptr<const UiFrame> UiPresenter::frame(std::shared_ptr<const ui::Presentation> projection,std::uint32_t width,std::uint32_t height,float scale) {
    check(projection!=nullptr,"UI presentation is absent.");
    check(width>=1 && width<=8192 && height>=1 && height<=8192 && std::isfinite(scale) && scale>=.25f && scale<=8,"Invalid UI presentation extent/scale.");
    auto& s=*impl_;
    if(s.projection==projection && s.width==width && s.height==height && s.scale==scale && s.packet)return s.packet;
    std::unique_ptr<UiDocument> replacement;
    auto* document=s.document.get();
    if(s.projection!=projection || !document) {
        replacement=std::make_unique<UiDocument>(document_source(*projection));document=replacement.get();
        for(const auto& row:projection->elements)if(row.element.kind!=ui::Kind::panel) {
            document->set_text(row.element.id,row.element.text);
            document->set_enabled(row.element.id,row.element.kind==ui::Kind::button ? row.eligible : row.effective_enabled);
        }
    }
    auto result=document->frame(width,height,scale,0);
    // Packet revisions describe authoritative values, not layout invocation count.
    auto packet=*result;packet.revision=projection->revision;auto frozen=freeze_ui_frame(std::move(packet));
    if(replacement)s.document=std::move(replacement);
    s.projection=std::move(projection);s.packet=std::move(frozen);s.width=width;s.height=height;s.scale=scale;return s.packet;
}
}
