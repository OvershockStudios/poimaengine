// SPDX-License-Identifier: Apache-2.0
#include "runtime_physical_recovery_state.hpp"
#include "runtime_physical_state_json.hpp"
#include "runtime_body_ids.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace poima {
namespace {
using Json=nlohmann::json;
void need(bool value,const char* message){if(!value)throw std::invalid_argument(message);}
void fields(const Json& j,std::initializer_list<const char*> names) {
    need(j.is_object()&&j.size()==names.size(),"Recovery state field count differs.");
    for(const auto* name:names)need(j.contains(name),"Recovery state required field is absent.");
}
std::uint64_t integer(const Json& j,std::uint64_t bound=physical_state_json::max_tick) {
    need(j.is_number_unsigned()&&j.get<std::uint64_t>()<=bound,"Recovery integer must be canonical unsigned and bounded.");
    return j.get<std::uint64_t>();
}
double number(const Json& j,double low,double high) {
    need(j.is_number_float(),"Recovery scalar must retain its canonical floating-point type.");const auto v=j.get<double>();
    need(std::isfinite(v)&&v>=low&&v<=high,"Recovery number exceeds its finite bounds.");return v;
}
template<std::size_t N>std::array<double,N> vector(const Json& j,double low,double high) {
    need(j.is_array()&&j.size()==N,"Recovery vector size differs.");std::array<double,N> r{};
    for(std::size_t i=0;i<N;++i)r[i]=number(j[i],low,high);return r;
}
std::string text(const Json& j) {
    need(j.is_string(),"Recovery entity must be text.");auto s=j.get<std::string>();
    need(!s.empty()&&s.size()<=256,"Recovery entity size differs.");
    for(unsigned char c:s)need(c>=32&&c!=127,"Recovery entity contains control characters.");return s;
}
bool boolean(const Json& j){need(j.is_boolean(),"Recovery flag must be Boolean.");return j.get<bool>();}
JPH::BodyID body(const Json& j) {
    const auto raw=static_cast<std::uint32_t>(integer(j,std::numeric_limits<std::uint32_t>::max()));
    need(raw!=JPH::BodyID::cInvalidBodyID&&!(raw&JPH::BodyID::cBroadPhaseBit)&&
        (raw&JPH::BodyID::cMaxBodyIndex)<RuntimeBodyIds::capacity,"Recovery body identity has invalid native bits or slot.");return JPH::BodyID(raw);
}
Json transform_json(const RuntimeTransform& t){return {{"position",t.position},{"rotation",t.rotation},{"scale",t.scale}};}
RuntimeTransform transform(const Json& j) {
    fields(j,{"position","rotation","scale"});RuntimeTransform t;
    t.position=vector<3>(j.at("position"),-1e6,1e6);t.rotation=vector<4>(j.at("rotation"),-1.000001,1.000001);
    const double norm=std::hypot(std::hypot(t.rotation[0],t.rotation[1]),std::hypot(t.rotation[2],t.rotation[3]));
    need(std::abs(norm-1)<=physical_rig::unit_tolerance,"Recovery rotation must retain a unit quaternion.");
    t.scale=vector<3>(j.at("scale"),1e-8,1e8);return t;
}
Json frame_json(const RuntimePhysicalRecoveryFrame& f) {
    Json nodes=Json::array();for(const auto& p:f.nodes)nodes.push_back({{"entity",p.entity},{"local",transform_json(p.local)}});
    return {{"tick",f.tick},{"actor",transform_json(f.actor)},{"wrapper",transform_json(f.wrapper)},{"nodes",std::move(nodes)}};
}
RuntimePhysicalRecoveryFrame frame(const Json& j) {
    fields(j,{"tick","actor","wrapper","nodes"});RuntimePhysicalRecoveryFrame f;
    f.tick=integer(j.at("tick"));f.actor=transform(j.at("actor"));f.wrapper=transform(j.at("wrapper"));
    const auto& nodes=j.at("nodes");need(nodes.is_array()&&!nodes.empty()&&nodes.size()<=10000,"Recovery needs all bounded original nodes.");
    std::set<std::string> names;for(const auto& p:nodes){fields(p,{"entity","local"});auto id=text(p.at("entity"));
        need(names.insert(id).second,"Recovery frame repeats a node.");f.nodes.push_back({std::move(id),transform(p.at("local"))});}return f;
}
Json query_json(const RuntimePhysicalRecoveryQueryResult& q) {
    Json supports=Json::array();for(const auto& s:q.supports) {
        if(!s){supports.push_back(nullptr);continue;}
        supports.push_back({{"body",s->body.GetIndexAndSequenceNumber()},{"subshape",s->subshape},{"user_data",s->user_data},
            {"position",s->position},{"normal",s->normal},{"sole_gap",s->sole_gap},{"static_body",s->static_body}});
    }
    Json blocker=nullptr;if(q.blocker){const auto& b=*q.blocker;blocker={{"body",b.body.GetIndexAndSequenceNumber()},{"subshape",b.subshape},
        {"point_on_capsule",b.point_on_capsule},{"point_on_obstacle",b.point_on_obstacle},{"normal",b.normal},{"penetration",b.penetration},{"normal_valid",b.normal_valid}};}
    return {{"refusal",static_cast<std::uint32_t>(q.refusal)},{"support_index",q.support_index?Json(*q.support_index):Json(nullptr)},
        {"supports",std::move(supports)},{"blocker",std::move(blocker)},{"body_visits",q.body_visits},
        {"support_hits",q.support_hits},{"proxy_hits",q.proxy_hits},{"motor_hits",q.motor_hits}};
}
RuntimePhysicalRecoveryQueryResult query(const Json& j) {
    fields(j,{"refusal","support_index","supports","blocker","body_visits","support_hits","proxy_hits","motor_hits"});
    RuntimePhysicalRecoveryQueryResult q;q.refusal=static_cast<RuntimePhysicalRecoveryRefusal>(integer(j.at("refusal"),static_cast<std::uint32_t>(RuntimePhysicalRecoveryRefusal::InvalidNativeResult)));
    if(!j.at("support_index").is_null())q.support_index=static_cast<std::uint32_t>(integer(j.at("support_index"),1));
    const auto& supports=j.at("supports");need(supports.is_array()&&supports.size()==2,"Recovery needs two support results.");
    for(std::size_t i=0;i<2;++i)if(!supports[i].is_null()) {
        const auto& s=supports[i];fields(s,{"body","subshape","user_data","position","normal","sole_gap","static_body"});
        RuntimePhysicalRecoverySupport out;out.body=body(s.at("body"));out.subshape=static_cast<std::uint32_t>(integer(s.at("subshape"),UINT32_MAX));
        out.user_data=integer(s.at("user_data"),UINT64_MAX);out.position=vector<3>(s.at("position"),-1e6,1e6);
        out.normal=vector<3>(s.at("normal"),-1.000001,1.000001);out.sole_gap=number(s.at("sole_gap"),-2,2);out.static_body=boolean(s.at("static_body"));q.supports[i]=out;
    }
    if(!j.at("blocker").is_null()) {
        const auto& b=j.at("blocker");fields(b,{"body","subshape","point_on_capsule","point_on_obstacle","normal","penetration","normal_valid"});
        RuntimePhysicalRecoveryBlocker out;out.body=body(b.at("body"));out.subshape=static_cast<std::uint32_t>(integer(b.at("subshape"),UINT32_MAX));
        out.point_on_capsule=vector<3>(b.at("point_on_capsule"),-1e6,1e6);out.point_on_obstacle=vector<3>(b.at("point_on_obstacle"),-1e6,1e6);
        out.normal=vector<3>(b.at("normal"),-1.000001,1.000001);out.penetration=number(b.at("penetration"),-1e6,1e6);out.normal_valid=boolean(b.at("normal_valid"));q.blocker=out;
    }
    q.body_visits=static_cast<std::uint32_t>(integer(j.at("body_visits"),4097));
    q.support_hits=static_cast<std::uint32_t>(integer(j.at("support_hits"),16385));q.proxy_hits=static_cast<std::uint32_t>(integer(j.at("proxy_hits"),16385));q.motor_hits=static_cast<std::uint32_t>(integer(j.at("motor_hits"),16385));
    need(q.support_hits+q.proxy_hits+q.motor_hits<=16385,"Recovery aggregate hit count exceeds bounded traversal.");
    const auto accepted_support=[](const RuntimePhysicalRecoverySupport& s) {
        const auto& n=s.normal;
        return s.static_body&&std::abs(std::hypot(n[0],n[1],n[2])-1)<=1e-5&&
            n[1]>=.17364817766693041&&s.sole_gap>=-1.000001&&s.sole_gap<=1.000001;
    };
    // The helper records a prefix of completed rays. Only the current failed
    // ray may carry a nonstatic, steep or inside-origin support witness.
    need(!q.supports[1]||q.supports[0],"Recovery support results are not a completed prefix.");
    const auto failure=q.refusal;
    const bool support_failure=failure==RuntimePhysicalRecoveryRefusal::MissingSupport||
        failure==RuntimePhysicalRecoveryRefusal::MovingSupport||failure==RuntimePhysicalRecoveryRefusal::InvalidSupport;
    if(support_failure) {
        need(q.support_index&&!q.blocker&&q.proxy_hits==0&&q.motor_hits==0,"Recovery support refusal has invalid query phase or index.");
        const auto index=*q.support_index;
        for(std::size_t n=0;n<2;++n) {
            if(n<index)need(q.supports[n]&&accepted_support(*q.supports[n]),"Recovery preceding support is not accepted static geometry.");
            if(n>index)need(!q.supports[n],"Recovery support query continued after refusal.");
        }
        if(failure==RuntimePhysicalRecoveryRefusal::MissingSupport)need(!q.supports[index],"Missing recovery support retains a hit.");
        else {
            need(q.supports[index].has_value(),"Recovery failed hit witness is absent.");
            need(q.supports[index]->static_body==(failure==RuntimePhysicalRecoveryRefusal::InvalidSupport),"Recovery support motion type contradicts its refusal.");
        }
    } else {
        need(!q.support_index,"Recovery clearance/budget result retains a support refusal index.");
        for(const auto& s:q.supports)if(s)need(accepted_support(*s),"Recovery completed support is not accepted static geometry.");
    }
    const bool blocked=failure==RuntimePhysicalRecoveryRefusal::ProxyBlocked||failure==RuntimePhysicalRecoveryRefusal::MotorBlocked;
    need(q.blocker.has_value()==blocked,"Recovery blocker membership contradicts its result.");
    if(blocked||q.clear())need(q.supports[0]&&q.supports[1],"Recovery capsule query lacks two completed static supports.");
    if(q.blocker) {
        const auto& b=*q.blocker;const auto& n=b.normal;const double length=std::hypot(n[0],n[1],n[2]);
        need(b.normal_valid?std::abs(length-1)<=1e-5:n==std::array<double,3>{},"Recovery blocker normal validity contradicts its payload.");
        if(failure==RuntimePhysicalRecoveryRefusal::ProxyBlocked)
            need(b.penetration>0&&q.proxy_hits>0&&q.motor_hits==0,"Recovery proxy blocker has no penetrating proxy contact.");
        else need(b.penetration>=-double(runtime_physical_recovery_padding)&&q.motor_hits>0,"Recovery motor blocker is outside its queried padding.");
    }
    const auto completed=static_cast<std::uint32_t>(q.supports[0].has_value())+static_cast<std::uint32_t>(q.supports[1].has_value());
    need(q.support_hits>=completed&&q.body_visits>=completed,"Recovery query counts cannot account for its retained supports.");
    if(q.proxy_hits||q.motor_hits)need(completed==2,"Recovery capsule contacts precede complete support checks.");
    if(failure!=RuntimePhysicalRecoveryRefusal::BudgetExceeded)
        need(q.body_visits<=4096&&q.support_hits+q.proxy_hits+q.motor_hits<=16384,"Non-budget recovery result exceeds maximum query work.");
    else need(q.body_visits>=2||q.support_hits+q.proxy_hits+q.motor_hits>=2,"Recovery budget exhausted before the minimum possible budget.");
    return q;
}
RuntimePhysicalRecoveryStatus status(const Json& j) {
    return static_cast<RuntimePhysicalRecoveryStatus>(integer(j,static_cast<std::uint32_t>(RuntimePhysicalRecoveryStatus::SoleOutOfRange)));
}
Json assessment_json(const RuntimePhysicalRecoveryAssessment& a) {
    return {{"tick",a.tick},{"structure_revision",a.structure_revision},{"status",static_cast<std::uint32_t>(a.status)},{"actor",transform_json(a.actor)},
        {"maximum_linear_speed",a.maximum_linear_speed},{"maximum_angular_speed",a.maximum_angular_speed},
        {"minimum_upright_dot",a.minimum_upright_dot},{"wrapper_distance",a.wrapper_distance},{"wrapper_angle",a.wrapper_angle},
        {"clearance",a.clearance?query_json(*a.clearance):Json(nullptr)}};
}
RuntimePhysicalRecoveryAssessment assessment(const Json& j) {
    fields(j,{"tick","structure_revision","status","actor","maximum_linear_speed","maximum_angular_speed","minimum_upright_dot","wrapper_distance","wrapper_angle","clearance"});
    RuntimePhysicalRecoveryAssessment a;a.tick=integer(j.at("tick"));a.structure_revision=integer(j.at("structure_revision"));a.status=status(j.at("status"));a.actor=transform(j.at("actor"));
    a.maximum_linear_speed=number(j.at("maximum_linear_speed"),0,2e6);a.maximum_angular_speed=number(j.at("maximum_angular_speed"),0,2e4);
    a.minimum_upright_dot=number(j.at("minimum_upright_dot"),-1.000001,1.000001);
    a.wrapper_distance=number(j.at("wrapper_distance"),0,2e6);a.wrapper_angle=number(j.at("wrapper_angle"),0,3.141592653589794);
    if(!j.at("clearance").is_null())a.clearance=query(j.at("clearance"));
    if(a.status==RuntimePhysicalRecoveryStatus::Ready||a.status==RuntimePhysicalRecoveryStatus::Stabilizing)
        need(a.clearance&&a.clearance->clear(),"Ready/stabilizing recovery must have complete clear current geometry.");
    else if(a.status>=RuntimePhysicalRecoveryStatus::MissingSupport&&a.status<=RuntimePhysicalRecoveryStatus::InvalidNativeResult) {
        const auto expected=static_cast<std::uint32_t>(a.status)-static_cast<std::uint32_t>(RuntimePhysicalRecoveryStatus::MissingSupport)+
            static_cast<std::uint32_t>(RuntimePhysicalRecoveryRefusal::MissingSupport);
        need(a.clearance&&static_cast<std::uint32_t>(a.clearance->refusal)==expected,"Recovery assessment status contradicts its actual query refusal.");
    } else need(!a.clearance,"Recovery pre-query/locomotion assessment contains unrelated geometry.");
    return a;
}
Json transfer_json(const RuntimePhysicalRecoveryTransfer& t) {
    return {{"tick",t.tick},{"generation",t.generation},{"motor_velocity",t.motor_velocity},
        {"outgoing",physical_state_json::handoff_state_json(t.outgoing)},{"admission",assessment_json(t.admission)}};
}
RuntimePhysicalRecoveryTransfer transfer(const Json& j) {
    fields(j,{"tick","generation","motor_velocity","outgoing","admission"});RuntimePhysicalRecoveryTransfer t;
    t.tick=integer(j.at("tick"));t.generation=integer(j.at("generation"));t.motor_velocity=vector<3>(j.at("motor_velocity"),-2,2);
    need(std::hypot(t.motor_velocity[0],t.motor_velocity[1],t.motor_velocity[2])<=2,"Recovery transfer motor velocity exceeds its vector speed bound.");
    t.outgoing=physical_state_json::parse_handoff_state(j.at("outgoing"));t.admission=assessment(j.at("admission"));
    // The old handoff reader also accepts numeric integer spellings. This new
    // envelope admits only its exact emitted representation; do not change the
    // old format or quietly coerce a historical witness inside this one.
    need(physical_state_json::handoff_state_json(t.outgoing).dump()==j.at("outgoing").dump(),
        "Recovery outgoing handoff must retain its canonical typed representation.");
    need(t.generation>0&&t.generation<=t.tick&&t.tick>0&&t.outgoing.passive&&t.outgoing.passive->takeover_tick<t.tick&&
        t.admission.tick+1==t.tick&&t.admission.status==RuntimePhysicalRecoveryStatus::Ready,"Recovery transfer chronology/ownership is invalid.");return t;
}
}
const char* physical_recovery_status_name(RuntimePhysicalRecoveryStatus status) {
    static constexpr const char* names[]={"locomotion","history_unavailable","not_standing","not_upright","moving","wrapper_displaced",
        "missing_support","moving_support","invalid_support","proxy_blocked","motor_blocked","query_budget","invalid_native_result","stabilizing","ready","returned","sole_out_of_range"};
    const auto value=static_cast<std::uint32_t>(status);if(value>=std::size(names))throw std::invalid_argument("Unknown physical recovery status.");return names[value];
}
nlohmann::json physical_recovery_state_json(const RuntimePhysicalRecoveryState& s) {
    Json j={{"version",std::uint32_t(1)},{"previous",s.previous?frame_json(*s.previous):Json(nullptr)},{"current",s.current?frame_json(*s.current):Json(nullptr)},
        {"assessment",s.assessment?assessment_json(*s.assessment):Json(nullptr)},{"stable_ticks",s.stable_ticks},{"generation",s.generation},
        {"last_attempt_tick",s.last_attempt_tick?Json(*s.last_attempt_tick):Json(nullptr)},
        {"last_attempt_result",static_cast<std::uint32_t>(s.last_attempt_result)},{"transfer",s.transfer?transfer_json(*s.transfer):Json(nullptr)}};
    (void)parse_physical_recovery_state(j);return j;
}
RuntimePhysicalRecoveryState parse_physical_recovery_state(const nlohmann::json& j) {
    fields(j,{"version","previous","current","assessment","stable_ticks","generation","last_attempt_tick","last_attempt_result","transfer"});
    need(integer(j.at("version"),1)==1,"Unsupported physical recovery state version.");RuntimePhysicalRecoveryState s;
    if(!j.at("previous").is_null())s.previous=frame(j.at("previous"));if(!j.at("current").is_null())s.current=frame(j.at("current"));
    if(!j.at("assessment").is_null())s.assessment=assessment(j.at("assessment"));
    s.stable_ticks=static_cast<std::uint32_t>(integer(j.at("stable_ticks"),60));s.generation=integer(j.at("generation"));
    if(!j.at("last_attempt_tick").is_null())s.last_attempt_tick=integer(j.at("last_attempt_tick"));
    s.last_attempt_result=status(j.at("last_attempt_result"));if(!j.at("transfer").is_null())s.transfer=transfer(j.at("transfer"));
    need(!s.previous||(s.current&&s.previous->tick+1==s.current->tick),"Recovery full-pose history must be consecutive.");
    if(s.previous){need(s.previous->nodes.size()==s.current->nodes.size(),"Recovery history node population differs.");
        for(std::size_t i=0;i<s.current->nodes.size();++i)need(s.previous->nodes[i].entity==s.current->nodes[i].entity,"Recovery history node order differs.");}
    need(!s.assessment||(s.current&&s.assessment->tick==s.current->tick),"Recovery assessment is not from the current full-pose frame.");
    if(s.assessment) {
        const auto& left=s.assessment->actor;const auto& right=s.current->actor;
        need(detail::handoff_array_same_bits(left.position,right.position)&&detail::handoff_array_same_bits(left.rotation,right.rotation)&&
            detail::handoff_array_same_bits(left.scale,right.scale),"Recovery assessment actor differs from the current complete frame.");
    }
    need(!s.last_attempt_tick||(s.current&&*s.last_attempt_tick>0&&*s.last_attempt_tick<=s.current->tick),"Recovery attempt is zero or in the future.");
    need(s.last_attempt_tick||s.last_attempt_result==RuntimePhysicalRecoveryStatus::HistoryUnavailable,"Recovery attempt result has no attempt.");
    need((s.generation==0)==!s.transfer,"Recovery generation and transfer membership differ.");
    if(s.transfer)need(s.current&&s.transfer->tick<=s.current->tick&&s.transfer->generation==s.generation&&
        s.last_attempt_tick&&*s.last_attempt_tick>=s.transfer->tick&&
        (*s.last_attempt_tick!=s.transfer->tick||s.last_attempt_result==RuntimePhysicalRecoveryStatus::Returned),"Recovery transfer differs from its generation or successful attempt.");
    if(s.last_attempt_result==RuntimePhysicalRecoveryStatus::Returned)
        need(s.transfer&&s.last_attempt_tick&&*s.last_attempt_tick==s.transfer->tick,"Recovery returned result lacks its exact historical transfer.");
    const bool eligible=s.assessment&&(s.assessment->status==RuntimePhysicalRecoveryStatus::Ready||s.assessment->status==RuntimePhysicalRecoveryStatus::Stabilizing);
    need((s.stable_ticks>0)==eligible,"Recovery stability count and eligible geometry differ.");
    if(eligible)need(s.previous.has_value(),"Recovery eligibility lacks complete previous pose history.");
    return s;
}
} // namespace poima
