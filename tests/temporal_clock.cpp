// SPDX-License-Identifier: Apache-2.0
#include "poima/temporal_clock.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

int main() {
    using poima::TemporalClock;
    auto time=[](int ms) { return TemporalClock::Time{}+std::chrono::milliseconds(ms); };
    unsigned checks=0;
    auto equal=[&](float actual,float expected) {
        ++checks;
        if(std::abs(actual-expected)>0.0001f) throw std::runtime_error("Temporal interval differs");
    };
    TemporalClock live;
    equal(live.begin(time(0),true,false),TemporalClock::fixed_milliseconds);
    live.accept(); // Epoch zero is a valid accepted timestamp.
    equal(live.begin(time(25),true,false),25);
    // Abandoned preparation must not advance the accepted clock.
    equal(live.begin(time(40),true,false),40);
    live.accept();
    equal(live.begin(time(48),true,false),8);
    live.accept();
    equal(live.begin(time(48),true,false),1);
    equal(live.begin(time(500),true,false),100); // Hidden/paused interval is bounded.
    live.accept();
    equal(live.begin(time(501),true,true),TemporalClock::fixed_milliseconds);
    live.accept();
    equal(live.begin(time(517),true,false),16);
    live.reset();
    live.accept(); // Reset discarded both accepted and pending state.
    equal(live.begin(time(600),true,false),TemporalClock::fixed_milliseconds);
    TemporalClock capture, replay;
    for(int ms:{0,2,45,900,901}) {
        equal(capture.begin(time(ms),false,false),TemporalClock::fixed_milliseconds);
        capture.accept();
        equal(replay.begin(time(ms*2),false,false),TemporalClock::fixed_milliseconds);
        replay.accept();
    }
    // Independent live view does not inherit another view's clock.
    TemporalClock other;
    equal(other.begin(time(601),true,false),TemporalClock::fixed_milliseconds);
    constexpr bool expected_modes[]{false,false,true,false,true,false,true,false};
    unsigned mode=0;
    for(bool player:{false,true})for(bool hosted:{false,true})for(bool replaying:{false,true}) {
        ++checks;
        if(poima::live_temporal_clock(player,hosted,replaying)!=expected_modes[mode++])
            throw std::runtime_error("Clock mode selection differs");
    }
    std::cout << "{\"passed\":true,\"checks\":" << checks << "}\n";
}
