// SPDX-License-Identifier: Apache-2.0
#include "poima/ui_document.hpp"
#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <RmlUi/Core/ElementText.h>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
namespace poima {
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void text_bound(const std::string& text,std::size_t maximum,bool empty=false) {
    check((empty || !text.empty()) && text.size()<=maximum && text.find('\0')==std::string::npos,"UI text exceeds bounds or contains NUL.");
    std::size_t at=0;
    while(at<text.size()) {
        const auto first=static_cast<unsigned char>(text[at++]);if(first<0x80)continue;
        unsigned count=0;std::uint32_t value=0,minimum=0;
        if(first>=0xc2 && first<=0xdf) {count=1;value=first&31;minimum=0x80;}
        else if(first>=0xe0 && first<=0xef) {count=2;value=first&15;minimum=0x800;}
        else if(first>=0xf0 && first<=0xf4) {count=3;value=first&7;minimum=0x10000;}
        else check(false,"UI text is not valid UTF-8.");
        check(count<=text.size()-at,"Truncated UI UTF-8.");
        while(count--) {const auto byte=static_cast<unsigned char>(text[at++]);check((byte&0xc0)==0x80,"UI text is not valid UTF-8.");value=(value<<6)|(byte&63);}
        check(value>=minimum && value<=0x10ffff && !(value>=0xd800 && value<=0xdfff),"Invalid UI UTF-8 code point.");
    }
}
void diagnose(std::string& error,const std::string& message) {if(error.empty())error=message.substr(0,2048);}
struct System final:Rml::SystemInterface {
    double time=0;std::string* error=nullptr;
    double GetElapsedTime() override {return time;}
    bool LogMessage(Rml::Log::Type level,const Rml::String& message) override {
        if(error && (level==Rml::Log::LT_ERROR || level==Rml::Log::LT_WARNING || level==Rml::Log::LT_ASSERT))diagnose(*error,"RmlUi: "+message);
        return true;
    }
    void SetClipboardText(const Rml::String&) override {}
    void GetClipboardText(Rml::String& value) override {value.clear();}
};
struct Files final:Rml::FileInterface {
    System* system=nullptr;
    Rml::FileHandle Open(const Rml::String&) override {if(system->error)diagnose(*system->error,"UI external file/resource access is disabled.");return 0;}
    void Close(Rml::FileHandle) override {}
    std::size_t Read(void*,std::size_t,Rml::FileHandle) override {return 0;}
    bool Seek(Rml::FileHandle,long,int) override {return false;}
    std::size_t Tell(Rml::FileHandle) override {return 0;}
};
struct Globals {
    std::mutex mutex;System system;Files files;std::size_t users=0,font_bytes=0;std::uint64_t next=0;
    struct Font {std::shared_ptr<const std::vector<std::uint8_t>> bytes;bool loaded=false;};
    std::map<std::string,Font> fonts;
    Globals() {files.system=&system;}
    void acquire() {
        if(users==0) {Rml::SetSystemInterface(&system);Rml::SetFileInterface(&files);check(Rml::Initialise(),"RmlUi initialization failed.");}
        ++users;
    }
    void release() {
        if(--users==0) {Rml::Shutdown();fonts.clear();font_bytes=0;Rml::SetSystemInterface(nullptr);Rml::SetFileInterface(nullptr);}
    }
};
Globals& globals() {static Globals value;return value;}
struct Operation {
    System& system;std::string* previous;double previous_time;
    Operation(std::string& error,double time):system(globals().system),previous(system.error),previous_time(system.time) {system.error=&error;system.time=time;}
    ~Operation() {system.error=previous;system.time=previous_time;}
};
struct Renderer final:Rml::RenderInterface {
    struct Geometry {std::vector<UiVertex> vertices;std::vector<std::uint32_t> indices;};
    std::string& error;std::map<Rml::CompiledGeometryHandle,Geometry> geometry;
    std::map<Rml::TextureHandle,UiTexture> textures;std::uintptr_t next_geometry=1,next_texture=1;
    std::size_t resident_vertices=0,resident_indices=0,resident_bytes=0;
    UiFrame packet;std::map<Rml::TextureHandle,std::uint32_t> frame_textures;
    bool scissor_enabled=false;std::array<std::int32_t,4> clip{};std::array<float,16> matrix=UiDraw{}.transform;
    explicit Renderer(std::string& e):error(e) {}
    void unsupported(const char* feature) {diagnose(error,std::string("Unsupported UI rendering feature: ")+feature);}
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,Rml::Span<const int> indices) override {
        if(vertices.size()>max_ui_vertices-resident_vertices || indices.size()>max_ui_indices-resident_indices) {diagnose(error,"UI resident geometry budget exceeded.");return 0;}
        Geometry value;value.vertices.reserve(vertices.size());value.indices.reserve(indices.size());
        for(const auto& v:vertices)value.vertices.push_back({v.position.x,v.position.y,v.tex_coord.x,v.tex_coord.y,{v.colour.red,v.colour.green,v.colour.blue,v.colour.alpha}});
        for(int i:indices) {if(i<0 || static_cast<std::size_t>(i)>=vertices.size()) {diagnose(error,"Invalid UI geometry index.");return 0;}value.indices.push_back(static_cast<std::uint32_t>(i));}
        const auto handle=next_geometry++;geometry.emplace(handle,std::move(value));resident_vertices+=vertices.size();resident_indices+=indices.size();return handle;
    }
    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {const auto it=geometry.find(handle);if(it!=geometry.end()) {resident_vertices-=it->second.vertices.size();resident_indices-=it->second.indices.size();geometry.erase(it);}}
    void RenderGeometry(Rml::CompiledGeometryHandle handle,Rml::Vector2f translation,Rml::TextureHandle texture) override {
        if(!error.empty())return;
        const auto found=geometry.find(handle);if(found==geometry.end()) {diagnose(error,"UI geometry handle is absent.");return;}const auto& mesh=found->second;
        if(packet.draws.size()>=max_ui_draws || mesh.vertices.size()>max_ui_vertices-packet.vertices.size() || mesh.indices.size()>max_ui_indices-packet.indices.size()) {diagnose(error,"UI frame geometry budget exceeded.");return;}
        UiDraw draw;draw.first_index=static_cast<std::uint32_t>(packet.indices.size());draw.index_count=static_cast<std::uint32_t>(mesh.indices.size());draw.transform=matrix;draw.translation={translation.x,translation.y};
        draw.scissor=scissor_enabled ? clip : std::array<std::int32_t,4>{0,0,static_cast<int>(packet.width),static_cast<int>(packet.height)};
        draw.scissor[0]=std::clamp(draw.scissor[0],0,static_cast<int>(packet.width));draw.scissor[2]=std::clamp(draw.scissor[2],draw.scissor[0],static_cast<int>(packet.width));
        draw.scissor[1]=std::clamp(draw.scissor[1],0,static_cast<int>(packet.height));draw.scissor[3]=std::clamp(draw.scissor[3],draw.scissor[1],static_cast<int>(packet.height));
        if(texture) {
            auto present=frame_textures.find(texture);
            if(present==frame_textures.end()) {
                const auto image=textures.find(texture);if(image==textures.end()) {diagnose(error,"UI texture handle is absent.");return;}
                if(packet.textures.size()>=max_ui_textures) {diagnose(error,"UI frame texture budget exceeded.");return;}
                const auto index=static_cast<std::uint32_t>(packet.textures.size());packet.textures.push_back(image->second);present=frame_textures.emplace(texture,index).first;
            }
            draw.texture=present->second;
        }
        const auto offset=static_cast<std::uint32_t>(packet.vertices.size());packet.vertices.insert(packet.vertices.end(),mesh.vertices.begin(),mesh.vertices.end());
        for(auto i:mesh.indices)packet.indices.push_back(offset+i);
        packet.draws.push_back(draw);
    }
    Rml::TextureHandle LoadTexture(Rml::Vector2i&,const Rml::String&) override {unsupported("external texture resource");return 0;}
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> bytes,Rml::Vector2i extent) override {
        if(extent.x<1 || extent.y<1 || extent.x>4096 || extent.y>4096 || bytes.size()!=std::size_t(extent.x)*std::size_t(extent.y)*4 || bytes.size()>max_ui_texture_bytes-resident_bytes || textures.size()>=max_ui_textures) {diagnose(error,"UI generated texture budget/extent invalid.");return 0;}
        UiTexture texture;texture.width=static_cast<std::uint32_t>(extent.x);texture.height=static_cast<std::uint32_t>(extent.y);texture.rgba.assign(bytes.begin(),bytes.end());const auto handle=next_texture++;textures.emplace(handle,std::move(texture));resident_bytes+=bytes.size();return handle;
    }
    void ReleaseTexture(Rml::TextureHandle handle) override {const auto it=textures.find(handle);if(it!=textures.end()) {resident_bytes-=it->second.rgba.size();textures.erase(it);}}
    void EnableScissorRegion(bool enable) override {scissor_enabled=enable;}
    void SetScissorRegion(Rml::Rectanglei r) override {clip={r.Left(),r.Top(),r.Right(),r.Bottom()};}
    void SetTransform(const Rml::Matrix4f* transform) override {matrix=UiDraw{}.transform;if(transform)for(int row=0;row<4;++row)for(int col=0;col<4;++col)matrix[static_cast<std::size_t>(4*col+row)]=transform->GetRow(row)[col];}
    void EnableClipMask(bool enable) override {if(enable)unsupported("clip mask");}
    void RenderToClipMask(Rml::ClipMaskOperation,Rml::CompiledGeometryHandle,Rml::Vector2f) override {unsupported("clip mask");}
    Rml::LayerHandle PushLayer() override {unsupported("layer");return 0;}
    void CompositeLayers(Rml::LayerHandle,Rml::LayerHandle,Rml::BlendMode,Rml::Span<const Rml::CompiledFilterHandle>) override {unsupported("layer composition");}
    void PopLayer() override {unsupported("layer");}
    Rml::TextureHandle SaveLayerAsTexture() override {unsupported("layer texture");return 0;}
    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override {unsupported("layer mask");return 0;}
    Rml::CompiledFilterHandle CompileFilter(const Rml::String&,const Rml::Dictionary&) override {unsupported("filter");return 0;}
    void ReleaseFilter(Rml::CompiledFilterHandle) override {}
    Rml::CompiledShaderHandle CompileShader(const Rml::String&,const Rml::Dictionary&) override {unsupported("shader");return 0;}
    void RenderShader(Rml::CompiledShaderHandle,Rml::CompiledGeometryHandle,Rml::Vector2f,Rml::TextureHandle) override {unsupported("shader");}
    void ReleaseShader(Rml::CompiledShaderHandle) override {}
};
}
struct UiDocument::Impl {
    std::string error,name,focus;Renderer renderer{error};Rml::Context* context=nullptr;Rml::ElementDocument* document=nullptr;bool joined=false;
    std::vector<UiElementBinding> bindings;std::map<std::string,Rml::Element*> elements;std::map<std::string,std::string> texts;
    std::vector<Rml::Element*> hit_regions;
    std::uint32_t width=640,height=480;float scale=1;double time=0;std::uint64_t revision=0;
    void valid() const {if(!error.empty())throw std::runtime_error(error);}
    void update() {
        valid();check(context->Update(),"RmlUi layout update failed.");valid();
        if(!focus.empty() && (!element(focus)->IsVisible(true) || !enabled(element(focus)))) {element(focus)->Blur();focus.clear();}
    }
    std::string plain_text(Rml::Element* e) {
        if(auto* text=dynamic_cast<Rml::ElementText*>(e))return text->GetText();
        std::string result;for(int i=0;i<e->GetNumChildren();++i)result+=plain_text(e->GetChild(i));return result;
    }
    void init(UiDocumentSource source) {
        text_bound(source.rml,1024*1024);check(source.elements.size()<=256 && source.fonts.size()<=16,"UI registration/font budget exceeded.");
        auto lower=source.rml;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        check(lower.find("<script")==std::string::npos,"UI scripts are disabled.");
        auto& g=globals();g.acquire();joined=true;
        for(auto& font:source.fonts) {
            text_bound(font.family,128);check(!font.bytes.empty() && font.bytes.size()<=16u*1024u*1024u,"UI font byte budget exceeded.");
            const auto family=Rml::StringUtilities::ToLower(font.family);
            const auto existing=g.fonts.find(family);
            if(existing!=g.fonts.end()) {check(existing->second.loaded,"UI font family previously failed to load.");check(*existing->second.bytes==font.bytes,"UI font family already has different immutable bytes.");continue;}
            check(g.fonts.size()<64 && font.bytes.size()<=64u*1024u*1024u-g.font_bytes,"Global UI font budget exceeded.");
            auto owned=std::make_shared<const std::vector<std::uint8_t>>(std::move(font.bytes));
            // Own bytes before passing them to RmlUi, including failed font loads.
            g.fonts.emplace(family,Globals::Font{owned,false});g.font_bytes+=owned->size();
            check(Rml::LoadFontFace(Rml::Span<const Rml::byte>(owned->data(),owned->size()),family,Rml::Style::FontStyle::Normal),"RmlUi rejected in-memory font.");valid();g.fonts.at(family).loaded=true;
        }
        name="poima-ui-"+std::to_string(++g.next);context=Rml::CreateContext(name,{static_cast<int>(width),static_cast<int>(height)},&renderer);check(context,"RmlUi context creation failed.");
        // Input feedback is event-driven; scrolling must not depend on an
        // advancing animation clock to reach the requested viewport position.
        context->SetDefaultScrollBehavior(Rml::ScrollBehavior::Instant,1.f);
        document=context->LoadDocumentFromMemory(source.rml);valid();check(document,"RmlUi rejected document.");
        std::vector<Rml::Element*> pending{document};std::set<std::string> ids;std::size_t nodes=0;
        while(!pending.empty()) {
            auto* element=pending.back();pending.pop_back();check(++nodes<=4096,"UI DOM node budget exceeded.");
            const auto& tag=element->GetTagName();check(tag=="body" || tag=="div" || tag=="span" || tag=="p" || tag=="button" || tag=="br" || tag=="#text","Unsupported UI element tag.");
            if(!element->GetId().empty())check(ids.insert(element->GetId()).second,"Duplicate UI element ID.");
            for(const auto& [attribute,value]:element->GetAttributes()) { (void)value;check(!attribute.starts_with("on") && !attribute.starts_with("data-"),"UI scripts/data binding attributes are unsupported."); }
            for(int i=0;i<element->GetNumChildren();++i)pending.push_back(element->GetChild(i));
        }
        for(auto& binding:source.elements) {
            text_bound(binding.id,128);check(binding.kind==UiElementKind::label || binding.kind==UiElementKind::button,"Invalid UI binding kind.");
            if(binding.kind==UiElementKind::button)text_bound(binding.action,128);else check(binding.action.empty(),"Labels cannot declare actions.");
            auto* element=document->GetElementById(binding.id);check(element,"Registered UI element is absent.");
            check(binding.kind!=UiElementKind::button || element->GetTagName()=="button","UI button binding requires a button element.");
            check(elements.emplace(binding.id,element).second,"Duplicate UI element registration.");
        }
        check(source.hit_regions.size()<=257,"UI hit-region budget exceeded.");
        std::set<std::string> region_ids;
        for(const auto& id:source.hit_regions) {
            text_bound(id,128);auto* e=document->GetElementById(id);
            check(e && e->GetTagName()=="div" && region_ids.insert(id).second && !elements.contains(id),"Invalid or duplicate UI hit region.");
            for(auto* ancestor=e->GetParentNode();ancestor;ancestor=ancestor->GetParentNode())
                check(std::none_of(elements.begin(),elements.end(),[&](const auto& item){return item.second==ancestor;}),"Text registration cannot contain a hit region.");
            hit_regions.push_back(e);
        }
        // Text setters must never be able to destroy another registered handle.
        for(const auto& [id,e]:elements) {
            (void)id;for(auto* parent=e->GetParentNode();parent;parent=parent->GetParentNode())
                check(std::none_of(elements.begin(),elements.end(),[&](const auto& item){return item.second==parent;}),"Registered UI elements cannot contain other registered elements.");
        }
        bindings=std::move(source.elements);document->Show();update();
    }
    ~Impl() {
        auto& g=globals();std::lock_guard lock(g.mutex);Operation operation(error,time);
        if(context) {Rml::RemoveContext(name);Rml::ReleaseRenderManagers();}
        if(joined)g.release();
    }
    Rml::Element* element(const std::string& id) {const auto found=elements.find(id);check(found!=elements.end(),"UI element is not registered.");return found->second;}
    const UiElementBinding& binding(const std::string& id) {const auto found=std::find_if(bindings.begin(),bindings.end(),[&](const auto& b){return b.id==id;});check(found!=bindings.end(),"UI element is not registered.");return *found;}
    bool enabled(Rml::Element* e) {for(;e;e=e->GetParentNode())if(e->HasAttribute("disabled") || e->IsPseudoClassSet("disabled"))return false;return true;}
    std::array<float,4> bounds(Rml::Element* e) {const auto p=e->GetAbsoluteOffset(Rml::BoxArea::Border),size=e->GetBox().GetSize(Rml::BoxArea::Border);return {p.x,p.y,p.x+size.x,p.y+size.y};}
    std::array<float,4> clipping(Rml::Element* e) {
        std::array<float,4> out{0,0,float(width),float(height)};Rml::Rectanglei clip;
        if(Rml::ElementUtilities::GetClippingRegion(e,clip)) {out[0]=std::max(out[0],float(clip.Left()));out[1]=std::max(out[1],float(clip.Top()));out[2]=std::min(out[2],float(clip.Right()));out[3]=std::min(out[3],float(clip.Bottom()));}
        return out;
    }
    bool hit(Rml::Element* e,float x,float y) {
        if(!e->IsVisible(true) || !enabled(e) || x<0 || y<0 || x>=static_cast<float>(width) || y>=static_cast<float>(height))return false;
        const auto clip=clipping(e);if(x<clip[0] || y<clip[1] || x>=clip[2] || y>=clip[3])return false;
        for(auto* found=context->GetElementAtPoint({x,y});found;found=found->GetParentNode()) {
            if(found==e)return true;
        }
        return false;
    }
    bool hittable(Rml::Element* e) {
        if(!e->IsVisible(true) || !enabled(e))return false;
        auto b=bounds(e);Rml::Rectanglei clip;
        b[0]=std::max(b[0],0.f);b[1]=std::max(b[1],0.f);b[2]=std::min(b[2],float(width));b[3]=std::min(b[3],float(height));
        if(Rml::ElementUtilities::GetClippingRegion(e,clip)) {b[0]=std::max(b[0],float(clip.Left()));b[1]=std::max(b[1],float(clip.Top()));b[2]=std::min(b[2],float(clip.Right()));b[3]=std::min(b[3],float(clip.Bottom()));}
        if(b[0]>=b[2] || b[1]>=b[3])return false;
        for(int y=0;y<5;++y)for(int x=0;x<5;++x) {
            if(hit(e,b[0]+(b[2]-b[0])*(static_cast<float>(x)+.5f)/5,b[1]+(b[3]-b[1])*(static_cast<float>(y)+.5f)/5))return true;
        }
        return false;
    }
};
UiDocument::UiDocument(UiDocumentSource source):impl_(std::make_unique<Impl>()) {auto& g=globals();std::lock_guard lock(g.mutex);Operation operation(impl_->error,0);impl_->init(std::move(source));}
UiDocument::~UiDocument()=default;
std::shared_ptr<const UiFrame> UiDocument::frame(std::uint32_t width,std::uint32_t height,float scale,double time) {
    auto& g=globals();std::lock_guard lock(g.mutex);
    check(width>=1 && width<=8192 && height>=1 && height<=8192 && std::isfinite(scale) && scale>=.25f && scale<=8 && std::isfinite(time) && time>=impl_->time,"Invalid UI extent/scale or nonmonotonic presentation time.");
    auto& s=*impl_;Operation operation(s.error,time);s.valid();s.time=time;s.width=width;s.height=height;s.scale=scale;
    s.context->SetDimensions({static_cast<int>(width),static_cast<int>(height)});s.context->SetDensityIndependentPixelRatio(scale);s.update();
    auto& renderer=s.renderer;renderer.packet={};renderer.packet.width=width;renderer.packet.height=height;renderer.packet.revision=++s.revision;renderer.frame_textures.clear();renderer.scissor_enabled=false;renderer.matrix=UiDraw{}.transform;
    check(s.context->Render(),"RmlUi rendering failed.");s.valid();return freeze_ui_frame(std::move(renderer.packet));
}
std::vector<UiElementInspection> UiDocument::inspect() {
    auto& g=globals();std::lock_guard lock(g.mutex);auto& s=*impl_;Operation operation(s.error,s.time);s.update();std::vector<UiElementInspection> out;
    for(const auto& b:s.bindings) {auto* e=s.element(b.id);out.push_back({b.id,b.action,s.texts.contains(b.id)?s.texts.at(b.id):s.plain_text(e),b.kind,e->IsVisible(true),s.enabled(e),b.kind==UiElementKind::button && s.hittable(e),s.focus==b.id,s.bounds(e),s.clipping(e)});}s.valid();return out;
}
void UiDocument::set_text(const std::string& id,const std::string& text) {
    text_bound(text,16384,true);auto& g=globals();std::lock_guard lock(g.mutex);auto& s=*impl_;Operation operation(s.error,s.time);s.valid();auto* e=s.element(id);
    // A text node never parses the caller's value as markup.
    auto node=s.document->CreateTextNode(text);check(bool(node),"UI text node allocation failed.");
    s.texts.insert_or_assign(id,text);while(e->GetNumChildren())e->RemoveChild(e->GetChild(0));e->AppendChild(std::move(node));
}
void UiDocument::set_enabled(const std::string& id,bool enabled) {auto& g=globals();std::lock_guard lock(g.mutex);auto& s=*impl_;Operation operation(s.error,s.time);s.valid();auto* e=s.element(id);e->SetPseudoClass("disabled",!enabled);if(enabled)e->RemoveAttribute("disabled");else e->SetAttribute("disabled",true);}
void UiDocument::set_visible(const std::string& id,bool visible) {auto& g=globals();std::lock_guard lock(g.mutex);auto& s=*impl_;Operation operation(s.error,s.time);s.valid();check(s.element(id)->SetProperty("visibility",visible?"visible":"hidden"),"UI visibility property rejected.");}
UiPointerTarget UiDocument::pointer_target(float x,float y) {
    check(std::isfinite(x) && std::isfinite(y),"Invalid UI pointer position.");auto& g=globals();std::lock_guard lock(g.mutex);auto& s=*impl_;Operation operation(s.error,s.time);s.update();
    UiPointerTarget target;
    if(x<0 || y<0 || x>=float(s.width) || y>=float(s.height))return target;
    for(auto* e=s.context->GetElementAtPoint({x,y});e;e=e->GetParentNode()) {
        if(!e->IsVisible(true))continue;
        // RmlUi projects the point before testing an ancestor scissor. The
        // scissor is in viewport coordinates, so also check the original point.
        const auto clip=s.clipping(e);if(x<clip[0] || y<clip[1] || x>=clip[2] || y>=clip[3])continue;
        if(std::find(s.hit_regions.begin(),s.hit_regions.end(),e)!=s.hit_regions.end())target.region=true;
        if(!target.button)for(const auto& b:s.bindings)if(b.kind==UiElementKind::button && s.element(b.id)==e)target.button=b.id;
    }
    s.valid();return target;
}
void UiDocument::pointer_move(float x,float y) {
    check(std::isfinite(x) && std::isfinite(y) && std::abs(x)<=1000000 && std::abs(y)<=1000000,"Invalid UI pointer position.");
    auto& g=globals();std::lock_guard lock(g.mutex);auto& s=*impl_;Operation operation(s.error,s.time);s.update();s.context->ProcessMouseMove(static_cast<int>(std::floor(x)),static_cast<int>(std::floor(y)),0);s.valid();
}
void UiDocument::pointer_button(bool down) {auto& g=globals();std::lock_guard lock(g.mutex);auto& s=*impl_;Operation operation(s.error,s.time);s.update();if(down)s.context->ProcessMouseButtonDown(0,0);else s.context->ProcessMouseButtonUp(0,0);s.update();s.valid();}
void UiDocument::pointer_leave() {auto& g=globals();std::lock_guard lock(g.mutex);auto& s=*impl_;Operation operation(s.error,s.time);s.context->ProcessMouseLeave();s.valid();}
void UiDocument::pointer_wheel(float delta) {
    check(std::isfinite(delta) && std::abs(delta)<=100,"Invalid UI wheel delta.");auto& g=globals();std::lock_guard lock(g.mutex);auto& s=*impl_;Operation operation(s.error,s.time);s.update();s.context->ProcessMouseWheel(delta,0);s.update();s.valid();
}
bool UiDocument::focus(const std::optional<std::string>& id,bool scroll) {
    auto& g=globals();std::lock_guard lock(g.mutex);auto& s=*impl_;Operation operation(s.error,s.time);s.update();
    if(!id) {if(auto* e=s.context->GetFocusElement())e->Blur();s.focus.clear();s.valid();return true;}
    auto* e=s.element(*id);if(s.binding(*id).kind!=UiElementKind::button || !e->IsVisible(true) || !s.enabled(e))return false;
    if(!e->Focus(true))return false;
    if(scroll)e->ScrollIntoView();
    s.focus=*id;s.update();s.valid();return true;
}
void UiDocument::set_pressed(const std::string& id,bool value) {auto& g=globals();std::lock_guard lock(g.mutex);auto& s=*impl_;Operation operation(s.error,s.time);s.valid();s.element(id)->SetPseudoClass("active",value);s.valid();}
std::optional<std::string> UiDocument::pointer_activate(float x,float y) {
    check(std::isfinite(x) && std::isfinite(y),"Invalid UI pointer position.");auto& g=globals();std::lock_guard lock(g.mutex);auto& s=*impl_;Operation operation(s.error,s.time);s.update();
    for(auto* e=s.context->GetElementAtPoint({x,y});e;e=e->GetParentNode())for(const auto& b:s.bindings)if(b.kind==UiElementKind::button && s.element(b.id)==e && s.hit(e,x,y))return b.action;
    s.valid();return {};
}
std::optional<std::string> UiDocument::activate(const std::string& id) {auto& g=globals();std::lock_guard lock(g.mutex);auto& s=*impl_;Operation operation(s.error,s.time);s.update();const auto& b=s.binding(id);if(b.kind==UiElementKind::button && s.hittable(s.element(id)))return b.action;return {};}
std::optional<std::string> UiDocument::focus_next(bool reverse) {
    auto& g=globals();std::lock_guard lock(g.mutex);auto& s=*impl_;Operation operation(s.error,s.time);s.update();const auto count=s.bindings.size();if(!count)return {};
    auto found=std::find_if(s.bindings.begin(),s.bindings.end(),[&](const auto& b){return b.id==s.focus;});std::size_t start=found==s.bindings.end() ? (reverse?0:count-1) : static_cast<std::size_t>(found-s.bindings.begin());
    for(std::size_t n=1;n<=count;++n) {const auto i=reverse?(start+count-n)%count:(start+n)%count;const auto& b=s.bindings[i];if(b.kind==UiElementKind::button && s.hittable(s.element(b.id)) && s.element(b.id)->Focus()) {s.focus=b.id;return s.focus;}}
    return {};
}
std::optional<std::string> UiDocument::activate_focused() {auto& g=globals();std::lock_guard lock(g.mutex);auto& s=*impl_;Operation operation(s.error,s.time);s.update();if(s.focus.empty())return {};const auto& b=s.binding(s.focus);return s.hittable(s.element(s.focus))?std::optional<std::string>{b.action}:std::nullopt;}
}
