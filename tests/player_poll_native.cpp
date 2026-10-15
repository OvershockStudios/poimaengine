// SPDX-License-Identifier: Apache-2.0
#include "poima/player_poll.hpp"
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace poima;
namespace {
void check(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
template<class F>void rejects(F operation) {
    bool denied=false;try {operation();}catch(const std::invalid_argument&) {denied=true;}
    check(denied,"Invalid pacing interval accepted.");
}
constexpr std::uint64_t ms=1000000;
void exact_deadlines() {
    PlayerIdlePacing pacing;check(pacing.remaining_ms(0)==0,"Initial poll is not immediate.");
    pacing.defer(123,16);
    check(pacing.remaining_ms(123)==16 && pacing.remaining_ms(123+3*ms)==13,"Incorrect idle deadline.");
    check(pacing.remaining_ms(123+16*ms-1)==1,"Fractional deadline rounded down into a spin.");
    check(pacing.remaining_ms(123+16*ms)==0 && pacing.remaining_ms(123+17*ms)==0,"Deadline did not become due.");
    pacing.defer(100*ms,26);check(pacing.remaining_ms(105*ms)==21,"Combined inactive/not-drawable pacing changed.");
    pacing.reset();check(pacing.remaining_ms(0)==0,"Reset did not admit immediate poll.");
    pacing.defer(7*ms,16);rejects([&]{pacing.defer(8*ms,1001);});
    rejects([&]{pacing.defer(std::numeric_limits<std::uint64_t>::max()-5,1);});
    check(pacing.remaining_ms(8*ms)==15,"Rejected interval changed a valid deadline.");
}

void zero_and_maximum_intervals() {
    PlayerIdlePacing pacing;pacing.defer(123,0);
    check(pacing.remaining_ms(123)==0,"Zero delay was not immediately due.");
    const auto end=std::numeric_limits<std::uint64_t>::max();
    pacing.defer(end-1000*ms,1000);
    check(pacing.remaining_ms(end-1)==1 && pacing.remaining_ms(end)==0,
          "Maximum admitted deadline overflowed or rounded down.");
    pacing.reset();check(pacing.remaining_ms(1)==0,"Reset retained a saturated deadline.");
}
}
int main() {
    try {
        exact_deadlines();zero_and_maximum_intervals();
        std::cout<<"2 player idle deadline groups passed; no window/transport claim\n";return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
