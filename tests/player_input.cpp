// SPDX-License-Identifier: Apache-2.0
#include "poima/player.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace poima;
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main() {
    try {
        PlayerInput input;
        input.button(PlayerAction::forward,true); input.button(PlayerAction::right,true);
        input.button(PlayerAction::jump,true); input.button(PlayerAction::jump,false);
        input.button(PlayerAction::use,true);input.button(PlayerAction::use,false);
        input.look(2.5,-1.25); input.look(.5,.25);
        auto first=input.consume("player");
        check(first.entity=="player" && first.move==std::array<float,2>{1,1} && first.look==std::array<float,2>{3,-1} && first.jump && first.use,"Sub-tick input was lost.");
        auto second=input.consume("player");
        check(second.move==first.move && second.look==std::array<float,2>{0,0} && !second.jump && !second.use,"Edges repeated across ticks.");
        input.button(PlayerAction::jump,true); check(input.consume("player").jump,"Jump press lost.");
        input.button(PlayerAction::jump,true); check(!input.consume("player").jump,"Key repeat retriggered jump.");
        input.button(PlayerAction::use,true);check(input.consume("player").use,"Use press lost.");
        input.button(PlayerAction::use,true);check(!input.consume("player").use,"Key repeat retriggered use.");
        input.button(PlayerAction::use,false);input.button(PlayerAction::use,true);
        input.button(PlayerAction::backward,true); check(input.consume("player").move[1]==0,"Opposed movement did not cancel.");
        input.look(200,0); check(input.consume("player").look[0]==180 && input.consume("player").look[0]==20,"Look overflow was lost.");
        input.look(4,5); input.clear(); auto cleared=input.consume("player");
        check(cleared.move==std::array<float,2>{0,0} && cleared.look==std::array<float,2>{0,0} && !cleared.jump && !cleared.use,"Focus loss retained input.");
        PlayerClock a,b;
        unsigned fast=0,slow=0;
        for(int i=0;i<144;++i) fast+=a.advance(1.0/144,true);
        for(int i=0;i<30;++i) slow+=b.advance(1.0/30,true);
        check(fast==60 && slow==60,"Fixed timestep depends on presentation rate.");
        PlayerClock paused; check(paused.advance(.01,true)==0,"Fractional time advanced.");
        check(paused.advance(100,false)==0 && paused.advance(.01,true)==0,"Pause caught up elapsed wall time.");
        check(paused.advance(10,true)==8 && paused.dropped_seconds()>9.8,"Stall catch-up is unbounded.");
        const auto stall=paused.last_sample();
        check(stall.observed && stall.active && stall.elapsed_seconds==10 &&
              stall.accepted_seconds==8*Runtime::fixed_dt &&
              stall.dropped_seconds==10-8*Runtime::fixed_dt && stall.planned_ticks==8 &&
              stall.committed_ticks==0 && stall.accumulator_seconds>=0,
              "Clock observation changed or concealed bounded stall policy.");
        paused.advance(10,false);const auto suspension=paused.last_sample();
        check(suspension.observed && !suspension.active && suspension.elapsed_seconds==10 &&
              suspension.accepted_seconds==0 && suspension.dropped_seconds==0 &&
              suspension.accumulator_seconds==0 && suspension.planned_ticks==0 &&
              paused.dropped_seconds()==stall.dropped_seconds,
              "Paused observation invented discarded time or reset lifetime drops.");
        PlayerClock boundary;
        // Exact double reproducer: epsilon-assisted rounding formerly returned
        // nine ticks from the second call despite the eight-tick work budget.
        check(boundary.advance(0.016666666665666664,true)==0,"Boundary fraction unexpectedly advanced.");
        check(boundary.advance(8*Runtime::fixed_dt,true)==8,"Fractional carry exceeded the eight-tick cap.");
        check(boundary.dropped_seconds()==0,"Work capping discarded accepted fractional time.");
        check(boundary.last_sample().planned_ticks==8 && boundary.last_sample().dropped_seconds==0 &&
              boundary.last_sample().accumulator_seconds>0,
              "Clock diagnostics lost accepted fractional carry.");
        check(boundary.advance(.0001,true)==1,"Work cap lost the prior fractional carry.");
        PlayerClock bounded;
        check(bounded.advance(0.016666666665666664,true)==0,"Bounded fixture fraction unexpectedly advanced.");
        for(int i=0;i<100;++i)check(bounded.advance(8*Runtime::fixed_dt,true)==8,"Repeated bounded polls exceeded the work cap.");
        bounded.advance(0,false);
        check(bounded.advance(0,true)==0,"Suspension retained capped fractional time.");
        bool rejected=false; try { paused.advance(std::numeric_limits<double>::quiet_NaN(),true); } catch(const std::invalid_argument&) { rejected=true; }
        check(rejected,"NaN frame duration accepted.");
        std::cout<<"Input edges, focus reset, bounded look, presentation-independent fixed ticks and stall limits passed.\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
