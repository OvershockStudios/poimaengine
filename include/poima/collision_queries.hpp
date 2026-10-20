// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace poima {
inline constexpr double capsule_query_collision_tolerance=1e-5;
inline constexpr std::uint32_t capsule_query_body_budget=512,capsule_query_contact_budget=2048,capsule_query_tick_budget=32;
// Fixed-orientation capsule translated along direction * distance. Position is
// its center; cylinder half-height excludes the spherical caps, on local Y.
// A zero half-height selects a sphere; zero distance performs an overlap query.
// Direction is nonzero even for overlap queries; every component is bounded
// to +/-1e6 and its length is at least1e-12. Center/endpoints remain +/-1e6m.
struct CapsuleCastRequest {
    std::array<double,3> position{};
    std::array<double,4> rotation{0,0,0,1};
    double radius=.05,half_height=.2;
    std::array<double,3> direction{0,0,-1};
    double distance=1;
    std::vector<std::string> ignore;
    bool operator==(const CapsuleCastRequest&) const = default;
};
struct CapsuleCastHit {
    std::string entity;
    double fraction=0,distance=0;
    // Contact points, not shape centers. Finite +/-1e12m because an allowed
    // center at the input boundary can have support points beyond that bound.
    std::array<double,3> point_on_query{},point_on_obstacle{};
    std::optional<std::array<double,3>> normal;
    double penetration_depth=0;
    std::optional<std::uint32_t> triangle;
    // A blocking contact found before movement, including touching within the
    // fixed narrow-phase tolerance; no clear starting volume is implied.
    bool initial_overlap=false;
    bool operator==(const CapsuleCastHit&) const = default;
};
// Conservative authored envelope for selected foreground geometry. It is not
// an automatic fit of arbitrary imported meshes or animated arm poses.
struct ViewmodelClearanceProfile {
    std::array<double,3> position{};
    std::array<double,4> rotation{0,0,0,1};
    double radius=.05,half_height=.2,max_retraction=.6,skin=.005;
    bool operator==(const ViewmodelClearanceProfile&) const = default;
};
enum class ViewmodelClearanceStatus : std::uint32_t { Clear=0,Retracted=1,BlockedAnchor=2,BlockedFinal=3 };
struct ViewmodelClearanceResult {
    ViewmodelClearanceProfile profile;
    CapsuleCastRequest cast;
    std::optional<CapsuleCastHit> anchor_hit,sweep_hit,final_hit;
    double retraction=0;
    ViewmodelClearanceStatus status=ViewmodelClearanceStatus::BlockedFinal;
    bool resolved=false,suppressed=false;
    std::vector<std::string> ignored_entities,suppressed_entities;
    bool operator==(const ViewmodelClearanceResult&) const = default;
};
// Pure authoring/query validation, available without the physics backend.
void validate_capsule_cast_request(const CapsuleCastRequest& request);
void validate_viewmodel_clearance_profile(const ViewmodelClearanceProfile& profile);
}
