// SPDX-License-Identifier: Apache-2.0
#include "runtime_physics_bootstrap_json.hpp"
#include <Jolt/Physics/Body/BodyManager.h>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace poima::physical_bootstrap_json {
namespace {
using Json=nlohmann::json;
constexpr auto capacity=RuntimeBodyIds::capacity;
constexpr auto u32_max=std::numeric_limits<std::uint32_t>::max();
void need(bool value,const char* message){if(!value)throw std::invalid_argument(message);}
void fields(const Json& value,std::initializer_list<const char*> keys){need(value.is_object()&&value.size()==keys.size(),"Physics bootstrap/owner object fields differ.");for(const auto* key:keys)need(value.contains(key),"Required physics bootstrap/owner field is missing.");}
void array(const Json& value,std::size_t maximum){need(value.is_array()&&value.size()<=maximum,"Physics bootstrap/owner array exceeds its bound.");}
std::uint32_t integer(const Json& value,std::uint32_t maximum=u32_max){need(value.is_number_unsigned(),"Physics bootstrap/owner integers must be canonical unsigned JSON integers.");const auto wide=value.get<std::uint64_t>();need(wide<=maximum,"Physics bootstrap/owner unsigned integer exceeds its bound.");return static_cast<std::uint32_t>(wide);}
bool boolean(const Json& value){need(value.is_boolean(),"Physics bootstrap/owner flag must be Boolean.");return value.get<bool>();}
void name_valid(std::string_view value,bool part=false) {
    need(!value.empty()&&value.size()<=(part?64u:256u)&&value.find('\0')==std::string_view::npos,"Physics binding name length/NUL differs.");if(!part)return;
    const auto alnum=[](unsigned char c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');};need(alnum(static_cast<unsigned char>(value.front())),"Physics part name must begin with an ASCII alphanumeric.");
    for(unsigned char c:value)need(alnum(c)||c=='_'||c=='.'||c=='-',"Physics part name has an unsupported character.");
}
std::string name(const Json& value,bool part=false){need(value.is_string(),"Physics binding names must be strings.");const auto& result=value.get_ref<const std::string&>();name_valid(result,part);return result;}
void envelope(const Json& value,std::string_view expected){need(value.at("format").is_string()&&value.at("format").get_ref<const std::string&>()==expected&&integer(value.at("version"),1)==1,"Physics bootstrap/owner format/version differs.");}
void raw_valid(std::uint32_t raw){need(raw!=JPH::BodyID::cInvalidBodyID&&(raw&JPH::BodyID::cBroadPhaseBit)==0&&(raw&JPH::BodyID::cMaxBodyIndex)<capacity,"Physics body ID has invalid bits/slot.");}
JPH::BodyID body_id(const Json& value){const auto raw=integer(value);raw_valid(raw);return JPH::BodyID(raw);}
std::uint32_t raw(JPH::BodyID id){const auto value=id.GetIndexAndSequenceNumber();raw_valid(value);return value;}
void sorted(std::string_view previous,std::string_view current,bool first){need(first||previous<current,"Physics entity rows must be strictly sorted and unique.");}
struct Bindings {std::set<std::string> bodies;std::map<std::string,std::vector<std::string>> rigs;};
Bindings bindings(const RuntimeDefinition& definition) {
    need(definition.entities.size()<=10000,"Physics bootstrap definition exceeds entity capacity.");Bindings result;std::set<std::string> entities;std::size_t total_bindings=0;
    for(const auto& entity:definition.entities) {
        name_valid(entity.id);need(entities.insert(entity.id).second,"Physics bootstrap definition has duplicate entity identities.");
        if(entity.collider||entity.mesh_collider||entity.character){need(++total_bindings<=capacity,"Frozen physics definition exceeds body-slot capacity.");result.bodies.insert(entity.id);}
        if(entity.physical_rig) {
            const auto& parts=entity.physical_rig->profile.parts;need(!parts.empty()&&parts.size()<=64,"Frozen physical profile part population differs.");total_bindings+=parts.size();need(total_bindings<=capacity,"Frozen physics definition exceeds body-slot capacity.");std::vector<std::string> names;std::set<std::string> seen;
            for(const auto& part:parts){name_valid(part.name,true);need(seen.insert(part.name).second,"Frozen physical profile has duplicate part names.");names.push_back(part.name);}result.rigs.emplace(entity.id,std::move(names));
        }
    }
    return result;
}
void validate(const RuntimePhysicsBootstrap& value,const Bindings& expected) {
    const auto snapshot=value.ledger.snapshot();need(snapshot.sealed,"Cold physics bootstrap requires a sealed complete body ledger.");
    need(value.bodies.size()==expected.bodies.size()&&value.rigs.size()==expected.rigs.size()&&value.bodies.size()<=capacity&&value.rigs.size()<=capacity,"Physics bootstrap entity membership differs from the frozen definition.");
    std::array<bool,capacity> used{};std::set<std::uint32_t> full_ids,groups;std::size_t count=0;std::uint32_t largest_group=0;
    const auto bind=[&](JPH::BodyID id){const auto full=raw(id);need(value.ledger.owns(id)&&!used[id.GetIndex()]&&full_ids.insert(full).second,"Physics binding ID is unowned, stale or uses an already-bound slot.");used[id.GetIndex()]=true;need(++count<=capacity,"Physics bindings exceed body capacity.");};
    for(const auto& [entity,id]:value.bodies){name_valid(entity);need(expected.bodies.contains(entity),"Unexpected ordinary physics body binding.");bind(id);}
    for(const auto& [entity,rig]:value.rigs) {
        name_valid(entity);const auto found=expected.rigs.find(entity);need(found!=expected.rigs.end()&&rig.parts.size()==found->second.size(),"Physical rig binding/part population differs from its frozen profile.");
        need(rig.collision_group!=0&&groups.insert(rig.collision_group).second,"Physical collision groups must be nonzero and unique.");largest_group=std::max(largest_group,rig.collision_group);for(auto id:rig.parts)bind(id);
    }
    need(value.next_collision_group!=0&&value.next_collision_group>largest_group,"Next physical collision group must exceed every retained group.");
    need(count==value.ledger.live(),"Occupied body ledger must exactly equal all actual ordinary/rig-part bindings.");
}
void owner_shape(const JPH::PoimaBodyOwnerState& value) {
    need(value.mActiveRigidOrder.size()<=capacity&&value.mInvalidatedBodies.size()<=capacity&&value.mInvalidationQueue.size()<=JPH::PoimaBodyOwnerState::cMaxInvalidationQueue,"Physics owner array population exceeds its bound.");
    std::array<bool,capacity> active{};for(auto id:value.mActiveRigidOrder){(void)raw(id);need(!active[id.GetIndex()],"Physics owner active order repeats a slot/full ID.");active[id.GetIndex()]=true;}
    bool first=true;std::uint32_t previous=0;for(auto id:value.mInvalidatedBodies){(void)raw(id);need(first||id.GetIndex()>previous,"Physics owner invalidated IDs must be strictly sorted by slot.");first=false;previous=id.GetIndex();}
    for(auto id:value.mInvalidationQueue)(void)raw(id);
}
Json id_array(const JPH::BodyIDVector& value){Json result=Json::array();for(auto id:value)result.push_back(raw(id));return result;}
JPH::BodyIDVector id_array(const Json& value,std::size_t maximum){array(value,maximum);JPH::BodyIDVector result;result.reserve(value.size());for(const auto& id:value)result.push_back(body_id(id));return result;}
} // namespace

nlohmann::json bootstrap_json(const RuntimePhysicsBootstrap& value,const RuntimeDefinition& definition) {
    const auto expected=bindings(definition);validate(value,expected);const auto snapshot=value.ledger.snapshot();Json rows=Json::array(),bodies=Json::array(),rigs=Json::array();
    for(const auto& row:snapshot.rows)rows.push_back({{"index",row.index},{"generation",row.generation},{"occupied",row.occupied}});
    for(const auto& [entity,id]:value.bodies)bodies.push_back({{"entity",entity},{"id",raw(id)}});
    for(const auto& [entity,rig]:value.rigs){Json parts=Json::array();const auto& names=expected.rigs.at(entity);for(std::size_t n=0;n<rig.parts.size();++n)parts.push_back({{"part",names[n]},{"id",raw(rig.parts[n])}});rigs.push_back({{"entity",entity},{"parts",std::move(parts)},{"collision_group",rig.collision_group}});}
    return {{"format","poima.physics-bootstrap"},{"version",std::uint32_t{1}},{"ledger",{{"sealed",snapshot.sealed},{"rows",std::move(rows)}}},{"bodies",std::move(bodies)},{"rigs",std::move(rigs)},{"next_collision_group",value.next_collision_group}};
}
RuntimePhysicsBootstrap parse_bootstrap(const nlohmann::json& value,const RuntimeDefinition& definition) {
    fields(value,{"format","version","ledger","bodies","rigs","next_collision_group"});envelope(value,"poima.physics-bootstrap");const auto expected=bindings(definition);RuntimePhysicsBootstrap result;
    const auto& ledger=value.at("ledger");fields(ledger,{"sealed","rows"});RuntimeBodyIds::Snapshot snapshot;snapshot.sealed=boolean(ledger.at("sealed"));need(snapshot.sealed,"Cold body ledger must be sealed.");const auto& rows=ledger.at("rows");array(rows,capacity);snapshot.rows.reserve(rows.size());
    for(const auto& row:rows){fields(row,{"index","generation","occupied"});snapshot.rows.push_back({integer(row.at("index"),capacity-1),integer(row.at("generation"),JPH::BodyID::cMaxSequenceNumber),boolean(row.at("occupied"))});}result.ledger=RuntimeBodyIds::from_snapshot(snapshot);
    const auto& bodies=value.at("bodies");array(bodies,capacity);bool first=true;std::string previous;
    for(const auto& row:bodies){fields(row,{"entity","id"});const auto entity=name(row.at("entity"));sorted(previous,entity,first);first=false;previous=entity;result.bodies.emplace(entity,body_id(row.at("id")));}
    const auto& rigs=value.at("rigs");array(rigs,capacity);first=true;previous.clear();
    for(const auto& row:rigs) {
        fields(row,{"entity","parts","collision_group"});const auto entity=name(row.at("entity"));sorted(previous,entity,first);first=false;previous=entity;
        const auto found=expected.rigs.find(entity);need(found!=expected.rigs.end(),"Unknown physical rig identity in bootstrap.");const auto& parts=row.at("parts");array(parts,64);need(parts.size()==found->second.size(),"Physical bootstrap must preserve every configured part in profile order.");RuntimePhysicalRigIdentity rig;
        for(std::size_t n=0;n<parts.size();++n){fields(parts[n],{"part","id"});need(name(parts[n].at("part"),true)==found->second[n],"Physical bootstrap named part order differs from the frozen profile.");rig.parts.push_back(body_id(parts[n].at("id")));}rig.collision_group=integer(row.at("collision_group"));result.rigs.emplace(entity,std::move(rig));
    }
    result.next_collision_group=integer(value.at("next_collision_group"));validate(result,expected);return result;
}
nlohmann::json owner_json(const JPH::PoimaBodyOwnerState& value) {
    owner_shape(value);return {{"format","poima.physics-owner-state"},{"version",std::uint32_t{1}},{"active_rigid_order",id_array(value.mActiveRigidOrder)},{"invalidation_queue",id_array(value.mInvalidationQueue)},{"invalidated_bodies",id_array(value.mInvalidatedBodies)}};
}
JPH::PoimaBodyOwnerState parse_owner(const nlohmann::json& value) {
    fields(value,{"format","version","active_rigid_order","invalidation_queue","invalidated_bodies"});envelope(value,"poima.physics-owner-state");JPH::PoimaBodyOwnerState result;
    result.mActiveRigidOrder=id_array(value.at("active_rigid_order"),capacity);result.mInvalidatedBodies=id_array(value.at("invalidated_bodies"),capacity);result.mInvalidationQueue=id_array(value.at("invalidation_queue"),JPH::PoimaBodyOwnerState::cMaxInvalidationQueue);owner_shape(result);return result;
}
void validate_owner_history(const JPH::PoimaBodyOwnerState& value,const RuntimeBodyIds& ledger) {
    owner_shape(value);const auto snapshot=ledger.snapshot();need(snapshot.sealed,"Physics owner history requires the full sealed ledger.");std::array<const RuntimeBodyIds::SnapshotRow*,capacity> used{};for(const auto& row:snapshot.rows)used[row.index]=&row;
    std::array<bool,capacity> flagged{},queued_live{};for(auto id:value.mActiveRigidOrder)need(ledger.owns(id),"Physics owner active ID is not owned by the complete ledger.");
    for(auto id:value.mInvalidatedBodies){need(ledger.owns(id),"Physics invalidated flag ID is not owned by the ledger.");flagged[id.GetIndex()]=true;}
    for(auto id:value.mInvalidationQueue) {
        const auto* row=used[id.GetIndex()];need(row!=nullptr&&row->generation>=id.GetSequenceNumber(),"Physics invalidation queue references an unallocated/future ledger generation.");
        if(ledger.owns(id)){need(flagged[id.GetIndex()]&&!queued_live[id.GetIndex()],"Live queued IDs must have exactly one matching owned invalidation flag.");queued_live[id.GetIndex()]=true;}
    }
    for(auto id:value.mInvalidatedBodies)need(queued_live[id.GetIndex()],"Owned invalidation flag has no live queue token.");
}
} // namespace poima::physical_bootstrap_json
