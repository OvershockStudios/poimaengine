// SPDX-License-Identifier: Apache-2.0
// Actual C++ shared-library consumer of the native gameplay ABI. It deliberately
// does not impersonate CoreCLR, a C# game, or a Native AOT compiler output.
#include "runtime_physical_ik_fixture.hpp"
#include "poima/gameplay_abi.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <iterator>
#include <stdexcept>

namespace {
void need(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
void reply(PoimaGameCall& call,const std::string& text) {
    need(call.output && call.output_capacity>text.size(),"Fixture output capacity is insufficient.");
    std::memcpy(call.output,text.c_str(),text.size()+1);
}
void tick(PoimaGameCall& call) {
    using physical_ik_test::CallbackState;
    need(call.state && call.state_bytes==sizeof(CallbackState),"Fixture state size differs.");
    need(call.services && call.services->version==7 && call.services->bytes>=424,"Fixture services are incomplete.");
    auto& state=*static_cast<CallbackState*>(call.state);
    const auto* ik=reinterpret_cast<const PoimaGameTwoBoneIkServicesV1*>(call.services);
    const auto* physical=reinterpret_cast<const PoimaGamePhysicalRigServicesV1*>(call.services);
    const PoimaEntityId actor{0,100},rig{0,101},end{0,104};
    PoimaGameError error{};
    PoimaGamePhysicalRigSummaryV1 phase{};phase.version=1;phase.bytes=sizeof(phase);
    need(physical->physical_rig_summary(call.services->context,&rig,&phase,&error)==0 && phase.present==1,"Fixture physical phase read failed.");
    PoimaGameTwoBoneIkStateV1 before{};before.version=1;before.bytes=sizeof(before);
    need(ik->ik_get(call.services->context,&rig,1,&before,&error)==0 && before.present==1,"Fixture IK read failed.");
    if(phase.mode==0) {
        PoimaGameEntity actor_pose{},end_pose{};
        need(call.services->entity(call.services->context,&actor,&actor_pose,&error)==0,"Fixture actor read failed.");
        need(call.services->entity(call.services->context,&end,&end_pose,&error)==0,"Fixture endpoint read failed.");
        const double radians=state.expected_yaw*std::acos(-1.)/180.;
        need(std::abs(actor_pose.world[0]-std::cos(radians))<2e-6 && std::abs(actor_pose.world[2]+std::sin(radians))<2e-6,
            "Callback did not observe the planned caller yaw.");
        state.end_gap=0;state.target_gap=0;
        for(unsigned axis=0;axis<3;++axis) {
            state.end_gap=std::max(state.end_gap,std::abs(end_pose.world[12+axis]-before.end_position[axis]));
            state.target_gap=std::max(state.target_gap,std::abs(end_pose.world[12+axis]-before.target[axis]));
        }
        need(state.end_gap<2e-7 && state.target_gap<2e-6,"Planned callback bones and world-space IK diagnostics disagree.");
    } else { need(phase.mode==1,"Unknown fixture physical phase.");++state.passive_reads; }
    if(state.mode==1 || state.mode==2 || state.mode==4 || state.mode==5) {
        PoimaGameTwoBoneIkCommandV1 command{};command.version=1;command.bytes=sizeof(command);
        command.entity=rig;command.slot=1;command.enabled=1;command.space=before.space;command.weight=before.weight;
        std::copy(std::begin(before.target),std::end(before.target),command.target);
        std::copy(std::begin(before.pole),std::end(before.pole),command.pole);
        command.target[0]+=.025;
        if(state.mode==2)command.target[0]=std::numeric_limits<double>::quiet_NaN();
        const auto result=ik->ik_set(call.services->context,&command,&error);
        if(state.mode==2)need(result!=0,"Malformed callback IK command was accepted.");
        else { need(result==0,"Valid callback IK command was rejected before its commit boundary.");++state.writes; }
        PoimaGameTwoBoneIkStateV1 after{};after.version=1;after.bytes=sizeof(after);
        need(ik->ik_get(call.services->context,&rig,1,&after,&error)==0,"Fixture post-write IK read failed.");
        need(std::memcmp(&before,&after,sizeof(before))==0,"Staged callback IK write leaked into the same callback's getter.");
        ++state.staged_same;
    }
    if(state.mode==3 || state.mode==4) {
        PoimaGamePhysicalRigCommandV1 death{};death.version=1;death.bytes=sizeof(death);death.entity=rig;death.flags=1;
        need(physical->physical_rig_command(call.services->context,&death,&error)==0,"Fixture activation staging failed.");
    }
    if(state.mode==5)throw std::runtime_error("Intentional failure after a staged IK write.");
    ++state.calls;
}
}

#ifdef _WIN32
#define PHYSICAL_IK_EXPORT __declspec(dllexport)
#else
#define PHYSICAL_IK_EXPORT __attribute__((visibility("default")))
#endif
extern "C" PHYSICAL_IK_EXPORT int32_t POIMA_CALL poima_gameplay_entry(PoimaGameCall* source,uint32_t bytes) noexcept {
    try {
        need(source && bytes==sizeof(PoimaGameCall) && source->version==1,"Invalid fixture entry call.");
        auto& call=*source;
        switch(call.operation) {
        case 1: {
            auto manifest=physical_ik_test::schema();manifest["handle"]=1;
            manifest["requirements"]=physical_ik_test::requirements();
            // This compiled C++ module has no dynamic-code execution facility.
            manifest["diagnostics"]={{"dynamic_code_supported",false},{"dynamic_code_compiled",false}};
            reply(call,manifest.dump());break;
        }
        case 2: need(call.state && call.state_bytes==sizeof(physical_ik_test::CallbackState),"Invalid fixture initialization.");
            *static_cast<physical_ik_test::CallbackState*>(call.state)={};reply(call,"");break;
        case 3: tick(call);reply(call,"");break;
        case 4: case 6: reply(call,"");break;
        case 5: reply(call,"{\"active_modules\":0,\"retired_alive\":0}");break;
        default: throw std::runtime_error("Unsupported fixture operation.");
        }
        return 0;
    } catch(const std::exception& error) {
        if(source && source->output && source->output_capacity) {
            const auto n=std::min<std::size_t>(std::strlen(error.what()),source->output_capacity-1);
            std::memcpy(source->output,error.what(),n);source->output[n]=0;
        }
        return 1;
    }
}
