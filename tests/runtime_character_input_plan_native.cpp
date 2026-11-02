// SPDX-License-Identifier: Apache-2.0
#include "runtime_character_input_plan.hpp"
#include <iostream>
#include <stdexcept>

using namespace poima;
namespace {
void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
void near(float actual,float expected) {check(std::isfinite(actual) && std::abs(actual-expected)<2e-6f,"Prepared movement differs from independent axis geometry.");}
void controls_and_edges() {
    RuntimeInput input;input.move={0,1};input.look={90,100};input.jump=true;
    const auto plan=detail::prepare_character_input(0,0,JPH::Quat::sIdentity(),-2,true,4,5,input,true);
    check(plan.yaw==90 && plan.pitch==85 && plan.write_rotation && plan.request_jump,"Look clamp or grounded jump not prepared.");
    near(plan.requested_velocity.GetX(),-4);near(plan.requested_velocity.GetY(),5);near(plan.requested_velocity.GetZ(),0);
    check(input.look==std::array<float,2>{90,100} && input.move==std::array<float,2>{0,1} && input.jump,"Planning mutated its caller input.");
    const auto later=detail::prepare_character_input(plan.yaw,plan.pitch,plan.rotation,2,true,4,5,input,false);
    check(later.yaw==90 && later.pitch==85 && !later.write_rotation && !later.request_jump && later.rotation==plan.rotation,
        "Held look/jump replayed after the first batch frame.");
    near(later.requested_velocity.GetY(),2);near(later.requested_velocity.GetX(),-4);
    const auto air=detail::prepare_character_input(0,0,JPH::Quat::sIdentity(),-3,false,4,5,input,true);
    check(!air.request_jump,"Airborne jump intent was accepted.");near(air.requested_velocity.GetY(),-3);
}
void diagonal_neutral_and_existing_orientation() {
    RuntimeInput input;input.move={1,1};
    const auto diagonal=detail::prepare_character_input(0,0,JPH::Quat::sIdentity(),1,false,3,5,input,true);
    near(diagonal.requested_velocity.GetX(),3/std::sqrt(2.0f));near(diagonal.requested_velocity.GetZ(),-3/std::sqrt(2.0f));
    near(diagonal.requested_velocity.LengthSq(),10);
    check(!diagonal.write_rotation,"Zero yaw requested native contact invalidation.");
    // Initial native heading need not be recreated from the scalar yaw when
    // no yaw edge exists. Preserve every quaternion component, including roll.
    const auto rotation=JPH::Quat::sRotation(JPH::Vec3::sAxisZ(),.5f);
    input.move={1,0};input.look={0,-180};
    const auto roll=detail::prepare_character_input(47,-10,rotation,1,true,2,5,input,true);
    check(roll.rotation==rotation && !roll.write_rotation && roll.yaw==47 && roll.pitch==-85,"Pitch-only input changed native orientation.");
    near(roll.requested_velocity.GetX(),2*std::cos(.5f));near(roll.requested_velocity.GetY(),1);
    const RuntimeInput neutral;
    const auto idle=detail::prepare_character_input(47,-10,rotation,-3,false,2,5,neutral,true);
    near(idle.requested_velocity.GetX(),0);near(idle.requested_velocity.GetZ(),0);near(idle.requested_velocity.GetY(),-3);
    check(!idle.write_rotation && !idle.request_jump,"Neutral input requested a native orientation or jump mutation.");
    input.look={10,0};
    const auto wrap=detail::prepare_character_input(179,3,rotation,0,true,2,5,input,true);
    check(wrap.yaw==-171 && wrap.pitch==3 && wrap.write_rotation,"Yaw wrap differs from remainder policy.");
}
}
int main() {
    try {controls_and_edges();diagonal_neutral_and_existing_orientation();std::cout<<"Input planning: edges, independent axis geometry, neutral/air/diagonal and unchanged orientation pass.\n";return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
