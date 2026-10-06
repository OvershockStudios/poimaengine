// SPDX-License-Identifier: Apache-2.0
#include "poima/ui_presenter.hpp"
#include "poima/ui_document.hpp"
#include <span>
#include <iostream>
#include <stdexcept>
namespace poima {std::span<const std::uint8_t> embedded_ui_font();}
namespace {
void check(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F f) {bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"Invalid presentation succeeded.");}
void document_input_tests() {
    using namespace poima;UiDocumentSource source;const auto font=embedded_ui_font();source.fonts.push_back({"Input Test",{font.begin(),font.end()}});
    source.rml=R"(<rml><head><style>body {margin:0px;font-family:Input Test;} #region {position:absolute;left:20px;top:20px;width:80px;height:40px;overflow:hidden;clip:always;} button {position:absolute;left:0px;top:0px;width:100px;height:40px;margin:0px;padding:0px;border-width:0px;transform:translateX(30px);}</style></head><body><div id="region"><button id="button"></button></div></body></rml>)";
    source.elements.push_back({"button",UiElementKind::button,"go"});source.hit_regions.push_back("region");
    UiDocument doc(source);doc.frame(320,180,1,0);
    check(doc.pointer_target(60,30).button=="button","Transformed exact pointer hit failed.");
    check(!doc.pointer_target(110,30).button,"Ancestor clipping failed pointer target.");
    check(doc.pointer_target(25,30).region && !doc.pointer_target(25,30).button,"Panel padding did not form a distinct hit region.");
    doc.set_enabled("button",false);check(doc.pointer_target(60,30).button=="button" && !doc.focus("button"),"Disabled capture/focus separation failed.");
    doc.set_enabled("button",true);check(doc.focus("button"),"Real document focus failed.");
    source.hit_regions.push_back("button");rejects([&]{UiDocument invalid(source);});
}
void changing_hud_tests() {
    using namespace poima;using namespace poima::ui;
    const std::string panel(32,'1'),label(32,'2'),button(32,'3');
    Model model({{panel,"","Panel",Kind::panel},{label,panel,"HUD",Kind::label,"Health 100"},{button,panel,"Button",Kind::button,"Resume","resume"}});
    UiPresenter presenter;presenter.frame(model.presentation(),640,480);
    float x=0,y=0;bool found=false;
    for(int yy=0;yy<240 && !found;yy+=4)for(int xx=0;xx<300 && !found;xx+=4) {
        presenter.input({UiInputKind::pointer_down,float(xx),float(yy)});
        if(presenter.input({UiInputKind::pointer_up,float(xx),float(yy)}).activated==button) {found=true;x=float(xx);y=float(yy);}
    }
    check(found,"HUD button could not be located.");
    presenter.input({UiInputKind::pointer_down,x,y});
    auto stale=presenter.input({UiInputKind::pointer_up,x,y},false);
    check(stale.consumed && !stale.activated,"Stale release escaped UI ownership or activated.");
    check(presenter.input({UiInputKind::pointer_down,x,y},false).consumed,"Stale visible button leaked pointer down.");
    presenter.frame(model.presentation(),640,480);
    stale=presenter.input({UiInputKind::pointer_up,x,y});
    check(stale.consumed && !stale.activated,"Redraw rearmed a consume-only gesture.");
    check(!presenter.input({UiInputKind::pointer_down,639,479},false).consumed,"Stale nonmodal UI swallowed outside input.");
    check(!presenter.input({UiInputKind::pointer_up,639,479},false).consumed,"Stale nonmodal UI captured an outside release.");
    presenter.input({UiInputKind::accept_down});
    stale=presenter.input({UiInputKind::accept_up},false);
    check(stale.consumed && !stale.activated,"Stale accept release activated or leaked.");

    presenter.input({UiInputKind::pointer_down,x,y});
    model.edit(0,{{label,"Health 099"}});presenter.frame(model.presentation(),640,480);
    check(presenter.input({UiInputKind::pointer_up,x,y}).activated==button,"Ordinary HUD tick cancelled held click.");
    presenter.input({UiInputKind::accept_down});
    model.edit(1,{{label,"Health 098"}});presenter.frame(model.presentation(),640,480);
    check(presenter.input({UiInputKind::accept_up}).activated==button,"Ordinary HUD tick cancelled held keyboard accept.");
    presenter.input({UiInputKind::pointer_down,x,y});
    model.edit(2,{{label,"Health\nHealth\nHealth\nHealth"}});presenter.frame(model.presentation(),640,480);
    auto release=presenter.input({UiInputKind::pointer_up,x,y});
    check(release.consumed && !release.activated,"Reflow retained armed pointer or leaked owned release.");
    presenter.input({UiInputKind::accept_down});
    model.edit(3,{{label,"Health 097"}});presenter.frame(model.presentation(),640,480);
    check(!presenter.input({UiInputKind::accept_up}).activated,"Reflow retained armed keyboard accept.");
    presenter.input({UiInputKind::accept_down});
    model.edit(4,{{button,std::nullopt,std::nullopt,false}});presenter.frame(model.presentation(),640,480);
    check(!presenter.input({UiInputKind::accept_up}).activated,"Disabled replacement retained armed accept.");
    model.edit(5,{{button,std::nullopt,std::nullopt,true}},panel);presenter.frame(model.presentation(),640,480);
    check(presenter.input({UiInputKind::pointer_down,639,479}).consumed && presenter.input({UiInputKind::pointer_up,639,479}).consumed,"Modal leaked outside pointer input.");
    presenter.input({UiInputKind::focus_next});presenter.input({UiInputKind::accept_down});
    model.edit(6,{},std::string{});presenter.frame(model.presentation(),640,480);
    check(!presenter.input({UiInputKind::accept_up}).activated,"Modal ownership change retained armed accept.");
}
void input_tests() {
    using namespace poima;using namespace poima::ui;
    const std::string panel(32,'1'),button(32,'2');
    Model model({{panel,"","Panel",Kind::panel},{button,panel,"Button",Kind::button,"Go","go"}});
    UiPresenter presenter;rejects([&]{presenter.input({UiInputKind::focus_next});});
    auto projection=model.presentation();auto before=presenter.frame(projection,640,480);
    auto focus=presenter.input({UiInputKind::focus_next});check(focus.consumed && focus.focused==button,"Logical focus failed.");
    auto feedback=presenter.frame(projection,640,480);check(feedback!=before && feedback->revision>before->revision,"Focus failed to invalidate packet.");
    presenter.input({UiInputKind::accept_down});presenter.input({UiInputKind::accept_down});
    check(presenter.input({UiInputKind::accept_up}).activated==button,"Focused release failed.");
    check(!presenter.input({UiInputKind::accept_up}).activated,"Repeated release activated twice.");
    presenter.input({UiInputKind::accept_down});presenter.frame(projection,800,600);
    check(!presenter.input({UiInputKind::accept_up}).activated,"Resize retained armed accept.");
    model.edit(0,{{button,"Changed"}});projection=model.presentation();presenter.frame(projection,640,480);
    check(presenter.input({UiInputKind::accept_down}).focused==button,"Projection replacement lost focus.");
    check(presenter.input({UiInputKind::accept_up}).activated==button,"Preserved focus failed.");
    presenter.reset_input();float hit_x=0,hit_y=0;bool hit=false;
    for(int y=0;y<160 && !hit;y+=4)for(int x=0;x<300 && !hit;x+=4) {
        presenter.input({UiInputKind::pointer_down,float(x),float(y)});
        if(presenter.input({UiInputKind::pointer_up,float(x),float(y)}).activated==button) {hit=true;hit_x=float(x);hit_y=float(y);}
    }
    check(hit,"Default button is not pointer-addressable.");
    check(presenter.input({UiInputKind::pointer_down,hit_x,hit_y}).consumed,"Pointer down leaked.");
    presenter.input({UiInputKind::pointer_move,639,479});
    const auto dragged=presenter.input({UiInputKind::pointer_up,639,479});check(dragged.consumed && !dragged.activated,"Drag-off release leaked or activated.");
    presenter.input({UiInputKind::pointer_down,hit_x,hit_y});presenter.input({UiInputKind::pointer_leave});
    check(!presenter.input({UiInputKind::pointer_up,hit_x,hit_y}).activated,"Leave retained armed pointer.");
    rejects([&]{presenter.input({UiInputKind::pointer_wheel,0,0,101});});
    model.edit(1,{{button,std::nullopt,std::nullopt,false}});projection=model.presentation();presenter.frame(projection,640,480);
    check(!presenter.input({UiInputKind::accept_down}).focused,"Disabled target retained focus.");
    check(presenter.input({UiInputKind::pointer_down,hit_x,hit_y}).consumed,"Disabled button leaked gesture.");
    check(!presenter.input({UiInputKind::pointer_up,hit_x,hit_y}).activated,"Disabled button activated.");
    auto id=[](int n){const auto digits=std::to_string(n);return std::string(32-digits.size(),'0')+digits;};
    Definition long_menu{{id(1),"","Long menu",Kind::panel}};
    for(int n=2;n<=25;++n)long_menu.push_back({id(n),id(1),"Button",Kind::button,"Long menu button","go"});
    Model scrolling(long_menu);UiPresenter menu;const auto menu_projection=scrolling.presentation();menu.frame(menu_projection,640,240);
    for(int n=2;n<=25;++n)check(menu.input({UiInputKind::focus_next}).focused==id(n),"Stable focus order skipped an offscreen eligible button.");
    menu.frame(menu_projection,640,240);bool last_visible=false;
    for(int y=0;y<240 && !last_visible;y+=4) {
        menu.input({UiInputKind::pointer_down,40,float(y)});
        last_visible=menu.input({UiInputKind::pointer_up,40,float(y)}).activated==id(25);
    }
    check(last_visible,"Keyboard focus did not scroll the last button into actual pointer view.");
    check(menu.input({UiInputKind::pointer_wheel,40,40,-100}).consumed,"Scrollable panel did not consume wheel.");
    menu.frame(menu_projection,640,240);bool first_visible=false;
    for(int y=0;y<240 && !first_visible;y+=4) {
        menu.input({UiInputKind::pointer_down,40,float(y)});
        first_visible=menu.input({UiInputKind::pointer_up,40,float(y)}).activated==id(2);
    }
    check(first_visible,"Wheel did not instantly scroll to the first button.");
    check(menu.input({UiInputKind::pointer_down,295,15}).consumed,"Scrollbar drag was not owned by UI.");
    menu.input({UiInputKind::pointer_move,295,225});
    check(!menu.input({UiInputKind::pointer_up,295,225}).activated,"Scrollbar drag activated a button.");
    menu.frame(menu_projection,640,240);last_visible=false;
    for(int y=0;y<240 && !last_visible;y+=4) {
        menu.input({UiInputKind::pointer_down,40,float(y)});
        last_visible=menu.input({UiInputKind::pointer_up,40,float(y)}).activated==id(25);
    }
    check(last_visible,"Scrollbar thumb drag did not expose last button.");
}
}
int main() {try {
    document_input_tests();
    input_tests();
    changing_hud_tests();
    using namespace poima;using namespace poima::ui;
    const std::string panel(32,'1'),label(32,'2'),button(32,'3');
    Model model({{panel,"","Menu",Kind::panel},{label,panel,"Status",Kind::label,"Health 100"},
        {button,panel,"Resume",Kind::button,"Resume","resume"}});
    auto initial=model.presentation();check(initial==model.presentation(),"Unchanged model lost projection identity.");
    Model copy=model;check(copy.presentation()==initial,"Model copy did not share immutable projection.");
    UiPresenter presenter;auto packet=presenter.frame(initial,640,480);
    check(!packet->draws.empty() && !packet->textures.empty(),"Embedded font/default layout produced no textured UI.");
    check(packet==presenter.frame(initial,640,480),"Unchanged presentation missed cache.");
    auto resized=presenter.frame(initial,800,600);check(resized!=packet && resized->width==800,"Extent did not invalidate layout.");
    auto scaled=presenter.frame(initial,800,600,2);check(scaled!=resized,"Scale did not invalidate layout.");
    rejects([&]{model.edit(9,{{label,"Rejected"}});});check(model.presentation()==initial,"Rejected edit invalidated projection.");
    const auto saved=model.save();
    model.edit(0,{{label,"<img src='missing.png'/> & literal"}});
    auto edited=model.presentation();check(edited!=initial && edited->revision==1 && copy.presentation()==initial,"Edit mutated shared projection.");
    check(initial->elements[1].element.text=="Health 100","Old projection was mutated.");
    auto literal=presenter.frame(edited,640,480);check(!literal->draws.empty() && literal->revision>scaled->revision,"Literal text/revision failed.");
    model.load(saved);check(model.presentation()!=edited && model.presentation()->revision==0,"Load did not invalidate projection.");
    model.edit(0,{{panel,std::nullopt,false}});
    check(!model.presentation()->elements[2].eligible && !model.presentation()->elements[2].effective_visible,"Inherited visibility not projected.");
    check(presenter.frame(model.presentation(),640,480)->draws.empty(),"Hidden tree still rendered.");
    rejects([&]{presenter.frame(initial,0,480);});
    auto malformed=std::make_shared<Presentation>(*initial);malformed->elements[0].element.parent=panel;
    rejects([&]{presenter.frame(malformed,640,480);});
    std::cout<<"Immutable UI projection, pointer/focus ownership, HUD gesture preservation, modal/reflow cancellation, scrollbar/wheel/focus scrolling, embedded font, literal text and cache invalidation passed.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
