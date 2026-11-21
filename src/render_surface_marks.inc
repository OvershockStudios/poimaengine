// SPDX-License-Identifier: Apache-2.0
// Included in Context. Keep marks out of the per-object stream except for
// their range. The receiver's current rigid transform supplies the frame.
    void release_stale_surface_mark_binding(FrameSlot& slot) {
        if(slot.surface_mark_masks && slot.surface_mark_masks.Get()!=white_surface_mark_mask.Get() &&
            slot.surface_mark_masks.Get()!=surface_mark_array.texture.Get()) {
            slot.surface_mark_binding=nullptr;
            slot.surface_mark_masks=nullptr;
        }
    }

    void validate_surface_mark_catalog() {
        namespace rs=render_schedule;
        const auto& catalog=scene->surface_mark_masks;
        require(catalog.size()<=rs::max_surface_mark_mask_layers,"Surface mark mask array exceeds its layer budget.");
        std::string_view previous;
        for(const auto& mask:catalog) {
            require(mask.sha256.size()==64 && mask.sha256.find_first_not_of("0123456789abcdef")==std::string::npos &&
                previous<mask.sha256 && mask.image && !mask.image->srgb && !mask.image->mips.empty(),
                "Surface mark masks require sorted unique cooked digests and linear images.");
            const auto& mip=mask.image->mips.front();
            require(mip.width>0 && mip.width<=rs::max_surface_mark_mask_side && mip.height>0 &&
                mip.height<=rs::max_surface_mark_mask_side && mip.rgba.size()==std::size_t(mip.width)*mip.height*4,
                "Surface mark mask base level has invalid dimensions or texels.");
            previous=mask.sha256;
        }
        if(!std::equal(catalog.begin(),catalog.end(),surface_mark_array.digests.begin(),surface_mark_array.digests.end(),
            [](const SceneSurfaceMarkMask& mask,const std::string& digest){return mask.sha256==digest;})) {
            surface_mark_array={};
            for(auto& slot:slots)if(!slot.occupied)release_stale_surface_mark_binding(slot);
        }
        sampled_surface_mark_masks=false;
    }

    std::uint32_t surface_mark_mask_metadata(const std::string& digest) {
        if(digest.empty())return 0;
        const auto& catalog=scene->surface_mark_masks;
        const auto mask=std::lower_bound(catalog.begin(),catalog.end(),digest,
            [](const SceneSurfaceMarkMask& image,const std::string& key){return image.sha256<key;});
        require(mask!=catalog.end() && mask->sha256==digest,"Surface mark mask is absent from its frozen scene catalog.");
        const auto& mip=mask->image->mips.front();
        sampled_surface_mark_masks=true;
        const auto layer=static_cast<std::uint32_t>(mask-catalog.begin())+1;
        return layer | ((mip.width-1)<<5u) | ((mip.height-1)<<13u);
    }

    void pack_surface_marks(const SceneObject& object,DrawItem& item) {
        if(object.surface_marks.empty())return;
        require(!object.skin && !object.camera_draw && !object.stream_geometry &&
            item.constants.base_metallic[3]>=0 && item.constants.alpha[2]==0 && rigid_transform(object.world),
            "Surface marks require rigid, opaque world geometry with PBR shading.");
        require(object.surface_marks.size()<=max_surface_marks_per_receiver &&
            object.surface_marks.size()<=max_scene_surface_marks-surface_marks.size(),
            "Surface marks exceed the per-receiver or scene limit.");

        const auto direction=[&](const std::array<float,3>& local,double w) {
            std::array<float,3> result{};
            for(std::size_t row=0;row<3;++row) {
                const double world=object.world[row]*local[0]+object.world[4+row]*local[1]+
                    object.world[8+row]*local[2]+object.world[12+row]*w;
                require(std::isfinite(world) && std::abs(world)<=std::numeric_limits<float>::max(),
                    "Surface mark frame exceeds float range.");
                result[row]=static_cast<float>(world);
            }
            return result;
        };
        const auto dot=[](const std::array<float,3>& a,const std::array<float,3>& b) {
            return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
        };
        const auto unit_interval=[](float value) {return value>=0 && value<=1;};
        const auto positive=[](float value) {return std::isfinite(value) && value>0;};

        item.surface_mark_offset=static_cast<std::uint32_t>(surface_marks.size());
        item.surface_mark_count=static_cast<std::uint32_t>(object.surface_marks.size());
        item.local_surface_marks=object.surface_marks;
        for(const auto& mark:object.surface_marks) {
            require(std::abs(dot(mark.normal,mark.normal)-1)<1e-4f &&
                std::abs(dot(mark.tangent,mark.tangent)-1)<1e-4f &&
                std::abs(dot(mark.normal,mark.tangent))<1e-4f &&
                std::all_of(mark.half_extent_m.begin(),mark.half_extent_m.end(),positive) && positive(mark.half_depth_m) &&
                unit_interval(mark.minimum_normal_dot) && unit_interval(mark.edge_fraction) && unit_interval(mark.opacity) &&
                unit_interval(mark.roughness) && std::all_of(mark.linear_rgb.begin(),mark.linear_rgb.end(),unit_interval),
                "Invalid surface mark dimensions, orientation, or material parameters.");
            const auto center=direction(mark.center_m,1),normal=direction(mark.normal,0),tangent=direction(mark.tangent,0);
            surface_marks.push_back({{center[0],center[1],center[2],mark.half_depth_m},
                {normal[0],normal[1],normal[2],mark.minimum_normal_dot},
                {tangent[0],tangent[1],tangent[2],mark.half_extent_m[0]},
                {mark.linear_rgb[0],mark.linear_rgb[1],mark.linear_rgb[2],mark.opacity},
                {mark.half_extent_m[1],mark.roughness,mark.edge_fraction},surface_mark_mask_metadata(mark.mask_sha256)});
        }
    }

    nvrhi::BufferHandle make_surface_mark_buffer(std::size_t capacity) {
        nvrhi::BufferDesc desc;
        desc.byteSize=capacity*sizeof(GpuSurfaceMark);
        desc.structStride=sizeof(GpuSurfaceMark);
        desc.initialState=nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState=true;
        desc.debugName="Surface mark payload";
        auto buffer=checked->createBuffer(desc);
        require(bool(buffer),"Surface mark buffer allocation failed.");
        return buffer;
    }

    nvrhi::TextureHandle make_surface_mark_mask_texture(std::uint32_t width,std::uint32_t height,std::uint32_t layers) {
        const auto features=physical.getFormatProperties(vk::Format::eR8Unorm,dispatch).optimalTilingFeatures;
        require(bool(features&vk::FormatFeatureFlagBits::eSampledImageFilterLinear),"GPU cannot linearly sample R8 surface mark masks.");
        nvrhi::TextureDesc desc;
        desc.width=width;desc.height=height;desc.arraySize=layers;
        desc.dimension=nvrhi::TextureDimension::Texture2DArray;desc.format=nvrhi::Format::R8_UNORM;
        desc.initialState=nvrhi::ResourceStates::ShaderResource;desc.keepInitialState=true;
        desc.debugName="Surface mark mask array";
        auto texture=checked->createTexture(desc);
        require(bool(texture),"Surface mark mask array allocation failed.");
        return texture;
    }

    void prepare_surface_mark_mask_array() {
        if(surface_mark_array.texture)return;
        const auto& catalog=scene->surface_mark_masks;
        require(!catalog.empty(),"Textured surface marks require a frozen mask catalog.");
        std::uint32_t width=0,height=0;
        SurfaceMarkMaskArray fresh;
        fresh.digests.reserve(catalog.size());
        for(const auto& mask:catalog) {
            const auto& mip=mask.image->mips.front();
            width=std::max(width,mip.width);height=std::max(height,mip.height);
            fresh.digests.push_back(mask.sha256);
        }
        fresh.texture=make_surface_mark_mask_texture(width,height,static_cast<std::uint32_t>(catalog.size()));
        const auto slice_bytes=std::size_t(width)*height;
        fresh.pending_texels.resize(slice_bytes*catalog.size());
        for(std::size_t layer=0;layer<catalog.size();++layer) {
            const auto& mip=catalog[layer].image->mips.front();
            auto* pixels=fresh.pending_texels.data()+layer*slice_bytes;
            for(std::uint32_t y=0;y<mip.height;++y)
                for(std::uint32_t x=0;x<mip.width;++x)
                    pixels[std::size_t(y)*width+x]=mip.rgba[(std::size_t(y)*mip.width+x)*4];
        }
        surface_mark_array=std::move(fresh);
    }

    nvrhi::BindingSetHandle make_surface_mark_binding(nvrhi::IBuffer* buffer,nvrhi::ITexture* masks) {
        auto set=checked->createBindingSet(nvrhi::BindingSetDesc()
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0,buffer))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(1,masks))
            .addItem(nvrhi::BindingSetItem::Sampler(0,surface_mark_mask_sampler)),surface_mark_layout);
        require(bool(set),"Surface mark binding creation failed.");
        return set;
    }

    void prepare_surface_mark_frame(FrameSlot& slot) {
        if(!scene_visible || surface_marks.empty())return;
        require(!slot.occupied,"Surface marks require a retired frame slot.");
        const auto count=surface_marks.size();
        if(!slot.surface_marks || slot.surface_marks->getDesc().byteSize<count*sizeof(GpuSurfaceMark)) {
            slot.surface_marks=make_surface_mark_buffer(std::bit_ceil(count));
            slot.surface_mark_binding=nullptr;
        }
        if(sampled_surface_mark_masks)prepare_surface_mark_mask_array();
        const auto& masks=sampled_surface_mark_masks ? surface_mark_array.texture : white_surface_mark_mask;
        if(!slot.surface_mark_binding || slot.surface_mark_masks.Get()!=masks.Get()) {
            slot.surface_mark_binding=make_surface_mark_binding(slot.surface_marks,masks);
            slot.surface_mark_masks=masks;
        }
    }

    void record_surface_mark_mask_upload(nvrhi::ITexture* masks) {
        require(masks==surface_mark_array.texture.Get(),"Scheduled surface mark mask upload differs from the staged array.");
        const auto& desc=masks->getDesc();
        const auto slice_bytes=std::size_t(desc.width)*desc.height;
        const auto& pixels=surface_mark_array.pending_texels;
        require(pixels.size()==slice_bytes*desc.arraySize,"Scheduled surface mark mask array has incomplete texels.");
        for(std::uint32_t layer=0;layer<desc.arraySize;++layer)
            commands->writeTexture(masks,layer,0,pixels.data()+layer*slice_bytes,desc.width);
    }

    const nvrhi::BindingSetHandle& world_surface_mark_binding(const FrameSlot& slot) const {
        return scene_visible && !surface_marks.empty() ? slot.surface_mark_binding : empty_surface_mark_binding;
    }
