// SPDX-License-Identifier: Apache-2.0
// Actual separately compiled C++ ABI callback, not a managed/AOT artifact.
#include "physical_grounded_gameplay_fixture.hpp"
#include "poima/gameplay_abi.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace {
void need(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void reply(PoimaGameCall& c,const std::string& text){need(c.output&&c.output_capacity>text.size(),"Grounded fixture output capacity differs.");std::memcpy(c.output,text.c_str(),text.size()+1);}
void tick(PoimaGameCall& c){
    need(c.state&&c.state_bytes==sizeof(grounded_test::CallbackState)&&c.services&&c.services->version==7&&c.services->bytes>=424,"Grounded callback state/services differ.");
    auto& s=*static_cast<grounded_test::CallbackState*>(c.state);const PoimaEntityId actor{0,100},rig{0,101},foot{0,105};
    const auto* physical=reinterpret_cast<const PoimaGamePhysicalRigServicesV1*>(c.services);
    const auto* ik=reinterpret_cast<const PoimaGameTwoBoneIkServicesV1*>(c.services);
    const auto* oriented=reinterpret_cast<const PoimaGameTwoBoneIkOrientationServicesV1*>(c.services);
    const auto* grounding=reinterpret_cast<const PoimaGameGroundingServicesV1*>(c.services);PoimaGameError error{};
    PoimaGamePhysicalRigSummaryV1 phase{};phase.version=1;phase.bytes=sizeof(phase);
    need(physical->physical_rig_summary(c.services->context,&rig,&phase,&error)==0&&phase.present,"Grounded callback physical owner absent.");
    PoimaGameTwoBoneIkStateV1 first{};first.version=1;first.bytes=sizeof(first);
    need(ik->ik_get(c.services->context,&rig,1,&first,&error)==0&&first.present,"Grounded callback IK absent.");
    PoimaGameGroundingStateV1 requested{};requested.version=1;requested.bytes=sizeof(requested);
    need(grounding->grounding_get(c.services->context,&rig,1,&requested,&error)==0&&requested.present,"Grounded callback requested controls absent.");s.requested_weight=requested.weight;
    for(unsigned n=0;n<32;++n){
        PoimaGameTwoBoneIkStateV1 again{};again.version=1;again.bytes=sizeof(again);
        need(ik->ik_get(c.services->context,&rig,1,&again,&error)==0&&std::memcmp(&first,&again,sizeof(first))==0,"Repeated callback IK getter changed cached candidate.");
        PoimaGameTwoBoneIkStateV2 frame{};frame.point.version=2;frame.point.bytes=sizeof(frame);
        need(oriented->ik_orientation_get(c.services->context,&rig,1,&frame,&error)==0&&frame.point.present&&frame.end_orientation_present,"Grounded orientation getter lost its generated sole frame.");
        for(unsigned axis=0;axis<3;++axis)need(frame.point.end_position[axis]==first.end_position[axis],"Point/oriented callback candidates differ.");++s.checks;
    }
    if(phase.mode==0){PoimaGameEntity a{},f{};
        need(c.services->entity(c.services->context,&actor,&a,&error)==0&&c.services->entity(c.services->context,&foot,&f,&error)==0,"Grounded callback entity reads failed.");
        const auto yaw=s.expected_yaw*std::acos(-1.)/180.;need(std::abs(a.world[0]-std::cos(yaw))<2e-6&&std::abs(a.world[2]+std::sin(yaw))<2e-6,"Grounded callback actor did not observe prepared caller yaw.");
        s.endpoint_gap=0;for(unsigned k=0;k<3;++k)s.endpoint_gap=std::max(s.endpoint_gap,std::abs(f.world[12+k]-first.end_position[k]));
        need(s.endpoint_gap<2e-7,"Grounded callback FK and point diagnostics disagree.");
    }else{need(phase.mode==1,"Grounded callback unknown physical mode.");++s.passive_reads;}
    const auto mode=s.mode;s.mode=0;
    if(mode==1||mode==3){PoimaGameGroundingCommandV1 control{};control.version=1;control.bytes=sizeof(control);control.entity=rig;control.slot=1;control.enabled=1;control.weight=.75;
        need(grounding->grounding_set(c.services->context,&control,&error)==0,"Grounded callback control staging failed.");++s.writes;
        PoimaGameGroundingStateV1 after{};after.version=1;after.bytes=sizeof(after);
        need(grounding->grounding_get(c.services->context,&rig,1,&after,&error)==0&&std::memcmp(&requested,&after,sizeof(after))==0,"Staged grounding multiplier leaked into its callback.");}
    if(mode==1||mode==2){PoimaGamePhysicalRigCommandV1 death{};death.version=1;death.bytes=sizeof(death);death.entity=rig;death.flags=1;
        need(physical->physical_rig_command(c.services->context,&death,&error)==0,"Grounded callback activation staging failed.");}
    if(mode<0||mode>3)throw std::runtime_error("Unknown grounded callback mode.");++s.calls;
}
}
#ifdef _WIN32
#define GROUNDED_EXPORT __declspec(dllexport)
#else
#define GROUNDED_EXPORT __attribute__((visibility("default")))
#endif
extern "C" GROUNDED_EXPORT int32_t POIMA_CALL poima_gameplay_entry(PoimaGameCall* source,uint32_t bytes) noexcept{
    try{need(source&&bytes==sizeof(PoimaGameCall)&&source->version==1,"Grounded fixture entry differs.");auto& c=*source;
        switch(c.operation){
        case 1:{auto m=grounded_test::schema();m["handle"]=1;m["requirements"]=grounded_test::requirements();m["diagnostics"]={{"dynamic_code_supported",false},{"dynamic_code_compiled",false}};reply(c,m.dump());break;}
        case 2:need(c.state&&c.state_bytes==sizeof(grounded_test::CallbackState),"Grounded fixture initialize differs.");*static_cast<grounded_test::CallbackState*>(c.state)={};reply(c,"");break;
        case 3:tick(c);reply(c,"");break;case 4:case 6:reply(c,"");break;
        case 5:reply(c,"{\"active_modules\":0,\"retired_alive\":0}");break;default:throw std::runtime_error("Unsupported grounded fixture operation.");}return 0;
    }catch(const std::exception& e){if(source&&source->output&&source->output_capacity){const auto n=std::min<std::size_t>(std::strlen(e.what()),source->output_capacity-1);std::memcpy(source->output,e.what(),n);source->output[n]=0;}return 1;}
}
