// SPDX-License-Identifier: Apache-2.0
#include "poima/ui_presenter.hpp"
#include "poima/ui_document.hpp"
#include <span>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace poima {std::span<const std::uint8_t> embedded_ui_font();}
namespace {
void check(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F f) {bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"Invalid presentation succeeded.");}
std::string stable_id(char digit) {return std::string(32,digit);}
poima::UiElementInspection control(poima::UiPresenter& p,const std::string& id) {
    const auto rows=p.inspect();const auto at=std::find_if(rows.begin(),rows.end(),[&](const auto& row){return row.id==id;});check(at!=rows.end(),"Authored control absent from layout inspection.");return *at;
}
void rectangle(const std::array<float,4>& actual,std::array<float,4> expected) {for(std::size_t i=0;i<4;++i)check(std::abs(actual[i]-expected[i])<.05f,"Authored responsive rectangle differs from expected geometry.");}
bool color(const poima::UiFrame& frame,std::array<std::uint8_t,4> value) {return std::any_of(frame.vertices.begin(),frame.vertices.end(),[&](const auto& vertex){return vertex.color==value;});}
void authored_layout_tests() {
    using namespace poima;using namespace poima::ui;
    const auto canvas=stable_id('1'),panel=stable_id('2'),title=stable_id('3'),save=stable_id('4'),resume=stable_id('5'),cover=stable_id('6');
    Definition definition{{canvas,"","Canvas",Kind::panel},{panel,canvas,"Menu",Kind::panel},{title,panel,"Title",Kind::label,"Paused"},{save,panel,"Save",Kind::button,"Save","save"},{resume,panel,"Resume",Kind::button,"Resume","resume"},{cover,"","Cover",Kind::panel,"","",false}};
    // Scientific notation is accepted by pinned RmlUi's strtof-based number
    // parser; this finite subnormal authored offset underflows to float zero.
    Layout root;root.position=Position::absolute;root.left=Length{1e-300};root.top=Length{0};root.width=Length{100,LengthUnit::percent};root.height=Length{100,LengthUnit::percent};root.padding=std::array<double,4>{0,0,0,0};root.direction=Direction::column;root.align=Align::center;root.justify=Justify::center;root.hit_test=HitTest::pass_through;definition[0].layout=root;
    Style transparent;transparent.background_color="#00000000";transparent.border_width=0;definition[0].style=transparent;
    Layout menu;menu.width=Length{320};menu.height=Length{200};menu.padding=std::array<double,4>{16,16,16,16};menu.gap=8;menu.shrink=0;menu.align=Align::stretch;definition[1].layout=menu;
    Style menu_style;menu_style.background_color="#241c24";menu_style.border_width=0;menu_style.disabled=ColorState{std::nullopt,"#563412",std::nullopt};definition[1].style=menu_style;
    for(std::size_t i=2;i<=4;++i) {Layout child;child.width=Length{100,LengthUnit::percent};child.height=Length{i==2?32.:40.};child.shrink=0;child.order=i==2?-10:i==3?2:1;definition[i].layout=child;}
    Style button_style;button_style.background_color="#6f243b";button_style.border_width=0;button_style.border_radius=6;button_style.hover=ColorState{std::nullopt,"#89354e",std::nullopt};button_style.focus=button_style.hover;button_style.pressed=ColorState{std::nullopt,"#531a2c",std::nullopt};button_style.disabled=ColorState{std::nullopt,"#403039",std::nullopt};definition[4].style=button_style;
    Layout overlay;overlay.position=Position::absolute;overlay.left=Length{336};overlay.top=Length{226};overlay.width=Length{288};overlay.height=Length{40};overlay.padding=std::array<double,4>{0,0,0,0};definition[5].layout=overlay;definition[5].style=transparent;
    Model model(definition);UiPresenter presenter;rejects([&]{presenter.inspect();});auto frame=presenter.frame(model.presentation(),960,540);
    const auto rows=presenter.inspect();check(rows.size()==3 && rows[0].id==title && rows[1].id==resume && rows[2].id==save,"Inspector traversal did not follow authored sibling order.");
    rectangle(control(presenter,resume).bounds,{336,226,624,266});rectangle(control(presenter,save).bounds,{336,274,624,314});
    check(color(*frame,{111,36,59,255}),"Authored button palette was not rendered.");
    check(!presenter.input({UiInputKind::pointer_down,5,5}).consumed && !presenter.input({UiInputKind::pointer_up,5,5}).consumed,"Pass-through fullviewport canvas swallowed gameplay input.");
    check(presenter.input({UiInputKind::pointer_down,325,175}).consumed,"Capturing menu padding did not own its gesture.");presenter.input({UiInputKind::pointer_up,325,175});
    check(presenter.input({UiInputKind::focus_next}).focused==resume && presenter.input({UiInputKind::focus_next}).focused==save,"Focus order did not match visual authored order.");presenter.reset_input();
    presenter.input({UiInputKind::pointer_move,350,240});check(color(*presenter.frame(model.presentation(),960,540),{137,53,78,255}),"Authored hover color was not presented.");
    presenter.input({UiInputKind::pointer_down,350,240});check(color(*presenter.frame(model.presentation(),960,540),{83,26,44,255}),"Authored pressed color was not presented.");
    model.edit(0,{{title,"Paused 2"}});presenter.frame(model.presentation(),960,540);check(presenter.input({UiInputKind::pointer_up,350,240}).activated==resume,"Unchanged authored target lost a held gesture during HUD update.");
    presenter.input({UiInputKind::pointer_down,350,240});model.edit(1,{{cover,std::nullopt,true}});presenter.frame(model.presentation(),960,540);
    auto blocked=presenter.input({UiInputKind::accept_down});check(blocked.consumed && blocked.focused==resume,"Occluded focus lost keyboard ownership.");blocked=presenter.input({UiInputKind::accept_up});check(blocked.consumed && !blocked.activated,"Fresh keyboard confirm activated a fully covered control.");
    presenter.input({UiInputKind::accept_down});model.edit(2,{{cover,std::nullopt,false}});presenter.frame(model.presentation(),960,540);blocked=presenter.input({UiInputKind::accept_up});check(blocked.consumed && !blocked.activated,"Removing cover rearmed an initially blocked held confirm.");auto release=presenter.input({UiInputKind::pointer_up,350,240});check(release.consumed && !release.activated,"Occlusion change retained an armed callback or lost release ownership.");
    presenter.input({UiInputKind::accept_down});model.edit(3,{{cover,std::nullopt,true}});presenter.frame(model.presentation(),960,540);blocked=presenter.input({UiInputKind::accept_up});check(blocked.consumed && !blocked.activated,"New cover retained a previously armed keyboard confirm.");model.edit(4,{{cover,std::nullopt,false}});presenter.frame(model.presentation(),960,540);
    presenter.input({UiInputKind::pointer_down,350,240});auto altered=std::make_shared<Presentation>(*model.presentation());altered->elements[4].element.action="different";presenter.frame(altered,960,540);check(!presenter.input({UiInputKind::pointer_up,350,240}).activated,"Changed action retained an armed gesture.");
    presenter.reset_input();presenter.frame(model.presentation(),640,360);rectangle(control(presenter,resume).bounds,{176,136,464,176});
    presenter.frame(model.presentation(),1920,1080,2);rectangle(control(presenter,resume).bounds,{672,452,1248,532});
    presenter.frame(model.presentation(),960,540);model.edit(5,{{panel,std::nullopt,std::nullopt,false}});frame=presenter.frame(model.presentation(),960,540);check(!control(presenter,resume).enabled && color(*frame,{86,52,18,255}) && color(*frame,{64,48,57,255}),"Inherited disabled state did not apply authored panel/button colors.");
}
void authored_scroll_tests() {
    using namespace poima;using namespace poima::ui;const auto panel=stable_id('1');Definition definition{{panel,"","Scroller",Kind::panel}};
    Layout layout;layout.position=Position::absolute;layout.left=Length{20};layout.top=Length{20};layout.width=Length{180};layout.height=Length{100};layout.padding=std::array<double,4>{0,0,0,0};layout.gap=5;layout.overflow=Overflow::auto_scroll;definition[0].layout=layout;Style style;style.border_width=0;definition[0].style=style;
    for(char n='2';n<='6';++n) {Element e{stable_id(n),panel,"Item",Kind::button,"Item","item"};Layout child;child.height=Length{40};child.shrink=0;e.layout=child;definition.push_back(e);}
    Model model(definition);UiPresenter p;p.frame(model.presentation(),320,180);const auto last=stable_id('6');check(!control(p,last).hittable,"Clipped control remained presentation-hittable.");
    const auto first=control(p,stable_id('2'));rectangle(first.clip,{20,20,200,120});check(!p.input({UiInputKind::pointer_down,40,125}).consumed,"Scroller captured input outside its clipped extent.");p.input({UiInputKind::pointer_up,40,125});
    for(int i=0;i<5;++i)p.input({UiInputKind::focus_next});p.frame(model.presentation(),320,180);check(control(p,last).hittable,"Visual-order focus failed to reveal clipped authored control.");
    check(p.input({UiInputKind::pointer_wheel,40,40,-100}).consumed,"Authored scroller did not own wheel input.");p.frame(model.presentation(),320,180);check(control(p,stable_id('2')).hittable && !control(p,last).hittable,"Authored scroller did not apply bounded wheel motion/clipping.");
}
void mixed_root_tests() {
    using namespace poima;using namespace poima::ui;const auto canvas=stable_id('1'),authored=stable_id('2'),legacy=stable_id('8'),old_button=stable_id('9');
    Definition d{{canvas,"","Authored root",Kind::panel},{authored,canvas,"Authored",Kind::button,"New","new"},{legacy,"","Legacy root",Kind::panel},{old_button,legacy,"Legacy",Kind::button,"Old","old"}};
    Layout root;root.position=Position::absolute;root.left=Length{80,LengthUnit::percent};root.top=Length{20};root.width=Length{100};root.height=Length{40};root.padding=std::array<double,4>{0,0,0,0};root.order=-100;d[0].layout=root;Style style;style.border_width=0;d[0].style=style;
    Layout child;child.height=Length{40};child.shrink=0;d[1].layout=child;
    Model model(d);UiPresenter p;p.frame(model.presentation(),640,360);rectangle(control(p,authored).bounds,{512,20,612,60});
    const auto old=control(p,old_button);check(old.bounds[0]>0 && old.bounds[2]<320,"Explicit canvas displaced the retained legacy sidebar.");
    // Separate legacy/sidebar and authored/canvas groups intentionally keep
    // traversal group-local instead of interleaving their differently laid-out roots.
    const auto rows=p.inspect();check(rows.size()==2 && rows[0].id==old_button && rows[1].id==authored,"Mixed-root traversal groups changed unexpectedly.");
}
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
    authored_layout_tests();
    authored_scroll_tests();
    mixed_root_tests();
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
