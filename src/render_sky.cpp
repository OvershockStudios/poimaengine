// SPDX-License-Identifier: Apache-2.0
#include "render_sky.hpp"

#include "poima/atmosphere_density_cs.hpp"
#include "poima/atmosphere_dimensions.hpp"
#include "poima/atmosphere_sky_view_cs.hpp"
#include "poima/scene.hpp"
#include "poima/sky_atmosphere_ps.hpp"
#include "poima/sky_clouds_ps.hpp"
#include "poima/sky_ps.hpp"
#include "poima/sky_vs.hpp"
#include "render_embedded_shader.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace poima::render_sky {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

float gpu_float(double value) {
    require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(),
        "Scene matrix exceeds GPU float range.");
    return static_cast<float>(value);
}

nvrhi::TextureHandle make_atmosphere_texture(nvrhi::IDevice& device, nvrhi::Format format,
    std::uint32_t width, std::uint32_t height, const char* name) {
    const auto support = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::ShaderLoad |
        nvrhi::FormatSupport::ShaderSample | nvrhi::FormatSupport::ShaderUavStore;
    require((device.queryFormatSupport(format) & support) == support,
        "Atmospheric sky requires filterable, writable floating-point lookup textures.");
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.isUAV = true;
    desc.isShaderResource = true;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = name;
    auto texture = device.createTexture(desc);
    require(bool(texture), "Atmospheric sky lookup texture allocation failed.");
    return texture;
}

static_assert(atmosphere::density_width % atmosphere::thread_group_size == 0 &&
    atmosphere::density_height % atmosphere::thread_group_size == 0 &&
    atmosphere::sky_view_width % atmosphere::thread_group_size == 0 &&
    atmosphere::sky_view_height % atmosphere::thread_group_size == 0);
}

void Pass::prepare_atmosphere(nvrhi::IDevice& device) {
    if (sky_view_compute_.pipeline) return;

    auto density = make_atmosphere_texture(device, nvrhi::Format::RGBA32_FLOAT,
        atmosphere::density_width, atmosphere::density_height, "Atmosphere optical density");
    auto view = make_atmosphere_texture(device, nvrhi::Format::RGBA16_FLOAT,
        atmosphere::sky_view_width, atmosphere::sky_view_height, "Atmosphere sky radiance");
    auto sampler = device.createSampler(nvrhi::SamplerDesc().setAllFilters(true)
        .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
    require(bool(sampler), "Atmospheric sky sampler creation failed.");

    const auto make_compute = [&](const auto& spirv, const nvrhi::BindingLayoutDesc& layout_desc,
        const nvrhi::BindingSetDesc& binding_desc) {
        const auto shader = create_embedded_shader(&device,
            nvrhi::ShaderDesc(nvrhi::ShaderType::Compute).setEntryName("compute_main"), spirv);
        const auto layout = device.createBindingLayout(layout_desc);
        require(shader && layout, "Atmospheric sky compute shader or layout creation failed.");
        auto pipeline = device.createComputePipeline(nvrhi::ComputePipelineDesc()
            .setComputeShader(shader).addBindingLayout(layout));
        auto bindings = device.createBindingSet(binding_desc, layout);
        require(pipeline && bindings, "Atmospheric sky compute pipeline or bindings creation failed.");
        return Compute{std::move(pipeline), std::move(bindings)};
    };
    auto density_compute = make_compute(poima_atmosphere_density_cs,
        nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
            .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0)),
        nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::Texture_UAV(0, density)));
    const std::uint32_t parameter_bytes = sizeof(sky_view_parameters_);
    auto view_compute = make_compute(poima_atmosphere_sky_view_cs,
        nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0, parameter_bytes))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
            .addItem(nvrhi::BindingLayoutItem::Sampler(0))
            .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0)),
        nvrhi::BindingSetDesc()
            .addItem(nvrhi::BindingSetItem::PushConstants(0, parameter_bytes))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(0, density))
            .addItem(nvrhi::BindingSetItem::Sampler(0, sampler))
            .addItem(nvrhi::BindingSetItem::Texture_UAV(0, view)));
    density_ = std::move(density);
    sky_view_ = std::move(view);
    atmosphere_sampler_ = std::move(sampler);
    density_compute_ = std::move(density_compute);
    sky_view_compute_ = std::move(view_compute);
}

nvrhi::BindingSetDesc Pass::sky_binding_desc(nvrhi::IBuffer* frame, nvrhi::ITexture* density,
    nvrhi::ITexture* view, nvrhi::ITexture* clouds) const {
    auto bindings = nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(Constants)))
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, frame));
    if (density) {
        bindings.addItem(nvrhi::BindingSetItem::Texture_SRV(0, density))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(1, view))
            .addItem(nvrhi::BindingSetItem::Sampler(0, atmosphere_sampler_));
    }
    if (clouds) bindings.addItem(nvrhi::BindingSetItem::Texture_SRV(2, clouds));
    return bindings;
}

void Pass::prepare_raster(nvrhi::IDevice& device, nvrhi::IBuffer* frame,
    const nvrhi::FramebufferInfo& framebuffer, Raster& raster) {
    if (!vertex_shader_) {
        vertex_shader_ = create_embedded_shader(&device,
            nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main"), poima_sky_vs);
        require(bool(vertex_shader_), "Procedural sky vertex shader creation failed.");
    }
    if (!raster.layout) {
        const auto pixel_desc = nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main");
        auto pixel_shader = clouds_enabled_ ? create_embedded_shader(&device, pixel_desc, poima_sky_clouds_ps) :
            atmosphere_enabled_ ? create_embedded_shader(&device, pixel_desc, poima_sky_atmosphere_ps) :
            create_embedded_shader(&device, pixel_desc, poima_sky_ps);
        auto layout = nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All)
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(Constants)))
            .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1));
        if (atmosphere_enabled_) {
            layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
        }
        if (clouds_enabled_) layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(2));
        auto binding_layout = device.createBindingLayout(layout);
        require(pixel_shader && binding_layout, "Procedural sky shader or layout creation failed.");
        raster.pixel_shader = std::move(pixel_shader);
        raster.layout = std::move(binding_layout);
    }
    if (!raster.bindings || raster.frame != frame) {
        auto binding_set = device.createBindingSet(sky_binding_desc(frame,
            atmosphere_enabled_ ? density_.Get() : nullptr, atmosphere_enabled_ ? sky_view_.Get() : nullptr,
            clouds_enabled_ ? cloud_view_.Get() : nullptr), raster.layout);
        require(bool(binding_set), "Procedural sky bindings creation failed.");
        raster.bindings = std::move(binding_set);
        raster.frame = frame;
    }
    if (!raster.pipeline) {
        nvrhi::GraphicsPipelineDesc description;
        description.VS = vertex_shader_;
        description.PS = raster.pixel_shader;
        description.bindingLayouts.push_back(raster.layout);
        description.renderState.depthStencilState.depthTestEnable = false;
        description.renderState.depthStencilState.depthWriteEnable = false;
        description.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        description.renderState.rasterState.scissorEnable = true;
        raster.pipeline = device.createGraphicsPipeline(description, framebuffer);
        require(bool(raster.pipeline), "Procedural sky pipeline creation failed.");
    }
}

void Pass::prepare(nvrhi::IDevice& device, nvrhi::IBuffer* frame,
    std::span<const nvrhi::FramebufferHandle> framebuffers, const SceneSnapshot& scene,
    const SceneLighting& lighting, double aspect, float srgb) {
    const auto& sky = lighting.environment.sky;
    enabled_ = sky.enabled;
    constants_ = {};
    atmosphere_enabled_ = false;
    clouds_enabled_ = false;
    cloud_shadows_enabled_ = false;
    cloud_shadow_position_ = {};
    observer_height_m_ = 0;
    if (!enabled_) return;

    require(rigid_transform(scene.camera_world), "Procedural sky requires a rigid camera transform.");
    require(frame && !framebuffers.empty() && framebuffers.front(), "Procedural sky needs a frame and framebuffer.");
    const float sky_gain = lighting.solar_lighting ? lighting.solar_lighting->sky_gain : 1.0f;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        constants_.right_tan[axis] = gpu_float(scene.camera_world[axis]);
        constants_.up_tan[axis] = gpu_float(scene.camera_world[4 + axis]);
        constants_.forward_srgb[axis] = gpu_float(-scene.camera_world[8 + axis]);
        constants_.zenith_exposure[axis] = sky.zenith[axis] * sky_gain;
        constants_.horizon_falloff[axis] = sky.horizon[axis] * sky_gain;
        constants_.ground_radius[axis] = sky.ground[axis] * sky_gain;
    }
    const double tan_y = std::tan(scene.vertical_fov * 0.0087266462599716478846);
    constants_.right_tan[3] = gpu_float(tan_y * aspect);
    constants_.up_tan[3] = gpu_float(tan_y);
    constants_.forward_srgb[3] = srgb;
    constants_.zenith_exposure[3] = lighting.environment.exposure;
    constants_.horizon_falloff[3] = sky.horizon_falloff;
    constants_.ground_radius[3] = static_cast<float>(2 * std::sin(sky.sun_size_degrees * 0.0043633231299858239423));

    // The authored sun reference selects the direction, even as it moves at runtime.
    const auto& profile = lighting.environment.atmosphere;
    bool sun_lit = false;
    for (const auto& sun : lighting.lights) {
        if (sky.sun.empty() || sun.entity_id != sky.sun || !sun.light.enabled || sun.light.kind != LightKind::directional) continue;
        const double length = std::hypot(sun.direction[0], sun.direction[1], sun.direction[2]);
        require(std::isfinite(length) && length > 0, "Sky sun direction is degenerate.");
        validate_light(sun.light);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            constants_.sun_intensity[axis] = gpu_float(-sun.direction[axis] / length);
            constants_.sun_color[axis] = sun.light.color[axis];
        }
        constants_.sun_intensity[3] = sky.sun_intensity;
        sun_lit = sun.light.intensity > 0;
        if (profile && profile->radiance_scale > 0) {
            const auto height = std::clamp(scene.camera_world[13] - profile->sea_level_m,
                0.0, static_cast<double>(atmosphere::max_observer_height_m));
            observer_height_m_ = gpu_float(height);
            sky_view_parameters_ = {gpu_float(height / 1000.0),
                std::clamp(constants_.sun_intensity[1], -1.0f, 1.0f), profile->aerosol_strength, 0};
            constants_.forward_srgb[3] = profile->radiance_scale * sky_gain;
            constants_.zenith_exposure[3] = sky_view_parameters_[0];
            constants_.sun_color[3] = sky_view_parameters_[2];
            atmosphere_enabled_ = true;
        }
        break;
    }
    if (atmosphere_enabled_) {
        prepare_atmosphere(device);
        const auto& clouds = profile->clouds;
        clouds_enabled_ = clouds && clouds->coverage > 0 && clouds->extinction_per_km > 0;
        if (clouds_enabled_) {
            const auto time = lighting.solar_lighting ? lighting.solar_lighting->world_milliseconds : 0;
            prepare_clouds(device, frame, scene, *profile, time);
            cloud_shadows_enabled_ = clouds->shadow_strength.value_or(0.0f) > 0 && sun_lit &&
                constants_.sun_intensity[1] > 0.08f;
            if (cloud_shadows_enabled_) prepare_cloud_shadow(device, *profile);
        }
    }
    prepare_raster(device, frame, framebuffers.front()->getFramebufferInfo(),
        clouds_enabled_ ? cloud_raster_ : atmosphere_enabled_ ? atmosphere_raster_ : gradient_);
}

bool Pass::atmosphere_density_dirty() const noexcept {
    return atmosphere_enabled_ && density_generation_ == 0;
}

bool Pass::atmosphere_view_dirty() const noexcept {
    return atmosphere_enabled_ &&
        (sky_view_generation_ == 0 || accepted_sky_view_parameters_ != sky_view_parameters_);
}

void Pass::compute_atmosphere_density(nvrhi::ICommandList& commands) {
    require(atmosphere_density_dirty() && !density_recorded_, "Unexpected atmosphere density dispatch.");
    nvrhi::ComputeState state;
    state.pipeline = density_compute_.pipeline;
    state.bindings.push_back(density_compute_.bindings);
    commands.setComputeState(state);
    commands.dispatch(atmosphere::density_width / atmosphere::thread_group_size,
        atmosphere::density_height / atmosphere::thread_group_size, 1);
    density_recorded_ = true;
}

void Pass::compute_atmosphere_view(nvrhi::ICommandList& commands) {
    require(atmosphere_view_dirty() && !sky_view_recorded_ && (density_generation_ || density_recorded_),
        "Atmosphere sky-view dispatch needs recorded optical density and a changed sample.");
    require(sky_view_generation_ < std::numeric_limits<std::uint64_t>::max(), "Atmosphere view generation exhausted.");
    nvrhi::ComputeState state;
    state.pipeline = sky_view_compute_.pipeline;
    state.bindings.push_back(sky_view_compute_.bindings);
    commands.setComputeState(state);
    commands.setPushConstants(sky_view_parameters_.data(), sizeof(sky_view_parameters_));
    commands.dispatch(atmosphere::sky_view_width / atmosphere::thread_group_size,
        atmosphere::sky_view_height / atmosphere::thread_group_size, 1);
    sky_view_recorded_ = true;
}

void Pass::commit_atmosphere() noexcept {
    if (density_recorded_) {
        ++density_generation_;
        density_recorded_ = false;
    }
    if (sky_view_recorded_) {
        accepted_sky_view_parameters_ = sky_view_parameters_;
        ++sky_view_generation_;
        sky_view_recorded_ = false;
    }
    if (cloud_noise_recorded_) cloud_noise_initialized_ = true;
    cloud_noise_recorded_ = false;
    cloud_view_recorded_ = false;
    if (cloud_shadow_recorded_) {
        accepted_cloud_shadow_parameters_ = cloud_shadow_parameters_;
        ++cloud_shadow_generation_;
        cloud_shadow_recorded_ = false;
    }
}

void Pass::draw(nvrhi::ICommandList& commands, nvrhi::IFramebuffer* framebuffer,
    const nvrhi::Viewport& viewport) const {
    const auto& raster = clouds_enabled_ ? cloud_raster_ : atmosphere_enabled_ ? atmosphere_raster_ : gradient_;
    require(!atmosphere_view_dirty() || sky_view_recorded_, "Atmospheric sky draw needs a current sky-view lookup.");
    require(!clouds_enabled_ || cloud_view_recorded_, "Atmospheric sky draw needs a current cloud view.");
    nvrhi::GraphicsState state;
    state.pipeline = raster.pipeline;
    state.framebuffer = framebuffer;
    state.bindings.push_back(raster.bindings);
    state.viewport.addViewportAndScissorRect(viewport);
    commands.setGraphicsState(state);
    commands.setPushConstants(&constants_, sizeof(constants_));
    commands.draw(nvrhi::DrawArguments().setVertexCount(3));
}

void Pass::detach_framebuffer() noexcept {
    gradient_.pipeline = nullptr;
    atmosphere_raster_.pipeline = nullptr;
    cloud_raster_.pipeline = nullptr;
}

void Pass::reset() noexcept { *this = {}; }

}
