// SPDX-License-Identifier: Apache-2.0
#include "runtime_entity_ids.hpp"
#include <array>
#include <iostream>
#include <limits>
#include <type_traits>
using poima::RuntimeEntityIds;
namespace {
bool same(PoimaEntityId a,PoimaEntityId b) { return a.high==b.high && a.low==b.low; }
bool same(RuntimeEntityIds::Cursor a,RuntimeEntityIds::Cursor b) { return same(a.next,b.next) && a.exhausted==b.exhausted; }
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
template<class F>void rejects(F action) { bool rejected=false;try {action();}catch(const std::exception&){rejected=true;}check(rejected,"Invalid identity operation succeeded."); }
constexpr auto max=std::numeric_limits<std::uint64_t>::max();
void exclusions_and_retirement() {
    const std::array<PoimaEntityId,5> authored{{{0,5},{0,2},{max,max},{0,1},{0,4}}};
    RuntimeEntityIds ids(authored);check(same(ids.allocate(),{0,3}),"Authored exclusions changed first identity.");
    check(same(ids.allocate(),{0,6}),"Authored exclusions changed next identity.");
    check(ids.allocated({0,3}) && ids.allocated({0,6}) && !ids.allocated({0,5}) && !ids.allocated({0,7}) && !ids.allocated({0,0}),"Allocated/authored/null classifications differ.");
    // Retire every spawned object: nothing is released into this allocator.
    check(same(ids.allocate(),{0,7}),"Retired public identity was recycled.");
    check(ids.authored({max,max}),"High-bit authored identity was lost.");
}
void checkpoint_and_restore() {
    const std::array<PoimaEntityId,2> authored{{{0,3},{0,7}}};RuntimeEntityIds ids(authored);
    (void)ids.allocate();const auto checkpoint=ids;
    const auto a=ids.allocate(),b=ids.allocate();ids=checkpoint;
    check(!ids.allocated(a) && !ids.allocated(b),"Rollback retained tentative identity frontier.");
    check(same(ids.allocate(),a) && same(ids.allocate(),b),"Failed attempt consumed future public identities.");
    RuntimeEntityIds restored(authored,ids.cursor());
    for(unsigned i=0;i<10000;++i)check(same(ids.allocate(),restored.allocate()),"Saved frontier changed continued allocation.");
}
void carry_and_exhaustion() {
    RuntimeEntityIds ids({},{{42,max},false});
    check(same(ids.allocate(),{42,max}) && same(ids.allocate(),{43,0}),"128-bit frontier did not carry exactly.");
    const std::array<PoimaEntityId,2> crossing{{{43,0},{42,max}}};
    RuntimeEntityIds skipped(crossing,{{42,max},false});
    check(same(skipped.allocate(),{43,1}),"Authored exclusions spanning a carry were not skipped.");
    RuntimeEntityIds last({},{{max,max},false});
    check(same(last.allocate(),{max,max}) && last.cursor().exhausted && same(last.cursor().next,{0,0}),"Final identity did not exhaust without wrap.");
    auto before=last.cursor();rejects([&]{(void)last.allocate();});check(same(before,last.cursor()),"Failed allocation changed exhausted frontier.");
    RuntimeEntityIds restored({},last.cursor());check(restored.allocated({max,max}),"Exhausted saved frontier did not restore.");rejects([&]{(void)restored.allocate();});
    const std::array<PoimaEntityId,2> authored{{{max,max-1},{max,max}}};RuntimeEntityIds blocked(authored,{{max,max-1},false});
    before=blocked.cursor();rejects([&]{(void)blocked.allocate();});check(same(before,blocked.cursor()),"Exclusion exhaustion partially advanced frontier.");
}
void validation_and_full_inventory() {
    const std::array<PoimaEntityId,1> zero{{{0,0}}};rejects([&]{RuntimeEntityIds ids(zero);});
    const std::array<PoimaEntityId,2> duplicates{{{9,4},{9,4}}};rejects([&]{RuntimeEntityIds ids(duplicates);});
    rejects([]{RuntimeEntityIds ids({},{{0,0},false});});rejects([]{RuntimeEntityIds ids({},{{0,1},true});});
    std::vector<PoimaEntityId> authored;for(unsigned i=10000;i>0;--i)authored.push_back({0,i});
    RuntimeEntityIds ids(authored);check(same(ids.allocate(),{0,10001}),"Full unsorted authored inventory was not excluded.");
    authored.push_back({0,10001});rejects([&]{RuntimeEntityIds invalid(authored);});
}
}
int main() {
    static_assert(std::is_nothrow_copy_constructible_v<RuntimeEntityIds> && std::is_nothrow_copy_assignable_v<RuntimeEntityIds>);
    try {exclusions_and_retirement();checkpoint_and_restore();carry_and_exhaustion();validation_and_full_inventory();std::cout<<"Runtime public entity IDs: 4 groups passed.\n";return 0;}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
