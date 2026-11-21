// SPDX-License-Identifier: Apache-2.0
#include "render_viewmodel.hpp"

#include "poima/assets.hpp"
#include "poima/scene_ps.hpp"
#include "poima/scene_vs.hpp"
#include "poima/viewmodel_composite_ps.hpp"
#include "poima/viewmodel_composite_vs.hpp"
#include "poima/viewmodel_resolve_ps.hpp"
#include "poima/viewmodel_resolve_vs.hpp"
#include "render_embedded_shader.hpp"
#include "render_vulkan_instance.hpp"

#include <stdexcept>
#include <string>

namespace poima::render_viewmodel {
namespace {
using CompositeConstants = std::array<float, 8>;
static_assert(sizeof(CompositeConstants) == 32);

void require(bool condition, const std::string& detail) {
    if (!condition) throw std::runtime_error(detail);
}

nvrhi::BindingSetHandle composition_bindings(nvrhi::IDevice& device, const Resources& bundle, nvrhi::ITexture* source) {
    const auto desc = nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(CompositeConstants)))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, source))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(1, bundle.resolved));
    return device.createBindingSet(desc, bundle.compose_layout);
}
}

std::uint64_t logical_bytes(const Configuration& config) {
    // One-sample resolved/color aliases count once. Multisample bundles
    // own color(8*N), depth(4*N), resolved(8) and composite(8) per pixel.
    const std::uint64_t bytes_per_pixel = config.samples == 1 ? 20u : 12u * config.samples + 16u;
    const auto object_bytes = std::uint64_t(config.object_capacity) * config.object_stride;
    const auto cluster_bytes = std::uint64_t(config.cluster_cells) * (config.cluster_capacity + 1 + config.frame_slots) * sizeof(std::uint32_t);
    return std::uint64_t(config.width) * config.height * bytes_per_pixel + config.frame_bytes + object_bytes + cluster_bytes;
}

std::shared_ptr<Resources> stage(nvrhi::IDevice& device, const vk::PhysicalDevice& physical,
                                 const vk::detail::DispatchLoaderDynamic& dispatch,
                                 const Configuration& config, const Inputs& inputs) {
    auto bundle = std::make_shared<Resources>();
    bundle->width = config.width;
    bundle->height = config.height;
    bundle->samples = config.samples;
    bundle->bytes = logical_bytes(config);

    const auto check_support = [&](nvrhi::Format format, vk::Format native_format, std::uint32_t count) {
        const auto supported = physical.getFormatProperties(native_format, dispatch).optimalTilingFeatures;
        const bool depth = format == nvrhi::Format::D32;
        const auto attachment = depth ? vk::FormatFeatureFlagBits::eDepthStencilAttachment : vk::FormatFeatureFlagBits::eColorAttachment;
        const auto needed = vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferSrc | vk::FormatFeatureFlagBits::eTransferDst | attachment;
        require((supported & needed) == needed, "Foreground image format is unsupported.");
        const auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
            (depth ? vk::ImageUsageFlagBits::eDepthStencilAttachment : vk::ImageUsageFlagBits::eColorAttachment);
        const auto properties = physical.getImageFormatProperties(native_format, vk::ImageType::e2D, vk::ImageTiling::eOptimal, usage, {}, dispatch);
        require(bool(properties.sampleCounts & static_cast<vk::SampleCountFlagBits>(count)) && config.width <= properties.maxExtent.width &&
                config.height <= properties.maxExtent.height && properties.maxExtent.depth >= 1 && properties.maxMipLevels >= 1 && properties.maxArrayLayers >= 1,
                "Selected GPU lacks requested foreground format/extent/sample support.");
    };
    // Validate exact format/sample usages before allocating any attachments.
    check_support(nvrhi::Format::RGBA16_FLOAT, vk::Format::eR16G16B16A16Sfloat, config.samples);
    check_support(nvrhi::Format::D32, vk::Format::eD32Sfloat, config.samples);
    if (config.samples > 1) check_support(nvrhi::Format::RGBA16_FLOAT, vk::Format::eR16G16B16A16Sfloat, 1);

    const auto image = [&](nvrhi::Format format, std::uint32_t count, const char* name) {
        nvrhi::TextureDesc desc;
        desc.width = config.width;
        desc.height = config.height;
        desc.format = format;
        desc.sampleCount = count;
        desc.isRenderTarget = true;
        desc.isShaderResource = true;
        desc.dimension = count > 1 ? nvrhi::TextureDimension::Texture2DMS : nvrhi::TextureDimension::Texture2D;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = name;
        auto result = device.createTexture(desc);
        require(bool(result), std::string("Foreground allocation failed: ") + name);
        return result;
    };
    bundle->color = image(nvrhi::Format::RGBA16_FLOAT, config.samples, "Foreground output-local HDR coverage");
    bundle->depth = image(nvrhi::Format::D32, config.samples, "Foreground independent self depth");
    bundle->resolved = config.samples == 1 ? bundle->color : image(nvrhi::Format::RGBA16_FLOAT, 1, "Foreground arithmetic-average HDR coverage");
    bundle->composite = image(nvrhi::Format::RGBA16_FLOAT, 1, "World plus foreground linear HDR");

    nvrhi::BufferDesc frame;
    frame.byteSize = config.frame_bytes;
    frame.isConstantBuffer = true;
    frame.initialState = nvrhi::ResourceStates::ConstantBuffer;
    frame.keepInitialState = true;
    frame.debugName = "Foreground frame constants";
    bundle->frame = device.createBuffer(frame);

    nvrhi::BufferDesc objects;
    objects.byteSize = std::uint64_t(config.object_capacity) * config.object_stride;
    objects.structStride = config.object_stride;
    objects.initialState = nvrhi::ResourceStates::ShaderResource;
    objects.keepInitialState = true;
    objects.debugName = "Foreground immutable submission objects";
    bundle->objects = device.createBuffer(objects);

    nvrhi::BufferDesc cells;
    cells.byteSize = std::uint64_t(config.cluster_cells) * sizeof(std::uint32_t);
    cells.structStride = sizeof(std::uint32_t);
    cells.canHaveUAVs = true;
    cells.initialState = nvrhi::ResourceStates::ShaderResource;
    cells.keepInitialState = true;
    cells.debugName = "Foreground cluster counts";
    bundle->counts = device.createBuffer(cells);
    cells.byteSize *= config.cluster_capacity;
    cells.debugName = "Foreground sorted cluster indices";
    bundle->indices = device.createBuffer(cells);

    nvrhi::BufferDesc readback;
    readback.byteSize = std::uint64_t(config.cluster_cells) * sizeof(std::uint32_t);
    readback.cpuAccess = nvrhi::CpuAccessMode::Read;
    readback.initialState = nvrhi::ResourceStates::CopyDest;
    readback.keepInitialState = true;
    readback.debugName = "Foreground slot cluster count readback";
    for (std::size_t slot = 0; slot < config.frame_slots; ++slot) {
        auto& handle = bundle->readbacks.at(slot);
        handle = device.createBuffer(readback);
        require(bool(handle), "Foreground readback allocation failed.");
    }
    require(bundle->frame && bundle->objects && bundle->counts && bundle->indices, "Foreground buffer allocation failed.");
    const auto cluster = nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, bundle->frame))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, inputs.lights))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, bundle->counts))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, bundle->indices));
    bundle->cluster_bindings = device.createBindingSet(cluster, inputs.cluster_layout);

    const auto vertex = nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main");
    const auto pixel = nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main");
    bundle->vertex = create_embedded_shader(&device, vertex, poima_scene_vs);
    bundle->pixel = create_embedded_shader(&device, pixel, poima_scene_ps);
    const nvrhi::VertexAttributeDesc attributes[] = {
        nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(0).setElementStride(sizeof(MeshVertex)),
        nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(12).setElementStride(sizeof(MeshVertex)),
        nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(24).setElementStride(sizeof(MeshVertex)),
        nvrhi::VertexAttributeDesc().setName("TANGENT").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(32).setElementStride(sizeof(MeshVertex))
    };
    bundle->input = device.createInputLayout(attributes, 4, bundle->vertex);
    bundle->framebuffer = device.createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(bundle->color).setDepthAttachment(bundle->depth));
    bundle->compose_framebuffer = device.createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(bundle->composite));
    require(bundle->vertex && bundle->pixel && bundle->input && bundle->framebuffer && bundle->compose_framebuffer && bundle->cluster_bindings,
            "Foreground immutable bindings are unavailable.");

    nvrhi::GraphicsPipelineDesc pipeline;
    pipeline.VS = bundle->vertex;
    pipeline.PS = bundle->pixel;
    pipeline.inputLayout = bundle->input;
    pipeline.bindingLayouts.push_back(inputs.scene_layout);
    pipeline.bindingLayouts.push_back(inputs.surface_mark_layout);
    pipeline.renderState.depthStencilState.depthTestEnable = true;
    pipeline.renderState.depthStencilState.depthWriteEnable = true;
    pipeline.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
    pipeline.renderState.rasterState.frontCounterClockwise = true;
    pipeline.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    bundle->pipeline = device.createGraphicsPipeline(pipeline, bundle->framebuffer->getFramebufferInfo());
    pipeline.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;
    bundle->culled_pipeline = device.createGraphicsPipeline(pipeline, bundle->framebuffer->getFramebufferInfo());

    bundle->compose_vertex = create_embedded_shader(&device, vertex, poima_viewmodel_composite_vs);
    bundle->compose_pixel = create_embedded_shader(&device, pixel, poima_viewmodel_composite_ps);
    const auto compose_layout = nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(CompositeConstants)))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1));
    bundle->compose_layout = device.createBindingLayout(compose_layout);
    nvrhi::GraphicsPipelineDesc composite;
    composite.VS = bundle->compose_vertex;
    composite.PS = bundle->compose_pixel;
    composite.bindingLayouts.push_back(bundle->compose_layout);
    composite.renderState.depthStencilState.depthTestEnable = false;
    composite.renderState.depthStencilState.depthWriteEnable = false;
    composite.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    bundle->compose_pipeline = device.createGraphicsPipeline(composite, bundle->compose_framebuffer->getFramebufferInfo());
    require(bundle->pipeline && bundle->culled_pipeline && bundle->compose_vertex && bundle->compose_pixel && bundle->compose_layout && bundle->compose_pipeline,
            "Foreground pipeline allocation failed.");

    if (config.samples > 1) {
        bundle->resolve_vertex = create_embedded_shader(&device, vertex, poima_viewmodel_resolve_vs);
        bundle->resolve_pixel = create_embedded_shader(&device, pixel, poima_viewmodel_resolve_ps);
        bundle->resolve_layout = device.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Pixel).addItem(nvrhi::BindingLayoutItem::Texture_SRV(0)));
        bundle->resolve_bindings = device.createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::Texture_SRV(0, bundle->color)), bundle->resolve_layout);
        bundle->resolve_framebuffer = device.createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(bundle->resolved));
        require(bundle->resolve_vertex && bundle->resolve_pixel && bundle->resolve_layout && bundle->resolve_bindings && bundle->resolve_framebuffer,
                "Foreground resolve resources unavailable.");
        nvrhi::GraphicsPipelineDesc resolve;
        resolve.VS = bundle->resolve_vertex;
        resolve.PS = bundle->resolve_pixel;
        resolve.bindingLayouts.push_back(bundle->resolve_layout);
        resolve.renderState.depthStencilState.depthTestEnable = false;
        resolve.renderState.depthStencilState.depthWriteEnable = false;
        resolve.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        bundle->resolve_pipeline = device.createGraphicsPipeline(resolve, bundle->resolve_framebuffer->getFramebufferInfo());
        require(bool(bundle->resolve_pipeline), "Foreground resolve pipeline allocation failed.");
    }
    bundle->compose_bindings = composition_bindings(device, *bundle, inputs.world_radiance);
    require(bool(bundle->compose_bindings), "Foreground staged composition bindings unavailable.");
    return bundle;
}

void bind_composition(nvrhi::IDevice& device, Resources& bundle, nvrhi::ITexture* source) {
    auto bindings = composition_bindings(device, bundle, source);
    require(bool(bindings), "Foreground composition bindings unavailable.");
    bundle.compose_bindings = std::move(bindings);
}

void record_composition(nvrhi::ICommandList& commands, const Resources& bundle,
                        const std::array<float, 4>& world_viewport) {
    const CompositeConstants parameters{world_viewport[0], world_viewport[1], world_viewport[2], world_viewport[3],
                                        float(bundle.width), float(bundle.height), 0, 0};
    nvrhi::GraphicsState state;
    state.pipeline = bundle.compose_pipeline;
    state.framebuffer = bundle.compose_framebuffer;
    state.bindings.push_back(bundle.compose_bindings);
    state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(bundle.width), float(bundle.height)));
    commands.setGraphicsState(state);
    commands.setPushConstants(parameters.data(), sizeof(parameters));
    commands.draw(nvrhi::DrawArguments().setVertexCount(3));
}

void record_resolve(nvrhi::ICommandList& commands, const Resources& bundle) {
    require(bundle.samples > 1 && bundle.resolved.Get() != bundle.color.Get(), "Foreground resolve requires distinct multisample and single-sample images.");
    nvrhi::GraphicsState state;
    state.pipeline = bundle.resolve_pipeline;
    state.framebuffer = bundle.resolve_framebuffer;
    state.bindings.push_back(bundle.resolve_bindings);
    state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(bundle.width), float(bundle.height)));
    commands.setGraphicsState(state);
    commands.draw(nvrhi::DrawArguments().setVertexCount(3));
}
}
