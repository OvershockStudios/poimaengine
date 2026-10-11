// SPDX-License-Identifier: Apache-2.0
// Pure committed-lens presentation contract; no runtime, SDL or graphics device.
#include "poima/player_lens.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void close(double actual,double expected,const char* message) {
    check(std::isfinite(actual) && std::abs(actual-expected)<=2e-12*std::max(1.,std::abs(expected)),message);
}
template<class F> void rejects(F action) {
    try {action();}catch(const std::invalid_argument&) {return;}
    throw std::runtime_error("Invalid lens argument was admitted.");
}
PlayerLensSample sample(std::uint64_t tick=0,double fov=70,double vm=75) {
    return {"session-a","source-a","camera-a",tick,0,{fov,.1,100},SceneLens{vm,.01,10},70};
}
double oracle(double from,double to,double alpha) {
    constexpr double half_radians=0.0087266462599716478846;
    // Independently interpolate perspective scale using fractional powers.
    const double a=std::tan(from*half_radians),b=std::tan(to*half_radians);
    return std::atan(a*std::pow(b/a,alpha))/half_radians;
}
void curve_and_defaults() {
    PlayerLensHistory history;auto a=sample(4,110,100),b=sample(5,25,40);
    history.reset(a,"load");
    auto initial=history.presentation(.25,true);
    check(initial.enabled && !initial.interpolating && initial.alpha==1 && initial.reset_reason=="load" &&
        initial.previous_tick==4 && initial.target_tick==4 && initial.rendered_world==a.world,
        "Reset did not expose an exact target with explicit reason.");
    history.commit(b);
    for(double alpha:{0.,.25,.5,1.}) {
        const auto shown=history.presentation(alpha,true);
        close(shown.rendered_world.vertical_fov,oracle(110,25,alpha),"World curve differs from perspective-power oracle.");
        close(shown.rendered_viewmodel->vertical_fov,oracle(100,40,alpha),"Foreground curve differs from independent oracle.");
        check(shown.alpha==alpha && shown.previous_tick==4 && shown.target_tick==5 && shown.reset_reason.empty() &&
            shown.previous_world==a.world && shown.target_world==b.world && shown.previous_viewmodel==a.viewmodel &&
            shown.target_viewmodel==b.viewmodel && shown.interpolating==(alpha<1),"Presentation lost committed pair metadata.");
        check(shown.rendered_world.near_plane==.1 && shown.rendered_world.far_plane==100 &&
            shown.rendered_viewmodel->near_plane==.01 && shown.rendered_viewmodel->far_plane==10,
            "FOV interpolation modified clipping.");
    }
    check(history.presentation(0,true).rendered_world==a.world && history.presentation(1,true).rendered_world==b.world &&
        history.presentation(0,true).rendered_viewmodel==a.viewmodel && history.presentation(1,true).rendered_viewmodel==b.viewmodel,
        "Interpolation rounded exact endpoints.");
    const auto disabled=history.presentation(.25,false);
    check(!disabled.enabled && !disabled.interpolating && disabled.alpha==1 && disabled.rendered_world==b.world &&
        disabled.rendered_viewmodel==b.viewmodel,"Default disabled path changed target lens.");
    history.commit(sample(6,25,40));
    check(!history.presentation(.5,true).interpolating && history.presentation(.5,true).rendered_world==b.world,
        "Unchanged lenses drifted.");
}
void catchup_and_reversal() {
    PlayerLensHistory history;history.commit(sample());
    for(std::uint64_t tick=1;tick<=8;++tick)history.commit(sample(tick,70-double(tick)*3,75-double(tick)*2));
    const auto final=history.presentation(.5,true);
    check(final.previous_tick==7 && final.target_tick==8 && final.previous_world.vertical_fov==49 &&
        final.target_world.vertical_fov==46,"Eight catch-up Ticks retained an older interval.");
    close(final.rendered_world.vertical_fov,oracle(49,46,.5),"Catch-up interpolated more than its final pair.");
    history.commit(sample(9,65,73));
    const auto reversed=history.presentation(.25,true);
    check(reversed.previous_world.vertical_fov==46 && reversed.target_world.vertical_fov==65,
        "Reversal did not start at the committed predecessor.");
    close(reversed.rendered_world.vertical_fov,oracle(46,65,.25),"Reversal used a hidden eased trajectory.");
    auto independent=sample(10,60,70);history.commit(independent);independent.world.vertical_fov=5;independent.source_id="mutated";
    check(history.presentation(1,true).rendered_world.vertical_fov==60,"History borrowed caller sample storage.");
}
void preference_composition() {
    PlayerLensHistory history;history.commit(sample(1,70,75));history.commit(sample(2,35,50));
    constexpr double half_radians=0.0087266462599716478846;
    for(double alpha:{0.,.25,.5,1.}) {
        const auto shown=history.presentation(alpha,true,90);
        const double q=std::tan(70*half_radians)*std::pow(std::tan(35*half_radians)/std::tan(70*half_radians),alpha);
        const double expected=std::atan(std::tan(90*half_radians)*q/std::tan(70*half_radians))/half_radians;
        close(shown.rendered_world.vertical_fov,expected,"Preference did not compose with interpolated perspective zoom.");
        close(shown.rendered_viewmodel->vertical_fov,oracle(75,50,alpha),"World preference leaked into foreground.");
        check(shown.authored_fov==70 && shown.preference_fov==90 && shown.target_world.vertical_fov==35,
            "Preference overwrote authoritative target or authored baseline.");
    }
    check(history.presentation(0,true,90).rendered_world.vertical_fov==90,"Neutral preference endpoint was not exact.");
    auto external=sample(3,60);external.authored_fov.reset();history.reset(external,"external_camera");
    check(history.presentation(.5,true,95).rendered_world.vertical_fov==95,"External camera lost absolute preference semantics.");
    auto clamped=sample(4,150);clamped.authored_fov=5;history.reset(clamped,"clamp");
    check(history.presentation(1,false,150).rendered_world.vertical_fov==150,"Preference composition exceeded FOV bounds.");
}
void reset_boundaries() {
    const auto boundary=[](auto mutate,const char* reason) {
        PlayerLensHistory history;history.commit(sample(10));auto changed=sample(11,35,50);mutate(changed);history.commit(changed);
        auto shown=history.presentation(.25,true);
        check(shown.reset_reason==reason && !shown.interpolating && shown.alpha==1 && shown.previous_tick==11 &&
            shown.rendered_world==changed.world && shown.rendered_viewmodel==changed.viewmodel,"Unsafe boundary retained an interpolation pair.");
        changed.tick=12;history.commit(changed);
        check(history.presentation(.25,true).reset_reason.empty(),"Reset reason survived a subsequent consecutive pair.");
    };
    boundary([](auto& s){s.session_id="session-b";},"session_changed");
    boundary([](auto& s){s.source_id="source-b";},"source_changed");
    boundary([](auto& s){s.camera_id="camera-b";},"camera_changed");
    boundary([](auto& s){s.material_revision=1;},"material_revision_changed");
    boundary([](auto& s){s.world.near_plane=.2;},"world_clip_changed");
    boundary([](auto& s){s.world.far_plane=150;},"world_clip_changed");
    boundary([](auto& s){s.viewmodel.reset();},"viewmodel_presence_changed");
    boundary([](auto& s){s.viewmodel->near_plane=.02;},"viewmodel_clip_changed");
    boundary([](auto& s){s.viewmodel->far_plane=15;},"viewmodel_clip_changed");
    boundary([](auto& s){s.authored_fov=75;},"authored_fov_changed");
    boundary([](auto& s){s.authored_fov.reset();},"authored_fov_changed");
    PlayerLensHistory history;history.reset(sample(9),"pause");
    check(history.presentation(0,true).reset_reason=="pause","Caller reset reason was replaced.");
    for(std::uint64_t tick:{9u,7u,13u}) {
        history.commit(sample(tick,35));check(history.presentation(.5,true).reset_reason=="discontinuous_tick",
            "Repeated, old or skipped Tick was admitted as consecutive.");
    }
    history.reset(sample(std::numeric_limits<std::uint64_t>::max()),"maximum_tick");history.commit(sample(0));
    check(history.presentation(.5,true).reset_reason=="discontinuous_tick","Tick wrap invented a consecutive interval.");
}
void reconcile_and_observation() {
    PlayerLensHistory history;auto target=sample(2,35,50);history.commit(sample(1));history.commit(target);
    const auto before=history.presentation(.25,true);history.reconcile(target);
    const auto after=history.presentation(.25,true);
    check(after.previous_tick==before.previous_tick && after.target_tick==before.target_tick &&
        after.rendered_world==before.rendered_world && after.reset_reason==before.reset_reason,
        "Repeated owner observation changed the committed pair.");
    target.world.vertical_fov=40;history.reconcile(target);
    check(history.presentation(.25,true).reset_reason=="external_change" && history.presentation(.25,true).alpha==1,
        "External same-Tick edit was blended as gameplay.");
    target.tick=3;history.reconcile(target);
    check(history.presentation(.25,true).previous_tick==3 && history.presentation(.25,true).reset_reason=="external_change",
        "External consecutive step incorrectly used commit semantics.");
    target.source_id="loaded-source";history.reconcile(target);
    check(history.presentation(.25,true).reset_reason=="source_changed","Fresh load lost precise ownership reset.");
}
void invalid_and_atomic() {
    PlayerLensHistory empty;
    bool absent=false;try {empty.presentation(.5,true);}catch(const std::logic_error&) {absent=true;}
    check(absent,"Uninitialized history fabricated presentation.");
    PlayerLensHistory history;history.commit(sample(1));history.commit(sample(2,35,50));
    const auto old=history.presentation(.25,true);
    const auto invalid_sample=[&](auto mutate) {
        auto bad=sample(3);mutate(bad);
        rejects([&]{history.commit(bad);});rejects([&]{history.reset(bad,"bad");});rejects([&]{history.reconcile(bad);});
        const auto retained=history.presentation(.25,true);
        check(retained.previous_tick==old.previous_tick && retained.target_tick==old.target_tick && retained.rendered_world==old.rendered_world,
            "Rejected sample changed history.");
    };
    for(double value:{4.,151.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
        invalid_sample([&](auto& s){s.world.vertical_fov=value;});
        invalid_sample([&](auto& s){s.viewmodel->vertical_fov=value;});
        invalid_sample([&](auto& s){s.authored_fov=value;});
        rejects([&]{history.presentation(.5,false,value);});
    }
    invalid_sample([](auto& s){s.world.near_plane=.0009;});
    invalid_sample([](auto& s){s.world.far_plane=s.world.near_plane;});
    invalid_sample([](auto& s){s.viewmodel->far_plane=1e7+1;});
    invalid_sample([](auto& s){s.viewmodel->near_plane=std::numeric_limits<double>::quiet_NaN();});
    for(double phase:{-.01,1.01,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()})
        rejects([&]{history.presentation(phase,false);});
}
}
int main() {
    try {
        curve_and_defaults();catchup_and_reversal();preference_composition();reset_boundaries();
        reconcile_and_observation();invalid_and_atomic();
        std::cout<<"6 native player lens groups passed\n";return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
