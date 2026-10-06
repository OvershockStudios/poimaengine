// SPDX-License-Identifier: Apache-2.0
#include "poima/player.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace poima {
PlayerControlResult PlayerSession::control(const std::string&,std::uint64_t,const std::string&) {
    throw std::runtime_error("This player owner does not provide native UI controls.");
}
void PlayerInput::button(PlayerAction action, bool down) {
    const auto index=static_cast<std::size_t>(action);
    if(index>=held_.size()) throw std::invalid_argument("Invalid player action.");
    if(action==PlayerAction::jump && down && !held_[index]) jump_=true;
    if(action==PlayerAction::use && down && !held_[index])use_=true;
    held_[index]=down;
}
void PlayerInput::look(double yaw,double pitch) {
    if(!std::isfinite(yaw) || !std::isfinite(pitch)) throw std::invalid_argument("Invalid mouse delta.");
    // Bound a backlog from pathological event sources without losing ordinary
    // sub-tick input. Runtime limits each consumed look delta to 180 degrees.
    look_[0]=std::clamp(look_[0]+yaw,-720.0,720.0);
    look_[1]=std::clamp(look_[1]+pitch,-720.0,720.0);
}
void PlayerInput::clear() { held_.fill(false); look_.fill(0); jump_=false;use_=false; }
RuntimeInput PlayerInput::consume(const std::string& entity) {
    RuntimeInput result; result.entity=entity;
    result.move={float(held_[3])-float(held_[2]),float(held_[0])-float(held_[1])};
    for(std::size_t k=0;k<2;++k)result.look[k]=static_cast<float>(std::clamp(look_[k],-180.0,180.0));
    result.jump=jump_;result.use=use_;commit_tick();return result;
}
void PlayerInput::commit_tick() noexcept {
    for(auto& value:look_)value-=std::clamp(value,-180.0,180.0);
    jump_=false;use_=false;
}
std::uint32_t PlayerClock::advance(double elapsed,bool active) {
    if(!std::isfinite(elapsed) || elapsed<0) throw std::invalid_argument("Invalid frame duration.");
    if(!active) { accumulated_=0; return 0; }
    // Keep the fixed step; discard excess wall time instead of a spiral of
    // catch-up work after a debugger stop, window drag or severe stall.
    const double accepted=std::min(elapsed,8*Runtime::fixed_dt);
    dropped_+=elapsed-accepted; accumulated_+=accepted;
    // The boundary tolerance can round an almost-full old fraction plus eight
    // accepted ticks up to nine. Enforce the work cap explicitly; retain the
    // remaining fraction for a later poll instead of discarding earned time.
    const auto ticks=std::min(8u,static_cast<std::uint32_t>(std::floor((accumulated_+1e-12)/Runtime::fixed_dt)));
    accumulated_=std::max(0.0,accumulated_-ticks*Runtime::fixed_dt);
    return ticks;
}
}
