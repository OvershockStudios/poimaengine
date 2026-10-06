// SPDX-License-Identifier: Apache-2.0
#include "poima/ui_presenter.hpp"
#include <iostream>
#include <stdexcept>
namespace {
void check(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F f) {bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"Invalid presentation succeeded.");}
}
int main() {try {
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
    auto literal=presenter.frame(edited,640,480);check(!literal->draws.empty() && literal->revision==1,"Literal text/revision failed.");
    model.load(saved);check(model.presentation()!=edited && model.presentation()->revision==0,"Load did not invalidate projection.");
    model.edit(0,{{panel,std::nullopt,false}});
    check(!model.presentation()->elements[2].eligible && !model.presentation()->elements[2].effective_visible,"Inherited visibility not projected.");
    check(presenter.frame(model.presentation(),640,480)->draws.empty(),"Hidden tree still rendered.");
    rejects([&]{presenter.frame(initial,0,480);});
    auto malformed=std::make_shared<Presentation>(*initial);malformed->elements[0].element.parent=panel;
    rejects([&]{presenter.frame(malformed,640,480);});
    std::cout<<"Immutable UI projection, embedded font, default layout, literal text and cache invalidation passed.\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
