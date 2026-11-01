// SPDX-License-Identifier: Apache-2.0
#include "../src/runtime_body_ids.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using Ledger=poima::RuntimeBodyIds;
using Row=Ledger::SnapshotRow;
unsigned checks=0;
void need(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
JPH::BodyID id(std::uint32_t index,std::uint8_t generation){return JPH::BodyID(index,generation);}
template<class E,class F> void rejects(F&& operation,const char* message) {
    bool rejected=false;try{operation();}catch(const E&){rejected=true;}need(rejected,message);
}
Ledger roundtrip(const Ledger& original) {
    const auto snapshot=original.snapshot();const auto cold=Ledger::from_snapshot(snapshot);
    need(cold==original,"Typed snapshot must retain every native ledger field.");
    need(cold.snapshot()==snapshot,"Typed sparse snapshot must round trip without normalization.");
    need(cold.live()==original.live()&&cold.available()==original.available()&&cold.exhausted()==original.exhausted(),"Derived allocator counters changed.");
    return cold;
}
void empty_and_partial_startup() {
    Ledger empty;const auto initial=empty.snapshot();need(!initial.sealed&&initial.rows.empty(),"New ledger must retain empty unsealed startup.");(void)roundtrip(empty);
    empty.seal();need(empty.snapshot().sealed&&empty.snapshot().rows.empty(),"Explicit empty seal must not be lost.");(void)roundtrip(empty);
    Ledger startup;startup.reserve(id(9,7));startup.reserve(id(4,0));startup.reserve(id(2,255));
    const auto sparse=startup.snapshot();
    need(!sparse.sealed&&sparse.rows==std::vector<Row>{{2,255,true},{4,0,true},{9,7,true}},"Startup rows must be sorted and preserve occupied generation zero/max.");
    need(startup.live()==3&&startup.exhausted()==0&&startup.available()==Ledger::capacity-3,"Occupied generation255 is not exhausted until release.");
    auto restored=roundtrip(startup);startup.reserve(id(11,0));restored.reserve(id(11,0));need(restored==startup,"Restored partial startup must permit exactly the same further reservations.");
    rejects<std::invalid_argument>([&]{restored.reserve(id(4,0));},"Used generation-zero reservation must not be mistaken for an unused slot.");
    need(restored==startup,"Rejected duplicate reservation changed a restored startup ledger.");
    auto partial_warm=startup,partial_cold=restored;
    for(const auto expected:{id(0,1),id(1,1),id(3,1),id(5,1)}) {
        need(partial_warm.allocate()==expected&&partial_cold.allocate()==expected,"Unsealed startup cold allocation must skip occupied reservations and choose the exact same new IDs.");
        need(partial_warm==partial_cold&&partial_cold.snapshot().sealed,"Successful allocation must seal both reconstructed partial startups identically.");
    }
    startup.seal();restored.seal();need(startup==restored,"Sealing reconstructed startup changed identity state.");
    rejects<std::logic_error>([&]{restored.reserve(id(20,1));},"Restored sealed inventory must reject late bootstrap reservations.");
    need(restored==startup,"Rejected late startup reservation changed ledger.");
}
void mixed_retired_and_future_ids() {
    Ledger warm;warm.reserve(id(0,254));warm.reserve(id(1,0));warm.reserve(id(3,23));warm.reserve(id(8,0));warm.seal();
    warm.release(id(0,254));warm.release(id(1,0));warm.release(id(3,23));
    const auto last=warm.allocate();need(last==id(0,255),"Last available generation must be255, never wrapped or skipped.");warm.release(last);
    const auto one=warm.allocate(),two=warm.allocate(),three=warm.allocate();
    need(one==id(1,1)&&two==id(2,1)&&three==id(3,24),"Allocation sequence must distinguish retired and never-used slots.");warm.release(three);
    const auto state=warm.snapshot();
    need(state.sealed&&state.rows==std::vector<Row>{{0,255,false},{1,1,true},{2,1,true},{3,24,false},{8,0,true}},"Sparse history must keep every used/retired/exhausted slot and omit only never-used gaps.");
    need(warm.live()==3&&warm.exhausted()==1&&warm.available()==Ledger::capacity-4,"Mixed occupied/retired/exhausted counters are wrong.");
    auto cold=roundtrip(warm);
    const std::array<JPH::BodyID,4> expected{id(3,25),id(4,1),id(5,1),id(6,1)};
    for(const auto wanted:expected) {
        need(warm.allocate()==wanted&&cold.allocate()==wanted,"Cold next-allocation sequence differs from independent expected IDs.");need(cold==warm,"Future allocation altered reconstructed identity history.");
    }
    warm.release(expected[0]);cold.release(expected[0]);need(warm.allocate()==id(3,26)&&cold.allocate()==id(3,26),"Retired generation history was dropped during reconstruction.");
    need(cold==warm,"Future release/reallocation histories diverged.");
    need(!cold.owns(id(0,255))&&!cold.owns(id(3,25))&&cold.owns(id(3,26))&&cold.owns(id(8,0)),"Stale generation rejection changed after cold reconstruction.");
    const auto before=cold;rejects<std::invalid_argument>([&]{cold.release(id(3,25));},"Cold ledger accepted a stale release.");need(cold==before,"Stale release mutated reconstructed ledger.");
    // Gen0 released slots still belong in history even though their next ID is1.
    Ledger zero;zero.reserve(id(17,0));zero.release(id(17,0));need(zero.snapshot().rows==std::vector<Row>{{17,0,false}},"Released generation-zero provenance was elided.");(void)roundtrip(zero);
}
void every_slot_and_exhaustion() {
    Ledger active;
    for(std::uint32_t i=0;i<Ledger::capacity;++i)active.reserve(id(i,255));
    need(!active.snapshot().sealed&&active.live()==Ledger::capacity&&active.available()==0&&active.exhausted()==0,"Full startup gen255 inventory has incorrect occupancy/exhaustion.");
    auto occupied=roundtrip(active);const auto before_occupied=occupied;
    rejects<std::overflow_error>([&]{(void)occupied.allocate();},"Fully occupied snapshot unexpectedly allocated.");need(occupied==before_occupied,"Failed capacity allocation mutated ledger or sealing.");
    active.seal();for(std::uint32_t i=0;i<Ledger::capacity;++i)active.release(id(i,255));
    const auto rows=active.snapshot();need(rows.sealed&&rows.rows.size()==Ledger::capacity,"Every exhausted slot must remain serialized.");
    for(std::uint32_t i=0;i<Ledger::capacity;++i)need(rows.rows[i]==Row{i,255,false},"Exhausted slot was relabeled, reordered or wrapped.");
    need(active.live()==0&&active.available()==0&&active.exhausted()==Ledger::capacity,"Exhaustion counters must be derived from every retired255 row.");
    auto cold=roundtrip(active);const auto original=cold;
    rejects<std::overflow_error>([&]{(void)cold.allocate();},"Exhausted snapshot wrapped to a stale body identity.");need(cold==original,"Exhaustion rejection mutated reconstructed ledger.");
}
void invalid_snapshots_are_const_and_atomic() {
    Ledger ledger;ledger.reserve(id(7,0));ledger.reserve(id(19,255));ledger.seal();ledger.release(id(19,255));
    const auto good=ledger.snapshot();const auto original_ledger=ledger;
    for(unsigned mutation=0;mutation<10;++mutation) {
        auto invalid=good;
        switch(mutation) {
        case 0:std::swap(invalid.rows[0],invalid.rows[1]);break;
        case 1:invalid.rows[1].index=invalid.rows[0].index;break;
        case 2:invalid.rows[1].index=Ledger::capacity;break;
        case 3:invalid.rows[1].index=std::numeric_limits<std::uint32_t>::max();break;
        case 4:invalid.rows[0].generation=256;break;
        case 5:invalid.rows[0].generation=std::numeric_limits<std::uint32_t>::max();break;
        case 6:invalid.sealed=false;break;
        case 7:invalid.rows={Row{3,0,false}};invalid.sealed=false;break;
        case 8:invalid.rows.resize(Ledger::capacity+1);break;
        case 9:invalid.rows.insert(invalid.rows.begin(),Row{6,1,true});invalid.rows[1].index=5;break;
        }
        const auto before=invalid;
        rejects<std::invalid_argument>([&]{(void)Ledger::from_snapshot(invalid);},"Malformed sparse ledger snapshot admitted.");
        need(invalid==before&&ledger==original_ledger,"Rejected snapshot changed caller metadata or a live ledger.");
    }
    Ledger::Snapshot sealed_zero{true,{{3,0,false}}};const auto zero=Ledger::from_snapshot(sealed_zero);
    need(zero.snapshot()==sealed_zero&&zero.live()==0&&zero.exhausted()==0&&zero.available()==Ledger::capacity,"Reserved/released gen0 is a valid used slot, not exhausted.");
    // A typed snapshot does not prove completeness against an external history
    // or Jolt inventory. Missing history is a DIFFERENT ledger, never normalized
    // back to an assumed one; outer archive/identity guards own that join.
    auto omitted=good;omitted.rows.pop_back();need(Ledger::from_snapshot(omitted)!=ledger,"Omitted retired history must not reconstruct as the complete original ledger.");
}
}
int main() {
    try {empty_and_partial_startup();mixed_retired_and_future_ids();every_slot_and_exhaustion();invalid_snapshots_are_const_and_atomic();std::cout<<"Runtime body ID snapshots: "<<checks<<" typed history/allocation/admission checks passed.\n";return 0;}
    catch(const std::exception& error){std::cerr<<"Runtime body ID snapshots: "<<error.what()<<'\n';return 1;}
}
