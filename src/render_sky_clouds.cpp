// SPDX-License-Identifier: Apache-2.0
#include "render_sky.hpp"

#include "poima/atmosphere_dimensions.hpp"
#include "poima/cloud_noise_cs.hpp"
#include "poima/cloud_view_cs.hpp"
#include "poima/render_schedule.hpp"
#include "poima/scene.hpp"
#include "render_embedded_shader.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace poima::render_sky {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

float advected_origin_km(double position_m, double wind_m_per_s, double seconds) {
    // Reduce position and displacement separately to retain wind motion and
    // ensure the noise lookup remains periodic at large world coordinates.
    constexpr double period_m = 64000.0;
    const auto position = std::fmod(position_m, period_m);
    const auto travel = std::fmod(wind_m_per_s * seconds, period_m);
    const auto offset = std::fmod(position - travel, period_m);
    const auto result = static_cast<float>((offset < 0 ? offset + period_m : offset) * 0.001);
    return result < 64.f ? result : 0.f;
}

nvrhi::TextureHandle make_cloud_texture(nvrhi::IDevice& device, nvrhi::Format format,
    std::uint32_t width, std::uint32_t height, std::uint32_t layers, const char* name) {
    const auto required = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::ShaderLoad |
        nvrhi::FormatSupport::ShaderSample | nvrhi::FormatSupport::ShaderUavStore;
    require((device.queryFormatSupport(format) & required) == required,
        "Volumetric clouds require filterable, writable RGBA8 and RGBA16F textures.");
    nvrhi::TextureDesc description;
    description.width = width;
    description.height = height;
    description.arraySize = layers;
    description.dimension = layers == 1 ? nvrhi::TextureDimension::Texture2D : nvrhi::TextureDimension::Texture2DArray;
    description.format = format;
    description.isShaderResource = true;
    description.isUAV = true;
    description.initialState = nvrhi::ResourceStates::ShaderResource;
    description.keepInitialState = true;
    description.debugName = name;
    auto texture = device.createTexture(description);
    require(bool(texture), "Volumetric cloud lookup allocation failed.");
    return texture;
}

constexpr std::uint32_t noise_group_layers = 4;
static_assert(atmosphere::cloud_noise_size % atmosphere::thread_group_size == 0 &&
    atmosphere::cloud_noise_size % noise_group_layers == 0);

void require_bindings(nvrhi::IBindingSet* bindings, const nvrhi::BindingSetDesc& expected) {
    require(bindings && std::ranges::equal(bindings->getDesc()->bindings, expected.bindings),
        "Volumetric cloud binding differs from its validated schedule.");
}
}

void Pass::allocate_cloud_tables(nvrhi::IDevice& device) {
    const auto noise_size = atmosphere::cloud_noise_size;
    auto noise = make_cloud_texture(device, nvrhi::Format::RGBA8_UNORM,
        noise_size, noise_size, noise_size, "Volumetric cloud noise");
    auto view = make_cloud_texture(device, nvrhi::Format::RGBA16_FLOAT,
        atmosphere::cloud_view_width, atmosphere::cloud_view_height, 1, "Volumetric cloud scattering and transmittance");
    auto noise_sampler = device.createSampler(nvrhi::SamplerDesc().setAllFilters(true)
        .setAllAddressModes(nvrhi::SamplerAddressMode::Wrap));
    require(bool(noise_sampler), "Volumetric cloud noise sampler creation failed.");

    const auto shader_desc = nvrhi::ShaderDesc(nvrhi::ShaderType::Compute).setEntryName("compute_main");
    auto noise_shader = create_embedded_shader(&device, shader_desc, poima_cloud_noise_cs);
    auto view_shader = create_embedded_shader(&device, shader_desc, poima_cloud_view_cs);
    auto noise_layout = device.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0)));
    auto view_layout = device.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
        .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1))
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(CloudParameters)))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
        .addItem(nvrhi::BindingLayoutItem::Sampler(0))
        .addItem(nvrhi::BindingLayoutItem::Sampler(1))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0)));
    require(noise_shader && view_shader && noise_layout && view_layout,
        "Volumetric cloud shader or compute layout creation failed.");
    auto noise_pipeline = device.createComputePipeline(nvrhi::ComputePipelineDesc()
        .setComputeShader(noise_shader).addBindingLayout(noise_layout));
    auto view_pipeline = device.createComputePipeline(nvrhi::ComputePipelineDesc()
        .setComputeShader(view_shader).addBindingLayout(view_layout));
    auto noise_bindings = device.createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::Texture_UAV(0, noise)), noise_layout);
    require(noise_pipeline && view_pipeline && noise_bindings,
        "Volumetric cloud compute pipeline or bindings creation failed.");

    cloud_noise_ = std::move(noise);
    cloud_view_ = std::move(view);
    cloud_noise_sampler_ = std::move(noise_sampler);
    cloud_noise_compute_ = {std::move(noise_pipeline), std::move(noise_bindings)};
    cloud_view_compute_.pipeline = std::move(view_pipeline);
    cloud_view_layout_ = std::move(view_layout);
}

nvrhi::BindingSetDesc Pass::cloud_view_binding_desc(nvrhi::IBuffer* frame, nvrhi::ITexture* noise,
    nvrhi::ITexture* density, nvrhi::ITexture* view) const {
    return nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, frame))
        .addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(CloudParameters)))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, noise))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(1, density))
        .addItem(nvrhi::BindingSetItem::Sampler(0, cloud_noise_sampler_))
        .addItem(nvrhi::BindingSetItem::Sampler(1, atmosphere_sampler_))
        .addItem(nvrhi::BindingSetItem::Texture_UAV(0, view));
}

void Pass::prepare_clouds(nvrhi::IDevice& device, nvrhi::IBuffer* frame, const SceneSnapshot& scene,
    const AtmosphereSettings& atmosphere, std::uint64_t world_milliseconds) {
    const auto& profile = *atmosphere.clouds;
    const auto seconds = static_cast<double>(world_milliseconds) * 0.001;
    cloud_parameters_ = {
        .profile = {profile.base_height_m * 0.001f, profile.thickness_m * 0.001f,
            profile.coverage, profile.extinction_per_km},
        .origin = {advected_origin_km(scene.camera_world[12], profile.wind_m_per_s[0], seconds),
            advected_origin_km(scene.camera_world[14], profile.wind_m_per_s[1], seconds),
            static_cast<float>((scene.camera_world[13] - atmosphere.sea_level_m) * 0.001), atmosphere.aerosol_strength},
        .sun = {constants_.sun_intensity[0], constants_.sun_intensity[1], constants_.sun_intensity[2], constants_.forward_srgb[3]},
        .tint = {constants_.sun_color[0], constants_.sun_color[1], constants_.sun_color[2], 0}
    };

    if (!cloud_view_) allocate_cloud_tables(device);
    if (cloud_view_compute_.bindings && cloud_frame_ == frame) return;
    auto bindings = device.createBindingSet(
        cloud_view_binding_desc(frame, cloud_noise_, density_, cloud_view_), cloud_view_layout_);
    require(bool(bindings), "Volumetric cloud view bindings creation failed.");
    cloud_view_compute_.bindings = std::move(bindings);
    cloud_frame_ = frame;
}

std::uint64_t Pass::cloud_resident_bytes() const noexcept {
    return cloud_view_ ? atmosphere::cloud_noise_bytes + atmosphere::cloud_view_bytes : 0;
}

void Pass::verify_cloud_bindings(const render_schedule::Schedule& plan) const {
    require(plan.settings.volumetric_clouds && clouds_enabled_,
        "Volumetric cloud state differs from its validated schedule.");
    const auto texture = [&](render_schedule::ResourceId id) {
        return reinterpret_cast<nvrhi::ITexture*>(plan.resource(id).identity);
    };
    const auto& resources = plan.frame;
    auto* frame = reinterpret_cast<nvrhi::IBuffer*>(plan.resource(resources.frame).identity);
    auto* density = texture(resources.atmosphere_density);
    auto* noise = texture(resources.cloud_noise);
    auto* clouds = texture(resources.cloud_view);
    require_bindings(cloud_noise_compute_.bindings,
        nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::Texture_UAV(0, noise)));
    require_bindings(cloud_view_compute_.bindings, cloud_view_binding_desc(frame, noise, density, clouds));
    require_bindings(cloud_raster_.bindings, sky_binding_desc(frame, density, texture(resources.atmosphere_view), clouds));
}

void Pass::compute_cloud_noise(nvrhi::ICommandList& commands) {
    require(cloud_noise_dirty() && !cloud_noise_recorded_, "Unexpected volumetric cloud noise dispatch.");
    nvrhi::ComputeState state;
    state.pipeline = cloud_noise_compute_.pipeline;
    state.bindings.push_back(cloud_noise_compute_.bindings);
    commands.setComputeState(state);
    constexpr auto group = atmosphere::thread_group_size;
    commands.dispatch(atmosphere::cloud_noise_size / group, atmosphere::cloud_noise_size / group,
        atmosphere::cloud_noise_size / noise_group_layers);
    cloud_noise_recorded_ = true;
}

void Pass::compute_cloud_view(nvrhi::ICommandList& commands) {
    require(clouds_enabled_ && !cloud_view_recorded_ && (!cloud_noise_dirty() || cloud_noise_recorded_) &&
        (!atmosphere_density_dirty() || density_recorded_), "Volumetric cloud view dispatch needs current optical density and cloud noise.");
    nvrhi::ComputeState state;
    state.pipeline = cloud_view_compute_.pipeline;
    state.bindings.push_back(cloud_view_compute_.bindings);
    commands.setComputeState(state);
    commands.setPushConstants(&cloud_parameters_, sizeof(CloudParameters));
    constexpr auto group = atmosphere::thread_group_size;
    commands.dispatch((atmosphere::cloud_view_width + group - 1) / group,
        (atmosphere::cloud_view_height + group - 1) / group, 1);
    cloud_view_recorded_ = true;
}

}
