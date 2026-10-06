// SPDX-License-Identifier: Apache-2.0
#include "poima/ui_document.hpp"
#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace poima;
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F&& action) {bool failed=false;try {action();}catch(const std::exception&) {failed=true;}check(failed,"Unsupported/invalid UI operation was accepted.");}
std::vector<std::uint8_t> font;
// The clip fixture uses clip:always: absolutely positioned children do not grow
// the pinned RmlUi scroll extent that overflow:hidden alone tests.
UiDocumentSource source(std::string extra_style={},std::string extra_body={}) {
    UiDocumentSource value;value.fonts={{"Poima UI",font}};
    value.rml=R"(<rml><head><style>
body { font-family: Poima UI; font-size: 18px; margin: 0px; width: 100%; height: 100%; }
#health { position: absolute; left: 10px; top: 10px; width: 200px; height: 25px; }
button { position: absolute; width: 100px; height: 30px; background-color: #203050; color: #ffffff; tab-index: auto; }
button:focus { background-color: #804010; }
#go { left: 10px; top: 50px; } #save { left: 120px; top: 50px; }
#clip { position: absolute; left: 10px; top: 100px; width: 20px; height: 40px; overflow: hidden; clip: always; }
#partial { left: 10px; top: 0px; } #hiddenclip { left: 30px; top: 0px; }
)"+extra_style+R"(</style></head><body><div id="health">Health <span>100</span></div><button id="go">Continue</button><button id="save">Save</button><div id="clip"><button id="partial">Part</button><button id="hiddenclip">Hidden</button></div>)"+extra_body+"</body></rml>";
    value.elements={{"health",UiElementKind::label,{}},{"go",UiElementKind::button,"continue"},{"save",UiElementKind::button,"save"},{"partial",UiElementKind::button,"partial"},{"hiddenclip",UiElementKind::button,"hidden"}};return value;
}
UiElementInspection inspect(UiDocument& doc,const std::string& id) {const auto all=doc.inspect();const auto it=std::find_if(all.begin(),all.end(),[&](const auto& e){return e.id==id;});check(it!=all.end(),"UI inspection omitted registration.");return *it;}
void layout_and_actions() {
    UiDocument doc(source());auto before=doc.frame(320,180,1,0);validate_ui_frame(*before);
    check(!before->vertices.empty() && !before->indices.empty() && !before->textures.empty(),"Text layout produced no native geometry/font atlas.");
    check(inspect(doc,"health").text=="Health 100","Initial label inspection returned markup instead of text.");
    check(doc.pointer_activate(50,65)==doc.activate("go") && doc.activate("go")=="continue","Pointer and semantic button actions differ.");
    check(!doc.pointer_activate(-1,65) && !doc.pointer_activate(300,170),"Unregistered pointer target produced an action.");
    check(inspect(doc,"partial").hittable && doc.activate("partial")=="partial" && doc.pointer_activate(25,115)=="partial","Partially clipped button lost its usable visible portion.");
    check(!inspect(doc,"hiddenclip").hittable && !doc.activate("hiddenclip") && !doc.pointer_activate(60,115),"Fully clipped button activated.");
    check(doc.focus_next()=="go" && doc.activate_focused()=="continue","Keyboard/gamepad focus did not activate first usable button.");
    auto focused=doc.frame(320,180,1,0);check(focused->vertices.size()!=before->vertices.size() || !std::equal(focused->vertices.begin(),focused->vertices.end(),before->vertices.begin(),[](const auto& a,const auto& b){return a.x==b.x && a.y==b.y && a.u==b.u && a.v==b.v && a.color==b.color;}),"Actual RmlUi focus styling did not change geometry/color.");
    doc.set_enabled("go",false);check(!doc.activate("go") && !doc.pointer_activate(50,65) && !doc.activate_focused(),"Disabled button remained actionable/focused.");
    check(!inspect(doc,"go").enabled && !inspect(doc,"go").focused,"Disabled focus inspection stale.");
    doc.set_enabled("go",true);doc.set_visible("go",false);check(!doc.activate("go") && !doc.pointer_activate(50,65),"Hidden button remained actionable.");
    doc.set_visible("go",true);check(doc.activate("go")=="continue","Re-enabled visible button did not recover.");
    doc.set_text("health","<button id='injected'>Not markup & text</button>");
    check(inspect(doc,"health").text=="<button id='injected'>Not markup & text</button>","Plain UI text did not round-trip.");
    check(doc.inspect().size()==5 && doc.activate("go")=="continue","Text injection changed semantic registrations.");
    rejects([&]{doc.set_text("health",std::string("\xc0\x80",2));});
    const auto old_width=before->width;auto resized=doc.frame(640,360,2,1);
    check(resized->width==640 && resized->height==360 && before->width==old_width,"Resize mutated a previously published frame.");
    rejects([&]{doc.frame(640,360,2,.5);});rejects([&]{doc.frame(0,360,2,1);});
    check(doc.activate("go")=="continue","Rejected frame arguments mutated action state.");
}
void serialized_contexts() {
    UiDocument first(source()),second(source());std::exception_ptr errors[2];
    auto work=[&](UiDocument& doc,int index) {try {for(int n=0;n<4;++n) {doc.set_text("health","Health "+std::to_string(n));validate_ui_frame(*doc.frame(320,180,1,n));check(doc.activate("go")=="continue","Concurrent context lost actions.");}}catch(...) {errors[index]=std::current_exception();}};
    std::thread a([&]{work(first,0);}),b([&]{work(second,1);});a.join();b.join();
    for(const auto& error:errors)if(error)std::rethrow_exception(error);
}
void unsupported_and_lifetime() {
    std::shared_ptr<const UiFrame> retained;
    {
        auto first=std::make_unique<UiDocument>(source());UiDocument second(source());
        retained=first->frame(320,180,1,0);const auto vertices=retained->vertices.size();
        first.reset();check(!second.frame(320,180,1,0)->textures.empty(),"Destroying one context invalidated shared font lifetime.");
        validate_ui_frame(*retained);check(retained->vertices.size()==vertices,"Context destruction changed retained packet.");
        auto nested=source({},"<div id='outer'><span id='inner'>Nested</span></div>");nested.elements.push_back({"outer",UiElementKind::label,{}});nested.elements.push_back({"inner",UiElementKind::label,{}});
        rejects([&]{UiDocument invalid(std::move(nested));});
        auto alias=source();alias.fonts.front().family="poima ui";
        {UiDocument matching(alias);check(matching.activate("go")=="continue","Case-insensitive font alias failed to share immutable font bytes.");}
        alias.fonts.front().bytes.back()^=1;
        rejects([&]{UiDocument conflicting(alias);});
        auto broken=source();broken.fonts={{"Broken UI",{1,2,3,4}}};
        rejects([&]{UiDocument invalid(broken);});rejects([&]{UiDocument invalid(broken);});
        check(second.activate("go")=="continue","Failed document construction corrupted surviving context.");
        auto duplicate=source();duplicate.elements.push_back(duplicate.elements.front());rejects([&]{UiDocument invalid(duplicate);});
        auto script=source();script.rml.insert(script.rml.find("</head>"),"<script>alert(1)</script>");rejects([&]{UiDocument invalid(script);});
        auto external=source();external.rml.insert(external.rml.find("</head>"),"<link type='text/rcss' href='forbidden.css'/>");rejects([&]{UiDocument invalid(external);});
        for(const auto& style:{std::string("#go { filter: blur(2px); }"),std::string("#go { decorator: linear-gradient(red, blue); }")})
            rejects([&]{UiDocument invalid(source(style));(void)invalid.frame(320,180,1,0);});
        UiDocument transformed(source("#go { transform: translateX(5px); }"));const auto transformed_frame=transformed.frame(320,180,1,0);
        check(std::any_of(transformed_frame->draws.begin(),transformed_frame->draws.end(),[](const auto& draw){return draw.transform!=UiDraw{}.transform;}),"Ordinary transform was silently discarded.");
    }
    validate_ui_frame(*retained);check(!retained->textures.front().rgba.empty(),"Shutdown freed immutable frame atlas storage.");
    UiDocument restarted(source());check(restarted.activate("go")=="continue","RmlUi global reinitialization failed.");
}
}
int main(int argc,char** argv) {
    try {
        check(argc==2,"Usage: ui-document-test FONT.ttf");const auto bytes=std::filesystem::file_size(argv[1]);check(bytes>0 && bytes<=16u*1024u*1024u,"Invalid test font size.");
        font.resize(static_cast<std::size_t>(bytes));std::ifstream input(argv[1],std::ios::binary);input.read(reinterpret_cast<char*>(font.data()),static_cast<std::streamsize>(font.size()));check(bool(input),"Cannot read test font.");
        layout_and_actions();serialized_contexts();unsupported_and_lifetime();
        std::cout<<"Native RmlUi document layout/atlas, pointer/semantic/focus actions, clipping/visibility, literal text, immutable packet/global font lifetime and unsupported-resource checks passed.\n";return 0;
    }catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
