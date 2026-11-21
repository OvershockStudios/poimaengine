// SPDX-License-Identifier: Apache-2.0
#include "render_ui.hpp"

#include "poima/editor_overlay_ps.hpp"
#include "poima/editor_overlay_vs.hpp"
#include "poima/game_ui_ps.hpp"
#include "poima/game_ui_vs.hpp"
#include "poima/scene.hpp"
#include "poima/ui_input.hpp"
#include "render_embedded_shader.hpp"
#if POIMA_EDITOR
#include "poima/editor_ui_ps.hpp"
#include "poima/editor_ui_vs.hpp"
#endif
#if POIMA_GAME_UI
#include "poima/ui_presenter.hpp"
#endif

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace poima::render_ui {
namespace {
void require(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}
}

Game::Game() = default;
Game::~Game() = default;

float effective_scale(SDL_Window* window, std::optional<float> override_scale) {
    if(override_scale)return *override_scale;
    return window ? std::clamp(SDL_GetWindowDisplayScale(window),.25f,8.f) : 1.f;
}

void Game::validate(const SceneSnapshot* scene, Target target) const {
    if(!scene)return;
    require(!scene->ui || !scene->logical_ui,"A scene cannot supply both a UI packet and a logical UI presentation.");
#if !POIMA_GAME_UI
    require(!scene->logical_ui || scene->logical_ui->elements.empty(),"This renderer was built without native game UI presentation support.");
#endif
    if(!scene || !scene->ui)return;
    validate_ui_frame(*scene->ui);
    require(scene->ui->width==target.width && scene->ui->height==target.height,"Game UI packet extent differs from the render target; relayout before drawing.");
    if(!scene->ui->draws.empty())require(target.srgb,
        "Game UI requires an sRGB composition attachment; UNORM composition is not supported.");
}

void Game::resolve(const SceneSnapshot* scene, bool visible, std::optional<nvrhi::Viewport> viewport,
                   Target target, SDL_Window* window, std::optional<float> override_scale) {
    resolve_packet(scene, visible, viewport, target, window, override_scale);
    const auto& packet=game_ui_source;
    if(packet) {
        validate_ui_frame(*packet);
        require(packet->width==target.width && packet->height==target.height,"Resolved UI packet extent differs from target.");
        if(!packet->draws.empty())require(target.srgb,"Game UI requires an sRGB composition attachment; UNORM composition is not supported.");
    }
}

void Game::resolve_packet(const SceneSnapshot* scene, bool visible, std::optional<nvrhi::Viewport> viewport,
                      Target target, SDL_Window* window, std::optional<float> override_scale) {
    if(startup_overlay) {game_ui_source=startup_overlay;return;}
    game_ui_source=scene ? scene->ui : nullptr;
#if POIMA_GAME_UI
    if(!scene || !scene->logical_ui || scene->logical_ui->elements.empty()) {
        game_ui_presenter.reset();game_ui_layout_frame.reset();game_ui_composed_frame.reset();game_ui_observation.reset();return;
    }
    if(!visible) {game_ui_source.reset();game_ui_observation.reset();return;}
    const auto view=viewport.value_or(nvrhi::Viewport(static_cast<float>(target.width),static_cast<float>(target.height)));
    const std::array<std::int32_t,4> rect{static_cast<std::int32_t>(std::floor(view.minX)),static_cast<std::int32_t>(std::floor(view.minY)),
        static_cast<std::int32_t>(std::ceil(view.maxX)),static_cast<std::int32_t>(std::ceil(view.maxY))};
    if(rect[0]>=rect[2] || rect[1]>=rect[3]) {game_ui_source.reset();game_ui_observation.reset();return;}
    if(!game_ui_presenter)game_ui_presenter=std::make_unique<UiPresenter>();
    const float scale=effective_scale(window, override_scale);
    const auto layout=game_ui_presenter->frame(scene->logical_ui,static_cast<std::uint32_t>(rect[2]-rect[0]),static_cast<std::uint32_t>(rect[3]-rect[1]),scale);
    game_ui_observation=game_ui_presenter->layout_observation();
    const std::array<std::uint32_t,2> target_extent{target.width,target.height};
    if(game_ui_layout_frame!=layout || game_ui_layout_rect!=rect || game_ui_layout_extent!=target_extent) {
        auto packet=*layout;packet.width=target.width;packet.height=target.height;
        for(auto& draw:packet.draws) {
            // Translate homogeneous coordinates after the layout transform.
            for(std::size_t col=0;col<4;++col) {
                draw.transform[col*4]+=static_cast<float>(rect[0])*draw.transform[col*4+3];
                draw.transform[col*4+1]+=static_cast<float>(rect[1])*draw.transform[col*4+3];
            }
            draw.scissor[0]+=rect[0];draw.scissor[2]+=rect[0];draw.scissor[1]+=rect[1];draw.scissor[3]+=rect[1];
        }
        game_ui_source=freeze_ui_frame(std::move(packet));game_ui_composed_frame=game_ui_source;game_ui_layout_frame=layout;game_ui_layout_rect=rect;game_ui_layout_extent=target_extent;
    }else game_ui_source=game_ui_composed_frame;
#else
    (void)visible;
    (void)viewport;
    (void)target;
    (void)window;
    (void)override_scale;
#endif
}

void Game::reset_input() {
#if POIMA_GAME_UI
    if(game_ui_presenter)game_ui_presenter->reset_input();
#endif
}
UiInputResult Game::input(const UiInput& event, bool visible, bool activate) {
#if POIMA_GAME_UI
    if(game_ui_presenter && game_ui_source && visible) {
        auto local=event;
        local.x-=static_cast<float>(game_ui_layout_rect[0]);
        local.y-=static_cast<float>(game_ui_layout_rect[1]);
        return game_ui_presenter->input(local,activate);
    }
#else
    (void)event;(void)visible;(void)activate;
#endif
    return {};
}

void Game::prepare(nvrhi::IDevice& checked, nvrhi::ICommandList& commands, std::uint64_t& idle_waits) {
    const auto& packet = game_ui_source;
    if (!packet || packet->draws.empty()) {
        clear_packet_gpu();
        return;
    }
    if(!game_ui_layout) {
        game_ui_vs=create_embedded_shader(&checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main"),poima_game_ui_vs);
        game_ui_ps=create_embedded_shader(&checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main"),poima_game_ui_ps);
        const nvrhi::VertexAttributeDesc attributes[]={
            nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(UiVertex,x)).setElementStride(sizeof(UiVertex)),
            nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(UiVertex,u)).setElementStride(sizeof(UiVertex)),
            nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGBA8_UNORM).setOffset(offsetof(UiVertex,color)).setElementStride(sizeof(UiVertex))};
        game_ui_input=checked.createInputLayout(attributes,3,game_ui_vs);
        game_ui_layout=checked.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All)
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0,80)).addItem(nvrhi::BindingLayoutItem::Texture_SRV(0)).addItem(nvrhi::BindingLayoutItem::Sampler(0)));
        nvrhi::SamplerDesc sampler;sampler.addressU=sampler.addressV=nvrhi::SamplerAddressMode::Clamp;sampler.minFilter=sampler.magFilter=true;
        game_ui_sampler=checked.createSampler(sampler);
        require(game_ui_vs && game_ui_ps && game_ui_input && game_ui_layout && game_ui_sampler,"Game UI shader/layout creation failed.");
    }
    if(!game_ui_pipeline) {
        nvrhi::GraphicsPipelineDesc description;description.VS=game_ui_vs;description.PS=game_ui_ps;
        description.inputLayout=game_ui_input;description.bindingLayouts.push_back(game_ui_layout);
        description.renderState.depthStencilState.depthTestEnable=false;description.renderState.depthStencilState.depthWriteEnable=false;
        description.renderState.rasterState.cullMode=nvrhi::RasterCullMode::None;description.renderState.rasterState.scissorEnable=true;
        auto& blend=description.renderState.blendState.targets[0];blend.blendEnable=true;
        blend.srcBlend=blend.srcBlendAlpha=nvrhi::BlendFactor::One;
        blend.destBlend=blend.destBlendAlpha=nvrhi::BlendFactor::InvSrcAlpha;
        game_ui_pipeline=checked.createGraphicsPipeline(description,game_ui_framebuffers.front()->getFramebufferInfo());
        require(bool(game_ui_pipeline),"Game UI composition pipeline creation failed.");
    }
    if(game_ui_frame==packet)return;
    // Reuse capacity and unchanged atlas slots across layout packets. Retain
    // only the current packet/resources; replacement is bounded by two packets.
    auto buffer=[&](nvrhi::BufferHandle& target,std::size_t& capacity,std::size_t bytes,bool vertex) {
        if(target && capacity>=bytes)return;
        nvrhi::BufferDesc desc;desc.byteSize=std::max<std::size_t>(bytes,4);desc.isVertexBuffer=vertex;desc.isIndexBuffer=!vertex;
        desc.initialState=vertex ? nvrhi::ResourceStates::VertexBuffer : nvrhi::ResourceStates::IndexBuffer;desc.keepInitialState=true;
        auto result=checked.createBuffer(desc);require(bool(result),"Game UI buffer creation failed.");target=std::move(result);capacity=desc.byteSize;
    };
    buffer(game_ui_vertices,game_ui_vertex_capacity,packet->vertices.size()*sizeof(UiVertex),true);
    buffer(game_ui_indices,game_ui_index_capacity,packet->indices.size()*sizeof(std::uint32_t),false);
    std::vector<nvrhi::TextureHandle> textures;std::vector<nvrhi::BindingSetHandle> texture_bindings;
    textures.reserve(packet->textures.size()+1);texture_bindings.reserve(packet->textures.size()+1);
    bool uploads=false;
    auto upload_texture=[&](const UiTexture& source) {
        // Linear premultiplied half floats ensure filtering occurs in linear light.
        std::vector<std::uint16_t> pixels;pixels.reserve(source.rgba.size());
        auto half=[](float value)->std::uint16_t {
            if(value==0)return 0;
            if(value<0x1p-14f)return static_cast<std::uint16_t>(std::lround(std::ldexp(value,24)));
            int exponent=0;const float mantissa=std::frexp(value,&exponent);
            return static_cast<std::uint16_t>((exponent+14)*1024+std::lround((mantissa*2-1)*1024));
        };
        for(std::size_t i=0;i<source.rgba.size();i+=4) {
            const auto c=ui_linear_color({source.rgba[i],source.rgba[i+1],source.rgba[i+2],source.rgba[i+3]});
            for(auto value:c)pixels.push_back(half(value));
        }
        nvrhi::TextureDesc desc;desc.width=source.width;desc.height=source.height;desc.format=nvrhi::Format::RGBA16_FLOAT;
        desc.initialState=nvrhi::ResourceStates::ShaderResource;desc.keepInitialState=true;
        auto texture=checked.createTexture(desc);require(bool(texture),"Game UI texture creation failed.");
        auto binding=checked.createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0,80))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(0,texture)).addItem(nvrhi::BindingSetItem::Sampler(0,game_ui_sampler)),game_ui_layout);
        require(bool(binding),"Game UI texture binding failed.");
        if(!uploads) {commands.open();uploads=true;}
        commands.writeTexture(texture,0,0,pixels.data(),std::size_t(source.width)*8);
        textures.push_back(std::move(texture));texture_bindings.push_back(std::move(binding));
    };
    for(std::size_t i=0;i<packet->textures.size();++i) {
        const auto& texture=packet->textures[i];
        if(game_ui_frame && i<game_ui_frame->textures.size() && texture.width==game_ui_frame->textures[i].width &&
            texture.height==game_ui_frame->textures[i].height && texture.rgba==game_ui_frame->textures[i].rgba) {
            textures.push_back(game_ui_textures[i]);texture_bindings.push_back(game_ui_bindings[i]);
        }else upload_texture(texture);
    }
    if(game_ui_frame) {textures.push_back(game_ui_textures.back());texture_bindings.push_back(game_ui_bindings.back());}
    else upload_texture(UiTexture{1,1,{255,255,255,255}});
    if(uploads) {
        commands.close();checked.executeCommandList(&commands);++idle_waits;require(checked.waitForIdle(),"Game UI texture upload failed.");
    }
    game_ui_textures.swap(textures);game_ui_bindings.swap(texture_bindings);
    game_ui_frame=packet;
}

void Game::record(nvrhi::ICommandList& commands, Target target, std::uint32_t image_index) const {
    if(!game_ui_frame)return;
    const auto& frame=*game_ui_frame;
    if(!frame.vertices.empty())commands.writeBuffer(game_ui_vertices,frame.vertices.data(),frame.vertices.size()*sizeof(UiVertex));
    if(!frame.indices.empty())commands.writeBuffer(game_ui_indices,frame.indices.data(),frame.indices.size()*sizeof(std::uint32_t));
    for(const auto& draw:frame.draws) {
        const auto& clip=draw.scissor;if(!draw.index_count || clip[0]==clip[2] || clip[1]==clip[3])continue;
        nvrhi::GraphicsState state;state.pipeline=game_ui_pipeline;state.framebuffer=game_ui_framebuffers.at(image_index);
        state.bindings.push_back(draw_binding(draw));
        state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(game_ui_vertices));
        state.indexBuffer=nvrhi::IndexBufferBinding(game_ui_indices,nvrhi::Format::R32_UINT,0);
        state.viewport.addViewport(nvrhi::Viewport(static_cast<float>(target.width),static_cast<float>(target.height)));
        state.viewport.addScissorRect(nvrhi::Rect(clip[0],clip[2],clip[1],clip[3]));
        std::array<float,20> constants{};std::copy(draw.transform.begin(),draw.transform.end(),constants.begin());
        constants[16]=draw.translation[0];constants[17]=draw.translation[1];constants[18]=static_cast<float>(frame.width);constants[19]=static_cast<float>(frame.height);
        commands.setGraphicsState(state);commands.setPushConstants(constants.data(),sizeof(constants));
        commands.drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.index_count).setStartIndexLocation(draw.first_index));
    }
}


std::shared_ptr<const UiLayoutObservation> Game::observation() const noexcept {
#if POIMA_GAME_UI
    return game_ui_observation;
#else
    return {};
#endif
}

nvrhi::IBindingSet* Game::draw_binding(const UiDraw& draw) const {
    return game_ui_bindings.at(draw.texture == ui_white_texture ? game_ui_frame->textures.size() : draw.texture).Get();
}

void Game::add_framebuffer(nvrhi::IDevice& checked, nvrhi::ITexture* texture) {
    auto framebuffer = checked.createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
    require(bool(framebuffer), "Game UI composition framebuffer creation failed.");
    game_ui_framebuffers.push_back(framebuffer);
}

void Game::detach_targets() noexcept {
    game_ui_framebuffers.clear();
    game_ui_pipeline = nullptr;
}

void Game::clear_packet_gpu() noexcept {
    game_ui_frame.reset();
    game_ui_bindings.clear();
    game_ui_textures.clear();
    game_ui_vertices = nullptr;
    game_ui_indices = nullptr;
    game_ui_vertex_capacity = game_ui_index_capacity = 0;
}

void Game::release_gpu() noexcept {
    detach_targets();
    game_ui_bindings.clear();
    game_ui_textures.clear();
    game_ui_vertices = nullptr;
    game_ui_indices = nullptr;
    game_ui_sampler = nullptr;
    game_ui_input = nullptr;
    game_ui_layout = nullptr;
    game_ui_vs = nullptr;
    game_ui_ps = nullptr;
    game_ui_frame.reset();
}

void Overlay::set_vertices(const std::vector<EditorOverlayVertex>& triangles) {
    auto copy = triangles;
    overlay_data.swap(copy);
}

void Overlay::add_framebuffer(nvrhi::IDevice& checked, nvrhi::ITexture* texture) {
    auto framebuffer = checked.createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
    require(bool(framebuffer), "Hosted overlay framebuffer creation failed.");
    overlay_framebuffers.push_back(framebuffer);
}

void Overlay::detach_targets() noexcept {
    overlay_framebuffers.clear();
    overlay_pipeline = nullptr;
}

void Overlay::release_gpu() noexcept {
    detach_targets();
    overlay_bindings = nullptr;
    overlay_layout = nullptr;
    overlay_vertices = nullptr;
    overlay_input = nullptr;
    overlay_vs = nullptr;
    overlay_ps = nullptr;
}

void Overlay::prepare(nvrhi::IDevice& checked) {
    if(!active())return;
    if(!overlay_vertices) {
        overlay_vs=create_embedded_shader(&checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main"),poima_editor_overlay_vs);
        overlay_ps=create_embedded_shader(&checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main"),poima_editor_overlay_ps);
        const nvrhi::VertexAttributeDesc attributes[]={
            nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(EditorOverlayVertex,x)).setElementStride(sizeof(EditorOverlayVertex)),
            nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(offsetof(EditorOverlayVertex,color)).setElementStride(sizeof(EditorOverlayVertex))};
        overlay_input=checked.createInputLayout(attributes,2,overlay_vs);
        overlay_layout=checked.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All).addItem(nvrhi::BindingLayoutItem::PushConstants(0,16)));
        require(overlay_vs && overlay_ps && overlay_input && overlay_layout,"Hosted overlay shader/layout creation failed.");
        overlay_bindings=checked.createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0,16)),overlay_layout);
        nvrhi::BufferDesc buffer;buffer.byteSize=max_editor_overlay_vertices*sizeof(EditorOverlayVertex);buffer.isVertexBuffer=true;
        buffer.initialState=nvrhi::ResourceStates::VertexBuffer;buffer.keepInitialState=true;buffer.debugName="Hosted editor overlay vertices";
        overlay_vertices=checked.createBuffer(buffer);
        require(overlay_bindings && overlay_vertices,"Hosted overlay bindings/buffer creation failed.");
    }
    if(!overlay_pipeline) {
        nvrhi::GraphicsPipelineDesc description;description.VS=overlay_vs;description.PS=overlay_ps;description.inputLayout=overlay_input;description.bindingLayouts.push_back(overlay_layout);
        description.renderState.depthStencilState.depthTestEnable=false;description.renderState.depthStencilState.depthWriteEnable=false;
        description.renderState.rasterState.cullMode=nvrhi::RasterCullMode::None;description.renderState.rasterState.scissorEnable=true;
        auto& blend=description.renderState.blendState.targets[0];blend.blendEnable=true;
        blend.srcBlend=nvrhi::BlendFactor::SrcAlpha;blend.destBlend=nvrhi::BlendFactor::InvSrcAlpha;
        blend.srcBlendAlpha=nvrhi::BlendFactor::One;blend.destBlendAlpha=nvrhi::BlendFactor::InvSrcAlpha;
        overlay_pipeline=checked.createGraphicsPipeline(description,overlay_framebuffers.front()->getFramebufferInfo());
        require(bool(overlay_pipeline),"Hosted overlay pipeline creation failed.");
    }
}
void Overlay::record(nvrhi::ICommandList& commands, Target target, std::uint32_t image_index) const {
    if(!active())return;
    commands.writeBuffer(overlay_vertices,overlay_data.data(),overlay_data.size()*sizeof(EditorOverlayVertex));
    nvrhi::GraphicsState state;state.pipeline=overlay_pipeline;state.framebuffer=overlay_framebuffers.at(image_index);state.bindings.push_back(overlay_bindings);
    state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(overlay_vertices));
    state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(target.width),static_cast<float>(target.height)));
    const float constants[4]={target.srgb ? 1.0f : 0.0f,0,0,0};
    commands.setGraphicsState(state);commands.setPushConstants(constants,sizeof(constants));
    commands.draw(nvrhi::DrawArguments().setVertexCount(static_cast<std::uint32_t>(overlay_data.size())));
}


#if POIMA_EDITOR
namespace {
struct EditorPushConstants {
    float transform[4];
    float target[4];
};
static_assert(sizeof(EditorPushConstants) == 32);
}

void Editor::create_scene_target(nvrhi::IDevice& checked, const nvrhi::TextureDesc& swapchain_desc) {
    // One full-window image keeps Scene UVs stable within this frame,
    // including docking, floating panels and framebuffer DPI scaling.
    // The existing extent limit bounds this additional image to 128 MiB.
    auto scene_image_desc=swapchain_desc;
    scene_image_desc.isShaderResource=true;
    scene_image_desc.initialState=nvrhi::ResourceStates::ShaderResource;
    scene_image_desc.keepInitialState=true;
    scene_image_desc.debugName="Editor Scene compositing image";
    ui_scene=checked.createTexture(scene_image_desc);
    require(bool(ui_scene),"Editor Scene compositing image creation failed.");
    if(ui_layout)create_scene_bindings(checked);
}

void Editor::create_scene_bindings(nvrhi::IDevice& checked) {
    ui_scene_bindings=checked.createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0,sizeof(EditorPushConstants)))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0,ui_scene)).addItem(nvrhi::BindingSetItem::Sampler(0,ui_sampler)),ui_layout);
    require(bool(ui_scene_bindings),"Editor Scene compositing bindings failed.");
}
void Editor::prepare(nvrhi::IDevice& checked, nvrhi::ICommandList& commands, std::uint64_t& idle_waits) {
    require(ImGui::GetCurrentContext()!=nullptr,"Create an ImGui context before the editor viewport.");
    ui_context=ImGui::GetCurrentContext();
    unsigned char* pixels=nullptr;int width=0,height=0;
    auto& io=ImGui::GetIO();io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    require(pixels && width>0 && height>0,"Editor font atlas is empty.");
    nvrhi::TextureDesc td;td.width=static_cast<std::uint32_t>(width);td.height=static_cast<std::uint32_t>(height);
    td.format=nvrhi::Format::RGBA8_UNORM;td.initialState=nvrhi::ResourceStates::ShaderResource;td.keepInitialState=true;td.debugName="Editor font atlas";
    ui_font=checked.createTexture(td);require(bool(ui_font),"Editor font texture creation failed.");
    commands.open();commands.writeTexture(ui_font,0,0,pixels,static_cast<std::size_t>(width)*4);commands.close();checked.executeCommandList(&commands);
    ++idle_waits;require(checked.waitForIdle(),"Editor font upload failed.");
    nvrhi::SamplerDesc sd;sd.addressU=sd.addressV=nvrhi::SamplerAddressMode::Clamp;sd.minFilter=sd.magFilter=true;
    ui_sampler=checked.createSampler(sd);
    ui_vs=create_embedded_shader(&checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main"),poima_editor_ui_vs);
    ui_ps=create_embedded_shader(&checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main"),poima_editor_ui_ps);
    const nvrhi::VertexAttributeDesc attrs[]={
        nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(ImDrawVert,pos)).setElementStride(sizeof(ImDrawVert)),
        nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(ImDrawVert,uv)).setElementStride(sizeof(ImDrawVert)),
        nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGBA8_UNORM).setOffset(offsetof(ImDrawVert,col)).setElementStride(sizeof(ImDrawVert))};
    ui_input=checked.createInputLayout(attrs,3,ui_vs);
    ui_layout=checked.createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0,sizeof(EditorPushConstants))).addItem(nvrhi::BindingLayoutItem::Texture_SRV(0)).addItem(nvrhi::BindingLayoutItem::Sampler(0)));
    require(ui_vs && ui_ps && ui_input && ui_layout && ui_sampler,"Editor UI shader/layout creation failed.");
    ui_bindings=checked.createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0,sizeof(EditorPushConstants)))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0,ui_font)).addItem(nvrhi::BindingSetItem::Sampler(0,ui_sampler)),ui_layout);
    create_scene_bindings(checked);
    nvrhi::GraphicsPipelineDesc pd;pd.VS=ui_vs;pd.PS=ui_ps;pd.inputLayout=ui_input;pd.bindingLayouts.push_back(ui_layout);
    pd.renderState.depthStencilState.depthTestEnable=false;pd.renderState.depthStencilState.depthWriteEnable=false;
    pd.renderState.rasterState.cullMode=nvrhi::RasterCullMode::None;
    pd.renderState.rasterState.scissorEnable=true;
    auto& blend=pd.renderState.blendState.targets[0];blend.blendEnable=true;blend.srcBlend=nvrhi::BlendFactor::SrcAlpha;blend.destBlend=nvrhi::BlendFactor::InvSrcAlpha;
    blend.srcBlendAlpha=nvrhi::BlendFactor::One;blend.destBlendAlpha=nvrhi::BlendFactor::InvSrcAlpha;
    ui_pipeline=checked.createGraphicsPipeline(pd,ui_framebuffers.front()->getFramebufferInfo());
    require(ui_bindings && ui_pipeline,"Editor UI pipeline creation failed.");
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));io.BackendFlags|=ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendRendererName="poima_nvrhi";
}
void Editor::prepare_frame(nvrhi::IDevice& checked, const ImDrawData* data) {
    ui_data=data;ui_vertex_data.clear();ui_index_data.clear();
    if(!data || data->TotalVtxCount==0)return;
    require(data->Valid && data->TotalVtxCount>=0 && data->TotalIdxCount>=0 && data->TotalVtxCount<=4000000 && data->TotalIdxCount<=12000000,"Invalid or oversized editor UI draw data.");
    require(std::isfinite(data->DisplaySize.x) && std::isfinite(data->DisplaySize.y) && data->DisplaySize.x>0 && data->DisplaySize.y>0,"Invalid editor UI display size.");
    ui_vertex_data.reserve(static_cast<std::size_t>(data->TotalVtxCount));ui_index_data.reserve(static_cast<std::size_t>(data->TotalIdxCount));
    for(int list=0;list<data->CmdListsCount;++list) {
        const auto* source=data->CmdLists[list];
        ui_vertex_data.insert(ui_vertex_data.end(),source->VtxBuffer.begin(),source->VtxBuffer.end());
        ui_index_data.insert(ui_index_data.end(),source->IdxBuffer.begin(),source->IdxBuffer.end());
        for(const auto& command:source->CmdBuffer) {
            require(!command.UserCallback || command.UserCallback==ImDrawCallback_ResetRenderState,"Custom editor UI draw callbacks are unavailable.");
            if(command.UserCallback)continue;
            require(command.GetTexID()==static_cast<ImTextureID>(1) || command.GetTexID()==static_cast<ImTextureID>(2),
                "Editor UI accepts only the font atlas (1) and Scene image (2).");
            require(std::isfinite(command.ClipRect.x) && std::isfinite(command.ClipRect.y) && std::isfinite(command.ClipRect.z) && std::isfinite(command.ClipRect.w),"Editor UI clip rectangle must be finite.");
            require(std::uint64_t(command.IdxOffset)+command.ElemCount<=static_cast<std::uint64_t>(source->IdxBuffer.Size) && command.VtxOffset<=static_cast<unsigned>(source->VtxBuffer.Size),"Editor UI command exceeds its draw list.");
        }
    }
    require(ui_vertex_data.size()==static_cast<std::size_t>(data->TotalVtxCount) && ui_index_data.size()==static_cast<std::size_t>(data->TotalIdxCount),"Editor UI totals do not match draw lists.");
    auto allocate=[&](nvrhi::BufferHandle& buffer,std::size_t& capacity,std::size_t bytes,bool vertex) {
        if(buffer && capacity>=bytes)return;
        capacity=bytes+16384;nvrhi::BufferDesc desc;desc.byteSize=capacity;desc.isVertexBuffer=vertex;desc.isIndexBuffer=!vertex;
        desc.initialState=vertex ? nvrhi::ResourceStates::VertexBuffer : nvrhi::ResourceStates::IndexBuffer;desc.keepInitialState=true;
        buffer=checked.createBuffer(desc);require(bool(buffer),"Editor UI buffer allocation failed.");
    };
    allocate(ui_vertices,ui_vertex_capacity,ui_vertex_data.size()*sizeof(ImDrawVert),true);
    allocate(ui_indices,ui_index_capacity,ui_index_data.size()*sizeof(ImDrawIdx),false);
}
void Editor::record(nvrhi::ICommandList& commands, Target target, std::uint32_t image_index) const {
    if(!ui_data || ui_vertex_data.empty())return;
    commands.writeBuffer(ui_vertices,ui_vertex_data.data(),ui_vertex_data.size()*sizeof(ImDrawVert));
    if(!ui_index_data.empty())commands.writeBuffer(ui_indices,ui_index_data.data(),ui_index_data.size()*sizeof(ImDrawIdx));
    const float sx=2.0f/ui_data->DisplaySize.x,sy=-2.0f/ui_data->DisplaySize.y;
    EditorPushConstants constants{{sx,sy,-1.0f-ui_data->DisplayPos.x*sx,1.0f-ui_data->DisplayPos.y*sy},
        {target.srgb ? 1.0f : 0.0f,0,0,0}};
    std::uint32_t vertex_offset=0,index_offset=0;
    for(int list=0;list<ui_data->CmdListsCount;++list) {
        const auto* source=ui_data->CmdLists[list];
        for(const auto& draw:source->CmdBuffer) {
            if(draw.UserCallback || draw.ElemCount==0)continue;
            auto x=[&](float value){return std::clamp((value-ui_data->DisplayPos.x)*ui_data->FramebufferScale.x,0.0f,static_cast<float>(target.width));};
            auto y=[&](float value){return std::clamp((value-ui_data->DisplayPos.y)*ui_data->FramebufferScale.y,0.0f,static_cast<float>(target.height));};
            const int left=static_cast<int>(std::floor(x(draw.ClipRect.x))),right=static_cast<int>(std::ceil(x(draw.ClipRect.z)));
            const int top=static_cast<int>(std::floor(y(draw.ClipRect.y))),bottom=static_cast<int>(std::ceil(y(draw.ClipRect.w)));
            if(right<=left || bottom<=top)continue;
            const bool scene_image=draw.GetTexID()==static_cast<ImTextureID>(2);
            constants.target[1]=scene_image ? 1.0f : 0.0f;
            nvrhi::GraphicsState state;state.pipeline=ui_pipeline;state.framebuffer=ui_framebuffers.at(image_index);state.bindings.push_back(scene_image ? ui_scene_bindings : ui_bindings);
            state.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(ui_vertices));
            state.indexBuffer=nvrhi::IndexBufferBinding(ui_indices,sizeof(ImDrawIdx)==2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT,0);
            state.viewport.addViewport(nvrhi::Viewport(static_cast<float>(target.width),static_cast<float>(target.height)));
            state.viewport.addScissorRect(nvrhi::Rect(left,right,top,bottom));commands.setGraphicsState(state);commands.setPushConstants(&constants,sizeof(constants));
            commands.drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.ElemCount).setStartIndexLocation(index_offset+draw.IdxOffset).setStartVertexLocation(vertex_offset+draw.VtxOffset));
        }
        vertex_offset+=static_cast<std::uint32_t>(source->VtxBuffer.Size);index_offset+=static_cast<std::uint32_t>(source->IdxBuffer.Size);
    }
}

void Editor::add_framebuffer(nvrhi::IDevice& checked, nvrhi::ITexture* texture) {
    auto framebuffer = checked.createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
    require(bool(framebuffer), "Editor UI framebuffer creation failed.");
    ui_framebuffers.push_back(framebuffer);
}

bool Editor::context_is_current() const noexcept {
    return ImGui::GetCurrentContext() == ui_context;
}

void Editor::detach_targets() noexcept {
    ui_framebuffers.clear();
    ui_scene_bindings = nullptr;
}

void Editor::release_gpu() noexcept {
    if (ui_context && context_is_current()) {
        auto& io = ImGui::GetIO();
        if (io.BackendRendererName && std::strcmp(io.BackendRendererName, "poima_nvrhi") == 0) {
            if (io.Fonts->TexID == static_cast<ImTextureID>(1)) io.Fonts->SetTexID(0);
            io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
            io.BackendRendererName = nullptr;
        }
    }
    ui_framebuffers.clear();
    ui_pipeline = nullptr;
    ui_bindings = nullptr;
    ui_scene_bindings = nullptr;
    ui_layout = nullptr;
    ui_vertices = nullptr;
    ui_indices = nullptr;
    ui_font = nullptr;
    ui_sampler = nullptr;
    ui_input = nullptr;
    ui_vs = nullptr;
    ui_ps = nullptr;
}

#endif
}
