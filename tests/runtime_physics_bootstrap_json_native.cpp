// SPDX-License-Identifier: Apache-2.0
#include "../src/runtime_physics_bootstrap_json.hpp"
#include <Jolt/Physics/Body/BodyManager.h>
#include <Jolt/Core/Memory.h>
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace poima;
namespace codec=physical_bootstrap_json;
using Json=nlohmann::json;
unsigned checks=0;
void need(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F&& operation){bool failed=false;try{operation();}catch(const std::exception&){failed=true;}need(failed,"Malformed physics bootstrap/owner state was accepted.");}
Json u(std::uint64_t value){return Json(value);}
JPH::BodyID id(std::uint32_t slot,std::uint32_t generation){need(slot<RuntimeBodyIds::capacity&&generation<=255,"Test ID must already be syntactically valid.");return JPH::BodyID(slot,static_cast<JPH::uint8>(generation));}
bool equal(const RuntimePhysicsBootstrap& a,const RuntimePhysicsBootstrap& b) {
    if(!(a.ledger==b.ledger)||a.bodies!=b.bodies||a.next_collision_group!=b.next_collision_group||a.rigs.size()!=b.rigs.size())return false;
    for(const auto& [entity,rig]:a.rigs){const auto found=b.rigs.find(entity);if(found==b.rigs.end()||rig.collision_group!=found->second.collision_group||rig.parts!=found->second.parts)return false;}return true;
}
bool equal(const JPH::PoimaBodyOwnerState& a,const JPH::PoimaBodyOwnerState& b){return a.mActiveRigidOrder==b.mActiveRigidOrder&&a.mInvalidatedBodies==b.mInvalidatedBodies&&a.mInvalidationQueue==b.mInvalidationQueue;}
struct Fixture {
    RuntimeDefinition definition;
    RuntimePhysicsBootstrap bootstrap;
    JPH::PoimaBodyOwnerState owner;
    Fixture() {
        // Typed frozen-definition membership only. Actual shape/animation/world
        // validity and native allocation are intentionally outside this test.
        RuntimeEntityDefinition actor;actor.id="actor";actor.character=CharacterController{};definition.entities.push_back(actor);
        RuntimeEntityDefinition wall;wall.id="wall";wall.collider=BoxCollider{};wall.collider->motion=BodyMotion::Dynamic;definition.entities.push_back(wall);
        RuntimeEntityDefinition mesh;mesh.id="mesh";mesh.mesh_collider=MeshCollider{};definition.entities.push_back(mesh);
        for(const std::string rig_id:{"rig-a","rig-b"}) {
            RuntimeEntityDefinition rig;rig.id=rig_id;rig.physical_rig.emplace();auto& p=rig.physical_rig->profile;p.root="pelvis";
            physical_rig::Part pelvis;pelvis.name="pelvis";pelvis.node=0;auto tip=pelvis;tip.name="tip";tip.node=1;p.parts={pelvis,tip};physical_rig::Joint joint;joint.name="joint";joint.parent="pelvis";joint.child="tip";p.joints={joint};definition.entities.push_back(rig);
        }
        RuntimeBodyIds::Snapshot ledger;ledger.sealed=true;ledger.rows={{0,0,true},{1,3,true},{2,4,false},{3,255,false},{4,7,true},{5,2,true},{6,0,false},{7,1,true},{8,1,true},{9,2,true}};
        bootstrap.ledger=RuntimeBodyIds::from_snapshot(ledger);bootstrap.bodies={{"actor",id(0,0)},{"mesh",id(7,1)},{"wall",id(1,3)}};
        bootstrap.rigs={{"rig-a",{{id(4,7),id(5,2)},7}},{"rig-b",{{id(8,1),id(9,2)},11}}};bootstrap.next_collision_group=13;
        owner.mActiveRigidOrder={id(9,2),id(1,3),id(4,7)};owner.mInvalidatedBodies={id(0,0),id(4,7)};
        owner.mInvalidationQueue={id(2,3),id(4,7),id(0,0),id(2,3),id(3,255),id(6,0)};
    }
};
template<class Change>void bad_bootstrap(Change change) {
    const Fixture fixture;auto wire=codec::bootstrap_json(fixture.bootstrap,fixture.definition);change(wire);const auto before=wire.dump();auto destination=fixture.bootstrap;
    rejects([&]{destination=codec::parse_bootstrap(wire,fixture.definition);});need(equal(destination,fixture.bootstrap)&&wire.dump()==before,"Rejected bootstrap parse partially changed output or const input.");
}
template<class Change>void bad_owner(Change change) {
    const Fixture fixture;auto wire=codec::owner_json(fixture.owner);change(wire);const auto before=wire.dump();auto destination=fixture.owner;
    rejects([&]{destination=codec::parse_owner(wire);});need(equal(destination,fixture.owner)&&wire.dump()==before,"Rejected owner syntax partially changed output or const input.");
}
void full_roundtrip() {
    Fixture f;const auto bootstrap=codec::bootstrap_json(f.bootstrap,f.definition);need(bootstrap.size()==6&&bootstrap["ledger"].size()==2,"Bootstrap has incomplete literal fields.");
    const auto decoded=codec::parse_bootstrap(Json::parse(bootstrap.dump()),f.definition);need(equal(decoded,f.bootstrap),"Complete bootstrap/ledger/part-ID/group roundtrip differs.");
    need(decoded.ledger.snapshot()==f.bootstrap.ledger.snapshot()&&decoded.ledger.live()==7&&decoded.ledger.exhausted()==1,"Retired/occupied/gen0/gen255 ledger rows or derived counters changed.");
    const auto owner=codec::parse_owner(Json::parse(codec::owner_json(f.owner).dump()));need(equal(owner,f.owner),"Owner order/flags/retired duplicate queue changed.");codec::validate_owner_history(owner,decoded.ledger);
    need(owner.mActiveRigidOrder[0]==id(9,2)&&owner.mInvalidationQueue[0]==owner.mInvalidationQueue[3],"Codec sorted active order or dropped retired duplicate IDs.");
    RuntimePhysicsBootstrap empty;empty.ledger.seal();RuntimeDefinition none;need(equal(codec::parse_bootstrap(codec::bootstrap_json(empty,none),none),empty),"Empty sealed world acquired phantom bindings.");
    JPH::PoimaBodyOwnerState no_owner;codec::validate_owner_history(codec::parse_owner(codec::owner_json(no_owner)),empty.ledger);
    auto queue=f.owner;const auto retired=id(2,3);while(queue.mInvalidationQueue.size()<JPH::PoimaBodyOwnerState::cMaxInvalidationQueue)queue.mInvalidationQueue.push_back(retired);const auto maximum=codec::parse_owner(codec::owner_json(queue));need(equal(queue,maximum),"Maximum bounded queue lost retired duplicate tokens.");codec::validate_owner_history(maximum,f.bootstrap.ledger);
}
void malformed_scalars_and_shapes() {
    bad_bootstrap([](auto& j){j["format"]="poima.physics-bootstrap.future";});bad_bootstrap([](auto& j){j["version"]=u(2);});bad_bootstrap([](auto& j){j["version"]=1;});bad_bootstrap([](auto& j){j["version"]=1.0;});
    bad_bootstrap([](auto& j){j.erase("bodies");});bad_bootstrap([](auto& j){j["counter"]=u(0);});bad_bootstrap([](auto& j){j["ledger"]["sealed"]=u(1);});bad_bootstrap([](auto& j){j["ledger"]["sealed"]=false;});
    bad_bootstrap([](auto& j){j["ledger"]["live"]=u(7);});bad_bootstrap([](auto& j){j["ledger"]["rows"][0]["generation"]=u(256);});bad_bootstrap([](auto& j){j["ledger"]["rows"][0]["generation"]=u(std::numeric_limits<std::uint64_t>::max());});
    bad_bootstrap([](auto& j){j["ledger"]["rows"][0]["index"]=u(4096);});bad_bootstrap([](auto& j){j["ledger"]["rows"][0]["occupied"]=u(1);});bad_bootstrap([](auto& j){j["ledger"]["rows"][1]["index"]=u(0);});
    bad_bootstrap([](auto& j){std::swap(j["ledger"]["rows"][0],j["ledger"]["rows"][1]);});
    for(const auto bad:{std::uint64_t(JPH::BodyID::cInvalidBodyID),std::uint64_t(JPH::BodyID::cBroadPhaseBit|2u),std::uint64_t(4096),std::uint64_t(1)<<32})bad_bootstrap([&](auto& j){j["bodies"][0]["id"]=u(bad);});
    bad_bootstrap([](auto& j){j["bodies"][0]["id"]=0;});bad_bootstrap([](auto& j){j["bodies"][0]["id"]=0.0;});bad_bootstrap([](auto& j){j["bodies"][0]["id"]="0";});bad_bootstrap([](auto& j){j["bodies"][0]["id"]=false;});
    bad_bootstrap([](auto& j){j["bodies"][0]["entity"]=std::string("actor\0bad",9);});bad_bootstrap([](auto& j){j["bodies"][0]["entity"]=std::string(257,'x');});
    bad_owner([](auto& j){j["version"]=u(0);});bad_owner([](auto& j){j["format"]=true;});bad_owner([](auto& j){j.erase("invalidation_queue");});bad_owner([](auto& j){j["opaque"]=Json::array();});
    bad_owner([](auto& j){j["active_rigid_order"]=Json::object();});bad_owner([](auto& j){j["active_rigid_order"][0]=u(JPH::BodyID::cBroadPhaseBit|1u);});bad_owner([](auto& j){j["active_rigid_order"][0]=u(4096);});
    bad_owner([](auto& j){j["active_rigid_order"].push_back(j["active_rigid_order"][0]);});bad_owner([](auto& j){j["invalidated_bodies"].push_back(j["invalidated_bodies"][0]);});
    bad_owner([](auto& j){std::swap(j["invalidated_bodies"][0],j["invalidated_bodies"][1]);});bad_owner([](auto& j){j["invalidation_queue"][0]=-1;});
    bad_owner([](auto& j){j["active_rigid_order"]=Json::array();for(unsigned n=0;n<4097;++n)j["active_rigid_order"].push_back(u(0));});
    bad_owner([](auto& j){j["invalidation_queue"]=Json::array();for(std::size_t n=0;n<=JPH::PoimaBodyOwnerState::cMaxInvalidationQueue;++n)j["invalidation_queue"].push_back(u(0));});
}
void exact_definition_membership() {
    bad_bootstrap([](auto& j){j["bodies"].erase(j["bodies"].begin());});bad_bootstrap([](auto& j){j["bodies"][0]["entity"]="aaa-unknown";});
    bad_bootstrap([](auto& j){std::swap(j["bodies"][0],j["bodies"][1]);});bad_bootstrap([](auto& j){j["bodies"][1]["id"]=j["bodies"][0]["id"];});
    bad_bootstrap([](auto& j){j["rigs"].erase(j["rigs"].begin());});bad_bootstrap([](auto& j){std::swap(j["rigs"][0],j["rigs"][1]);});bad_bootstrap([](auto& j){j["rigs"][0]["entity"]="unknown-rig";});
    bad_bootstrap([](auto& j){j["rigs"][0]["parts"].erase(j["rigs"][0]["parts"].begin());});bad_bootstrap([](auto& j){std::swap(j["rigs"][0]["parts"][0],j["rigs"][0]["parts"][1]);});
    bad_bootstrap([](auto& j){j["rigs"][0]["parts"][1]["part"]="pelvis";});bad_bootstrap([](auto& j){j["rigs"][0]["parts"][0]["id"]=j["bodies"][0]["id"];});
    bad_bootstrap([](auto& j){j["rigs"][0]["parts"][0]["id"]=u(id(4,8).GetIndexAndSequenceNumber());}); // future occupied generation
    bad_bootstrap([](auto& j){j["bodies"][0]["id"]=u(id(2,4).GetIndexAndSequenceNumber());}); // used but retired
    bad_bootstrap([](auto& j){j["ledger"]["rows"].push_back({{"index",u(10)},{"generation",u(1)},{"occupied",true}});}); // occupied orphan
    bad_bootstrap([](auto& j){j["rigs"][0]["collision_group"]=u(0);});bad_bootstrap([](auto& j){j["rigs"][1]["collision_group"]=j["rigs"][0]["collision_group"];});
    bad_bootstrap([](auto& j){j["next_collision_group"]=j["rigs"][1]["collision_group"];});bad_bootstrap([](auto& j){j["next_collision_group"]=u(0);});bad_bootstrap([](auto& j){j["rigs"][0]["collision_group"]=u(std::numeric_limits<std::uint32_t>::max());});
    Fixture f;const auto old=f.bootstrap;auto changed=f.bootstrap;changed.bodies.erase("actor");rejects([&]{(void)codec::bootstrap_json(changed,f.definition);});need(equal(f.bootstrap,old),"Rejected writer changed the source bootstrap.");
    auto changed_definition=f.definition;changed_definition.entities.push_back(changed_definition.entities.front());rejects([&]{(void)codec::parse_bootstrap(codec::bootstrap_json(f.bootstrap,f.definition),changed_definition);});
    changed_definition=f.definition;std::swap(changed_definition.entities[3].physical_rig->profile.parts[0],changed_definition.entities[3].physical_rig->profile.parts[1]);rejects([&]{(void)codec::parse_bootstrap(codec::bootstrap_json(f.bootstrap,f.definition),changed_definition);});
}
void future_and_live_queue_history() {
    const Fixture f;const auto reject_history=[&](JPH::PoimaBodyOwnerState changed){const auto wire=codec::owner_json(changed);const auto decoded=codec::parse_owner(Json::parse(wire.dump()));need(equal(decoded,changed),"Syntactic owner decoder dropped or relabeled invalid history tokens.");rejects([&]{codec::validate_owner_history(decoded,f.bootstrap.ledger);});need(equal(decoded,changed),"History rejection partially changed owner metadata.");};
    for(auto future:{id(2,5),id(4,8),id(99,1)}){auto changed=f.owner;changed.mInvalidationQueue.push_back(future);reject_history(changed);}
    auto duplicate_live=f.owner;duplicate_live.mInvalidationQueue.push_back(id(4,7));reject_history(duplicate_live);
    auto missing_flag=f.owner;missing_flag.mInvalidatedBodies.pop_back();reject_history(missing_flag);
    auto missing_queue=f.owner;auto& queue=missing_queue.mInvalidationQueue;queue.erase(std::remove(queue.begin(),queue.end(),id(4,7)),queue.end());reject_history(missing_queue);
    auto retired_flag=f.owner;retired_flag.mInvalidatedBodies={id(0,0),id(2,4),id(4,7)};reject_history(retired_flag);
    auto retired_active=f.owner;retired_active.mActiveRigidOrder.push_back(id(2,4));reject_history(retired_active);
    auto allowed=f.owner;allowed.mInvalidationQueue.push_back(id(4,6));allowed.mInvalidationQueue.push_back(id(4,6));codec::validate_owner_history(allowed,f.bootstrap.ledger);need(equal(codec::parse_owner(codec::owner_json(allowed)),allowed),"Retired prior generation of a currently occupied slot was normalized or rejected.");
    RuntimeBodyIds unsealed;rejects([&]{codec::validate_owner_history(f.owner,unsealed);});
}
}
int main(){JPH::RegisterDefaultAllocator();try{full_roundtrip();malformed_scalars_and_shapes();exact_definition_membership();future_and_live_queue_history();std::cout<<"Physics bootstrap/owner typed JSON: "<<checks<<" checks in four groups; no native world/cold-continuation claim.\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
