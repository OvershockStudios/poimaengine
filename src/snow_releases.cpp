// SPDX-License-Identifier: Apache-2.0
#include "poima/snow_releases.hpp"
#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace poima::snow {
namespace {

using Json = nlohmann::json;
constexpr std::uint64_t max_exact_integer = 9007199254740991ULL;
constexpr std::uint32_t max_ik_slot = 8;
constexpr unsigned cell_units = static_cast<unsigned>(spacing_m / unit_m);
constexpr std::uint32_t tile_triangle_count = 2 * tile_cells * tile_cells;

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

void require_fields(const Json& value, std::initializer_list<const char*> fields, const char* message) {
    require(value.is_object() && value.size() == fields.size() &&
        std::all_of(fields.begin(), fields.end(), [&](const char* field) { return value.contains(field); }), message);
}

std::uint64_t integer(const Json& value, std::uint64_t maximum, const char* message) {
    require(value.is_number_integer(), message);
    const auto number = value.get<std::uint64_t>();
    require(number <= maximum, message);
    return number;
}

bool valid_rig(std::string_view id) noexcept {
    return id.size() == RigKey{}.size() && id.find_first_not_of("0123456789abcdef") == std::string_view::npos &&
        id.find_first_not_of('0') != std::string_view::npos;
}

std::string_view rig_id(const RigKey& key) noexcept {
    return {key.data(), key.size()};
}

auto holding_key(const FootReleaseIntent& intent) noexcept {
    return std::tie(intent.rig, intent.slot);
}

auto ready_key(const FootReleaseIntent& intent) noexcept {
    return std::tie(intent.released_tick, intent.acquired_tick, intent.rig, intent.slot);
}

bool same_acquisition(const FootReleaseIntent& a, const FootReleaseIntent& b) noexcept {
    return a.acquired_tick == b.acquired_tick && holding_key(a) == holding_key(b);
}

bool uses_heading(const Definition& definition) noexcept {
    return definition.foot_release && definition.foot_release->sole.has_value();
}

void validate_intent(const FootReleaseIntent& intent, bool ready, bool shaped, std::uint64_t tick) {
    require(valid_rig(rig_id(intent.rig)), "Snow foot release rig must be a nonzero lowercase entity ID.");
    require(intent.slot >= 1 && intent.slot <= max_ik_slot, "Snow foot release IK slot is out of range.");
    require(intent.acquired_tick >= 1 && intent.acquired_tick <= tick, "Snow foot release acquisition tick is out of range.");
    require(ready ? intent.released_tick > intent.acquired_tick && intent.released_tick <= tick : intent.released_tick == 0,
        "Snow foot release tick is out of range.");
    require(release_center_in_triangle(intent.tile, intent.triangle, intent.center),
        "Snow foot release center is outside the frozen receiver triangle.");
    require(intent.heading_bin.has_value() == shaped, "Snow foot release heading must match the frozen sole profile.");
}

void validate_state(const FootReleaseState& state, bool shaped, std::uint64_t tick) {
    require(tick <= max_exact_integer, "Snow foot release snapshot tick is out of range.");
    require(state.held_count <= state.holding.size() && state.ready_count <= state.ready.size(),
        "Snow foot release snapshot exceeds its queue bounds.");
    const auto& counts = state.counts;
    require(counts.applied <= tick && counts.saturated <= tick - counts.applied &&
        counts.cancelled <= max_exact_integer && counts.overflow <= max_exact_integer,
        "Snow foot release count is out of range.");
    const auto acquisitions = counts.applied + counts.saturated + counts.cancelled + counts.overflow +
        state.held_count + state.ready_count;
    require(acquisitions <= max_held_releases * tick, "Snow foot release counts exceed the committed acquisition budget.");

    for (std::size_t i = 0; i < state.held_count; ++i) {
        const auto& intent = state.holding[i];
        validate_intent(intent, false, shaped, tick);
        require(i == 0 || holding_key(state.holding[i - 1]) < holding_key(intent),
            "Snow held releases must be ordered by rig and slot without duplicate slots.");
    }
    for (std::size_t i = 0; i < state.ready_count; ++i) {
        const auto& intent = state.ready[i];
        validate_intent(intent, true, shaped, tick);
        require(i == 0 || ready_key(state.ready[i - 1]) < ready_key(intent),
            "Snow ready releases must be in release order without duplicate rows.");
        const auto duplicate = [&](const auto& row) { return same_acquisition(row, intent); };
        require(std::none_of(state.holding.begin(), state.holding.begin() + state.held_count, duplicate) &&
            std::none_of(state.ready.begin(), state.ready.begin() + i, duplicate),
            "Snow foot release snapshot contains a duplicate acquisition.");
        require(std::none_of(state.ready.begin(), state.ready.begin() + i, [&](const auto& previous) {
            return holding_key(previous) == holding_key(intent) && previous.released_tick >= intent.acquired_tick;
        }), "Snow ready releases for one foot overlap in time.");
        require(std::none_of(state.holding.begin(), state.holding.begin() + state.held_count, [&](const auto& current) {
            return holding_key(current) == holding_key(intent) && intent.released_tick >= current.acquired_tick;
        }), "Snow ready release overlaps a holding acquisition.");
    }
}

Json save_intent(const FootReleaseIntent& intent, const Definition& definition, bool ready) {
    Json row = {{"rig", rig_id(intent.rig)}, {"slot", intent.slot}, {"acquired_tick", intent.acquired_tick},
        {"receiver", definition.tiles[intent.tile]}, {"triangle", intent.triangle}, {"center_units", intent.center}};
    if (ready) row["released_tick"] = intent.released_tick;
    if (intent.heading_bin) row["heading_bin"] = static_cast<unsigned>(*intent.heading_bin);
    return row;
}

FootReleaseIntent read_intent(const Json& row, const Definition& definition, bool ready, bool shaped) {
    constexpr std::array fields{"rig", "slot", "acquired_tick", "receiver", "triangle", "center_units"};
    require(row.is_object() && row.size() == fields.size() + ready + shaped &&
        std::ranges::all_of(fields, [&](const char* field) { return row.contains(field); }) &&
        row.contains("released_tick") == ready && row.contains("heading_bin") == shaped,
        ready ? "Snow ready release has invalid fields." : "Snow held release has invalid fields.");
    require(row.at("rig").is_string(), "Snow foot release rig must be an entity ID string.");
    require(row.at("receiver").is_string(), "Snow foot release receiver must be a tile entity ID string.");
    const auto& receiver = row.at("receiver").get_ref<const std::string&>();
    const auto tile = std::find(definition.tiles.begin(), definition.tiles.end(), receiver);
    require(tile != definition.tiles.end(), "Snow foot release receiver is not a frozen snow tile.");
    const auto& center = row.at("center_units");
    require(center.is_array() && center.size() == 2, "Snow foot release center needs [x, z] lattice units.");
    return {
        .rig = release_rig_key(row.at("rig").get_ref<const std::string&>()),
        .slot = static_cast<std::uint32_t>(integer(row.at("slot"), max_ik_slot, "Invalid snow foot release slot.")),
        .acquired_tick = integer(row.at("acquired_tick"), max_exact_integer, "Invalid snow foot release acquisition tick."),
        .triangle = static_cast<std::uint32_t>(integer(row.at("triangle"), tile_triangle_count - 1,
            "Invalid snow foot release triangle.")),
        .tile = static_cast<unsigned>(tile - definition.tiles.begin()),
        .center = {static_cast<std::uint16_t>(integer(center[0], (samples_x - 1) * cell_units,
                       "Snow foot release center X is out of range.")),
                   static_cast<std::uint16_t>(integer(center[1], (samples_z - 1) * cell_units,
                       "Snow foot release center Z is out of range."))},
        .released_tick = ready ? integer(row.at("released_tick"), max_exact_integer, "Invalid snow released_tick.") : 0,
        .heading_bin = shaped ? std::optional{static_cast<std::uint8_t>(integer(row.at("heading_bin"), 255,
            "Snow sole heading_bin must be an integer from 0 to 255."))} : std::nullopt,
    };
}

} // namespace

RigKey release_rig_key(std::string_view id) {
    require(valid_rig(id), "Snow foot release rig must be a nonzero lowercase entity ID.");
    RigKey key;
    std::copy(id.begin(), id.end(), key.begin());
    return key;
}

bool release_center_in_triangle(unsigned tile, std::uint32_t triangle, StampCenter center) noexcept {
    if (tile >= 2 || triangle >= tile_triangle_count) return false;
    const auto cell = triangle / 2;
    const auto x0 = (tile * tile_cells + cell % tile_cells) * cell_units;
    const auto z0 = (cell / tile_cells) * cell_units;
    if (center[0] < x0 || center[0] > x0 + cell_units || center[1] < z0 || center[1] > z0 + cell_units)
        return false;
    const auto u = center[0] - x0;
    const auto v = center[1] - z0;
    // The cooker emits [a,d,b] then [a,c,d]. Allow one quantum across
    // their shared diagonal for independently rounded ray coordinates.
    return triangle % 2 == 0 ? u + 1 >= v : v + 1 >= u;
}

Json save_foot_releases(const FootReleaseState& state, const Definition& definition) {
    validate_state(state, uses_heading(definition), max_exact_integer);
    auto holding = Json::array(), ready = Json::array();
    for (std::size_t i = 0; i < state.held_count; ++i) holding.push_back(save_intent(state.holding[i], definition, false));
    for (std::size_t i = 0; i < state.ready_count; ++i) ready.push_back(save_intent(state.ready[i], definition, true));
    return {{"holding", std::move(holding)}, {"ready", std::move(ready)},
        {"counts", {{"applied", state.counts.applied}, {"saturated", state.counts.saturated},
            {"cancelled", state.counts.cancelled}, {"overflow", state.counts.overflow}}}};
}

FootReleaseState restore_foot_releases(const Json& saved, const Definition& definition, std::uint64_t tick) {
    require_fields(saved, {"holding", "ready", "counts"}, "Snow foot release snapshot has invalid fields.");
    const auto& holding = saved.at("holding");
    const auto& ready = saved.at("ready");
    const auto& counts = saved.at("counts");
    require(holding.is_array() && holding.size() <= max_held_releases && ready.is_array() && ready.size() <= max_ready_releases,
        "Snow foot release snapshot exceeds its queue bounds.");
    require_fields(counts, {"applied", "saturated", "cancelled", "overflow"}, "Snow foot release counters have invalid fields.");

    FootReleaseState state;
    state.counts = {
        .applied = integer(counts.at("applied"), max_exact_integer, "Invalid snow foot release applied count."),
        .saturated = integer(counts.at("saturated"), max_exact_integer, "Invalid snow foot release saturated count."),
        .cancelled = integer(counts.at("cancelled"), max_exact_integer, "Invalid snow foot release cancelled count."),
        .overflow = integer(counts.at("overflow"), max_exact_integer, "Invalid snow foot release overflow count."),
    };
    const bool shaped = uses_heading(definition);
    for (const auto& row : holding) state.holding[state.held_count++] = read_intent(row, definition, false, shaped);
    for (const auto& row : ready) state.ready[state.ready_count++] = read_intent(row, definition, true, shaped);
    validate_state(state, shaped, tick);
    return state;
}

} // namespace poima::snow
