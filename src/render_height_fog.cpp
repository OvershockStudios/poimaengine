// SPDX-License-Identifier: Apache-2.0
#include "poima/water_bed.hpp"
#include "render_height_fog.hpp"

#include "poima/aerial_perspective_cs.hpp"
#include "poima/atmosphere_dimensions.hpp"
#include "poima/height_fog_aerial_cs.hpp"
#include "poima/height_fog_cs.hpp"
#include "poima/height_fog_water_cs.hpp"
#include "poima/height_fog_water_aerial_cs.hpp"
#include "poima/render_schedule.hpp"
#include "poima/scene.hpp"
#include "render_embedded_shader.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace poima::height_fog {
namespace {
struct alignas(16) FogConstants {
    std::array<float, 4> ambient_density;
    std::array<float, 4> distance_height;
    std::array<std::uint32_t, 4> spot_ordinals;
    std::array<float, 4> scattering;
};
struct alignas(16) FogAerialConstants {
    FogConstants fog;
    std::array<float, 4> aerial;
};
struct alignas(16) FogWaterConstants {
    FogConstants fog;
    WaterParameters water;
};
struct alignas(16) FogAerialWaterConstants {
    FogAerialConstants fog;
    WaterParameters water;
};
static_assert(sizeof(FogConstants) == 64 && offsetof(FogConstants, distance_height) == 16 &&
              offsetof(FogConstants, spot_ordinals) == 32 && offsetof(FogConstants, scattering) == 48);
static_assert(sizeof(FogAerialConstants) == 80 && offsetof(FogAerialConstants, aerial) == 64);
static_assert(sizeof(WaterParameters)==48 && offsetof(WaterParameters,dimensions)==28 &&
              offsetof(WaterParameters,radiance_wave_mode)==32);
static_assert(sizeof(FogWaterConstants)==112 && sizeof(FogAerialWaterConstants)==128);
static_assert(sizeof(AerialParameters) == 48 && offsetof(AerialParameters, sunlight_rgb) == 16 &&
              offsetof(AerialParameters, optics) == 32);

constexpr std::size_t variant(bool aerial, bool water) { return (aerial ? 1u : 0u) + (water ? 2u : 0u); }
constexpr std::uint32_t push_size(bool aerial, bool water) {
    return (aerial ? sizeof(FogAerialConstants) : sizeof(FogConstants)) + (water ? sizeof(WaterParameters) : 0);
}

nvrhi::BindingSetDesc fog_binding_desc(const Inputs& inputs, nvrhi::ITexture* destination,
                                      nvrhi::ITexture* scattering, nvrhi::ITexture* transmittance,
                                      nvrhi::ISampler* sampler) {
    nvrhi::BindingSetDesc desc;
    desc
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, inputs.frame))
        .addItem(nvrhi::BindingSetItem::PushConstants(0,push_size(inputs.atmosphere_density,inputs.water_columns)))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, inputs.surface_hdr))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(1, inputs.depth))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(5, inputs.shadow))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, inputs.lights))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(0, destination));
    if (inputs.atmosphere_density) {
        desc.addItem(nvrhi::BindingSetItem::Texture_SRV(2, scattering))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(3, transmittance))
            .addItem(nvrhi::BindingSetItem::Sampler(0, sampler));
    }
    if (inputs.water_columns) {
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7,inputs.water_columns))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(8,inputs.water_specular))
            .addItem(nvrhi::BindingSetItem::Sampler(1,inputs.water_sampler));
    }
    return desc;
}

nvrhi::BindingSetDesc aerial_binding_desc(const Inputs& inputs, nvrhi::ITexture* scattering,
                                         nvrhi::ITexture* transmittance, nvrhi::ISampler* sampler) {
    return nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, inputs.frame))
        .addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(AerialParameters)))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, inputs.atmosphere_density))
        .addItem(nvrhi::BindingSetItem::Sampler(0, sampler))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(0, scattering))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(1, transmittance));
}

nvrhi::ComputePipelineHandle make_pipeline(nvrhi::IDevice& device, nvrhi::IBindingLayout* layout, const auto& spirv) {
    const auto shader = create_embedded_shader(&device,
        nvrhi::ShaderDesc(nvrhi::ShaderType::Compute).setEntryName("compute_main"), spirv);
    if (!shader || !layout) throw std::runtime_error("Atmospheric media shader or layout creation failed.");
    auto pipeline = device.createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader).addBindingLayout(layout));
    if (!pipeline) throw std::runtime_error("Atmospheric media pipeline creation failed.");
    return pipeline;
}

void require_bindings(nvrhi::IBindingSet* bindings, const nvrhi::BindingSetDesc& expected) {
    if (!bindings) throw std::runtime_error("Atmospheric media schedule has no prepared bindings.");
    const auto& actual = bindings->getDesc()->bindings;
    if (actual.size() != expected.bindings.size())
        throw std::runtime_error("Atmospheric media schedule binding count changed.");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (!(actual[i] == expected.bindings[i]))
            throw std::runtime_error("Atmospheric media binding differs from its validated schedule.");
    }
}
}

WaterView project_water_view(const SceneWaterVolume& water,const std::array<double,3>& eye,float wave_mode) {
    namespace rs=render_schedule;
    const auto& sea=water.optics.offshore;
    const bool shaped=water.surface_swell || sea.has_value();
    static_assert(sizeof(SceneWaterColumn)==rs::water_column_bytes);
    static_assert(offsetof(SceneWaterColumn,corners)==16 && offsetof(SceneWaterColumn,swell_corners)==48);
    if (water.cells_x==0 || water.cells_z==0 || water.cells_x>rs::max_water_column_side ||
        water.cells_z>rs::max_water_column_side || water.columns.size()!=water.cells_x*water.cells_z ||
        !(water.cell_size_m>=.01 && water.cell_size_m<=256) ||
        (shaped && !water.optics.follow_terrain))
        throw std::runtime_error("Invalid water optics column grid.");
    if (!std::ranges::all_of(water.columns,[&](const SceneWaterColumn& cell) {
        const auto finite=[](auto lane){return std::isfinite(lane);};
        return std::isfinite(cell.floor_m) && std::isfinite(cell.stage_m) && cell.floor_m<=cell.stage_m &&
            (cell.flags&1u)==static_cast<std::uint32_t>(water.optics.follow_terrain) &&
            cell.inactive_wedges()<=(water.surface_swell ? 0xfu : 0u) &&
            (!water.optics.follow_terrain ||
             (std::ranges::all_of(cell.corners,finite) && std::ranges::all_of(cell.stages,finite))) &&
            (shaped ?
                finite(cell.swell_center_m) && std::ranges::all_of(cell.swell_corners,finite) :
                cell.swell_center_m==0 && cell.swell_corners==std::array<float,4>{});
    })) throw std::runtime_error("Invalid water column height.");

    WaterView result{};
    auto& parameters=result.parameters;
    std::array<double,3> local{};
    for (std::size_t axis=0;axis<local.size();++axis) {
        local[axis]=eye[axis]-water.origin_m[axis];
        const double extinction=water.optics.extinction_per_m[axis],scatter=water.optics.scattered_radiance[axis];
        if (!(std::isfinite(local[axis]) && std::abs(local[axis])<=std::numeric_limits<float>::max() &&
            extinction>=0 && extinction<=8 && scatter>=0 && scatter<=16))
            throw std::runtime_error("Invalid water optics coefficients or origin.");
        parameters.camera_cell_size[axis]=static_cast<float>(local[axis]);
        parameters.extinction_per_m[axis]=static_cast<float>(extinction);
        parameters.radiance_wave_mode[axis]=static_cast<float>(scatter);
    }
    parameters.camera_cell_size[3]=static_cast<float>(water.cell_size_m);
    parameters.dimensions=water.cells_x|(water.cells_z<<8u)|(water.optics.refraction ? 1u<<16u : 0u)|
        (shaped ? 1u<<17u : 0u);
    parameters.radiance_wave_mode[3]=wave_mode;
    const double x=local[0]/water.cell_size_m,z=local[2]/water.cell_size_m;
    if (sea) {
        const double level=sea->surface_height_m-water.origin_m[1];
        const double bed=level-sea->depth_m;
        const auto edge=static_cast<std::uint32_t>(sea->edge);
        if (edge>3 || !(std::abs(sea->surface_height_m)<=1e6) || !(sea->depth_m>=.1 && sea->depth_m<=10'000))
            throw std::runtime_error("Invalid offshore water edge or levels.");
        const double boundary=sea->edge==WaterEdge::max_x ? water.cells_x*water.cell_size_m :
            sea->edge==WaterEdge::max_z ? water.cells_z*water.cell_size_m : 0;
        result.offshore={static_cast<float>(edge+1),static_cast<float>(level),static_cast<float>(bed),
            static_cast<float>(boundary)};
        if (sea->contains(x,z,water.cells_x,water.cells_z) && local[1]>=bed && local[1]<level)
            result.submersion_m=level-local[1];
    }
    if (x>=0 && z>=0 && x<water.cells_x && z<water.cells_z) {
        const auto ix=static_cast<std::size_t>(x),iz=static_cast<std::size_t>(z);
        const auto& column=water.columns[iz*water.cells_x+ix];
        const auto levels=water.optics.follow_terrain ?
            water::fitted_water_levels(column.floor_m,column.corners,column.stages,
                static_cast<float>(x-static_cast<double>(ix)),static_cast<float>(z-static_cast<double>(iz)),
                column.swell_center_m,column.swell_corners,column.inactive_wedges()) :
            std::array<float,2>{column.floor_m,column.stage_m};
        if (local[1]>=levels[0] && local[1]<levels[1])
            result.submersion_m=static_cast<double>(levels[1])-local[1];
    }
    return result;
}

nvrhi::BufferHandle allocate_water_columns(nvrhi::IDevice& device) {
    namespace rs=render_schedule;
    constexpr auto side=rs::max_water_column_side;
    auto buffer=device.createBuffer(nvrhi::BufferDesc().setByteSize(side*side*rs::water_column_bytes)
        .setStructStride(rs::water_column_bytes).setInitialState(nvrhi::ResourceStates::ShaderResource)
        .setKeepInitialState(true).setDebugName("Committed water columns"));
    if (!buffer) throw std::runtime_error("Water optics column buffer allocation failed.");
    return buffer;
}

const Pass::Pipeline& Pass::prepare_compositor(nvrhi::IDevice& device, bool aerial, bool water) {
    auto& selected=compositors_[variant(aerial,water)];
    if (selected.handle) return selected;
    nvrhi::BindingLayoutDesc desc;
    desc.setVisibility(nvrhi::ShaderType::Compute)
        .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1))
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0,push_size(aerial,water)))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(5))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
    if (aerial) {
        desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(2))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(3))
            .addItem(nvrhi::BindingLayoutItem::Sampler(0));
    }
    if (water) {
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(8))
            .addItem(nvrhi::BindingLayoutItem::Sampler(1));
    }
    auto layout = device.createBindingLayout(desc);
    auto pipeline=water ?
        (aerial ? make_pipeline(device,layout,poima_height_fog_water_aerial_cs)
                : make_pipeline(device,layout,poima_height_fog_water_cs)) :
        (aerial ? make_pipeline(device,layout,poima_height_fog_aerial_cs)
                : make_pipeline(device,layout,poima_height_fog_cs));
    selected = {std::move(layout), std::move(pipeline)};
    return selected;
}

void Pass::prepare_aerial_tables(nvrhi::IDevice& device) {
    if (aerial_compute_.handle) return;
    const auto required = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::ShaderLoad |
        nvrhi::FormatSupport::ShaderSample | nvrhi::FormatSupport::ShaderUavStore;
    constexpr auto format = nvrhi::Format::RGBA16_FLOAT;
    if ((device.queryFormatSupport(format) & required) != required)
        throw std::runtime_error("Aerial perspective requires filterable, writable RGBA16F tables.");
    nvrhi::TextureDesc desc;
    desc.width = atmosphere::aerial_width;
    desc.height = atmosphere::aerial_height;
    desc.arraySize = atmosphere::aerial_depth;
    desc.dimension = nvrhi::TextureDimension::Texture2DArray;
    desc.format = format;
    desc.isShaderResource = true;
    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = "Aerial perspective in-scattered radiance";
    auto scattering = device.createTexture(desc);
    desc.debugName = "Aerial perspective transmittance";
    auto transmittance = device.createTexture(desc);
    auto sampler = device.createSampler(nvrhi::SamplerDesc().setAllFilters(true)
        .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
    if (!scattering || !transmittance || !sampler)
        throw std::runtime_error("Aerial perspective table or sampler allocation failed.");
    auto layout = device.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
        .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1))
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(AerialParameters)))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Sampler(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(1)));
    auto pipeline = make_pipeline(device, layout, poima_aerial_perspective_cs);
    aerial_scattering_ = std::move(scattering);
    aerial_transmittance_ = std::move(transmittance);
    aerial_sampler_ = std::move(sampler);
    aerial_compute_ = {std::move(layout), std::move(pipeline)};
}

void Pass::prepare(nvrhi::IDevice& device, const Inputs& inputs) {
    if (!inputs.frame || !inputs.surface_hdr || !inputs.depth || !inputs.shadow || !inputs.lights)
        throw std::runtime_error("Height fog requires frame, HDR, depth, shadow and light resources.");
    const auto& surface = inputs.surface_hdr->getDesc();
    const auto& depth = inputs.depth->getDesc();
    const auto& shadow = inputs.shadow->getDesc();
    const auto pixels = std::uint64_t(surface.width) * surface.height;
    if (!pixels || surface.sampleCount != 1 || surface.format != nvrhi::Format::RGBA16_FLOAT ||
        depth.sampleCount != 1 || depth.format != nvrhi::Format::D32 || depth.width != surface.width || depth.height != surface.height ||
        shadow.sampleCount != 1 || shadow.format != nvrhi::Format::D32 || shadow.dimension != nvrhi::TextureDimension::Texture2DArray ||
        pixels > render_schedule::height_fog_budget_bytes / 8)
        throw std::runtime_error("Height fog resources exceed the single-sample 96 MiB profile or have incompatible formats.");

    auto target = fogged_hdr_;
    if (!target) {
        nvrhi::TextureDesc desc;
        desc.width = surface.width;
        desc.height = surface.height;
        desc.format = nvrhi::Format::RGBA16_FLOAT;
        desc.isUAV = true;
        desc.isRenderTarget = true;
        desc.isShaderResource = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = "Height fog scene radiance";
        target = device.createTexture(desc);
        if (!target) throw std::runtime_error("Height fog image allocation failed.");
    }
    if (target->getDesc().width != surface.width || target->getDesc().height != surface.height ||
        target.Get() == inputs.surface_hdr)
        throw std::runtime_error("Height fog requires a distinct output at the current surface extent.");

    if (bindings_ && inputs_ == inputs) return;
    nvrhi::BindingSetHandle aerial_bindings;
    if (inputs.atmosphere_density) {
        const auto& density = inputs.atmosphere_density->getDesc();
        if (density.format != nvrhi::Format::RGBA32_FLOAT || density.dimension != nvrhi::TextureDimension::Texture2D ||
            density.width != atmosphere::density_width || density.height != atmosphere::density_height ||
            density.sampleCount != 1 || !density.isShaderResource)
            throw std::runtime_error("Aerial perspective requires the atmospheric optical-density table.");
        prepare_aerial_tables(device);
        aerial_bindings = device.createBindingSet(
            aerial_binding_desc(inputs, aerial_scattering_, aerial_transmittance_, aerial_sampler_), aerial_compute_.layout);
        if (!aerial_bindings) throw std::runtime_error("Aerial perspective bindings creation failed.");
    }
    if (inputs.water_columns && (!inputs.water_specular || !inputs.water_sampler))
        throw std::runtime_error("Water optics require the prepared environment descriptors.");
    const auto& compositor=prepare_compositor(device,inputs.atmosphere_density,inputs.water_columns);
    auto bindings = device.createBindingSet(
        fog_binding_desc(inputs, target, aerial_scattering_, aerial_transmittance_, aerial_sampler_), compositor.layout);
    if (!bindings) throw std::runtime_error("Atmospheric media compositor bindings creation failed.");
    fogged_hdr_ = std::move(target);
    bindings_ = std::move(bindings);
    aerial_bindings_ = std::move(aerial_bindings);
    inputs_ = inputs;
    buffer_bytes_ = pixels * 8;
}

void Pass::clear_targets() noexcept {
    bindings_ = nullptr;
    aerial_bindings_ = nullptr;
    fogged_hdr_ = nullptr;
    inputs_ = {};
    buffer_bytes_ = 0;
}

std::uint64_t Pass::aerial_buffer_bytes() const noexcept {
    return aerial_scattering_ ? 2 * atmosphere::aerial_table_bytes : 0;
}

void Pass::verify_bindings(const render_schedule::Schedule& plan) const {
    const auto& frame = plan.frame;
    const auto texture = [&](render_schedule::ResourceId id) {
        return reinterpret_cast<nvrhi::ITexture*>(plan.resource(id).identity);
    };
    const auto buffer = [&](render_schedule::ResourceId id) {
        return reinterpret_cast<nvrhi::IBuffer*>(plan.resource(id).identity);
    };
    // With all light ordinals zero the pass never reads the light buffer. The Vulkan layout
    // still expects a real descriptor when no light input is in the schedule.
    const bool aerial = plan.settings.aerial_perspective;
    const bool water=frame.water_columns!=render_schedule::none;
    if (aerial != (inputs_.atmosphere_density != nullptr))
        throw std::runtime_error("Aerial perspective state differs from its validated schedule.");
    if (water != (inputs_.water_columns!=nullptr))
        throw std::runtime_error("Water optics state differs from its validated schedule.");
    const Inputs scheduled{
        .frame = buffer(frame.frame),
        .surface_hdr = texture(render_schedule::surface_radiance(frame, plan.settings)),
        .depth = texture(frame.depth),
        .shadow = texture(frame.shadow),
        .lights = render_schedule::world_media_reads_lighting(plan.settings) ? buffer(frame.lights) : inputs_.lights,
        .atmosphere_density = aerial ? texture(frame.atmosphere_density) : nullptr,
        .water_columns=water ? buffer(frame.water_columns) : nullptr,
        .water_specular=water ? texture(frame.ibl_specular) : nullptr,
        .water_sampler=inputs_.water_sampler
    };
    const auto scattering = aerial ? texture(frame.aerial_scattering) : nullptr;
    const auto transmittance = aerial ? texture(frame.aerial_transmittance) : nullptr;
    if (aerial) {
        require_bindings(aerial_bindings_, aerial_binding_desc(scheduled, scattering, transmittance, aerial_sampler_));
    }
    require_bindings(bindings_,
        fog_binding_desc(scheduled, texture(frame.fogged_hdr), scattering, transmittance, aerial_sampler_));
}

void Pass::dispatch_aerial(nvrhi::ICommandList& commands, const AerialParameters& parameters) const {
    if (!aerial_bindings_ || !(parameters.optics[1] > 0) || !std::isfinite(parameters.optics[1]))
        throw std::runtime_error("Aerial perspective requires prepared bindings and a positive maximum distance.");
    nvrhi::ComputeState state;
    state.pipeline = aerial_compute_.handle;
    state.bindings.push_back(aerial_bindings_);
    commands.setComputeState(state);
    commands.setPushConstants(&parameters, sizeof(parameters));
    constexpr auto group = atmosphere::thread_group_size;
    commands.dispatch((atmosphere::aerial_width + group - 1) / group,
                      (atmosphere::aerial_height + group - 1) / group, 1);
}

void Pass::dispatch(nvrhi::ICommandList& commands, const HeightFog& profile,
                    const std::array<std::uint32_t, 2>& spot_ordinals, float aerial_max_distance_m,
                    const WaterParameters* water) const {
    if (profile.spot_lights.size() > spot_ordinals.size()) throw std::runtime_error("Height fog supports at most two spotlights.");
    const bool aerial = inputs_.atmosphere_density != nullptr;
    if ((water!=nullptr)!=(inputs_.water_columns!=nullptr))
        throw std::runtime_error("Water optics parameters differ from the prepared compositor.");
    if (aerial ? !(aerial_max_distance_m > 0) || !std::isfinite(aerial_max_distance_m) : aerial_max_distance_m != 0)
        throw std::runtime_error("Aerial perspective distance differs from the prepared compositor.");
    FogConstants constants{
        {profile.ambient_radiance[0], profile.ambient_radiance[1], profile.ambient_radiance[2], profile.density_per_m},
        {profile.start_distance_m, profile.max_distance_m, profile.base_height_m, profile.height_falloff_per_m},
        {spot_ordinals[0], spot_ordinals[1], 0, 0},
        {0, 0, profile.anisotropy, 0}
    };
    for (std::size_t i = 0; i < spot_ordinals.size(); ++i) {
        if (!spot_ordinals[i]) continue;
        if (i >= profile.spot_lights.size() || spot_ordinals[i] > max_scene_lights)
            throw std::runtime_error("Height fog spotlight ordinal exceeds the prepared profile.");
        constants.scattering[i] = profile.spot_lights[i].scattering;
    }
    nvrhi::ComputeState state;
    state.pipeline=compositors_[variant(aerial,water!=nullptr)].handle;
    state.bindings.push_back(bindings_);
    commands.setComputeState(state);
    if (aerial && water) {
        const FogAerialWaterConstants extended{{constants,{aerial_max_distance_m,0,0,0}},*water};
        commands.setPushConstants(&extended,sizeof(extended));
    } else if (water) {
        const FogWaterConstants extended{constants,*water};
        commands.setPushConstants(&extended,sizeof(extended));
    } else if (aerial) {
        const FogAerialConstants extended{constants, {aerial_max_distance_m, 0, 0, 0}};
        commands.setPushConstants(&extended, sizeof(extended));
    } else {
        commands.setPushConstants(&constants, sizeof(constants));
    }
    const auto& target = fogged_hdr_->getDesc();
    commands.dispatch((target.width + 7) / 8, (target.height + 7) / 8, 1);
}
}
