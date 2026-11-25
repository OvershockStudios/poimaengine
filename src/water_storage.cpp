// SPDX-License-Identifier: Apache-2.0
#include "poima/water_storage.hpp"

#include "poima/input_action.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace poima::water {
namespace {
using Json = nlohmann::json;

bool bounded(double value, double low, double high) noexcept {
    return value >= low && value <= high;
}

std::size_t cell_order(Cell cell, const GridConfig& grid) noexcept {
    return std::size_t(cell.z) * grid.cells_x + cell.x;
}

Json endpoint_json(const StorageEndpoint& endpoint) {
    if (const auto* cell = std::get_if<Cell>(&endpoint))
        return {{"cell", {cell->x, cell->z}}};
    return {{"reservoir", std::get<std::string>(endpoint)}};
}

Json reservoir_json(const ReservoirDefinition& tank) {
    Json result{{"id", tank.id}, {"floor_height_m", tank.floor_height_m}, {"area_m2", tank.area_m2},
        {"capacity_m3", tank.capacity_m3}, {"initial_volume_m3", tank.initial_volume_m3}};
    if (const auto& footprint=tank.footprint)
        result["footprint"]={{"min_xz_m",footprint->min_xz_m},{"size_xz_m",footprint->size_xz_m}};
    return result;
}

bool overlaps(const ReservoirFootprint& a, const ReservoirFootprint& b) noexcept {
    for (std::size_t axis=0;axis<2;++axis)
        if (a.edge(axis)<=b.min_xz_m[axis] || b.edge(axis)<=a.min_xz_m[axis]) return false;
    return true;
}

Json port_json(const StoragePortDefinition& port) {
    Json row{{"id", port.id}, {"from", endpoint_json(port.from)}, {"to", endpoint_json(port.to)},
        {"sill_height_m", port.sill_height_m}, {"openness", port.openness},
        {"max_flow_m3_s", port.max_flow_m3_s}};
    if (const auto& weir=port.overflow)
        row["overflow"]={{"width_m",weir->width_m},{"coefficient_sqrt_m_s",weir->coefficient_sqrt_m_s}};
    else
        row["conductance_m2_s"]=port.conductance_m2_s;
    if (const auto& sheet=port.sheet) {
        row["sheet"]={{"lip_xz_m",sheet->lip_xz_m},{"outlet_xz_m",sheet->outlet_xz_m},
            {"outlet_floor_m",sheet->outlet_floor_m}};
        if (sheet->aeration) row["sheet"]["aeration"]=*sheet->aeration;
    }
    if (!port.action.empty()) {
        row["action"] = port.action;
        row["action_openness"] = port.action_openness;
    }
    return row;
}

double read_number(const Json& array, std::size_t index) {
    const auto& value = array.at(index);
    if (!value.is_number() || !std::isfinite(value.get<double>()))
        throw std::invalid_argument("Saved water storage values must be finite numbers.");
    return value.get<double>();
}
}

Json storage_definition_json(const StorageDefinition& definition) {
    Json tanks = Json::array(), links = Json::array();
    for (const auto& tank : definition.reservoirs) tanks.push_back(reservoir_json(tank));
    for (const auto& port : definition.ports) links.push_back(port_json(port));
    return {{"reservoirs", std::move(tanks)}, {"ports", std::move(links)}};
}

WaterStorage::WaterStorage(const StorageDefinition& definition, const Grid& grid) : reservoirs_(definition.reservoirs) {
    if (reservoirs_.empty() || reservoirs_.size() > max_water_reservoirs ||
            definition.ports.empty() || definition.ports.size() > max_water_ports)
        throw std::invalid_argument("Water storage requires 1–32 reservoirs and 1–128 ports.");
    std::ranges::sort(reservoirs_, {}, &ReservoirDefinition::id);
    const auto& config = grid.config();
    const ReservoirFootprint terrain{{config.origin_x_m,config.origin_z_m},
        {config.cells_x*config.cell_size_m,config.cells_z*config.cell_size_m}};
    for (std::size_t i = 0; i < reservoirs_.size(); ++i) {
        const auto& tank = reservoirs_[i];
        if (!valid_input_action_name(tank.id) || (i && reservoirs_[i-1].id == tank.id) ||
                !bounded(tank.floor_height_m, -max_water_depth_m, max_water_depth_m) ||
                !bounded(tank.area_m2, .01, max_reservoir_area_m2) ||
                !bounded(tank.capacity_m3, .001, std::min(max_reservoir_capacity_m3, tank.area_m2 * max_water_depth_m)) ||
                !bounded(tank.initial_volume_m3, 0, tank.capacity_m3))
            throw std::invalid_argument("Invalid or duplicate water reservoir.");
        if (const auto& footprint = tank.footprint) {
            const auto area = footprint->size_xz_m[0] * footprint->size_xz_m[1];
            if (!(std::abs(area - tank.area_m2) <= tank.area_m2 * 1e-10) ||
                    tank.floor_height_m + tank.capacity_m3 / tank.area_m2 > max_water_depth_m)
                throw std::invalid_argument("Placed reservoir footprint area or vertical range differs from its capacity.");
            for (std::size_t axis=0;axis<2;++axis)
                if (!bounded(footprint->min_xz_m[axis],-max_water_depth_m,max_water_depth_m) ||
                    !bounded(footprint->size_xz_m[axis],.01,1e6) || footprint->edge(axis)>max_water_depth_m)
                    throw std::invalid_argument("Placed reservoir footprint is outside the supported world bounds.");
            if (overlaps(*footprint,terrain) || std::ranges::any_of(placed_,[&](auto prior) {
                return overlaps(*footprint,*reservoirs_[prior].footprint);
            })) throw std::invalid_argument("A placed reservoir footprint overlaps terrain water or another reservoir.");
            placed_.push_back(i);
        }
        volumes_[i] = tank.initial_volume_m3;
    }

    for (const auto& port : definition.ports) {
        if (!std::holds_alternative<std::string>(port.from) && !std::holds_alternative<std::string>(port.to))
            throw std::invalid_argument("A water storage port needs at least one reservoir endpoint.");
        for (const auto* endpoint : {&port.from, &port.to}) {
            if (const auto* cell = std::get_if<Cell>(endpoint)) {
                if (cell->x >= config.cells_x || cell->z >= config.cells_z)
                    throw std::invalid_argument("Water storage endpoint is outside the grid.");
                cells_.push_back(*cell);
            }
        }
    }
    const auto cell_index = [&](Cell cell) { return cell_order(cell, config); };
    std::ranges::sort(cells_, {}, cell_index);
    const auto duplicates = std::ranges::unique(cells_, {}, cell_index);
    cells_.erase(duplicates.begin(), duplicates.end());
    const auto resolve = [&](const StorageEndpoint& endpoint) -> std::size_t {
        if (const auto* cell = std::get_if<Cell>(&endpoint)) {
            const auto match = std::ranges::lower_bound(cells_, cell_index(*cell), {}, cell_index);
            return reservoirs_.size() + static_cast<std::size_t>(match - cells_.begin());
        }
        const auto& id = std::get<std::string>(endpoint);
        const auto tank = std::ranges::lower_bound(reservoirs_, id, {}, &ReservoirDefinition::id);
        if (tank == reservoirs_.end() || tank->id != id)
            throw std::invalid_argument("Water storage port references an unknown reservoir.");
        return static_cast<std::size_t>(tank - reservoirs_.begin());
    };

    for (const auto& port : definition.ports) {
        const auto from = resolve(port.from), to = resolve(port.to);
        if (from == to || !valid_input_action_name(port.id) ||
                !bounded(port.sill_height_m, -max_water_depth_m, max_water_depth_m) ||
                !bounded(port.max_flow_m3_s, 0, max_storage_rate) || port.max_flow_m3_s == 0 ||
                !bounded(port.openness, 0, 1) || !bounded(port.action_openness, 0, 1) ||
                (!port.action.empty() && !valid_input_action_name(port.action)))
            throw std::invalid_argument("Invalid water storage port endpoints, rates or gate control.");
        if (const auto& weir=port.overflow) {
            if (from>=reservoirs_.size() || port.conductance_m2_s!=0 ||
                !bounded(weir->width_m,.01,256) || !bounded(weir->coefficient_sqrt_m_s,.01,10))
                throw std::invalid_argument("An overflow port needs a reservoir source, width in [0.01,256] and coefficient in [0.01,10].");
            const auto& source=reservoirs_[from];
            const auto target_floor=to<reservoirs_.size() ? reservoirs_[to].floor_height_m :
                grid.sample(cells_[to-reservoirs_.size()]).floor_height_m;
            const auto rim_depth=port.sill_height_m-source.floor_height_m;
            if (!(rim_depth>0 && rim_depth<source.capacity_m3/source.area_m2 && port.sill_height_m>=target_floor))
                throw std::invalid_argument("An overflow crest must be above the source floor, below its full stage and at or above the receiving floor.");
        } else if (!(port.conductance_m2_s>0 && port.conductance_m2_s<=max_storage_rate)) {
            throw std::invalid_argument("A linear water port requires positive bounded conductance.");
        }
        ports_.push_back({port, from, to});
    }
    std::ranges::sort(ports_, {}, [](const Port& port) -> const std::string& { return port.definition.id; });
    for (std::size_t i = 0; i < ports_.size(); ++i) {
        if (i && ports_[i - 1].definition.id == ports_[i].definition.id)
            throw std::invalid_argument("Duplicate water storage port ID.");
        const auto& port=ports_[i];
        openness_[i] = port.definition.openness;
        if (const auto& sheet=port.definition.sheet) {
            constexpr double limit=1'000'000.;
            if (!port.definition.overflow || sheet_ports_.size()>=max_water_sheets ||
                !bounded(sheet->outlet_floor_m,-limit,limit))
                throw std::invalid_argument("Water sheets require overflow weirs and at most eight authored sheets.");
            for (std::size_t axis=0;axis<2;++axis)
                if (!bounded(sheet->lip_xz_m[axis],-limit,limit) || !bounded(sheet->outlet_xz_m[axis],-limit,limit))
                    throw std::invalid_argument("Water sheet endpoints must be within one million metres of the origin.");
            const auto dx=sheet->outlet_xz_m[0]-sheet->lip_xz_m[0],dz=sheet->outlet_xz_m[1]-sheet->lip_xz_m[1];
            const auto drop=port.definition.sill_height_m-sheet->outlet_floor_m;
            if (sheet->aeration && !bounded(*sheet->aeration,0.,1.))
                throw std::invalid_argument("Water sheet aeration must be within [0,1].");
            if (!bounded(std::hypot(dx,dz),.05,256) || !bounded(drop,.05,256))
                throw std::invalid_argument("Water sheet drop and horizontal reach must be in [0.05,256] metres.");
            const auto [target_x,target_z]=sheet->outlet_xz_m;
            if (port.to<reservoirs_.size()) {
                const auto& sink=reservoirs_[port.to];
                if (sink.footprint && !sink.footprint->contains(target_x,target_z))
                    throw std::invalid_argument("Water sheet outlet is outside the receiving reservoir footprint.");
            } else {
                const auto sample=grid.sample_world(target_x,target_z);
                const auto target=cells_[port.to-reservoirs_.size()];
                if (!sample || sample->cell.x!=target.x || sample->cell.z!=target.z)
                    throw std::invalid_argument("Water sheet outlet must be inside its receiving cell.");
            }
            sheet_ports_.push_back(i);
        }
    }
}

double WaterStorage::volume_m3() const noexcept {
    return std::accumulate(volumes_.begin(), volumes_.begin() + reservoirs_.size(), 0.);
}

double WaterStorage::capacity_m3() const noexcept {
    return std::accumulate(reservoirs_.begin(), reservoirs_.end(), 0.,
        [](double total, const ReservoirDefinition& tank) { return total + tank.capacity_m3; });
}

std::optional<double> WaterStorage::transfer_m3(std::string_view port_id) const noexcept {
    const auto found=std::lower_bound(ports_.begin(),ports_.end(),port_id,
        [](const Port& port,std::string_view name){ return port.definition.id<name; });
    if (found==ports_.end() || found->definition.id!=port_id) return std::nullopt;
    return transfers_[static_cast<std::size_t>(found-ports_.begin())];
}

ReservoirSample WaterStorage::sample(std::size_t i) const {
    const auto& tank = reservoirs_.at(i);
    return {tank.id, tank.footprint.value(), tank.floor_height_m, tank.floor_height_m + volumes_[i] / tank.area_m2};
}

std::optional<ReservoirSample> WaterStorage::sample_world(double x_m, double z_m) const {
    for (const auto i : placed_)
        if (reservoirs_[i].footprint->contains(x_m,z_m)) return sample(i);
    return std::nullopt;
}

std::optional<WaterSheetSample> WaterStorage::sheet_sample(const Grid& grid,std::size_t i) const {
    if (transfers_[i]<=0) return std::nullopt;
    const auto& port=ports_[i];
    const auto& sheet=*port.definition.sheet;
    const auto width=port.definition.overflow->width_m;
    const auto crest=port.definition.sill_height_m;
    const double dx=sheet.outlet_xz_m[0]-sheet.lip_xz_m[0],dz=sheet.outlet_xz_m[1]-sheet.lip_xz_m[1];
    const auto length=std::hypot(dx,dz);
    const auto receiving_stage=port.to<reservoirs_.size() ? reservoirs_[port.to].floor_height_m+
        volumes_[port.to]/reservoirs_[port.to].area_m2 : grid.sample(cells_[port.to-reservoirs_.size()]).surface_height_m;
    const auto drop=crest-std::max(sheet.outlet_floor_m,receiving_stage);
    // Keep the end anchored to the receiving surface as its stage changes.
    // A submerged lip has no free-falling visual.
    if (drop<=1e-6) return std::nullopt;
    const auto speed=length/std::sqrt(2*drop/9.81);
    const auto thickness=std::min({transfers_[i]/(step_seconds*width*speed),drop,8.});
    if (thickness<=0) return std::nullopt;
    return WaterSheetSample{port.definition.id,{sheet.lip_xz_m[0],crest,sheet.lip_xz_m[1]},
        {dx/length,dz/length},width,length,drop/(length*length),thickness,speed,sheet.aeration};
}

std::vector<WaterSheetSample> WaterStorage::sheets(const Grid& grid) const {
    std::vector<WaterSheetSample> result;
    result.reserve(sheet_ports_.size());
    for (auto i : sheet_ports_) if (auto sample=sheet_sample(grid,i)) result.push_back(*sample);
    return result;
}

WaterStorage::Step WaterStorage::stage(const Grid& grid, std::span<const std::string_view> pressed,
                                      std::span<const double> occupation_m) const {
    struct Node {
        double floor, depth, volume, space, slope;
        double delta = 0, requested_in = 0, accepted_in = 0;
        std::size_t degree = 0;
    };
    std::array<Node, max_water_reservoirs + max_water_ports> nodes;
    for (std::size_t i = 0; i < reservoirs_.size(); ++i) {
        const auto& tank = reservoirs_[i];
        nodes[i] = {tank.floor_height_m, volumes_[i] / tank.area_m2, volumes_[i],
            tank.capacity_m3 - volumes_[i], 1 / tank.area_m2};
    }
    const auto& config = grid.config();
    const auto area = config.cell_size_m * config.cell_size_m;
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        const auto sample = grid.sample(cells_[i]);
        const auto occupied = occupation_m.empty() ? 0. : occupation_m[cell_order(cells_[i], config)];
        nodes[reservoirs_.size() + i] = {sample.floor_height_m,
            sample.depth_m + body_stage_rise(sample.depth_m, occupied), sample.volume_m3,
            (max_water_depth_m - sample.depth_m) * area, (occupied > 0 ? 1.25 : 1.) / area};
    }
    Step next;
    next.openness = openness_;
    struct Proposal { std::size_t donor, receiver; double difference, rate, volume; };
    std::array<Proposal, max_water_ports> proposals{};
    for (std::size_t i = 0; i < ports_.size(); ++i) {
        const auto& port = ports_[i];
        const auto& rule = port.definition;
        if (!rule.action.empty() && std::ranges::find(pressed, rule.action) != pressed.end())
            next.openness[i] = openness_[i] == rule.openness ? rule.action_openness : rule.openness;
        if (next.openness[i] == 0) continue;
        const auto& from = nodes[port.from];
        const auto& to = nodes[port.to];
        const auto sill = std::max({rule.sill_height_m, from.floor, to.floor});
        const auto from_head = std::max(0., from.depth - (sill - from.floor));
        const auto to_head = std::max(0., to.depth - (sill - to.floor));
        if (from_head == to_head || (rule.overflow && from_head<to_head)) continue;
        auto& flow = proposals[i];
        const auto difference=std::abs(from_head-to_head);
        double rate=rule.conductance_m2_s*difference;
        if (const auto& weir=rule.overflow) {
            // Subtract downstream head power in a form that retains the
            // small positive difference when the two surfaces nearly meet.
            const auto upper=std::sqrt(from_head), lower=std::sqrt(to_head);
            rate=weir->coefficient_sqrt_m_s*weir->width_m*difference*(upper+to_head/(upper+lower));
        }
        flow = {port.from, port.to, difference, std::min(rule.max_flow_m3_s,rate), 0};
        if (to_head > from_head)
            std::swap(flow.donor, flow.receiver);
        if (nodes[flow.receiver].space == 0) {
            flow.difference = 0;
            continue;
        }
        ++nodes[flow.donor].degree;
        ++nodes[flow.receiver].degree;
    }
    for (std::size_t i = 0; i < ports_.size(); ++i) {
        auto& flow = proposals[i];
        if (flow.difference == 0) continue;
        const auto& donor = nodes[flow.donor];
        auto& receiver = nodes[flow.receiver];
        // An open link shares the endpoint's per-tick stage correction with its
        // other active links. Even very high conductance cannot invert an isolated pair.
        const auto donor_links = static_cast<double>(donor.degree);
        const auto receiver_links = static_cast<double>(receiver.degree);
        const auto stable_volume = flow.difference / (donor.slope * donor_links + receiver.slope * receiver_links);
        flow.volume = std::min({flow.rate * step_seconds, stable_volume, donor.volume / donor_links}) * next.openness[i];
        receiver.requested_in += flow.volume;
    }
    for (std::size_t i = 0; i < ports_.size(); ++i) {
        const auto& flow = proposals[i];
        if (flow.volume == 0) continue;
        auto& donor = nodes[flow.donor];
        auto& receiver = nodes[flow.receiver];
        const auto scale = std::min(1., receiver.space / receiver.requested_in);
        const auto moved = std::min(flow.volume * scale, std::max(0., receiver.space - receiver.accepted_in));
        donor.delta -= moved;
        receiver.delta += moved;
        receiver.accepted_in += moved;
        next.transfers[i] = flow.donor == ports_[i].from ? moved : -moved;
    }
    for (std::size_t i = 0; i < reservoirs_.size(); ++i)
        next.volumes[i] = std::clamp(volumes_[i] + nodes[i].delta, 0., reservoirs_[i].capacity_m3);
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        const auto& node = nodes[reservoirs_.size() + i];
        if (node.delta != 0)
            next.cell_changes[next.cell_change_count++] = {cells_[i], std::clamp(node.delta, -node.volume, node.space)};
    }
    return next;
}

void WaterStorage::commit(const Step& next) noexcept {
    volumes_ = next.volumes;
    openness_ = next.openness;
    transfers_ = next.transfers;
}

Json WaterStorage::inspect(const Grid* grid) const {
    Json tanks = Json::array(), ports = Json::array();
    for (std::size_t i = 0; i < reservoirs_.size(); ++i) {
        const auto& tank = reservoirs_[i];
        auto row = reservoir_json(tank);
        row["volume_m3"] = volumes_[i];
        row["surface_height_m"] = tank.floor_height_m + volumes_[i] / tank.area_m2;
        tanks.push_back(std::move(row));
    }
    for (std::size_t i = 0; i < ports_.size(); ++i) {
        auto row = port_json(ports_[i].definition);
        row["initial_openness"] = row["openness"];
        row["openness"] = openness_[i];
        row["discharge_m3_s"] = transfers_[i] / step_seconds;
        if (grid && ports_[i].definition.sheet) {
            row["active_sheet"]=nullptr;
            if (const auto sheet=sheet_sample(*grid,i)) {
                row["active_sheet"]={{"lip_m",sheet->lip_m},{"forward_xz",sheet->forward_xz},
                    {"width_m",sheet->width_m},{"length_m",sheet->length_m},
                    {"drop_per_m2",sheet->drop_per_m2},{"vertical_depth_m",sheet->vertical_depth_m},
                    {"launch_speed_m_s",sheet->launch_speed_m_s}};
                if (sheet->aeration) row["active_sheet"]["aeration"]=*sheet->aeration;
            }
        }
        ports.push_back(std::move(row));
    }
    return {{"volume_m3", volume_m3()}, {"reservoirs", std::move(tanks)}, {"ports", std::move(ports)}};
}

Json WaterStorage::save() const {
    return {{"volumes_m3", std::vector(volumes_.begin(), volumes_.begin() + reservoirs_.size())},
        {"openness", std::vector(openness_.begin(), openness_.begin() + ports_.size())},
        {"transfers_m3", std::vector(transfers_.begin(), transfers_.begin() + ports_.size())}};
}

void WaterStorage::restore(const Json& state, std::uint64_t tick) {
    const auto has_array = [&](const char* key, std::size_t size) {
        const auto field = state.find(key);
        return field != state.end() && field->is_array() && field->size() == size;
    };
    if (!state.is_object() || state.size() != 3 || !has_array("volumes_m3", reservoirs_.size()) ||
            !has_array("openness", ports_.size()) || !has_array("transfers_m3", ports_.size()))
        throw std::invalid_argument("Saved water storage shape does not match the plan.");
    Step next;
    std::array<double, max_water_reservoirs> delta{};
    for (std::size_t i = 0; i < ports_.size(); ++i) {
        const auto& port = ports_[i];
        next.openness[i] = read_number(state.at("openness"), i);
        next.transfers[i] = read_number(state.at("transfers_m3"), i);
        const auto limit = tick ? port.definition.max_flow_m3_s * step_seconds * next.openness[i] : 0.;
        if ((next.openness[i] != port.definition.openness &&
                (!tick || port.definition.action.empty() || next.openness[i] != port.definition.action_openness)) ||
                std::abs(next.transfers[i]) > limit || (port.definition.overflow && next.transfers[i]<0))
            throw std::invalid_argument("Saved water storage gate or transfer is outside the authored bounds.");
        if (port.from < reservoirs_.size()) delta[port.from] -= next.transfers[i];
        if (port.to < reservoirs_.size()) delta[port.to] += next.transfers[i];
    }
    for (std::size_t i = 0; i < reservoirs_.size(); ++i) {
        const auto& tank = reservoirs_[i];
        const auto volume = read_number(state.at("volumes_m3"), i);
        const auto previous = volume - delta[i];
        const auto tolerance = 32 * std::numeric_limits<double>::epsilon() * std::max(1., tank.capacity_m3);
        if (!bounded(volume, 0, tank.capacity_m3) || !bounded(previous, -tolerance, tank.capacity_m3 + tolerance) ||
                (!tick && volume != tank.initial_volume_m3) ||
                (tick == 1 && std::abs(previous - tank.initial_volume_m3) > tolerance))
            throw std::invalid_argument("Saved reservoir volume is outside its capacity or does not balance.");
        next.volumes[i] = volume;
    }
    commit(next);
}

}
