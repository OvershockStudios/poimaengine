// SPDX-License-Identifier: Apache-2.0
#include "poima/render_batches.hpp"
#include "../src/render_shadow_batches.hpp"
#include <array>
#include <iostream>
#include <string>

namespace {
using namespace poima::render_batches;
namespace shadows=poima::render_shadow_batches;
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
    std::array<Key,8> changed{original,original,original,original,original,original,original,original};
    changed[0].vertices=17;changed[1].indices=18;changed[2].previous_vertices=19;
    changed[3].bindings=20;changed[4].count=37;changed[5].pipeline=6;
    changed[6].vertex_offset=144;changed[7].index_offset=12;
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

using ShadowPlans=std::vector<std::vector<Batch>>;

// Reconstruct the former renderer path: scan every original draw for every
// layer and retain invisible slots before applying the existing draw planner.
ShadowPlans reference_shadow_plan(std::span<const shadows::Caster> casters,std::uint32_t layers,bool enabled) {
    ShadowPlans expected;
    std::vector<Candidate> layer;
    layer.reserve(casters.size());
    for(std::uint32_t view=0;view<layers;++view) {
        layer.clear();
        for(const auto& caster:casters)
            layer.push_back(draw(caster.object_index,(caster.layer_mask&(1u<<view))!=0,caster.key));
        expected.push_back(plan(layer,enabled));
    }
    return expected;
}

void expect_shadow_plan(std::span<const shadows::Caster> casters,std::uint32_t layers) {
    for(const bool enabled:{false,true}) {
        const auto actual=shadows::plan(casters,layers,enabled);
        check(actual==reference_shadow_plan(casters,layers,enabled),"Sparse shadow batches differ from per-view reference");
        check(shadows::plan(casters,layers,enabled)==actual,"Shadow planner is not deterministic");
    }
}

void shadow_layer_partitions() {
    auto index_offset=key();index_offset.index_offset=8192;
    auto vertex_offset=key();vertex_offset.vertex_offset=4096;
    const std::array<shadows::Caster,10> casters{{
        {10,3,key()},{11,1,key()},{12,3,key()},{13,3,index_offset},{14,3,index_offset},
        {15,2,vertex_offset},{16,3,vertex_offset},{17,1,key()},{18,0,key()},{19,3,key()}
    }};
    const ShadowPlans expected{
        {{0,3},{3,2},{6,1},{7,1},{9,1}},
        {{0,1},{2,1},{3,2},{5,2},{9,1}}
    };
    check(shadows::plan(casters,2,true)==expected,"Shadow gaps and geometry offsets changed the explicit partition");
    expect_shadow_plan(casters,2);
    const std::array<shadows::Caster,2> no_members{{{8,0,key()},{9,0,key()}}};
    expect_shadow_plan(no_members,0);
    expect_shadow_plan(no_members,16);
    expect_shadow_plan({},0);
    expect_shadow_plan({},16);
}

void shadow_exhaustive_masks_and_wide_cohort() {
    auto changed=key();changed.bindings=123;changed.pipeline=2;
    std::array<shadows::Caster,4> casters{{{5,0,key()},{6,0,key()},{7,0,changed},{12,0,changed}}};
    for(std::uint32_t masks=0;masks<(1u<<12);++masks) {
        for(std::size_t i=0;i<casters.size();++i)casters[i].layer_mask=(masks>>(3*i))&7u;
        expect_shadow_plan(casters,3);
    }

    std::vector<shadows::Caster> cohort;
    cohort.reserve(1024);
    for(std::uint32_t layers=0;layers<=16;++layers) {
        const auto allowed=(1u<<layers)-1u;
        cohort.clear();
        for(std::uint32_t i=0;i<1024;++i) {
            const auto group=i/7;
            auto identity=key(1+group%19);
            identity.bindings=100+group%7;
            identity.pipeline=group%2;
            identity.previous_vertices=group%11;
            identity.vertex_offset=std::uint64_t{group%4}<<35;
            identity.index_offset=group%5*96;
            const std::array masks{0u,1u<<(i%16),allowed,((i*0x9e37u)^(i>>2))};
            cohort.push_back({i+i/13,masks[(i/5)%masks.size()]&allowed,identity});
        }
        expect_shadow_plan(cohort,layers);
    }
}

void shadow_count_and_index_boundaries() {
    constexpr auto maximum=std::numeric_limits<std::uint32_t>::max();
    constexpr auto ends=1u|(1u<<15);
    const std::array<shadows::Caster,5> wrap{{
        {maximum-1,ends,key()},{maximum,ends,key()},{0,ends,key()},{1,ends,key()},{2,1,key()}
    }};
    expect_shadow_plan(wrap,16);
    const auto actual=shadows::plan(wrap,16,true);
    check(actual.front()==std::vector<Batch>{{0,2},{2,3}} && actual.back()==std::vector<Batch>{{0,2},{2,2}},
        "Shadow planner merged wrapped object IDs or lost the highest layer");

    constexpr std::uint32_t width=65536;
    std::vector<shadows::Caster> dense;
    dense.reserve(width+3);
    for(std::uint32_t i=0;i<width;++i)dense.push_back({i,1,key()});
    dense.push_back({width,0,key()});
    dense.push_back({width,1,key()});
    dense.push_back({width+1,1,key()});
    expect_shadow_plan(dense,1);
    check(shadows::plan(dense,1,true)==ShadowPlans{{{0,width},{width+1,2}}},
        "Shadow count or source index truncated at the uint16 boundary");
}

void shadow_invalid_layer_masks() {
    const auto rejected=[](std::span<const shadows::Caster> casters,std::uint32_t layers) {
        for(const bool enabled:{false,true}) {
            bool invalid=false;
            try {(void)shadows::plan(casters,layers,enabled);}
            catch(const std::invalid_argument&) {invalid=true;}
            check(invalid,"Invalid shadow view count or mask was accepted");
        }
    };
    rejected({},17);
    rejected({},std::numeric_limits<std::uint32_t>::max());
    const std::array<shadows::Caster,1> low{{{0,1,key()}}};
    rejected(low,0);
    const std::array<shadows::Caster,2> inactive{{{0,1,key()},{1,9,key()}}};
    rejected(inactive,3);
    const std::array<shadows::Caster,1> beyond_limit{{{0,1u<<16,key()}}};
    rejected(beyond_limit,16);
    const std::array<shadows::Caster,1> top_bit{{{0,1u<<31,key()}}};
    rejected(top_bit,16);
    // As with the regular planner, an oversized source span needs real storage.
}

void shadow_index_range_keys() {
    using poima::render_shadow_index_ranges::Range;
    auto original=key();original.index_offset=24;
    auto different_count=original;different_count.count=39;
    const std::array<Range,2> trimmed{{{3,18},{0,36}}};
    const std::array<Range,2> differently_trimmed{{{6,18},{0,36}}};
    const std::array<Range,2> first_inactive{{{0,0},{0,36}}};
    const std::array<Range,2> explicit_full{{{0,36},{0,36}}};
    const std::array<shadows::Caster,8> casters{{
        {10,3,original,trimmed},{11,3,original,trimmed},{12,3,original,differently_trimmed},
        {13,2,original,first_inactive},{14,3,original,differently_trimmed},{15,3,original},
        {16,3,original,explicit_full},{17,3,different_count,explicit_full}
    }};
    const ShadowPlans expected{{{0,2},{2,1},{4,1},{5,2},{7,1}},{{0,7},{7,1}}};
    check(shadows::plan(casters,2,true)==expected,"Range keys merged distinct spans or split equal full ranges.");
    check(shadows::plan(casters,2,false)==reference_shadow_plan(casters,2,false),
        "Range-aware direct draws changed caster order or mask membership.");

    // Compensating first indices yield the same effective byte offset, but
    // must not erase differences in the original geometry identity.
    auto shifted=original;shifted.index_offset=36;
    const std::array<Range,1> first{{{3,18}}},second{{{0,18}}};
    const std::array<shadows::Caster,2> offset_alias{{{1,1,original,first},{2,1,shifted,second}}};
    check(shadows::plan(offset_alias,1,true)==ShadowPlans{{{0,1},{1,1}}},
        "Matching trimmed byte offsets replaced distinct original index offsets.");

    const auto rejects=[](Key value,Range range) {
        const std::array<Range,1> selection{range};
        const std::array<shadows::Caster,1> input{{{0,1,value,selection}}};
        bool invalid=false;
        try {(void)shadows::plan(input,1,true);}
        catch(const std::invalid_argument&) {invalid=true;}
        check(invalid,"Invalid selected shadow index range was accepted.");
    };
    rejects(original,{0,0});
    rejects(original,{1,18});
    rejects(original,{3,4});
    rejects(original,{30,9});
    auto maximum=original;maximum.count=std::numeric_limits<std::uint32_t>::max();
    rejects(maximum,{maximum.count-3,6});

    const std::array<shadows::Caster,1> missing_slot{{{0,2,original,first}}};
    bool incomplete=false;
    try {(void)shadows::plan(missing_slot,2,true);}
    catch(const std::invalid_argument&) {incomplete=true;}
    check(incomplete,"Shadow range span omitted a requested layer slot.");
}
} // namespace

int main() {
    try {
        small_tables();every_key_field();integer_boundaries();exhaustive_small_visibility();
        wide_batch_and_gaps();many_keys_and_deterministic_cohort();
        shadow_layer_partitions();shadow_exhaustive_masks_and_wide_cohort();
        shadow_count_and_index_boundaries();shadow_invalid_layer_masks();shadow_index_range_keys();
        std::cout<<"11 portable render batch groups passed, "<<checks<<" checks\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';return 1;
    }
}
