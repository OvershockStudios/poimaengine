// SPDX-License-Identifier: Apache-2.0
// Included in Context. Scene sprites use the current camera and the opaque
// depth buffer. Their immutable textures share the ordinary texture cache.
    void warm_particle_sprites() {
        const auto& layer=scene->particle_sprites;
        if(layer.styles.empty()) {
            require(layer.instances.empty(),"Particle sprites require frozen styles.");
            return;
        }
        require(samples==1 && reconstruction_mode==ReconstructionMode::none,
            "Textured particle sprites require one world sample and reconstruction disabled.");
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
            (void)upload_texture(style.texture);
        }
    }
    void prepare_sprite_pipeline() {
        if(!sprite_framebuffer || sprite_framebuffer->getDesc().colorAttachments[0].texture!=scene_hdr.Get()) {
            sprite_framebuffer=checked->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(scene_hdr));
            require(bool(sprite_framebuffer),"Particle sprite framebuffer creation failed.");
        }
        if(sprite_pipeline)return;
        auto vs=create_embedded_shader(checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Vertex).setEntryName("vertex_main"),poima_particle_sprite_vs);
        auto ps=create_embedded_shader(checked,nvrhi::ShaderDesc(nvrhi::ShaderType::Pixel).setEntryName("pixel_main"),poima_particle_sprite_ps);
        sprite_layout=checked->createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::All)
            .addItem(nvrhi::BindingLayoutItem::PushConstants(0,sizeof(SpriteConstants)))
            .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(2)));
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
        sprite_pipeline=checked->createGraphicsPipeline(description,sprite_framebuffer->getFramebufferInfo());
        require(bool(sprite_pipeline),"Particle sprite pipeline creation failed.");
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
            slot.sprite_depth=nullptr;
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
            const auto& image=texture_cache.at(style.texture.get());
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
        if(slot.sprite_depth!=depth || slot.sprite_bindings.size()!=sprite_textures.size() ||
            !std::equal(sprite_textures.begin(),sprite_textures.end(),slot.sprite_bindings.begin(),slot.sprite_bindings.end(),
                [](const nvrhi::TextureHandle& texture,const SpriteBinding& previous_binding) { return texture==previous_binding.image; })) {
            slot.sprite_bindings.clear();
            for(const auto& texture:sprite_textures) {
                auto bindings=checked->createBindingSet(nvrhi::BindingSetDesc()
                    .addItem(nvrhi::BindingSetItem::PushConstants(0,sizeof(SpriteConstants)))
                    .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0,slot.sprite_instances))
                    .addItem(nvrhi::BindingSetItem::Texture_SRV(1,depth))
                    .addItem(nvrhi::BindingSetItem::Texture_SRV(2,texture,nvrhi::Format::UNKNOWN,nvrhi::TextureSubresourceSet(0,1,0,1))),
                    sprite_layout);
                require(bool(bindings),"Particle sprite bindings failed.");
                slot.sprite_bindings.push_back({texture,std::move(bindings)});
            }
            slot.sprite_depth=depth;
        }
        pending_draws.sprite_instances=sprite_instances.size();
        pending_draws.sprite_batches=sprite_batches.size();
        profiling::counter("renderer.sprite_instances",pending_draws.sprite_instances);
        profiling::counter("renderer.sprite_batches",pending_draws.sprite_batches);
    }
    void render_particle_sprites(const FrameSlot& slot) {
        nvrhi::GraphicsState state;
        state.pipeline=sprite_pipeline;
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
        for(const auto& batch:sprite_batches) {
            state.bindings[0]=slot.sprite_bindings[batch.texture].bindings;
            constants.first_and_reserved[0]=batch.first;
            commands->setGraphicsState(state);
            commands->setPushConstants(&constants,sizeof(constants));
            commands->draw(nvrhi::DrawArguments().setVertexCount(6).setInstanceCount(batch.count));
        }
    }
