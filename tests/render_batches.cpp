// SPDX-License-Identifier: Apache-2.0
#include "poima/render_batches.hpp"
#include <array>
#include <iostream>
#include <string>

namespace {
using namespace poima::render_batches;
std::uint64_t checks=0;
void check(bool value,const char* message) {
    ++checks;
    if(!value)throw std::runtime_error(message);
}
Key key(std::uintptr_t value=1) {return {value,2,3,4,36,5};}
Candidate draw(std::uint32_t object,bool visible=true,Key value=key()) {
    return {object,visible,value};
}
void unchanged(std::span<const Candidate> before,std::span<const Candidate> after) {
    check(before.size()==after.size(),"Candidate membership changed");
    for(std::size_t i=0;i<before.size();++i)
        check(before[i].object_index==after[i].object_index && before[i].visible==after[i].visible &&
            before[i].key==after[i].key,"Planner mutated caller-owned candidates");
}
// Expand original candidate ranges independently: every visible source slot
// appears once, in its original order, with no hidden/compacted resource IDs.
void invariants(std::span<const Candidate> input,const std::vector<Batch>& batches,bool enabled) {
    std::vector<std::size_t> visible;
    for(std::size_t i=0;i<input.size();++i)if(input[i].visible)visible.push_back(i);
    std::size_t cursor=0;
    for(const auto& batch:batches) {
        check(batch.count>0,"Empty batch");
        check(std::uint64_t{batch.first}+batch.count<=input.size(),"Batch exceeds original candidate range");
        if(!enabled)check(batch.count==1,"Direct path merged candidates");
        const auto& first=input[batch.first];
        for(std::uint64_t offset=0;offset<batch.count;++offset) {
            const auto index=std::size_t{batch.first}+static_cast<std::size_t>(offset);
            check(cursor<visible.size() && visible[cursor]==index,"Original visible order/membership changed");
            const auto& item=input[index];
            check(item.visible && item.key==first.key,"Batch crossed culling or exact key boundary");
            check(std::uint64_t{item.object_index}==std::uint64_t{first.object_index}+offset,"Object addressing wrapped, compacted or rebased");
            ++cursor;
        }
    }
    check(cursor==visible.size(),"Planner omitted visible candidates");
}
void expect(const std::vector<Candidate>& input,const std::vector<Batch>& expected,bool enabled=true) {
    const auto original=input;
    const auto actual=plan(input,enabled);
    check(actual==expected,"Independent expected partition differs");
    invariants(input,actual,enabled);
    unchanged(original,input);
    check(plan(input,enabled)==actual,"Value planner is not deterministic");
}

void small_tables() {
    expect({},{});
    expect({draw(7,false),draw(8,false)},{});
    expect({draw(41)},{{0,1}});
    expect({draw(41,false),draw(42)},{{1,1}});
    expect({draw(41),draw(42),draw(43)},{{0,3}});
    expect({draw(41),draw(42),draw(43)},{{0,1},{1,1},{2,1}},false);
    // Different visible slots can never be compacted across the culled slot.
    expect({draw(9),draw(10,false),draw(10),draw(11),draw(12,false),draw(12)},{{0,1},{2,2},{5,1}});
    expect({draw(80),draw(81),draw(900),draw(901),draw(901),draw(900)},{{0,2},{2,2},{4,1},{5,1}});
    expect({draw(80),draw(81),draw(900),draw(901),draw(901),draw(900)},{{0,1},{1,1},{2,1},{3,1},{4,1},{5,1}},false);
}

void every_key_field() {
    const Key original=key();
    std::array<Key,6> changed{original,original,original,original,original,original};
    changed[0].vertices=17;changed[1].indices=18;changed[2].previous_vertices=19;
    changed[3].bindings=20;changed[4].count=37;changed[5].pipeline=6;
    for(const auto& different:changed) {
        check(different!=original,"Fixture failed to distinguish one exact key member");
        expect({draw(0,true,original),draw(1,true,different),draw(2,true,different),draw(3,true,original)},{{0,1},{1,2},{3,1}});
    }
    // A,B,A remains three batches; matching nonadjacent resources do not help.
    expect({draw(40,true,key(100)),draw(41,true,key(200)),draw(42,true,key(100))},{{0,1},{1,1},{2,1}});
    // No resource identity is a sentinel in this portable value-only layer.
    const Key zero{};
    expect({draw(70,true,zero),draw(71,true,zero)},{{0,2}});
    auto no_indices=original;no_indices.indices=0;
    expect({draw(10,true,no_indices),draw(11,true,original),draw(12,true,no_indices)},{{0,1},{1,1},{2,1}});
}

void integer_boundaries() {
    constexpr auto maximum=std::numeric_limits<std::uint32_t>::max();
    constexpr auto address=std::numeric_limits<std::uintptr_t>::max();
    const Key extremes{address,address,address,address,maximum,maximum};
    expect({draw(maximum-2,true,extremes),draw(maximum-1,true,extremes),draw(maximum,true,extremes)},{{0,3}});
    expect({draw(maximum-1),draw(maximum),draw(0),draw(1)},{{0,2},{2,2}});
    expect({draw(maximum),draw(0)},{{0,1},{1,1}});
    expect({draw(0),draw(maximum),draw(0)},{{0,1},{1,1},{2,1}});
    // Batch.first must be a source index even when object IDs are unrelated.
    expect({draw(maximum,false),draw(250),draw(251),draw(0,false),draw(maximum)},{{1,2},{4,1}});
    auto low=extremes;low.previous_vertices=0;
    expect({draw(maximum-2,true,extremes),draw(maximum-1,true,low),draw(maximum,true,low)},{{0,1},{1,2}});
    // A >UINT32_MAX span is not fabricated: std::span requires an actually
    // valid backing range. The pre-allocation size guard is source-reviewed.
}

void exhaustive_small_visibility() {
    // All masks over an explicit eight-slot table. Visible runs below are
    // independent hand-declared eligibility islands; no expected scan mirrors
    // the production planner. Culling can split an island, never join islands.
    const std::array<Candidate,8> table{draw(10),draw(11),draw(50),draw(51),
        draw(52,true,key(2)),draw(53,true,key(2)),draw(99),draw(100)};
    constexpr std::array<unsigned,8> island{0,0,1,1,2,2,3,3};
    for(unsigned mask=0;mask<256;++mask) {
        std::vector<Candidate> input(table.begin(),table.end());
        unsigned expected=0;
        for(unsigned i=0;i<8;++i) {
            input[i].visible=(mask&(1u<<i))!=0;
            if(input[i].visible && (i==0 || !(mask&(1u<<(i-1))) || island[i]!=island[i-1]))++expected;
        }
        const auto batched=plan(input);check(batched.size()==expected,"Visibility/island table batch count differs");
        invariants(input,batched,true);invariants(input,plan(input,false),false);
    }
}

void wide_batch_and_gaps() {
    constexpr std::uint32_t width=65536;
    std::vector<Candidate> input;input.reserve(width+3);
    // Cross the uint16 boundary in one actual batch; index and count are uint32.
    for(std::uint32_t i=0;i<width;++i)input.push_back(draw(i));
    input.push_back(draw(width,false));input.push_back(draw(width));input.push_back(draw(width+1));
    expect(input,{{0,width},{width+1,2}});
    const auto direct=plan(input,false);check(direct.size()==width+2,"Large direct path object count differs");
    invariants(input,direct,false);
}

void many_keys_and_deterministic_cohort() {
    constexpr std::uint32_t groups=4096;
    std::vector<Candidate> input;std::vector<Batch> expected;
    std::uint64_t visible=0;
    for(std::uint32_t group=0;group<groups;++group) {
        const auto length=23+group%17;
        // Every group has its own exact full key, including identities much
        // larger than object IDs, and a culled delimiter even if keys recur.
        const Key value{std::uintptr_t{group}+1,std::uintptr_t{group}+5000,
            std::uintptr_t{group}+10000,std::uintptr_t{group}+15000,3+group%11,group%7};
        expected.push_back({static_cast<std::uint32_t>(input.size()),length});
        for(std::uint32_t j=0;j<length;++j)input.push_back(draw(group*64+j,true,value));
        input.push_back(draw(group*64+length,false,value));visible+=length;
    }
    expect(input,expected);
    const auto direct=plan(input,false);check(direct.size()==visible,"Dense direct path count conservation differs");
    invariants(input,direct,false);
    // Keys with identical vertices but independently skinned previous streams
    // must remain separate despite consecutive object IDs and all other values.
    std::vector<Candidate> skinned;
    for(std::uint32_t i=0;i<4096;++i) {auto value=key();value.previous_vertices=std::uintptr_t{i}+1;skinned.push_back(draw(i,true,value));}
    const auto separate=plan(skinned);check(separate.size()==skinned.size(),"Different previous streams were merged");
    invariants(skinned,separate,true);
}
} // namespace

int main() {
    try {
        small_tables();every_key_field();integer_boundaries();exhaustive_small_visibility();
        wide_batch_and_gaps();many_keys_and_deterministic_cohort();
        std::cout<<"6 portable render batch groups passed, "<<checks<<" checks\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';return 1;
    }
}
