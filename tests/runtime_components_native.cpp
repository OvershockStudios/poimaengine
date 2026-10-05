// SPDX-License-Identifier: Apache-2.0
#include "poima/runtime.hpp"
#include "poima/assets.hpp"
#include "runtime_components.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <cstdio>

using namespace poima;
using Json=nlohmann::json;
namespace {
void check(bool v,const char* message) { if(!v)throw std::runtime_error(message); }
template<class F>void rejects(F&& f,const char* message) { bool failed=false;try { f(); }catch(const std::exception&) { failed=true; }check(failed,message); }
std::string id(unsigned n) { char text[33];std::snprintf(text,sizeof text,"%032x",n);return text; }
components::Schema schema(unsigned type=100,unsigned fields=5) {
    const std::array<const char*,5> kinds{"int32","int64","float32","float64","entity"};Json values=Json::array();
    for(unsigned i=0;i<fields;++i) {
        const auto kind=fields==5 ? kinds[i] : "int32";Json initial=0;if(std::string(kind)=="int64")initial="0";if(std::string(kind)=="entity")initial=id(0);
        values.push_back({{"id",id(1000+i)},{"name","Field"+std::to_string(i)},{"kind",kind},{"default",initial}});
    }
    return components::parse_schema(Json{{"id",id(type)},{"name","Stats"},{"version",1},{"fields",values}}.dump());
}
components::Payload values(const components::Schema& s,int value,const std::string& reference=id(0)) {
    Json j=Json::object();for(const auto& field:s.fields) {
        switch(field.kind) {
        case components::Kind::int32:j[field.id]=value;break;
        case components::Kind::int64:j[field.id]="9223372036854775807";break;
        case components::Kind::float32:j[field.id]=1.25;break;
        case components::Kind::float64:j[field.id]=2.5;break;
        case components::Kind::entity:j[field.id]=reference;break;
        }
    }
    return components::parse_values(s,j.dump());
}
PoimaGameComponentType binding(const components::Schema& s) {
    PoimaGameComponentType b{};b.type=gameplay_id(s.id);std::copy(s.fingerprint.begin(),s.fingerprint.end(),b.fingerprint);b.bytes=static_cast<std::uint32_t>(s.bytes());return b;
}
RuntimeDefinition definition(unsigned count=3,unsigned fields=5) {
    RuntimeDefinition d;d.world_id=id(99);d.component_schemas={schema(100,fields),schema(101,fields)};
    for(unsigned i=1;i<=count;++i) { RuntimeEntityDefinition e;e.id=id(i);if(i!=count)e.components[d.component_schemas[0].id]=components::defaults(d.component_schemas[0]);d.entities.push_back(std::move(e)); }
    return d;
}
struct Store {
    entt::registry registry;std::map<std::string,entt::entity> ids;std::unique_ptr<RuntimeComponents> store;
    explicit Store(const RuntimeDefinition& d) {
        // Occupy several named IDs to prove custom pool allocation doesn't alias.
        registry.storage<int>(0);registry.storage<double>(1);registry.storage<float>(3);
        for(const auto& e:d.entities)ids.emplace(e.id,registry.create());store=std::make_unique<RuntimeComponents>(registry,ids,d);
    }
};
void storage_and_journal() {
    const auto d=definition();Store owned(d);auto& s=*owned.store;const auto& t=d.component_schemas[0];const auto b=binding(t);
    check(s.query(t.id,"",257)==std::vector<std::string>{id(1),id(2)},"Query is not sorted/frozen.");
    check(s.query(t.id,id(1),1)==std::vector<std::string>{id(2)},"Exclusive query cursor failed.");
    check(s.query(d.component_schemas[1].id,"",1).empty(),"Declared empty type must query empty.");
    check(!s.read(t.id,id(3)),"Missing component must be optional.");rejects([&]{s.read(t.id,id(999));},"Unknown entity read accepted.");
    std::array<PoimaEntityId,2> out{};check(s.query(b,{},out)==2 && out[0].low==1 && out[1].low==2,"ABI query order failed.");
    check(s.alive(gameplay_id(id(3))) && !s.alive({}) && !s.alive(gameplay_id(id(999))),"Liveness mismatch.");
    auto wrong=b;wrong.fingerprint[0]^=1;rejects([&]{s.query(wrong,{},out);},"Wrong fingerprint accepted.");wrong=b;wrong.reserved=1;rejects([&]{s.query(wrong,{},out);},"Reserved descriptor accepted.");
    auto initial=*s.read(t.id,id(1));auto v1=values(t,7,id(2));auto v2=values(t,9,id(3));s.begin_batch();s.stage(b,gameplay_id(id(1)),v1);
    check(*s.read(t.id,id(1))==initial,"Queued writes leaked into reads.");rejects([&]{s.stage(b,gameplay_id(id(1)),v2);},"Duplicate write accepted.");
    s.apply_tick();check(s.revision()==1 && *s.read(t.id,id(1))==v1,"First tick publication failed.");
    check(s.journal_entries()==1 && s.journal_bytes()==t.bytes(),"Journal copied untouched payloads.");
    s.stage(b,gameplay_id(id(1)),v2);s.apply_tick();check(s.revision()==2 && s.journal_entries()==1 && s.journal_bytes()==t.bytes(),"Repeated write duplicated original journal.");
    s.stage(b,gameplay_id(id(2)),v1);s.apply_tick();check(s.journal_entries()==2,"Second dirty payload missing.");
    s.rollback_batch();check(s.revision()==0 && *s.read(t.id,id(1))==initial && *s.read(t.id,id(2))==initial,"Whole-batch rollback failed.");
    s.edit(t.id,id(1),v1);s.edit(t.id,id(1),v1);check(s.revision()==2,"Equal paused edit must increment revision.");
    auto invalid=v1;invalid[4]=std::byte{1};rejects([&]{s.edit(t.id,id(1),invalid);},"Noncanonical wire padding accepted.");check(s.revision()==2 && *s.read(t.id,id(1))==v1,"Failed edit mutated value/revision.");
    rejects([&]{s.edit(t.id,id(1),values(t,7,id(999)));},"Dangling reference accepted.");
    rejects([&]{s.validate_module({});},"Missing instantiated module declaration accepted.");s.validate_module({t});
    auto label=t;label.name="Renamed";label.fields[0].name="Label";s.validate_module({label});
    auto mismatch=t;mismatch.fingerprint[0]^=1;rejects([&]{s.validate_module({mismatch});},"Incompatible module accepted.");
    const auto saved=s.save();Store restored(d);restored.store->load(saved);check(restored.store->save()==saved,"Component save roundtrip mismatch.");
    auto corrupt=Json::parse(saved);corrupt["types"][0]["instances"][1]["entity"]=id(1);rejects([&]{s.load(corrupt.dump());},"Duplicate restored membership accepted.");check(s.save()==saved,"Invalid restore changed live component state.");
    corrupt=Json::parse(saved);corrupt["types"][0]["instances"][0]["values"][4]=id(999);rejects([&]{s.load(corrupt.dump());},"Dangling restored reference accepted.");check(s.save()==saved,"Reference validation mutated live state.");
}
void budgets() {
    const auto d=definition(5000,32);Store owned(d);auto& s=*owned.store;const auto& t=d.component_schemas[0];const auto b=binding(t);const auto data=values(t,3);
    s.begin_batch();s.stage(b,gameplay_id(id(1)),data);s.apply_tick();
    check(s.journal_bytes()==512 && s.journal_entries()==1,"Single sparse write copied full multi-megabyte store.");s.rollback_batch();
    s.begin_batch();for(unsigned i=1;i<=4096;++i)s.stage(b,gameplay_id(id(i)),data);
    rejects([&]{s.stage(b,gameplay_id(id(4097)),data);},"4097th staged command accepted.");s.apply_tick();check(s.journal_entries()==4096 && s.journal_bytes()==2*1024*1024,"Exact command/journal budget differs.");s.rollback_batch();check(s.revision()==0,"Budget batch rollback lost revision.");
    s.edit(t.id,id(4097),data);check(s.revision()==1,"Failed staging left duplicate/queue residue.");
}
std::string resign(Json j) { const auto payload=j.at("payload").dump();j["sha256"]=sha256(std::as_bytes(std::span(payload.data(),payload.size())));return j.dump(); }
void runtime_snapshots() {
    auto d=definition();Runtime runtime(d);const auto& t=d.component_schemas[0];const auto value=values(t,42,id(2));runtime.component_edit(t.id,id(1),value);runtime.step(3,{});
    const std::string hash(64,'a');const auto saved=runtime.save_snapshot(hash);check(Json::parse(saved).at("version")==2,"Custom world didn't use snapshot v2.");
    auto restored=Runtime::from_snapshot(d,hash,saved);check(restored->component_revision()==1 && restored->component_read(t.id,id(1))==runtime.component_read(t.id,id(1)),"Runtime component restore mismatch.");
    check(restored->save_snapshot(hash)==saved,"Exact logical custom snapshot roundtrip failed.");
    auto invalid=Json::parse(saved);invalid["payload"]["components"]["types"][0]["fingerprint"]=std::string(64,'b');rejects([&]{Runtime::from_snapshot(d,hash,resign(invalid));},"Wrong saved schema fingerprint accepted.");
    check(runtime.save_snapshot(hash)==saved,"Failed candidate restore mutated original world.");
    auto legacy=d;legacy.component_schemas.clear();for(auto& e:legacy.entities)e.components.clear();Runtime old(legacy);const auto old_save=old.save_snapshot(hash);check(Json::parse(old_save).at("version")==1 && !Json::parse(old_save).at("payload").contains("components"),"Legacy snapshot shape changed.");
    check(Runtime::from_snapshot(legacy,hash,old_save)->save_snapshot(hash)==old_save,"Legacy roundtrip changed.");
    rejects([&]{Runtime::from_snapshot(d,hash,old_save);},"Custom definition accepted missing custom snapshot state.");
    auto bad=d;bad.entities[0].components[t.id][4]=std::byte{1};rejects([&]{Runtime candidate(bad);},"Direct runtime accepted invalid canonical bytes.");
    bad=d;bad.entities[0].components[id(998)]=value;rejects([&]{Runtime candidate(bad);},"Direct runtime accepted undeclared component.");
    bad=d;bad.component_schemas[0].fields[0].initial[4]=std::byte{1};rejects([&]{Runtime candidate(bad);},"Direct runtime normalized malformed default padding silently.");
    bad=d;std::swap(bad.component_schemas[0].fields[0],bad.component_schemas[0].fields[1]);rejects([&]{Runtime candidate(bad);},"Direct runtime accepted ambiguous noncanonical field layout.");
}
}
int main() {
    try { storage_and_journal();budgets();runtime_snapshots();std::cout<<"Runtime components: storage/query/journal/budgets/snapshot tests passed.\n";return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
