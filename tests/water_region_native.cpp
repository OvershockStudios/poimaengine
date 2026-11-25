// SPDX-License-Identifier: Apache-2.0
#include "poima/water_region.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::json;
using namespace std::string_view_literals;
using poima::water::FaceAxis;
using poima::water::Region;
using poima::water::RegionDefinition;
using poima::water::SurfaceRipples;

constexpr std::array toggle{"use.sluice"sv};
constexpr std::array repeated_toggle{"ignored"sv, "use.sluice"sv, "use.sluice"sv};

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template<class Exception, class Operation>
void rejects(Operation operation, const char* message) {
    try {
        operation();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

RegionDefinition canal() {
    return {
        .config = {.cells_x = 4, .cells_z = 2, .origin_x_m = 20, .origin_z_m = -10},
        .floor_heights_m = {.2, 0, -.25, -.5, 0, .1, -.2, -.5},
        .initial_depths_m = {1, 1.2, 0, 0, 1.2, 1.1, 0, 0},
        .faces = {
            {{1, 1}, FaceAxis::x, {0, .55}, "use.sluice", .75},
            {{2, 1}, FaceAxis::x, {.3, -.15}, "", 1},
            {{1, 0}, FaceAxis::x, {0, .5}, "use.sluice", .5},
        },
        .exchanges = {{{3, 0}, 0, .24}, {{0, 0}, .6, 0}},
        .plan_sha256 = std::string(64, 'a'),
    };
}

void flood_through_authored_actions() {
    const auto definition = canal();
    Region original(definition);
    const auto initial = original.save();
    require(original.plan_sha256() == definition.plan_sha256 && !original.last_report(), "Initial region pin or receipt differs.");
    require(original.face_rules().front().cell.z == 0, "Rules are not in canonical order.");
    for (int tick = 0; tick < 45; ++tick) {
        const auto result = original.step();
        require(std::abs(result.supplied_m3 - .01) < 1e-14 && result.drained_m3 == 0,
                "Metered water did not run each tick, or a drain acted on a dry cell.");
    }
    require(original.grid().sample({2, 0}).depth_m == 0 && original.grid().sample({2, 1}).depth_m == 0,
            "Water leaked through the closed sluices.");
    original.step(repeated_toggle);
    require(original.grid().face({1, 0}, FaceAxis::x).openness == .5 &&
            original.grid().face({1, 1}, FaceAxis::x).openness == .75 &&
            original.grid().face({2, 1}, FaceAxis::x).openness == .3,
            "Repeated edges toggled a rule twice, or altered an unrelated face.");
    require(original.grid().sample({2, 0}).depth_m > 0 && original.grid().sample({2, 1}).depth_m > 0 &&
            original.grid().sample({3, 0}).depth_m == 0, "Flow did not cross exactly one boundary on its first tick.");
    original.step();
    require(original.grid().face({1, 0}, FaceAxis::x).openness == .5, "A held gate was closed without a pressed edge.");
    require(original.grid().sample({3, 0}).depth_m > 0 && original.last_report()->drained_m3 == 0,
            "A drain consumed newly arrived flow before its next tick.");
    original.step();
    require(original.last_report()->drained_m3 > 0, "Metered drain did not observe its available water.");
    const auto checkpoint = original.save();
    require(checkpoint.at("action_face_openness") == Json::array({.5, .75}), "Canonical action face snapshot differs.");

    auto reordered = definition;
    std::reverse(reordered.faces.begin(), reordered.faces.end());
    std::reverse(reordered.exchanges.begin(), reordered.exchanges.end());
    Region replay(reordered);
    replay.restore(Json::parse(checkpoint.dump()), original.grid().tick());
    require(replay.save() == checkpoint, "Serialized checkpoint did not round-trip.");
    for (int tick = 0; tick < 110; ++tick) {
        const auto actions = tick == 24 || tick == 65 ? std::span<const std::string_view>{toggle} : std::span<const std::string_view>{};
        original.step(actions);
        replay.step(actions);
        require(original.save() == replay.save(), "Continued water state or receipts diverged.");
        if (tick == 24)
            require(original.grid().face({1, 0}, FaceAxis::x).openness == 0 &&
                    original.grid().face({1, 1}, FaceAxis::x).openness == 0, "A new edge did not close both sluices.");
    }
    replay.restore(initial, 0);
    require(replay.save() == initial && !replay.last_report(), "Restoring the original tick left dynamic state behind.");
}

void invalid_definitions_are_rejected() {
    auto bad = canal();
    bad.plan_sha256.front() = 'A';
    rejects<std::invalid_argument>([&] { const Region region(bad); }, "Malformed plan pin was accepted.");
    bad = canal();
    bad.config.cells_x = poima::water::max_region_side + 1;
    rejects<std::invalid_argument>([&] { const Region region(bad); }, "Oversized region was accepted.");
    bad = canal();
    bad.faces.resize(poima::water::max_region_faces + 1, bad.faces.front());
    rejects<std::invalid_argument>([&] { const Region region(bad); }, "Too many faces were accepted.");
    bad = canal();
    bad.exchanges.resize(poima::water::max_region_exchanges + 1, bad.exchanges.front());
    rejects<std::invalid_argument>([&] { const Region region(bad); }, "Too many exchanges were accepted.");
    bad = canal();
    bad.faces.push_back(bad.faces.front());
    rejects<std::invalid_argument>([&] { const Region region(bad); }, "Duplicate face was accepted.");
    bad = canal();
    bad.faces.front().action = "Use Sluice";
    rejects<std::invalid_argument>([&] { const Region region(bad); }, "An unknown action syntax was accepted.");
    bad = canal();
    bad.faces.front().action_openness = std::numeric_limits<double>::quiet_NaN();
    rejects<std::invalid_argument>([&] { const Region region(bad); }, "NaN face openness was accepted.");
    bad = canal();
    bad.exchanges.push_back(bad.exchanges.front());
    rejects<std::invalid_argument>([&] { const Region region(bad); }, "Duplicate exchange was accepted.");
    bad = canal();
    bad.exchanges.front().drain_m3_per_second = 1'000'001;
    rejects<std::invalid_argument>([&] { const Region region(bad); }, "An invalid exchange was accepted.");
}

void surface_ripples_are_frozen_presentation_data() {
    auto definition = canal();
    const SurfaceRipples profile{8, .4, 3600, 180};
    Region plain(definition);
    definition.surface_ripples = profile;
    Region rippled(definition);
    require(!plain.surface_ripples() && rippled.surface_ripples() == profile,
            "Water surface ripple selection changed at construction.");
    definition.surface_ripples = SurfaceRipples{.10, 0, 8, -180};
    Region zero_slope(definition);
    require(zero_slope.surface_ripples() == definition.surface_ripples && rippled.surface_ripples() == profile,
            "The frozen ripple profile changed with the authored definition or dropped zero slope.");
    for (const auto& settings : std::array{
            SurfaceRipples{.09, 0, 8, 0}, SurfaceRipples{8.1, 0, 8, 0},
            SurfaceRipples{1, -.01, 8, 0}, SurfaceRipples{1, .41, 8, 0},
            SurfaceRipples{1, 0, 7, 0}, SurfaceRipples{1, 0, 3601, 0},
            SurfaceRipples{1, 0, 8, -181}, SurfaceRipples{1, 0, 8, 181},
            SurfaceRipples{1, std::numeric_limits<double>::quiet_NaN(), 8, 0}}) {
        definition.surface_ripples = settings;
        rejects<std::invalid_argument>([&] { const Region region(definition); },
            "Out-of-bounds ripple settings reached a frozen water region.");
    }
    definition.surface_ripples = profile;
    // A shared synthetic plan pin isolates the physical data from styling.
    for (int tick = 0; tick < 65; ++tick) {
        const auto actions = tick == 12 ? std::span<const std::string_view>{toggle} : std::span<const std::string_view>{};
        plain.step(actions);
        rippled.step(actions);
        require(plain.save() == rippled.save(), "Ripple settings changed the water simulation or save layout.");
    }
    Region restored(definition);
    restored.restore(rippled.save(), rippled.grid().tick());
    require(restored.grid().tick() == 65 && restored.surface_ripples() == profile &&
            restored.save() == rippled.save(), "Cold restore changed the committed water tick or profile.");
}

void restore_and_step_are_atomic() {
    Region original(canal());
    const auto initial = original.save();
    original.step(toggle);
    const auto intact = original.save();
    const auto check_bad_restore = [&](const Json& incompatible, std::uint64_t tick) {
        rejects<std::invalid_argument>([&] { original.restore(incompatible, tick); }, "Invalid snapshot data was accepted.");
        require(original.save() == intact, "Rejected snapshot modified the region.");
    };
    check_bad_restore(intact, 2);
    auto bad = intact;
    bad["plan_sha256"] = std::string(64, 'b');
    check_bad_restore(bad, 1);
    bad = intact;
    bad["tick"] = 1.0;
    check_bad_restore(bad, 1);
    bad = intact;
    bad["unexpected"] = false;
    check_bad_restore(bad, 1);
    bad = intact;
    bad["depths_m"] = Json::array();
    check_bad_restore(bad, 1);
    bad = intact;
    bad["depths_m"][0] = true;
    check_bad_restore(bad, 1);
    bad = intact;
    bad["depths_m"][0] = std::numeric_limits<double>::infinity();
    check_bad_restore(bad, 1);
    bad = intact;
    bad["action_face_openness"][0] = .2;
    check_bad_restore(bad, 1);
    bad = intact;
    bad["last_report"]["volume_after_m3"] = 0;
    check_bad_restore(bad, 1);
    bad = intact;
    bad["last_report"]["volume_before_m3"] = 0;
    check_bad_restore(bad, 1);
    bad = intact;
    bad["last_report"] = nullptr;
    check_bad_restore(bad, 1);
    bad = initial;
    bad["depths_m"][0] = 0;
    check_bad_restore(bad, 0);
    bad = initial;
    bad["action_face_openness"][0] = .5;
    check_bad_restore(bad, 0);

    Region overflowing({
        .config = {.cells_x = 2, .cells_z = 1, .cell_size_m = .01},
        .floor_heights_m = {0, 0},
        .initial_depths_m = {850'000, 0},
        .faces = {{{0, 0}, FaceAxis::x, {0, 0}, "use.sluice", 1}},
        .exchanges = {{{0, 0}, 600, 0}},
        .plan_sha256 = std::string(64, 'c'),
    });
    overflowing.step();
    const auto before_overflow = overflowing.save();
    rejects<std::overflow_error>([&] { overflowing.step(toggle); }, "Grid overflow did not reject the gated tick.");
    require(overflowing.save() == before_overflow && overflowing.grid().face({0, 0}, FaceAxis::x).openness == 0,
            "Failed tick did not restore actual gate state and the prior receipt.");
}

void swell_shares_cell_edges_and_restores_phase() {
    RegionDefinition definition{
        .config={.cells_x=2,.cells_z=1,.origin_x_m=20,.origin_z_m=-10,.relaxation_per_second=0},
        .floor_heights_m={0,0},.initial_depths_m={1,1},.plan_sha256=std::string(64,'d'),
        .optics=poima::WaterOptics{.extinction_per_m={},.scattered_radiance={},.follow_terrain=true},
        .shore_corner_heights_m={0,0,0,0,0,0},
        .surface_swell={{4,.2,64,0}},
    };
    Region live(definition);
    const double amplitude=.4/std::numbers::pi;
    // Near-Z wedge has offsets of 0 and A along the edge and A/sqrt(2)
    // at its center. A horizontal sweep enters the rising surface.
    const auto y=1+amplitude*(.4+.2*(std::numbers::sqrt2-1));
    const auto sloped=live.surface_entry({20.3,y,-9.8},{20.5,y,-9.8});
    const auto dx=amplitude,dz=amplitude*(std::numbers::sqrt2-1);
    const auto scale=1/std::hypot(dx,1.,dz);
    require(sloped && sloped->cell && sloped->cell->x==0 && sloped->cell->z==0 && std::abs(sloped->fraction-.5)<1e-12 &&
        std::abs(sloped->normal[0]+dx*scale)<1e-13 && std::abs(sloped->normal[2]+dz*scale)<1e-13 &&
        std::abs(sloped->normal[1]-scale)<1e-13 &&
        !live.surface_entry({20.5,y,-9.8},{20.3,y,-9.8}),"A fitted swell entry or its normal differs.");
    const auto at_border=live.sample_world(21,-9.65);
    require(std::abs(at_border->surface_height_m-(1+amplitude))<1e-13,"Swell crest differs at the shared edge.");
    for (double x : {21-1e-8,21+1e-8})
        require(std::abs(live.sample_world(x,-9.65)->surface_height_m-at_border->surface_height_m)<2e-9,
            "Shared water edge is discontinuous.");
    for (int tick=0;tick<32;++tick) live.step();
    require(std::abs(live.sample_world(21,-9.65)->surface_height_m-(1-amplitude))<1e-13 &&
            live.grid().volume_m3()==2.0,"Swell did not reverse cleanly over the static volume.");
    Region cold(definition);
    cold.restore(live.save(),32);
    std::optional<std::array<double,3>> a,b;
    const auto first=live.sample_world(20.4,-9.8,&a);
    const auto restored=cold.sample_world(20.4,-9.8,&b);
    require(a && a==b && first->surface_height_m==restored->surface_height_m,
        "Cold-restored position or normal differs from the committed wave.");
    const auto live_entry=live.surface_entry({20.4,2,-9.8},{20.4,0,-9.8});
    const auto restored_entry=cold.surface_entry({20.4,2,-9.8},{20.4,0,-9.8});
    require(live_entry && restored_entry && live_entry->position_m==restored_entry->position_m &&
        live_entry->normal==restored_entry->normal && std::abs(live_entry->normal[1]-(*a)[1])<1e-14,
        "Cold-restored crossings do not match the saved swell phase.");

    definition.surface_swell[0].wavelength_m=3.99;
    rejects<std::invalid_argument>([&] { (void)Region(definition); },"Undersampled swell was admitted.");
    definition.surface_swell[0]={80,.2,64,0};
    rejects<std::invalid_argument>([&] { (void)Region(definition); },"Excessive swell amplitude was admitted.");
    definition.surface_swell[0]={4,.2,64,0};
    definition.optics.reset();
    definition.shore_corner_heights_m.clear();
    rejects<std::invalid_argument>([&] { (void)Region(definition); },"Swell without fitted optics was admitted.");
}

void offshore_joins_only_the_selected_edge() {
    RegionDefinition definition{
        .config={.cells_x=2,.cells_z=2,.origin_x_m=20,.origin_z_m=-10,.relaxation_per_second=0},
        .floor_heights_m={0,0,0,0},.initial_depths_m={1.125,.875,1.25,.75},
        .plan_sha256=std::string(64,'e'),
        .optics=poima::WaterOptics{.extinction_per_m={},.scattered_radiance={},.follow_terrain=true},
        .shore_corner_heights_m={0,0,0,0,0,0,0,0,0},
        .surface_swell={{4,.2,64,0}},
    };
    struct Shore {
        poima::WaterEdge edge;
        double x,z,dx,dz;
        bool outside_at_edge;
    };
    constexpr std::array shores{
        Shore{poima::WaterEdge::min_z,20.4,-10,0,-1,false},
        Shore{poima::WaterEdge::max_x,22,-9.6,1,0,true},
        Shore{poima::WaterEdge::max_z,20.4,-8,0,1,true},
        Shore{poima::WaterEdge::min_x,20,-9.6,-1,0,false},
    };
    for (const auto& shore : shores) {
        definition.optics->offshore=poima::WaterOffshore{shore.edge,1,20};
        Region region(definition);
        for (int phase=0;phase<2;++phase) {
            constexpr double epsilon=1e-7;
            const auto inside=region.sample_world(shore.x-shore.dx*epsilon,shore.z-shore.dz*epsilon);
            require(inside && std::abs(inside->surface_height_m-1)<epsilon,
                "Fitted water does not meet its offshore boundary.");
            require(!region.offshore_levels(shore.x-shore.dx*epsilon,shore.z-shore.dz*epsilon) &&
                region.offshore_levels(shore.x,shore.z).has_value()==shore.outside_at_edge,
                "The grid and offshore water disagree on boundary ownership.");
            const double x=shore.x+shore.dx*1e4,z=shore.z+shore.dz*1e4;
            const auto entry=region.surface_entry({x,2,z},{x,0,z});
            require(entry && !entry->cell && entry->normal==std::array{0.,1.,0.} &&
                entry->position_m==std::array{x,1.,z} && entry->fraction==.5,
                "The offshore entry differs from the shared medium plane.");
            require(region.offshore_levels(x,z)==std::optional(std::array{-19.,1.}) &&
                region.submersion_at(x,.75,z)==.25 && region.submersion_at(x,-20,z)==0 &&
                region.submersion_at(x,1,z)==0 &&
                !region.offshore_levels(shore.x-shore.dx*1e4,shore.z-shore.dz*1e4),
                "Offshore depth or directional ownership is incorrect.");
            for (int tick=0;tick<32;++tick) region.step();
        }
        require(region.grid().volume_m3()==4, "Offshore water changed the conserved local volume.");
    }

    auto invalid=definition;
    invalid.optics->follow_terrain=false;
    invalid.shore_corner_heights_m.clear();
    invalid.surface_swell.clear();
    rejects<std::invalid_argument>([&] { (void)Region(invalid); }, "An unfitted offshore edge was accepted.");
    invalid=definition;
    invalid.optics->offshore->depth_m=.01;
    rejects<std::invalid_argument>([&] { (void)Region(invalid); }, "An out-of-range offshore depth was accepted.");
}

void surface_segments_cross_cells_and_clip_to_the_wet_bed() {
    RegionDefinition definition{
        .config={.cells_x=4,.cells_z=4,.origin_x_m=17,.origin_z_m=-12,.relaxation_per_second=0},
        .floor_heights_m=std::vector<double>(16,0),.initial_depths_m=std::vector<double>(16,0),
        .plan_sha256=std::string(64,'f'),
    };
    definition.initial_depths_m[5]=.125;
    Region region(definition);
    for (bool reverse : {false,true}) {
        const std::array<double,3> a{16,.75,-13},b{21,-.5,-8};
        const auto hit=region.surface_entry(reverse ? std::array{b[0],a[1],b[2]} : a,
            reverse ? std::array{a[0],b[1],a[2]} : b);
        require(hit && hit->cell && hit->cell->x==1 && hit->cell->z==1 && hit->fraction==.5 &&
            hit->position_m==std::array{18.5,.125,-10.5},"A diagonal sweep missed the isolated wet cell.");
    }
    require(!region.surface_entry({16,.75,-11.25},{21,-.5,-11.25}) &&
        !region.surface_entry({18.5,0,-10.5},{18.5,.5,-10.5}),"An empty column or upward exit reported an entry.");
    const auto boundary=region.surface_entry({18,1,-10.75},{18,.125,-10.75});
    require(boundary && boundary->fraction==1 && boundary->cell && boundary->cell->x==1 && boundary->cell->z==1,
        "An endpoint entry on a shared grid edge was lost.");

    definition.config.cells_x=definition.config.cells_z=1;
    definition.floor_heights_m={.4};
    definition.initial_depths_m={.2};
    definition.optics=poima::WaterOptics{.extinction_per_m={},.scattered_radiance={},.follow_terrain=true};
    definition.shore_corner_heights_m={1,1,0,0};
    Region shore(definition);
    const auto wet=shore.surface_entry({17.5,1,-11.1},{17.5,0,-11.1});
    require(wet && std::abs(wet->position_m[1]-.6)<1e-13 &&
        !shore.surface_entry({17.5,1,-11.9},{17.5,0,-11.9}),
        "Fitted water failed to clip the stage against its local bed.");
}

}

int main() {
    try {
        flood_through_authored_actions();
        invalid_definitions_are_rejected();
        surface_ripples_are_frozen_presentation_data();
        restore_and_step_are_atomic();
        swell_shares_cell_edges_and_restores_phase();
        offshore_joins_only_the_selected_edge();
        surface_segments_cross_cells_and_clip_to_the_wet_bed();
        std::cout << "Water region tests passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
