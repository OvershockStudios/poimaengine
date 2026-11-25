// SPDX-License-Identifier: Apache-2.0
// Included in Context. Scene sprites use the current camera and the opaque
// depth buffer. Their immutable textures share the ordinary texture cache.
    void warm_particle_sprites() {
        const auto& layer=scene->particle_sprites;
        if(layer.styles.empty()) {
            require(layer.instances.empty(),"Particle sprites require frozen styles.");
            return;
        }
        require(samples==1,"Textured particle sprites require one world sample.");
        const auto near_plane=static_cast<float>(scene->near_plane),far_plane=static_cast<float>(scene->far_plane);
        require(rigid_transform(scene->camera_world) && std::isfinite(near_plane) && std::isfinite(far_plane) &&
            near_plane>0 && far_plane>near_plane,"Particle sprites require a rigid camera with finite float lens planes.");
        const auto features=physical.getFormatProperties(vk::Format::eR16G16B16A16Sfloat,dispatch).optimalTilingFeatures;
        require(bool(features&vk::FormatFeatureFlagBits::eColorAttachmentBlend),
            "Selected Vulkan device cannot blend particle sprites into HDR scene color.");
        std::map<std::string_view,const TextureImage*> seen;
        std::size_t base_bytes=0;
        for(const auto& style:layer.styles) {
            require(style.texture && !style.texture->mips.empty() &&
                std::isfinite(style.depth_fade_m) && style.depth_fade_m>=0 && style.depth_fade_m<=10,
                "Invalid frozen particle sprite style.");
            const auto [entry,inserted]=seen.try_emplace(style.texture_sha256,style.texture.get());
            require(entry->second==style.texture.get(),"Particle sprite texture identity resolves to different images.");
            if(!inserted)continue;
            const auto& base=style.texture->mips.front();
            require(seen.size()<=max_scene_particle_sprite_textures && base.width && base.height &&
                base.width<=max_scene_particle_sprite_texture_side && base.height<=max_scene_particle_sprite_texture_side &&
                base.rgba.size()==std::size_t(base.width)*base.height*4 &&
                base.rgba.size()<=max_scene_particle_sprite_texture_bytes-base_bytes,
                "Frozen particle sprite images exceed the texture budget.");
            base_bytes+=base.rgba.size();
            (void)scene_assets.upload_texture(asset_upload(), style.texture);
        }
    }
    void prepare_sprite_pipeline() {
        const bool temporal=temporal_sprites_active();
        auto* color=sprite_target();
        if(!sprite_framebuffer || sprite_framebuffer->getDesc().colorAttachments.size()!=(temporal ? 2u : 1u) ||
            sprite_framebuffer->getDesc().colorAttachments[0].texture!=color ||
            (temporal && sprite_framebuffer->getDesc().colorAttachments[1].texture!=sprite_targets.coverage())) {
            auto description=nvrhi::FramebufferDesc().addColorAttachment(color);
            if(temporal)description.addColorAttachment(sprite_targets.coverage());
            sprite_framebuffer=checked->createFramebuffer(description);
            require(bool(sprite_framebuffer),"Particle sprite framebuffer creation failed.");
        }
        const bool optical_water=water_parameters.has_value();
        auto& pipeline=sprite_pipelines[2*static_cast<std::size_t>(temporal)+static_cast<std::size_t>(optical_water)];
        if(pipeline)return;
        auto vs_desc=nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main");
        auto ps_desc=nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main");
        auto vs=temporal ? create_embedded_shader(checked,vs_desc,poima_particle_sprite_temporal_vs) :
            create_embedded_shader(checked,vs_desc,poima_particle_sprite_vs);
        nvrhi::ShaderHandle ps;
        if(optical_water) {
            ps=temporal ? create_embedded_shader(checked,ps_desc,poima_particle_sprite_temporal_water_ps) :
                create_embedded_shader(checked,ps_desc,poima_particle_sprite_water_ps);
        } else {
            ps=temporal ? create_embedded_shader(checked,ps_desc,poima_particle_sprite_temporal_ps) :
                create_embedded_shader(checked,ps_desc,poima_particle_sprite_ps);
        }
        auto& sprite_layout=sprite_layouts[optical_water];
        if(!sprite_layout) {
            auto desc=nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0,sizeof(SpriteConstants)))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(2));
            if(optical_water) {
                desc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1))
                    .addItem(nvrhi::BindingLayoutItem::Texture_SRV(5))
                    .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6))
                    .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7))
                    .addItem(nvrhi::BindingLayoutItem::Texture_SRV(8))
                    .addItem(nvrhi::BindingLayoutItem::Sampler(1));
            }
            sprite_layout=checked->createBindingLayout(desc);
        }
        require(vs && ps && sprite_layout,"Particle sprite shader or layout creation failed.");
        nvrhi::GraphicsPipelineDesc description;
        description.VS=vs;
        description.PS=ps;
        description.bindingLayouts.push_back(sprite_layout);
        description.renderState.depthStencilState.depthTestEnable=false;
        description.renderState.depthStencilState.depthWriteEnable=false;
        description.renderState.rasterState.cullMode=nvrhi::RasterCullMode::None;
        auto& blend=description.renderState.blendState.targets[0];
        blend.blendEnable=true;
        blend.srcBlend=nvrhi::BlendFactor::One;
        blend.destBlend=nvrhi::BlendFactor::InvSrcAlpha;
        blend.srcBlendAlpha=nvrhi::BlendFactor::Zero;
        blend.destBlendAlpha=nvrhi::BlendFactor::One;
        if(temporal)description.renderState.blendState.targets[1]=blend;
        pipeline=checked->createGraphicsPipeline(description,sprite_framebuffer->getFramebufferInfo());
        require(bool(pipeline),"Particle sprite pipeline creation failed.");
    }
    nvrhi::BindingSetDesc sprite_binding(const FrameSlot& slot,nvrhi::ITexture* image,bool optical_water) const {
        auto desc=nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(0,sizeof(SpriteConstants)))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0,slot.sprite_instances))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(1,depth))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(2,image,nvrhi::Format::UNKNOWN,nvrhi::TextureSubresourceSet(0,1,0,1)));
        if(optical_water) {
            const auto environment=environment_ibl.bound();
            desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(1,frame_buffer))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(5,shadows.texture()))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6,light_clusters.lights()))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7,water_columns))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(8,environment.specular))
                .addItem(nvrhi::BindingSetItem::Sampler(1,environment.sampler));
        }
        return desc;
    }
    void prepare_sprite_frame(FrameSlot& slot) {
        require(!slot.occupied,"Particle sprites require a retired frame slot.");
        sprite_instances.clear();
        sprite_textures.clear();
        sprite_batches.clear();
        pending_draws.sprite_instances=0;
        pending_draws.sprite_batches=0;
        if(!scene || !scene_visible || scene->particle_sprites.instances.empty()) {
            slot.sprite_bindings.clear();
            return;
        }
        const auto& layer=scene->particle_sprites;
        require(layer.instances.size()<=render_schedule::max_sprite_instances,"Too many live particle sprites.");
        const auto& camera=scene->camera_world;
        struct Candidate {const SceneParticleSprite* sprite;double depth;};
        std::vector<Candidate> candidates;
        candidates.reserve(layer.instances.size());
        std::uint64_t previous=0;
        for(const auto& sprite:layer.instances) {
            require(sprite.id>previous && sprite.style<layer.styles.size() && std::isfinite(sprite.size_m) &&
                sprite.size_m>0 && std::all_of(sprite.linear_rgba.begin(),sprite.linear_rgba.end(),
                    [](float value) { return std::isfinite(value) && value>=0 && value<=1; }),
                "Invalid ordered particle sprite instance.");
            previous=sprite.id;
            double view_depth=0;
            for(std::size_t axis=0;axis<3;++axis) {
                require(std::isfinite(sprite.position_m[axis]) &&
                    std::abs(sprite.position_m[axis])<=std::numeric_limits<float>::max(),
                    "Particle sprite position exceeds float range.");
                view_depth-=(sprite.position_m[axis]-camera[12+axis])*camera[8+axis];
            }
            require(std::isfinite(view_depth),"Particle sprite depth is nonfinite.");
            candidates.push_back({&sprite,view_depth});
        }
        std::sort(candidates.begin(),candidates.end(),[](const Candidate& left,const Candidate& right) {
            return left.depth==right.depth ? left.sprite->id<right.sprite->id : left.depth>right.depth;
        });
        for(const auto& candidate:candidates) {
            const auto& sprite=*candidate.sprite;
            const auto& style=layer.styles[sprite.style];
            const auto& image=scene_assets.texture_for(style.texture.get());
            const auto& base=style.texture->mips.front();
            auto rect=sprite.texel_rect;
            if(rect==std::array<std::uint32_t,4>{})rect={0,0,base.width,base.height};
            require(rect[2] && rect[3] && rect[0]<base.width && rect[1]<base.height &&
                rect[2]<=base.width-rect[0] && rect[3]<=base.height-rect[1],
                "Particle sprite texel rectangle is outside its texture.");
            const auto found=std::find(sprite_textures.begin(),sprite_textures.end(),image);
            const auto texture=static_cast<std::uint32_t>(found-sprite_textures.begin());
            if(found==sprite_textures.end())sprite_textures.push_back(image);
            if(sprite_batches.empty() || sprite_batches.back().texture!=texture)
                sprite_batches.push_back({static_cast<std::uint32_t>(sprite_instances.size()),0,texture});
            ++sprite_batches.back().count;
            sprite_instances.push_back({{static_cast<float>(sprite.position_m[0]),static_cast<float>(sprite.position_m[1]),
                static_cast<float>(sprite.position_m[2]),sprite.size_m},sprite.linear_rgba,{style.depth_fade_m,0,0,0},rect});
        }
        if(temporal_sprites_active()) {
            auto* world=world_radiance_texture();
            if(!sprite_targets.hdr()) {
                const auto required=vk::FormatFeatureFlagBits::eColorAttachment|
                    vk::FormatFeatureFlagBits::eColorAttachmentBlend|
                    vk::FormatFeatureFlagBits::eSampledImage|vk::FormatFeatureFlagBits::eTransferDst;
                const auto features=physical.getFormatProperties(vk::Format::eR8Unorm,dispatch).optimalTilingFeatures;
                require((features&required)==required,"Selected Vulkan device cannot blend temporal particle coverage.");
                require_reconstruction_composition_budget(world_composition_mask_bytes(),
                    render_sprite_targets::Targets::bytes_per_pixel*reconstruction_pixels());
            }
            sprite_targets.prepare(*checked.Get(),world);
        }
        prepare_sprite_pipeline();
        const auto size=sprite_instances.size()*sizeof(SpriteInstance);
        if(!slot.sprite_instances || slot.sprite_instances->getDesc().byteSize<size) {
            nvrhi::BufferDesc description;
            description.byteSize=std::bit_ceil(sprite_instances.size())*sizeof(SpriteInstance);
            description.structStride=sizeof(SpriteInstance);
            description.initialState=nvrhi::ResourceStates::ShaderResource;
            description.keepInitialState=true;
            description.debugName="Frame-owned particle sprite instances";
            slot.sprite_instances=checked->createBuffer(description);
            require(bool(slot.sprite_instances),"Particle sprite instance buffer allocation failed.");
            slot.sprite_bindings.clear();
        }
        const bool optical_water=water_parameters.has_value();
        if(optical_water) {
            frame_constants.sprite_water=*water_parameters;
            pending_water_optics->sprites=true;
        }
        slot.sprite_bindings.resize(sprite_textures.size());
        for(std::size_t i=0;i<sprite_textures.size();++i) {
            auto& cached=slot.sprite_bindings[i];
            const auto& image=sprite_textures[i];
            const auto desc=sprite_binding(slot,image.Get(),optical_water);
            if(cached.bindings) {
                const auto& previous=cached.bindings->getDesc()->bindings;
                if(std::equal(previous.begin(),previous.end(),desc.bindings.begin(),desc.bindings.end()))continue;
            }
            auto sprites_bound=checked->createBindingSet(desc,sprite_layouts[optical_water]);
            require(bool(sprites_bound),"Particle sprite bindings failed.");
            cached={image,std::move(sprites_bound)};
        }
        pending_draws.sprite_instances=sprite_instances.size();
        pending_draws.sprite_batches=sprite_batches.size();
        profiling::counter("renderer.sprite_instances",pending_draws.sprite_instances);
        profiling::counter("renderer.sprite_batches",pending_draws.sprite_batches);
    }
    void render_particle_sprites(const FrameSlot& slot) {
        const bool temporal=temporal_sprites_active();
        if(temporal)sprite_targets.record_inputs(*commands.Get(),world_radiance_texture());
        nvrhi::GraphicsState state;
        const bool optical_water=water_parameters.has_value();
        state.pipeline=sprite_pipelines[2*static_cast<std::size_t>(temporal)+static_cast<std::size_t>(optical_water)];
        state.framebuffer=sprite_framebuffer;
        state.viewport.addViewportAndScissorRect(raster_viewport());
        state.bindings.resize(1);
        SpriteConstants constants{};
        std::copy_n(frame_constants.view_projection,16,constants.view_projection.begin());
        for(unsigned i=0;i<3;++i) {
            constants.right_and_near[i]=static_cast<float>(scene->camera_world[i]);
            constants.up_and_far[i]=static_cast<float>(scene->camera_world[4+i]);
        }
        constants.right_and_near[3]=static_cast<float>(scene->near_plane);
        constants.up_and_far[3]=static_cast<float>(scene->far_plane);
        if(temporal) {
            constants.first_and_reserved[1]=std::bit_cast<std::uint32_t>(2.f*frame_constants.temporal_jitter[0]/frame_constants.cluster_viewport[2]);
            constants.first_and_reserved[2]=std::bit_cast<std::uint32_t>(-2.f*frame_constants.temporal_jitter[1]/frame_constants.cluster_viewport[3]);
        }
        for(const auto& batch:sprite_batches) {
            state.bindings[0]=slot.sprite_bindings[batch.texture].bindings;
            constants.first_and_reserved[0]=batch.first;
            commands->setGraphicsState(state);
            commands->setPushConstants(&constants,sizeof(constants));
            commands->draw(nvrhi::DrawArguments().setVertexCount(6).setInstanceCount(batch.count));
        }
    }
