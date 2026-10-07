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
#include <cstdlib>
#include <limits>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace allocation_test {
thread_local std::size_t remaining=std::numeric_limits<std::size_t>::max();
void consume() {
    if(remaining==std::numeric_limits<std::size_t>::max())return;
    if(remaining==0)throw std::bad_alloc();
    --remaining;
}
struct Budget {
    std::size_t previous;
    explicit Budget(std::size_t count):previous(remaining) { remaining=count; }
    ~Budget() { remaining=previous; }
};
}
void* operator new(std::size_t size) {
    allocation_test::consume();if(auto* p=std::malloc(size ? size : 1))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t size,std::align_val_t alignment) {
    allocation_test::consume();void* p=nullptr;
#ifdef _WIN32
    p=_aligned_malloc(size ? size : 1,static_cast<std::size_t>(alignment));
#else
    if(posix_memalign(&p,static_cast<std::size_t>(alignment),size ? size : 1)!=0)p=nullptr;
#endif
    if(p)return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size,std::align_val_t alignment) { return ::operator new(size,alignment); }
void operator delete(void* p,std::align_val_t) noexcept {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete[](void* p,std::align_val_t alignment) noexcept { ::operator delete(p,alignment); }
void operator delete(void* p,std::size_t,std::align_val_t alignment) noexcept { ::operator delete(p,alignment); }
void operator delete[](void* p,std::size_t,std::align_val_t alignment) noexcept { ::operator delete(p,alignment); }

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
    // Odd multiplication permutes the complete command range; probe collisions
    // and duplicates in orders unrelated to entity order or insertion order.
    s.begin_batch();for(unsigned i=0;i<4095;++i)s.stage(b,gameplay_id(id((i*2053)%4096+1)),data);
    // Leave capacity for one more write so only duplicate detection can reject.
    for(unsigned i=4095;i>0;--i)rejects([&]{s.stage(b,gameplay_id(id(((i-1)*2053)%4096+1)),data);},"Duplicate target escaped a populated staging index.");
    s.stage(b,gameplay_id(id((4095*2053)%4096+1)),data);
    rejects([&]{s.stage(b,gameplay_id(id(4097)),data);},"4097th staged command accepted.");s.apply_tick();check(s.journal_entries()==4096 && s.journal_bytes()==2*1024*1024,"Exact command/journal budget differs.");s.rollback_batch();check(s.revision()==0,"Budget batch rollback lost revision.");
    s.edit(t.id,id(4097),data);check(s.revision()==1,"Failed staging left duplicate/queue residue.");
}
void publish_no_alloc(RuntimeComponents& s) { allocation_test::Budget deny(0);s.publish_tick(); }
void rollback_no_alloc(RuntimeComponents& s) { allocation_test::Budget deny(0);s.rollback_batch(); }
void commit_no_alloc(RuntimeComponents& s) { allocation_test::Budget deny(0);s.commit_batch(); }
void staging_allocation_failures() {
    auto d=definition();d.entities[1].id="0123456789abcdeffedcba9876543210";
    const auto& t=d.component_schemas[0];const auto b=binding(t);
    const auto data=values(t,7,d.entities[1].id);
    const auto& other=d.component_schemas[1];d.entities[0].components[other.id]=components::defaults(other);
    // Fail both the payload copy and the first staging-vector allocation. Neither
    // may leave a duplicate-index entry behind; retry the identical key directly.
    for(std::size_t allowance=0;allowance<2;++allowance) {
        Store owned(d);auto& s=*owned.store;const auto baseline=s.save();s.begin_batch();bool failed=false;
        { allocation_test::Budget budget(allowance);try { s.stage(b,gameplay_id(id(1)),data); }catch(const std::bad_alloc&) { failed=true; } }
        check(failed,"Staging fault injection did not fail the intended allocation.");
        s.stage(b,gameplay_id(id(1)),data);rejects([&]{s.stage(b,gameplay_id(id(1)),data);},"Retried stage lost duplicate tracking.");
        s.stage(binding(other),gameplay_id(id(1)),data);
        s.prepare_tick();publish_no_alloc(s);
        check(*s.read(t.id,id(1))==data,"Reference validation failed to decode both entity ID halves.");
        check(*s.read(other.id,id(1))==data,"Staging index conflated different component types.");
        rollback_no_alloc(s);check(s.save()==baseline,"Failed-stage retry did not roll back.");
        s.begin_batch();s.stage(b,gameplay_id(id(1)),data);s.prepare_tick();publish_no_alloc(s);commit_no_alloc(s);
        check(*s.read(t.id,id(1))==data,"Staging index survived rollback into the next batch.");
    }
}
using Initial=std::map<std::string,components::Payload>;
ComponentSpawn spawn(Store& owned,unsigned number,const Initial& initial) {
    return {gameplay_id(id(number)),owned.registry.create(),&initial};
}
void lifecycle_rollback_and_stability() {
    const auto d=definition(5);Store owned(d);auto& s=*owned.store;const auto& t=d.component_schemas[0];const auto b=binding(t);
    const auto baseline=s.save();const auto initial=*s.read(t.id,id(4));
    Initial fresh{{t.id,values(t,11)}};std::vector<ComponentSpawn> births;
    // Cross multiple pinned EnTT payload pages, preserving old Cell pointers.
    for(unsigned i=100;i<1200;++i)births.push_back(spawn(owned,i,fresh));
    s.begin_batch();s.stage(b,gameplay_id(id(4)),values(t,17));s.apply_tick();
    const std::array removed{gameplay_id(id(2))};s.prepare_tick(births,removed);
    check(s.candidate_alive(gameplay_id(id(100))) && !s.candidate_alive(gameplay_id(id(2))) &&
        !s.alive(gameplay_id(id(100))) && s.alive(gameplay_id(id(2))),"Prepared membership escaped before publication.");
    publish_no_alloc(s);check(s.live_instances()==1103 && s.retained_instances()==1104,"Logical/physical component retention differs.");
    s.stage(b,gameplay_id(id(100)),values(t,23));s.apply_tick();
    check(s.journal_entries()==1,"Born-cell write copied an unnecessary original payload.");
    s.stage(b,gameplay_id(id(3)),values(t,29));
    const std::array born_removed{gameplay_id(id(101))};s.prepare_tick({},born_removed);publish_no_alloc(s);
    check(s.journal_entries()==2 && !s.alive(gameplay_id(id(101))),"Multi-tick journal or retirement failed.");
    rollback_no_alloc(s);check(s.save()==baseline && s.retained_instances()==4,"Structural rollback did not restore the original store.");
    check(*s.read(t.id,id(4))==initial && s.query(t.id,"",257)==std::vector<std::string>{id(1),id(2),id(3),id(4)},"Growth/rollback invalidated surviving Cell pointers.");
    // Physical middle deletion must not move the final Cell onto a stale row.
    s.begin_batch();s.stage(b,gameplay_id(id(4)),values(t,31));s.prepare_tick({},removed);publish_no_alloc(s);commit_no_alloc(s);
    check(s.retained_instances()==3 && s.query(t.id,"",257)==std::vector<std::string>{id(1),id(3),id(4)},"Middle deletion changed sorted membership.");
    s.edit(t.id,id(4),values(t,37));s.edit(t.id,id(3),values(t,41));
    check(*s.read(t.id,id(4))==values(t,37) && *s.read(t.id,id(3))==values(t,41),"Middle deletion invalidated surviving payload/journal pointers.");
    check(s.query(t.id,id(1),1)==std::vector<std::string>{id(3)} && s.query(t.id,id(3),1)==std::vector<std::string>{id(4)},"Structural pagination skipped/repeated IDs.");
    // A born-and-retired entity consumes retained storage until commit.
    const auto before=s.retained_instances();auto short_lived=spawn(owned,8000,fresh);s.begin_batch();
    s.prepare_tick(std::span(&short_lived,1));publish_no_alloc(s);
    const std::array retire{short_lived.id};s.prepare_tick({},retire);publish_no_alloc(s);
    check(s.retained_instances()==before+1 && s.live_instances()==before,"Born retirement was destroyed before commit.");
    commit_no_alloc(s);check(s.retained_instances()==before && !s.alive(short_lived.id),"Born retirement leaked a physical Cell.");
}
void candidate_references_and_guards() {
    auto d=definition();const auto& t=d.component_schemas[0];d.entities[0].components[t.id]=values(t,1,id(2));
    Store owned(d);auto& s=*owned.store;const auto b=binding(t);const auto baseline=s.save();
    const std::array removed{gameplay_id(id(2))};
    s.begin_batch();rejects([&]{s.prepare_tick({},removed);},"Untouched incoming reference survived despawn.");
    check(s.alive(gameplay_id(id(2))) && s.retained_instances()==2 && !s.candidate_alive(gameplay_id(id(2))),"Failed preparation changed current/candidate state.");
    s.stage(b,gameplay_id(id(1)),values(t,2));s.prepare_tick({},removed);publish_no_alloc(s);
    check(!s.alive(gameplay_id(id(2))) && *s.read(t.id,id(1))==values(t,2),"Reference repair plus despawn failed.");rollback_no_alloc(s);check(s.save()==baseline,"Reference repair rollback failed.");
    Initial first{{t.id,values(t,3,id(11))}},second{{t.id,values(t,4,id(10))}};
    std::array births{spawn(owned,10,first),spawn(owned,11,second)};
    s.begin_batch();s.stage(b,births[0].id,values(t,9,id(11)));s.prepare_tick(births);publish_no_alloc(s);
    check(*s.read(t.id,id(10))==values(t,9,id(11)) && *s.read(t.id,id(11))==values(t,4,id(10)),"Mutual spawn references/full override failed.");
    s.stage(b,gameplay_id(id(9999)),values(t,1));rejects([&]{s.prepare_tick();},"Unknown queued target accepted at final candidate.");
    rollback_no_alloc(s);check(s.save()==baseline,"Bad later target did not restore earlier spawn/override.");
    // Invalid strong references are intentionally deferred; bad wire is not.
    s.begin_batch();s.stage(b,gameplay_id(id(1)),values(t,2,id(9999)));rejects([&]{s.prepare_tick();},"Dangling final reference accepted.");rollback_no_alloc(s);
    s.begin_batch();auto bad=values(t,2);bad[4]=std::byte{1};rejects([&]{s.stage(b,gameplay_id(id(1)),bad);},"Bad wire was not rejected during staging.");rollback_no_alloc(s);
    Initial empty;auto duplicate=spawn(owned,1,empty);s.begin_batch();rejects([&]{s.prepare_tick(std::span(&duplicate,1));},"Existing public entity ID was reused.");rollback_no_alloc(s);
    auto aliased=ComponentSpawn{gameplay_id(id(12)),owned.ids.at(id(1)),&empty};s.begin_batch();rejects([&]{s.prepare_tick(std::span(&aliased,1));},"Existing native entity owner was reused.");rollback_no_alloc(s);
    auto invalid=spawn(owned,12,first);invalid.owner=entt::null;s.begin_batch();rejects([&]{s.prepare_tick(std::span(&invalid,1));},"Invalid native entity owner accepted.");rollback_no_alloc(s);
    s.begin_batch();s.stage(b,gameplay_id(id(2)),values(t,1));rejects([&]{s.prepare_tick({},removed);},"Write to despawned target accepted.");rollback_no_alloc(s);
    check(s.save()==baseline,"Guard failures changed committed state.");
}

void pending_births() {
    const auto d=definition();Store owned(d);auto& s=*owned.store;const auto& t=d.component_schemas[0];const auto b=binding(t);
    Initial first{{t.id,values(t,3)}},second{{t.id,values(t,4)}};
    const auto a=gameplay_id(id(10)),z=gameplay_id(id(11));const auto baseline=s.save();
    s.begin_batch();s.reserve_birth(a,first);s.reserve_birth(z,second);
    check(!s.alive(a) && s.query(t.id,"",257)==std::vector<std::string>{id(1),id(2)},"Pending births escaped committed membership/query.");
    std::vector<std::byte> output(t.bytes());std::uint32_t present=1;
    rejects([&]{s.get(b,a,output,present);},"Pending birth became readable before publication.");
    rejects([&]{s.reserve_birth(a,first);},"Duplicate pending birth accepted.");
    rejects([&]{s.reserve_birth(gameplay_id(id(1)),first);},"Live entity registered as pending birth.");
    rejects([&]{s.reserve_birth({},first);},"Zero pending identity accepted.");
    rejects([&]{s.stage_pending_checked(b,gameplay_id(id(999)),values(t,8));},"Managed staging accepted an arbitrary target.");
    rejects([&]{s.stage_pending_checked(binding(d.component_schemas[1]),a,values(d.component_schemas[1],8));},"Managed staging added a component absent from pending layout.");
    rejects([&]{s.stage_pending_checked(b,gameplay_id(id(3)),values(t,8));},"Managed staging added an absent component to live entity.");
    s.stage_pending_checked(b,a,values(t,9,id(11)));s.stage_pending_checked(b,z,values(t,8,id(10)));
    rejects([&]{s.stage_pending_checked(b,a,values(t,7));},"Duplicate pending initializer accepted.");
    rejects([&]{s.prepare_tick();},"Reserved births silently omitted from preparation.");
    auto changed=first;changed[t.id]=values(t,99);auto malformed=spawn(owned,10,changed);auto good_second=spawn(owned,11,second);
    std::array mismatch{malformed,good_second};rejects([&]{s.prepare_tick(mismatch);},"Reserved birth accepted different recipe payload.");
    // Matching content in another map is legitimate; pointer identity is not a contract.
    const auto first_copy=first;malformed.initial=&first_copy;std::array births{malformed,good_second};
    s.prepare_tick(births);check(s.candidate_alive(a) && !s.alive(a),"Pending candidate visibility differs.");publish_no_alloc(s);
    check(*s.read(t.id,id(10))==values(t,9,id(11)) && *s.read(t.id,id(11))==values(t,8,id(10)),"Same-tick initializers or mutual references failed.");
    // Publication must drop registration, while retained lineage still rejects reuse.
    rejects([&]{s.cancel_birth(a);},"Published pending registration survived publication.");
    rejects([&]{s.reserve_birth(a,first);},"Published identity could be re-reserved.");
    rollback_no_alloc(s);check(s.save()==baseline,"Pending birth rollback lost original store.");
    // Cancellation removes only that birth's staged writes, keeps surviving hash
    // entries sound, and must not allocate even when removing the middle entry.
    s.begin_batch();s.reserve_birth(a,first);s.reserve_birth(z,second);
    s.stage_pending_checked(b,gameplay_id(id(1)),values(t,20));
    s.stage_pending_checked(b,a,values(t,21));s.stage_pending_checked(b,z,values(t,22,id(10)));
    { allocation_test::Budget deny(0);s.cancel_birth(a); }
    rejects([&]{s.cancel_birth(a);},"Canceled birth remained registered.");
    rejects([&]{s.stage_pending_checked(b,a,values(t,23));},"Canceled identity remained writable.");
    rejects([&]{s.stage_pending_checked(b,z,values(t,23));},"Cancellation lost another initializer's duplicate guard.");
    rejects([&]{s.stage_pending_checked(b,gameplay_id(id(1)),values(t,23));},"Cancellation lost a committed target's duplicate guard.");
    rejects([&]{s.prepare_tick(std::span(&good_second,1));},"Cancellation left a surviving dangling reference valid.");
    rollback_no_alloc(s);check(s.save()==baseline,"Canceled/dangling birth rollback changed committed state.");
    s.begin_batch();s.reserve_birth(a,first);s.stage_pending_checked(b,a,values(t,31));
    { allocation_test::Budget deny(0);s.cancel_birth(a); }
    s.reserve_birth(a,first);s.stage_pending_checked(b,a,values(t,32));
    s.prepare_tick(std::span(&malformed,1));publish_no_alloc(s);commit_no_alloc(s);
    check(*s.read(t.id,id(10))==values(t,32),"Cancellation left staged hash/payload budget residue.");
    // A reservation discarded by commit/rollback cannot leak into the next batch.
    s.begin_batch();s.reserve_birth(z,second);commit_no_alloc(s);
    s.begin_batch();s.reserve_birth(z,second);rollback_no_alloc(s);
    s.begin_batch();s.reserve_birth(z,second);s.prepare_tick(std::span(&good_second,1));publish_no_alloc(s);commit_no_alloc(s);
    check(s.alive(z),"Reservation lifetime leaked across batch boundaries.");
}
// Insert before pending_reservation_guarantees(); invoke pending_payload_budget()
// in main next to pending_births() and pending_reservation_guarantees().
void pending_payload_budget() {
    RuntimeDefinition d;d.world_id=id(99);RuntimeEntityDefinition base;base.id=id(1);d.entities.push_back(base);
    for(unsigned n=0;n<64;++n)d.component_schemas.push_back(schema(100+n,32));
    Initial initial;for(const auto& t:d.component_schemas)initial.emplace(t.id,components::defaults(t));
    std::size_t recipe_bytes=0;for(const auto& [type,payload]:initial) { (void)type;recipe_bytes+=payload.size(); }
    check(recipe_bytes==32768 && components::max_command_bytes==2097152,"Pending byte-boundary fixture no longer represents 2 MiB.");
    // 63 recipes (63 * 32 KiB) plus 64 initializer writes (64 * 512 B)
    // exactly fill the shared pending/staged command payload allowance.
    for(const bool cancel_and_replace:{false,true}) {
        Store owned(d);auto& s=*owned.store;const auto baseline=s.save();s.begin_batch();
        std::vector<ComponentSpawn> births;
        for(unsigned n=0;n<63;++n) {
            births.push_back(spawn(owned,1000+n,initial));s.reserve_birth(births.back().id,initial);
        }
        for(const auto& t:d.component_schemas)s.stage_pending_checked(binding(t),births.front().id,values(t,7));
        Initial extra{{d.component_schemas.front().id,components::defaults(d.component_schemas.front())}};
        const auto extra_id=gameplay_id(id(9000));
        rejects([&]{s.reserve_birth(extra_id,extra);},"Reservation exceeded exact combined pending/initializer 2 MiB boundary.");
        rejects([&]{s.stage_pending_checked(binding(d.component_schemas.front()),births[1].id,values(d.component_schemas.front(),8));},
            "Initializer exceeded exact combined pending/initializer 2 MiB boundary.");
        if(cancel_and_replace) {
            // Cancel the only initialized birth. This must recover BOTH its
            // 32 KiB recipe and its 32 KiB initializer set, without allocations.
            const auto canceled=births.front().id;
            { allocation_test::Budget deny(0);s.cancel_birth(canceled); }
            births.erase(births.begin());
            // Reusing extra_id also proves the failed reservation above did not
            // leave a duplicate registration or retain its byte accounting.
            births.push_back(spawn(owned,9000,initial));s.reserve_birth(extra_id,initial);
            births.push_back(spawn(owned,9001,initial));s.reserve_birth(births.back().id,initial);
            check(births.size()==64,"Cancellation replacement fixture count differs.");
            rejects([&]{s.stage_pending_checked(binding(d.component_schemas.front()),births.front().id,values(d.component_schemas.front(),9));},
                "Replacement recipes did not consume the recovered full byte budget.");
        }
        s.prepare_tick(births);publish_no_alloc(s);
        check(s.live_instances()==births.size()*64 && s.live_bytes()==births.size()*recipe_bytes,
            "Exact pending byte boundary lost live component instances or bytes.");
        for(const auto& birth:births)for(const auto& t:d.component_schemas) {
            const auto expected=!cancel_and_replace && birth.id.high==births.front().id.high && birth.id.low==births.front().id.low
                ? values(t,7) : components::defaults(t);
            check(s.read(t.id,gameplay_id(birth.id))==std::optional<components::Payload>(expected),
                "Failed reservation/stage or cancellation corrupted surviving pending recipe/initializer state.");
        }
        rollback_no_alloc(s);check(s.save()==baseline && s.live_instances()==0 && s.retained_instances()==0,
            "Exact pending byte-budget rollback retained registrations or cells.");
    }
}

void pending_reservation_guarantees() {
    const auto d=definition();const auto& t=d.component_schemas[0];Initial initial{{t.id,values(t,3)}};
    {
        Store owned(d);auto& s=*owned.store;s.begin_batch();const auto a=gameplay_id(id(10));
        auto invalid=initial;invalid[t.id][4]=std::byte{1};rejects([&]{s.reserve_birth(a,invalid);},"Invalid pending wire payload accepted.");
        invalid=initial;invalid[id(999)]=values(t,3);rejects([&]{s.reserve_birth(a,invalid);},"Unknown pending component type accepted.");
        s.reserve_birth(a,initial);s.stage_pending_checked(binding(t),a,values(t,4));
        auto birth=spawn(owned,10,initial);s.prepare_tick(std::span(&birth,1));publish_no_alloc(s);rollback_no_alloc(s);
    }
    bool success=false;std::size_t failures=0;
    for(std::size_t budget=0;budget<64 && !success;++budget) {
        Store owned(d);auto& s=*owned.store;const auto baseline=s.save();s.begin_batch();bool failed=false;const auto numeric=gameplay_id(id(10));
        {allocation_test::Budget deny(budget);try {s.reserve_birth(numeric,initial);}catch(const std::bad_alloc&) {failed=true;}}
        if(failed) {++failures;s.reserve_birth(gameplay_id(id(10)),initial);}else success=true;
        auto birth=spawn(owned,10,initial);s.stage_pending_checked(binding(t),birth.id,values(t,7));
        s.prepare_tick(std::span(&birth,1));publish_no_alloc(s);rollback_no_alloc(s);
        check(s.save()==baseline,"Failed reservation changed committed or journal state.");
    }
    check(success && failures>0,"Pending reservation allocation failure coverage missing.");
    {
        Store owned(d);auto& s=*owned.store;Initial empty;s.begin_batch();
        for(unsigned n=0;n<components::max_commands;++n)s.reserve_birth(gameplay_id(id(10000+n)),empty);
        rejects([&]{s.reserve_birth(gameplay_id(id(20000)),empty);},"Pending birth count budget was not enforced.");
        const auto canceled=gameplay_id(id(10000));{allocation_test::Budget deny(0);s.cancel_birth(canceled);}
        s.reserve_birth(gameplay_id(id(20000)),empty);rollback_no_alloc(s);
    }
}
void preparation_allocation_failures() {
    const auto d=definition();const auto& t=d.component_schemas[0];const auto b=binding(t);
    Initial initial{{t.id,values(t,5)},{d.component_schemas[1].id,values(d.component_schemas[1],7)}};
    bool reached_success=false;std::size_t failed_points=0;
    for(std::size_t allowance=0;allowance<2048 && !reached_success;++allowance) {
        // A fresh registry each time preserves preparation's allocation sequence.
        Store owned(d);auto& s=*owned.store;const auto baseline=s.save();
        std::vector<ComponentSpawn> births;for(unsigned i=10;i<26;++i)births.push_back(spawn(owned,i,initial));
        s.begin_batch();s.stage(b,gameplay_id(id(1)),values(t,13));bool failed=false;
        { allocation_test::Budget budget(allowance);try { s.prepare_tick(births); }catch(const std::bad_alloc&) { failed=true; } }
        if(failed) {
            ++failed_points;check(s.retained_instances()==2 && !s.alive(births[0].id),"Failed preparation retained an unpublished Cell.");
        } else { publish_no_alloc(s);reached_success=true; }
        rollback_no_alloc(s);check(s.save()==baseline,"Allocation failure/retry did not preserve the batch checkpoint.");
        // Reuse identical owners immediately: leaked pool membership would fail.
        s.begin_batch();s.prepare_tick(births);publish_no_alloc(s);commit_no_alloc(s);
        check(s.live_instances()==34 && s.retained_instances()==34,"Post-failure retry leaked or omitted Cells.");
    }
    check(reached_success && failed_points>32,"Allocation fault injection did not cover staged Cell creation.");
    std::cout<<"Component preparation allocation failure points: "<<failed_points<<".\n";
}
void lifecycle_budgets() {
    // Exact2MiB spawn payload per tick; maximum-sized cells make retained count
    // and payload-byte ceilings coincide, while logical live size stays small.
    RuntimeDefinition d;d.world_id=id(99);RuntimeEntityDefinition base;base.id=id(1);d.entities.push_back(base);
    for(unsigned i=0;i<64;++i)d.component_schemas.push_back(schema(100+i,32));
    Initial initial;for(const auto& t:d.component_schemas)initial.emplace(t.id,components::defaults(t));
    {
        Store owned(d);auto& s=*owned.store;s.begin_batch();std::vector<PoimaEntityId> previous;
        for(unsigned group=0;group<16;++group) {
            std::vector<ComponentSpawn> births;for(unsigned n=0;n<64;++n)births.push_back(spawn(owned,100+group*64+n,initial));
            s.prepare_tick(births,previous);publish_no_alloc(s);previous.clear();for(const auto& birth:births)previous.push_back(birth.id);
        }
        check(s.retained_instances()==RuntimeComponents::max_retained_instances && s.retained_bytes()==RuntimeComponents::max_retained_bytes && s.live_instances()==4096,"Retained component count/byte boundary differs.");
        auto extra=spawn(owned,2000,initial);rejects([&]{s.prepare_tick(std::span(&extra,1),previous);},"Net-live accounting ignored full retained storage.");
        check(s.retained_instances()==RuntimeComponents::max_retained_instances && s.alive(previous.front()),"Retained budget rejection changed membership.");
        rollback_no_alloc(s);check(s.retained_instances()==0 && s.live_instances()==0 && s.revision()==0,"Retained budget rollback leaked state.");
    }
    {
        Store owned(d);auto& s=*owned.store;s.begin_batch();
        for(unsigned group=0;group<8;++group) {
            std::vector<ComponentSpawn> births;for(unsigned n=0;n<64;++n)births.push_back(spawn(owned,100+group*64+n,initial));
            s.prepare_tick(births);publish_no_alloc(s);
        }
        check(s.live_instances()==components::max_instances && s.live_bytes()==components::max_payload_bytes,"Live component boundary differs.");
        auto extra=spawn(owned,2000,initial);rejects([&]{s.prepare_tick(std::span(&extra,1));},"Live component budget exceeded silently.");rollback_no_alloc(s);
    }
    {
        auto small=definition();Store owned(small);auto& s=*owned.store;std::vector<ComponentSpawn> births;
        Initial empty;for(unsigned n=0;n<4097;++n)births.push_back(spawn(owned,10000+n,empty));
        s.begin_batch();rejects([&]{s.prepare_tick(births);},"4097th structural command accepted.");rollback_no_alloc(s);
    }
    {
        auto with_value=d;with_value.entities[0].components[d.component_schemas[0].id]=components::defaults(d.component_schemas[0]);
        Store owned(with_value);auto& s=*owned.store;std::vector<ComponentSpawn> births;
        for(unsigned n=0;n<64;++n)births.push_back(spawn(owned,100+n,initial));
        s.begin_batch();s.stage(binding(d.component_schemas[0]),gameplay_id(id(1)),components::defaults(d.component_schemas[0]));
        rejects([&]{s.prepare_tick(births);},"Combined spawn/value bytes exceeded2MiB.");rollback_no_alloc(s);check(s.retained_instances()==1,"Byte-budget failure leaked cells.");
    }
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
void collection_reference_lifecycle() {
    const Json declaration={{"id",id(100)},{"name","Inventory"},{"version",2},{"fields",Json::array({
        {{"id",id(1)},{"name","Items"},{"kind","array"},{"element_kind","entity"},{"capacity",3},{"default",Json::array()}},
        {{"id",id(2)},{"name","Owner"},{"kind","entity"},{"default",id(0)}}})}};
    const auto schema=components::parse_schema(declaration.dump());
    const auto payload=[&](Json items,const std::string& owner=id(0)) {
        return components::parse_values(schema,Json{{id(1),std::move(items)},{id(2),owner}}.dump());
    };
    RuntimeDefinition d;d.world_id=id(99);d.component_schemas={schema};
    for(unsigned i=1;i<=3;++i) {RuntimeEntityDefinition e;e.id=id(i);if(i==1)e.components[schema.id]=payload(Json::array({id(2),id(3)}));d.entities.push_back(std::move(e));}
    Store owned(d);auto& store=*owned.store;const auto before=store.save();const auto descriptor=binding(schema);
    const std::array<PoimaEntityId,1> removed{gameplay_id(id(3))};
    store.begin_batch();rejects([&]{store.prepare_tick({},removed);},"Collection reference survived target deletion.");
    rollback_no_alloc(store);check(store.save()==before,"Rejected collection deletion changed state.");
    store.begin_batch();const auto repaired=payload(Json::array({id(2)}));store.stage(descriptor,gameplay_id(id(1)),repaired);
    store.prepare_tick({},removed);store.publish_tick();check(!store.alive(gameplay_id(id(3))),"Collection repair failed to allow deletion.");
    rollback_no_alloc(store);check(store.save()==before,"Collection repair and deletion failed rollback.");
    // A scalar reference following the collection must use its real byte offset.
    store.begin_batch();const auto scalar=payload(Json::array(),id(3));store.stage(descriptor,gameplay_id(id(1)),scalar);
    rejects([&]{store.prepare_tick({},removed);},"Scalar reference after collection used the wrong offset.");rollback_no_alloc(store);
    auto bad=payload(Json::array({id(999)}));store.begin_batch();store.stage(descriptor,gameplay_id(id(1)),bad);
    rejects([&]{store.prepare_tick();},"Dangling collection item survived preparation.");rollback_no_alloc(store);
    store.edit(schema.id,id(1),payload(Json::array({id(3),id(2),id(3)})));const auto full=store.save();
    store.edit(schema.id,id(1),payload(Json::array()));store.load(full);check(store.save()==full,"Collection save/load lost order or duplicates.");
    const std::string hash(64,'a');Runtime runtime(d);const auto snapshot=runtime.save_snapshot(hash);
    check(Runtime::from_snapshot(d,hash,snapshot)->save_snapshot(hash)==snapshot,"Collection runtime snapshot changed.");
}

}
int main() {
    try { collection_reference_lifecycle();storage_and_journal();budgets();staging_allocation_failures();lifecycle_rollback_and_stability();candidate_references_and_guards();pending_births();pending_payload_budget();pending_reservation_guarantees();preparation_allocation_failures();lifecycle_budgets();runtime_snapshots();std::cout<<"Runtime components: storage/query/journal/lifecycle/pending-birth/fault-injection/budgets/snapshot tests passed.\n";return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
