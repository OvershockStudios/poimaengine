// SPDX-License-Identifier: Apache-2.0
#include "poima/water_region.hpp"

#include "poima/input_action.hpp"
#include "poima/surface_wetness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace poima::water {
namespace {

using Json = nlohmann::json;

constexpr char snapshot_format[] = "poima.water-region-state";
constexpr double max_depth_m = 1'000'000;
constexpr double max_exchange_rate = 1'000'000;
constexpr double nm_to_m = 1e-9;
constexpr std::uint64_t max_tick_rain_nm = wetness::max_batch * wetness::max_rain_nm;
constexpr std::uint64_t max_total_rain_nm = (wetness::max_quantum + 1) * wetness::max_rain_nm;

bool unit_interval(double value) noexcept {
    return value >= 0 && value <= 1;
}

bool lowercase_sha256(std::string_view value) noexcept {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const char digit) {
        return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
    });
}

Grid initial_grid(const RegionDefinition& definition) {
    if (definition.config.cells_x > max_region_side || definition.config.cells_z > max_region_side ||
        definition.faces.size() > max_region_faces || definition.exchanges.size() > max_region_exchanges)
        throw std::invalid_argument("Authored water region exceeds its size or rule limits.");
    if (!lowercase_sha256(definition.plan_sha256))
        throw std::invalid_argument("Water region requires a lowercase SHA-256 plan identity.");
    if (definition.surface_wave_layers.size() > max_surface_wave_layers)
        throw std::invalid_argument("Water expects at most four surface wave layers.");
    if (definition.surface_ripples && !definition.surface_wave_layers.empty())
        throw std::invalid_argument("Choose either surface_ripples or surface_wave_layers.");
    return Grid(definition.config, definition.floor_heights_m, definition.initial_depths_m);
}

void validate_surface_ripples(const SurfaceRipples& ripples) {
    if (!(ripples.wavelength_m >= .10 && ripples.wavelength_m <= 8.0))
        throw std::invalid_argument("Water surface ripple wavelength_m must be finite and in [0.10,8].");
    if (!(ripples.slope >= 0 && ripples.slope <= .4))
        throw std::invalid_argument("Water surface ripple slope must be finite and in [0,0.4].");
    if (ripples.period_ticks < 8 || ripples.period_ticks > 3600)
        throw std::invalid_argument("Water surface ripple period_ticks must be in [8,3600].");
    if (!(ripples.heading_degrees >= -180 && ripples.heading_degrees <= 180))
        throw std::invalid_argument("Water surface ripple heading_degrees must be finite and in [-180,180].");
}

void validate_wave_layers(std::span<const SurfaceRipples> layers) {
    double sum = 0;
    double rounding_error = 0;
    for (const auto& layer : layers) {
        validate_surface_ripples(layer);
        const double next = sum + layer.slope;
        // Carry rounding error so the slope limit does not depend on the sequence of partial sums.
        rounding_error += sum >= layer.slope ? (sum - next) + layer.slope : (layer.slope - next) + sum;
        sum = next;
    }
    if (sum + rounding_error > .4)
        throw std::invalid_argument("Water surface wave layer slopes must sum to at most 0.4.");
}

auto face_key(const RegionFace& rule) noexcept {
    return std::tuple{rule.cell.z, rule.cell.x, rule.axis};
}

auto cell_key(Cell cell) noexcept {
    return std::pair{cell.z, cell.x};
}

auto exchange_key(const CellExchange& rule) noexcept {
    return cell_key(rule.cell);
}

bool exact_fields(const Json& object, std::initializer_list<const char*> fields) {
    return object.is_object() && object.size() == fields.size() &&
        std::all_of(fields.begin(), fields.end(), [&](const char* name) { return object.contains(name); });
}

std::uint64_t unsigned_integer(const Json& value) {
    if (value.is_number_unsigned())
        return value.get<std::uint64_t>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        return static_cast<std::uint64_t>(value.get<std::int64_t>());
    throw std::invalid_argument("Water snapshot version and tick must be unsigned integers.");
}

double finite_nonnegative(const Json& value) {
    if (value.is_number()) {
        const double number = value.get<double>();
        if (std::isfinite(number) && number >= 0)
            return number;
    }
    throw std::invalid_argument("Water snapshot values must be finite nonnegative numbers.");
}

Json report_json(const std::optional<StepReport>& report, bool rain_enabled) {
    if (!report)
        return nullptr;
    Json result{{"tick", report->tick}, {"volume_before_m3", report->volume_before_m3},
        {"supplied_m3", report->supplied_m3}, {"drained_m3", report->drained_m3},
        {"volume_after_m3", report->volume_after_m3}};
    if (rain_enabled)
        result["rain_m3"] = report->rain_m3;
    return result;
}

std::optional<StepReport> validated_report(const Json& report, const Grid& grid,
                                          std::span<const CellExchange> exchanges,
                                          std::optional<double> rain_m3) {
    if (grid.tick() == 0) {
        if (!report.is_null())
            throw std::invalid_argument("Initial water snapshot cannot have a step receipt.");
        return std::nullopt;
    }
    if (rain_m3
            ? !exact_fields(report, {"tick", "volume_before_m3", "supplied_m3", "drained_m3", "volume_after_m3", "rain_m3"})
            : !exact_fields(report, {"tick", "volume_before_m3", "supplied_m3", "drained_m3", "volume_after_m3"}))
        throw std::invalid_argument("Water snapshot has an incomplete step receipt.");
    const StepReport result{unsigned_integer(report.at("tick")),
        finite_nonnegative(report.at("volume_before_m3")), finite_nonnegative(report.at("supplied_m3")),
        finite_nonnegative(report.at("drained_m3")), finite_nonnegative(report.at("volume_after_m3")),
        rain_m3 ? finite_nonnegative(report.at("rain_m3")) : 0.0};
    if (result.tick != grid.tick() || result.volume_after_m3 != grid.volume_m3() ||
        result.rain_m3 != rain_m3.value_or(0.0))
        throw std::invalid_argument("Water step receipt does not match the restored tick and volume.");

    double supply_limit = 0;
    double drain_limit = 0;
    for (const auto& exchange : exchanges) {
        supply_limit += exchange.source_m3_per_second * step_seconds;
        drain_limit += exchange.drain_m3_per_second * step_seconds;
    }
    const auto& config = grid.config();
    const double cell_area = config.cell_size_m * config.cell_size_m;
    const double before_limit = max_depth_m * config.cells_x * config.cells_z * cell_area;
    const double scale = std::max({1.0, before_limit, supply_limit, drain_limit});
    const double rate_rounding = 64 * std::numeric_limits<double>::epsilon() * scale;
    if (result.volume_before_m3 > before_limit + rate_rounding ||
        result.supplied_m3 > supply_limit + (supply_limit > 0 ? rate_rounding : 0) ||
        result.drained_m3 > drain_limit + (drain_limit > 0 ? rate_rounding : 0))
        throw std::invalid_argument("Water step receipt exceeds the region's volume or exchange rates.");

    const double actual_scale = std::max({1.0, result.volume_before_m3, result.volume_after_m3,
        result.supplied_m3, result.drained_m3, result.rain_m3});
    const double volume_rounding = 64 * std::numeric_limits<double>::epsilon() * actual_scale;
    const double expected_after = result.volume_before_m3 + result.supplied_m3 + result.rain_m3 - result.drained_m3;
    if (std::abs(result.volume_after_m3 - expected_after) > volume_rounding)
        throw std::invalid_argument("Water step receipt does not balance.");
    return result;
}

std::array<double,2> swell_patch_value(std::uint32_t x,std::uint32_t z,std::uint32_t seed) noexcept {
    constexpr std::uint32_t mask=(1u<<20)-1;
    std::uint32_t bits=((x&mask)*0x8da6b343u)^((z&mask)*0xd8163841u)^seed;
    bits=(bits^(bits>>16))*0x7feb352du;
    bits=(bits^(bits>>15))*0x846ca68bu;
    bits^=bits>>16;
    constexpr double scale=2.0/65535;
    return {static_cast<double>(bits&65535u)*scale-1.0,static_cast<double>(bits>>16)*scale-1.0};
}

std::array<double,2> swell_patch(const WaterSwellVariation& profile,double x,double z) noexcept {
    const double u=x/profile.scale_m,v=z/profile.scale_m;
    // The fitted grid provides nonnegative local coordinates on both axes.
    const auto ix=static_cast<std::uint32_t>(u),iz=static_cast<std::uint32_t>(v);
    const auto blend=[](double t) { return t*t*(3-2*t); };
    const double sx=blend(u-ix),sz=blend(v-iz);
    const auto a=swell_patch_value(ix,iz,profile.seed),b=swell_patch_value(ix+1,iz,profile.seed);
    const auto c=swell_patch_value(ix,iz+1,profile.seed),d=swell_patch_value(ix+1,iz+1,profile.seed);
    std::array<double,2> result{};
    for (std::size_t i=0;i<result.size();++i)
        result[i]=std::lerp(std::lerp(a[i],b[i],sx),std::lerp(c[i],d[i],sx),sz);
    return result;
}

}

nlohmann::json surface_ripples_json(const SurfaceRipples& ripples) {
    return {{"wavelength_m", ripples.wavelength_m}, {"slope", ripples.slope},
        {"period_ticks", ripples.period_ticks}, {"heading_degrees", ripples.heading_degrees}};
}

nlohmann::json surface_wave_layers_json(std::span<const SurfaceRipples> layers) {
    auto result = Json::array();
    for (const auto& layer : layers)
        result.push_back(surface_ripples_json(layer));
    return result;
}

nlohmann::json swell_variation_json(const WaterSwellVariation& profile) {
    return {{"scale_m",profile.scale_m},{"strength",profile.strength},{"seed",profile.seed}};
}

nlohmann::json listener_audio_json(const ListenerAudio& profile) {
    return {{"cutoff_hz", profile.cutoff_hz}, {"gain", profile.gain},
        {"full_depth_m", profile.full_depth_m}};
}

nlohmann::json optics_json(const WaterOptics& profile) {
    nlohmann::json result{{"extinction_per_m",profile.extinction_per_m},{"scattered_radiance",profile.scattered_radiance}};
    if(const auto& roughness=profile.reflection_roughness) result["reflection_roughness"]=*roughness;
    if(profile.solar_scattering.value_or(0)>0) result["solar_scattering"]=*profile.solar_scattering;
    if(profile.refraction) result["refraction"]=true;
    if(profile.follow_terrain) result["follow_terrain"]=true;
    if(profile.caustics_strength>0) result["caustics_strength"]=profile.caustics_strength;
    if(const auto& foam=profile.shore_foam)
        result["shore_foam"]={{"depth_m",foam->depth_m},{"opacity",foam->opacity}};
    if(const auto& sea=profile.offshore) {
        constexpr std::array names{"min_z","max_x","max_z","min_x"};
        result["offshore"]={{"edge",names.at(static_cast<std::size_t>(sea->edge))},
            {"surface_height_m",sea->surface_height_m},{"depth_m",sea->depth_m}};
    }
    return result;
}

Region::Region(const RegionDefinition& definition)
    : grid_(initial_grid(definition)), initial_depths_m_(definition.initial_depths_m),
      faces_(definition.faces), exchanges_(definition.exchanges), plan_sha256_(definition.plan_sha256),
      rain_cells_(definition.rain_cells), surface_ripples_(definition.surface_ripples),
      surface_wave_layers_(definition.surface_wave_layers), listener_audio_(definition.listener_audio),
      optics_(definition.optics), shore_corner_heights_m_(definition.shore_corner_heights_m),
      surface_swell_(definition.surface_swell), swell_variation_(definition.swell_variation) {
    const auto& config=grid_.config();
    const auto corner_count=static_cast<std::size_t>(config.cells_x+1)*(config.cells_z+1);
    if (optics_ && optics_->follow_terrain) {
        if (shore_corner_heights_m_.size()!=corner_count ||
            !std::ranges::all_of(shore_corner_heights_m_,[](double height){return std::isfinite(height) && std::abs(height)<=1e6;}))
            throw std::invalid_argument("Terrain-fit water requires one finite height per grid corner.");
    } else if (!shore_corner_heights_m_.empty()) {
        throw std::invalid_argument("Water corner heights require terrain fitting.");
    }
    if (surface_ripples_)
        validate_surface_ripples(*surface_ripples_);
    validate_wave_layers(surface_wave_layers_);
    double phase_gradient=0,amplitude_gradient=0;
    if (const auto& variation=swell_variation_) {
        if (surface_swell_.empty() ||
            !(variation->scale_m>=std::max(.1,8*config.cell_size_m) && variation->scale_m<=65'536 &&
              variation->strength>0 && variation->strength<=1))
            throw std::invalid_argument("Swell variation needs surface_swell, strength in (0,1] and a scale of at least eight cells in [0.1,65536] m.");
        // Cubic interpolation over [-1,1] samples changes at most 3 per
        // axis. The paired phase field is projected along each wave heading.
        const double max_gradient=3*std::numbers::sqrt2*variation->strength/variation->scale_m;
        phase_gradient=WaterSwellVariation::phase_radians*max_gradient;
        amplitude_gradient=.5*WaterSwellVariation::amplitude_blend*max_gradient;
    }
    if (!surface_swell_.empty()) {
        if (!optics_ || !optics_->follow_terrain || surface_swell_.size()>max_surface_wave_layers)
            throw std::invalid_argument("Water surface swell needs terrain fitting and one to four layers.");
        constexpr double tau=2*std::numbers::pi;
        double total_slope=0;
        for (const auto& wave : surface_swell_) {
            if (!(wave.wavelength_m>=std::max(.1,4*config.cell_size_m) && wave.wavelength_m<=1024 &&
                  wave.slope>0 && wave.slope<=.4 && wave.period_ticks>=8 && wave.period_ticks<=3600 &&
                  wave.heading_degrees>=-180 && wave.heading_degrees<=180))
                throw std::invalid_argument("Invalid swell layer. Use 4 or more cells per wavelength and a positive slope.");
            const double heading=wave.heading_degrees*std::numbers::pi/180;
            const std::array direction{std::cos(heading),std::sin(heading)};
            const double frequency=tau/wave.wavelength_m;
            const double amplitude=wave.slope/frequency;
            if (phase_gradient>0 && frequency+phase_gradient>tau/(4*config.cell_size_m))
                throw std::invalid_argument("Swell variation makes a local wavelength shorter than four cells.");
            swell_waves_.push_back({{frequency*direction[0],frequency*direction[1]},direction,amplitude,wave.period_ticks});
            total_slope+=wave.slope+amplitude*(phase_gradient+amplitude_gradient);
            swell_amplitude_m_+=amplitude;
        }
        if (total_slope>.4 || swell_amplitude_m_>std::min(.5*config.cell_size_m,8.))
            throw std::invalid_argument("Swell slopes must sum to at most 0.4 and amplitudes to at most half a cell or 8 metres.");
    }
    if (listener_audio_) {
        const auto& profile = *listener_audio_;
        if (!(profile.cutoff_hz >= 80 && profile.cutoff_hz <= 8000 && unit_interval(profile.gain) &&
              profile.full_depth_m >= .01 && profile.full_depth_m <= 10))
            throw std::invalid_argument("Water listener audio needs cutoff_hz in [80,8000], gain in [0,1] and full_depth_m in [0.01,10].");
    }
    if (optics_) {
        if (optics_->reflection_roughness && !unit_interval(*optics_->reflection_roughness))
            throw std::invalid_argument("Water reflection roughness must be in [0,1].");
        if (const auto strength=optics_->solar_scattering) {
            if (!unit_interval(*strength))
                throw std::invalid_argument("Water solar_scattering must be in [0,1].");
            if (*strength==0) optics_->solar_scattering.reset();
        }
        if (const auto& sea=optics_->offshore;
            sea && (!optics_->follow_terrain || static_cast<std::uint32_t>(sea->edge)>3 ||
                !(sea->depth_m>=.1 && sea->depth_m<=10'000) || !(std::abs(sea->surface_height_m)<=1e6)))
            throw std::invalid_argument("Offshore water needs terrain fitting, a valid edge, finite stage and depth in [0.1,10000].");
        if (!unit_interval(optics_->caustics_strength))
            throw std::invalid_argument("Water caustics_strength must be in [0,1].");
        if (const auto& foam = optics_->shore_foam;
            foam && (!(foam->depth_m >= .02 && foam->depth_m <= 3) || !unit_interval(foam->opacity)))
            throw std::invalid_argument("Water shore foam needs depth_m in [0.02,3] and opacity in [0,1].");
        for (double value : optics_->extinction_per_m)
            if (!(value >= 0 && value <= 8))
                throw std::invalid_argument("Water extinction_per_m channels must be in [0,8].");
        for (double value : optics_->scattered_radiance)
            if (!(value >= 0 && value <= 16))
                throw std::invalid_argument("Water scattered_radiance channels must be in [0,16].");
    }

    if (initial_depths_m_.empty())
        initial_depths_m_.resize(static_cast<std::size_t>(config.cells_x) * config.cells_z);

    std::ranges::sort(faces_, {}, face_key);
    for (std::size_t index = 0; index < faces_.size(); ++index) {
        const auto& rule = faces_[index];
        if ((index > 0 && face_key(faces_[index - 1]) == face_key(rule)) ||
            (!rule.action.empty() && !valid_input_action_name(rule.action)) || !unit_interval(rule.action_openness))
            throw std::invalid_argument("Invalid or duplicate authored water face rule.");
        grid_.set_face(rule.cell, rule.axis, rule.initial);
    }

    std::ranges::sort(exchanges_, {}, exchange_key);
    for (std::size_t index = 0; index < exchanges_.size(); ++index) {
        const auto& rule = exchanges_[index];
        if (rule.cell.x >= config.cells_x || rule.cell.z >= config.cells_z ||
            (index > 0 && exchange_key(exchanges_[index - 1]) == exchange_key(rule)) ||
            !(rule.source_m3_per_second >= 0 && rule.source_m3_per_second <= max_exchange_rate) ||
            !(rule.drain_m3_per_second >= 0 && rule.drain_m3_per_second <= max_exchange_rate))
            throw std::invalid_argument("Invalid or duplicate authored water exchange.");
    }

    if (rain_cells_.size() > initial_depths_m_.size())
        throw std::invalid_argument("Rain coverage exceeds the authored water grid.");
    std::ranges::sort(rain_cells_, {}, cell_key);
    for (std::size_t index = 0; index < rain_cells_.size(); ++index) {
        const auto cell = rain_cells_[index];
        if (cell.x >= config.cells_x || cell.z >= config.cells_z ||
            (index > 0 && cell_key(rain_cells_[index - 1]) == cell_key(cell)))
            throw std::invalid_argument("Invalid or duplicate authored water rain cell.");
    }
}

double Region::rain_volume_m3(std::uint64_t depth_nm) const noexcept {
    const double size = grid_.config().cell_size_m;
    return (static_cast<double>(depth_nm) * nm_to_m) *
        static_cast<double>(rain_cells_.size()) * (size * size);
}

double Region::cumulative_rain_m3() const noexcept {
    return rain_volume_m3(cumulative_rain_nm_);
}

double Region::swell_at(double cell_x,double cell_z) const noexcept {
    constexpr double tau=2*std::numbers::pi;
    const double size=grid_.config().cell_size_m;
    std::array<double,2> phase_shift{};
    double envelope=1;
    if (const auto& variation=swell_variation_) {
        const auto patch=swell_patch(*variation,size*cell_x,size*cell_z);
        const auto phase_gain=WaterSwellVariation::phase_radians*variation->strength/std::numbers::sqrt2;
        phase_shift={phase_gain*patch[0],phase_gain*patch[1]};
        envelope-=.5*WaterSwellVariation::amplitude_blend*variation->strength*(1+patch[1]);
    }
    double offset=0;
    for (const auto& wave : swell_waves_) {
        const auto phase=tau*static_cast<double>(grid_.tick()%wave.period_ticks)/wave.period_ticks;
        const double carrier=size*(wave.frequency[0]*cell_x+wave.frequency[1]*cell_z)-phase;
        const double shift=wave.direction[0]*phase_shift[0]+wave.direction[1]*phase_shift[1];
        offset+=wave.amplitude_m*std::sin(carrier+shift);
    }
    return offset*envelope;
}

FittedWaterBed Region::fitted_bed(Cell cell) const {
    const auto sample=grid_.sample(cell);
    const auto& sizing=grid_.config();
    if (shore_corner_heights_m_.empty())
        throw std::logic_error("Terrain-fit water profile is absent.");
    const auto stride=static_cast<std::size_t>(sizing.cells_x+1);
    const auto first=cell.z*stride+cell.x;
    FittedWaterBed out{sample.floor_height_m,
        {shore_corner_heights_m_[first],shore_corner_heights_m_[first+1],
         shore_corner_heights_m_[first+stride+1],shore_corner_heights_m_[first+stride]},{}};
    if (!swell_waves_.empty()) {
        const double x=cell.x,z=cell.z;
        out.swell_center_m=swell_at(x+.5,z+.5);
        out.swell_corners_m={swell_at(x,z),swell_at(x+1,z),swell_at(x+1,z+1),swell_at(x,z+1)};
    }
    if (optics_ && optics_->offshore) {
        const auto& sea=*optics_->offshore;
        const std::array boundary{cell.z==0,cell.x+1==sizing.cells_x,cell.z+1==sizing.cells_z,cell.x==0};
        const auto side=static_cast<std::size_t>(sea.edge);
        if (boundary[side]) {
            // The outer edge belongs to the ocean datum. The inner corners
            // and the center retain their committed local height and phase.
            const auto offset=sea.surface_height_m-sample.surface_height_m;
            out.stages_m.fill(sample.surface_height_m);
            out.swell_corners_m[side]=offset;
            out.swell_corners_m[(side+1)%4]=offset;
            return out;
        }
    }
    if (sample.depth_m>0) {
        out.stages_m.fill(sample.surface_height_m);
        return out;
    }

    const std::array<int,4> dx{0,1,0,-1},dz{-1,0,1,0};
    if (!swell_waves_.empty()) out.inactive_wedges=0xf;
    for (std::size_t side=0;side<dx.size();++side) {
        const double edge_floor=std::min(out.corners_m[side],out.corners_m[(side+1)%dx.size()]);
        out.stages_m[side]=std::min(sample.floor_height_m,edge_floor);
        const auto x=static_cast<int>(cell.x)+dx[side],z=static_cast<int>(cell.z)+dz[side];
        if (x<0 || z<0 || x>=static_cast<int>(sizing.cells_x) || z>=static_cast<int>(sizing.cells_z)) continue;
        const Cell donor{static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(z)};
        const auto neighbor=grid_.sample(donor);
        const double level=neighbor.surface_height_m;
        // Only extend into the wedge facing a wet neighbor. A depressed dry
        // center belongs to the flow solver, including partially filled gates.
        if (neighbor.depth_m==0 || level>sample.floor_height_m || level+swell_amplitude_m_<=edge_floor) continue;
        const Cell face{std::min(cell.x,donor.x),std::min(cell.z,donor.z)};
        const auto control=grid_.face(face,dx[side]!=0 ? FaceAxis::x : FaceAxis::z);
        if (control.openness>0 && (!control.sill_height_m || level>*control.sill_height_m)) {
            out.stages_m[side]=level;
            out.inactive_wedges&=~(1u<<side);
        }
    }
    return out;
}

std::optional<CellSample> Region::sample_world(double x_m,double z_m,
    std::optional<std::array<double,3>>* surface_normal) const {
    if (surface_normal) surface_normal->reset();
    auto sample=grid_.sample_world(x_m,z_m);
    if (!sample || shore_corner_heights_m_.empty()) return sample;
    const auto bed=fitted_bed(sample->cell);
    const auto& sizing=grid_.config();
    const auto u=std::clamp((x_m-sizing.origin_x_m)/sizing.cell_size_m-sample->cell.x,0.,1.);
    const auto v=std::clamp((z_m-sizing.origin_z_m)/sizing.cell_size_m-sample->cell.z,0.,1.);
    const auto levels=fitted_water_levels(bed.center_m,bed.corners_m,bed.stages_m,u,v,
        bed.swell_center_m,bed.swell_corners_m,bed.inactive_wedges);
    sample->floor_height_m=levels[0];
    sample->surface_height_m=levels[1];
    sample->depth_m=levels[1]-levels[0];
    if (surface_normal && (!swell_waves_.empty() || (optics_ && optics_->offshore)) && sample->depth_m>0) {
        const auto gradient=fitted_water_gradient(bed.swell_center_m,bed.swell_corners_m,
            fitted_water_side(u,v),sizing.cell_size_m);
        const auto scale=1/std::sqrt(1+gradient[0]*gradient[0]+gradient[1]*gradient[1]);
        *surface_normal=std::array{-gradient[0]*scale,scale,-gradient[1]*scale};
    }
    return sample;
}

std::optional<std::array<double,2>> Region::offshore_levels(double x_m,double z_m) const noexcept {
    if (!optics_ || !optics_->offshore || !std::isfinite(x_m) || !std::isfinite(z_m)) return std::nullopt;
    const auto& sea=*optics_->offshore;
    const auto& config=grid_.config();
    if (!sea.contains(x_m-config.origin_x_m,z_m-config.origin_z_m,
        config.cells_x*config.cell_size_m,config.cells_z*config.cell_size_m))
        return std::nullopt;
    return std::array{sea.surface_height_m-sea.depth_m,sea.surface_height_m};
}

double Region::submersion_at(double x_m,double y_m,double z_m) const {
    if (const auto local=sample_world(x_m,z_m)) return local->submersion_at(y_m);
    if (const auto ocean=offshore_levels(x_m,z_m); ocean && y_m>=(*ocean)[0] && y_m<(*ocean)[1])
        return (*ocean)[1]-y_m;
    return 0;
}

StepReport Region::step(std::span<const std::string_view> pressed, std::uint32_t rain_depth_nm,
                        std::optional<std::uint64_t> first_quantum) {
    if ((first_quantum && (rain_cells_.empty() || *first_quantum > wetness::max_quantum)) ||
        (rain_depth_nm > 0 && !first_quantum) || rain_depth_nm > max_tick_rain_nm ||
        rain_depth_nm > max_total_rain_nm - cumulative_rain_nm_)
        throw std::invalid_argument("Rainfall exceeds the water region's coverage, clock or volume bounds.");
    struct ChangedFace { std::size_t index; FaceControl previous; };
    std::array<ChangedFace, max_region_faces> changes;
    std::size_t changed = 0;
    try {
        for (std::size_t index = 0; index < faces_.size(); ++index) {
            const auto& rule = faces_[index];
            if (rule.action.empty() || std::ranges::find(pressed, rule.action) == pressed.end())
                continue;
            const auto previous = grid_.face(rule.cell, rule.axis);
            auto updated = rule.initial;
            if (previous.openness == rule.initial.openness)
                updated.openness = rule.action_openness;
            grid_.set_face(rule.cell, rule.axis, updated);
            changes[changed++] = {index, previous};
        }
        const auto rain_cells = rain_depth_nm ? std::span<const Cell>(rain_cells_) : std::span<const Cell>{};
        last_report_ = grid_.step(exchanges_, rain_cells, rain_depth_nm * nm_to_m);
    } catch (...) {
        // Only previously validated face values enter this buffer. Grid::step
        // leaves cells and its tick untouched when it rejects a step.
        while (changed > 0) {
            const auto& change = changes[--changed];
            const auto& rule = faces_[change.index];
            grid_.set_face(rule.cell, rule.axis, change.previous);
        }
        throw;
    }
    if (!first_rain_quantum_)
        first_rain_quantum_ = first_quantum;
    last_rain_nm_ = rain_depth_nm;
    cumulative_rain_nm_ += rain_depth_nm;
    return *last_report_;
}

Json Region::save() const {
    Json action_faces = Json::array();
    for (const auto& rule : faces_)
        if (!rule.action.empty())
            action_faces.push_back(grid_.face(rule.cell, rule.axis).openness);
    auto snapshot = grid_.snapshot();
    const bool rain_enabled = !rain_cells_.empty();
    Json result{{"format", snapshot_format}, {"version", rain_enabled ? 2 : 1}, {"plan_sha256", plan_sha256_},
        {"tick", snapshot.tick}, {"depths_m", std::move(snapshot.depths_m)},
        {"action_face_openness", std::move(action_faces)}, {"last_report", report_json(last_report_, rain_enabled)}};
    if (rain_enabled)
        result["rain"] = {{"first_quantum", first_rain_quantum_ ? Json(*first_rain_quantum_) : Json(nullptr)},
            {"cumulative_depth_nm", cumulative_rain_nm_}, {"last_step_depth_nm", last_rain_nm_}};
    return result;
}

void Region::restore(const Json& saved, std::uint64_t expected_tick,
                     std::optional<std::uint64_t> next_quantum) {
    const bool rain_enabled = !rain_cells_.empty();
    const bool fields_match = rain_enabled
        ? exact_fields(saved, {"format", "version", "plan_sha256", "tick", "depths_m", "action_face_openness", "last_report", "rain"})
        : exact_fields(saved, {"format", "version", "plan_sha256", "tick", "depths_m", "action_face_openness", "last_report"});
    if (!fields_match || saved.at("format") != snapshot_format ||
        unsigned_integer(saved.at("version")) != (rain_enabled ? 2u : 1u) ||
        saved.at("plan_sha256") != plan_sha256_ || unsigned_integer(saved.at("tick")) != expected_tick)
        throw std::invalid_argument("Water snapshot format, plan identity or tick does not match.");

    auto snapshot = grid_.snapshot();
    const auto& depths = saved.at("depths_m");
    if (!depths.is_array() || depths.size() != snapshot.depths_m.size())
        throw std::invalid_argument("Water snapshot depth count does not match the grid.");
    std::transform(depths.begin(), depths.end(), snapshot.depths_m.begin(), finite_nonnegative);
    if (expected_tick == 0 && snapshot.depths_m != initial_depths_m_)
        throw std::invalid_argument("Initial water snapshot depths do not match the plan.");
    snapshot.tick = expected_tick;
    auto candidate = Grid::restore(snapshot);

    const auto& openings = saved.at("action_face_openness");
    const auto action_count = static_cast<std::size_t>(std::ranges::count_if(faces_, [](const RegionFace& rule) {
        return !rule.action.empty();
    }));
    if (!openings.is_array() || openings.size() != action_count)
        throw std::invalid_argument("Water snapshot face count does not match the plan.");
    std::size_t index = 0;
    for (const auto& rule : faces_) {
        if (rule.action.empty())
            continue;
        const double openness = finite_nonnegative(openings.at(index++));
        auto control = rule.initial;
        if (openness != rule.initial.openness) {
            if (expected_tick == 0 || openness != rule.action_openness)
                throw std::invalid_argument("Water snapshot face openness is outside the plan's states.");
            control.openness = rule.action_openness;
        }
        candidate.set_face(rule.cell, rule.axis, control);
    }
    std::optional<std::uint64_t> first_quantum;
    std::uint64_t cumulative_rain_nm = 0;
    std::uint64_t last_rain_nm = 0;
    if (rain_enabled) {
        const auto& rain = saved.at("rain");
        if (!exact_fields(rain, {"first_quantum", "cumulative_depth_nm", "last_step_depth_nm"}))
            throw std::invalid_argument("Water snapshot has an incomplete rain receipt.");
        if (!rain.at("first_quantum").is_null())
            first_quantum = unsigned_integer(rain.at("first_quantum"));
        cumulative_rain_nm = unsigned_integer(rain.at("cumulative_depth_nm"));
        last_rain_nm = unsigned_integer(rain.at("last_step_depth_nm"));
        if (first_quantum.has_value() != next_quantum.has_value() ||
            (first_quantum && (!expected_tick || *first_quantum >= *next_quantum || *next_quantum > wetness::max_quantum + 1)))
            throw std::invalid_argument("Water snapshot rain receipt differs from the saved weather clock.");
        const auto quanta = first_quantum ? *next_quantum - *first_quantum : 0;
        if ((quanta && (quanta + wetness::max_batch - 1) / wetness::max_batch > expected_tick) ||
            cumulative_rain_nm > quanta * wetness::max_rain_nm ||
            last_rain_nm > max_tick_rain_nm || last_rain_nm > cumulative_rain_nm)
            throw std::invalid_argument("Water snapshot rain receipt exceeds the weather clock or tick bounds.");
    }
    const auto rain_m3 = rain_enabled ? std::optional(rain_volume_m3(last_rain_nm)) : std::nullopt;
    const auto receipt = validated_report(saved.at("last_report"), candidate, exchanges_, rain_m3);
    grid_ = std::move(candidate);
    last_report_ = receipt;
    first_rain_quantum_ = first_quantum;
    cumulative_rain_nm_ = cumulative_rain_nm;
    last_rain_nm_ = static_cast<std::uint32_t>(last_rain_nm);
}

}
