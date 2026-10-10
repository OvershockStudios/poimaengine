// SPDX-License-Identifier: Apache-2.0
#include "poima/player.hpp"
#include "poima/player_preferences.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace poima {
PlayerControlResult PlayerSession::control(const std::string&,std::uint64_t,const std::string&) {
    throw std::runtime_error("This player owner does not provide native UI controls.");
}
void validate_player_preferences(const PlayerOptions& options) {
    if(!options.preferences)return;
    const auto state=options.preferences->snapshot();
    if(state->values.samples!=options.render.samples || state->values.frames_in_flight!=options.render.frames_in_flight)
        throw std::invalid_argument("Player preferences must match the window's frozen launch sample/frame-slot configuration.");
}
SceneSnapshot player_snapshot(const PlayerOptions& options,const PlayerSession& session) {
    auto result=session.snapshot(options.camera);
    const auto preferences=options.preferences ? options.preferences->snapshot() : nullptr;
    const auto fov=preferences ? preferences->values.vertical_fov : options.vertical_fov;
    const auto scale=preferences ? preferences->values.ui_scale : options.ui_scale;
    if(fov) {
        const auto value=*fov;
        if(!std::isfinite(value) || value<5 || value>150)
            throw std::invalid_argument("Player vertical FOV must be finite and within 5..150 degrees.");
        result.vertical_fov=value;
    }
    if(scale && (!std::isfinite(*scale) || *scale<.25f || *scale>8.f))
        throw std::invalid_argument("Player UI scale must be finite and within 0.25..8.");
    return result;
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
    last_={};last_.observed=true;last_.active=active;last_.elapsed_seconds=elapsed;
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
    last_.accepted_seconds=accepted;last_.dropped_seconds=elapsed-accepted;
    last_.accumulator_seconds=accumulated_;last_.planned_ticks=ticks;
    return ticks;
}
}
