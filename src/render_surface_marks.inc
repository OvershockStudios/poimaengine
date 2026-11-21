// SPDX-License-Identifier: Apache-2.0
// Included in Context. Keep marks out of the per-object stream except for
// their range. The receiver's current rigid transform supplies the frame.
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
                {mark.half_extent_m[1],mark.roughness,mark.edge_fraction,0}});
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

    nvrhi::BindingSetHandle make_surface_mark_binding(nvrhi::IBuffer* buffer) {
        auto set=checked->createBindingSet(nvrhi::BindingSetDesc()
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0,buffer)),surface_mark_layout);
        require(bool(set),"Surface mark binding creation failed.");
        return set;
    }

    void prepare_surface_mark_frame(FrameSlot& slot) {
        if(!scene_visible || surface_marks.empty())return;
        require(!slot.occupied,"Surface marks require a retired frame slot.");
        const auto count=surface_marks.size();
        if(!slot.surface_marks || slot.surface_marks->getDesc().byteSize<count*sizeof(GpuSurfaceMark)) {
            auto buffer=make_surface_mark_buffer(std::bit_ceil(count));
            auto binding=make_surface_mark_binding(buffer);
            slot.surface_marks=std::move(buffer);
            slot.surface_mark_binding=std::move(binding);
        }
    }

    const nvrhi::BindingSetHandle& world_surface_mark_binding(const FrameSlot& slot) const {
        return scene_visible && !surface_marks.empty() ? slot.surface_mark_binding : empty_surface_mark_binding;
    }
