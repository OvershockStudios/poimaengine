// SPDX-License-Identifier: Apache-2.0
// Included in Runtime::Impl.
    static constexpr std::uint64_t max_surface_mark_id=9007199254740991ULL;
    struct SurfaceMarksState {
        std::uint64_t next_id=1;
        std::vector<RuntimeSurfaceMark> rows;
    };
    std::shared_ptr<const SurfaceMarksState> surface_marks;

    entt::entity surface_mark_receiver(const std::string& id) const {
        const auto entity=find(id);
        const auto& definition=topology->definitions.at(id);
        const auto* mesh=registry.try_get<RuntimeMesh>(entity);
        require(mesh && mesh->visible && mesh->material && mesh->material->alpha_mode==MaterialAlphaMode::opaque &&
            !definition.skinned_mesh && !definition.camera_draw && registry.all_of<Body>(entity),
            "Surface mark receiver needs a visible rigid opaque PBR world drawable with collision.");

        if(mesh->mesh) {
            require(definition.mesh_collider && definition.mesh_collider->mesh==mesh->mesh,
                "Surface mark receiver needs the same cooked primitive for rendering and triangle collision.");
        } else {
            require(definition.collider && definition.collider->half_extents==std::array<float,3>{.5f,.5f,.5f},
                "A built-in surface mark box needs matching 0.5 m collider half-extents.");
        }

        const auto& world=registry.get<Node>(entity).world;
        require(rigid_transform(world) && pose_frames::axes(world).basis==pose_frames::Basis::regular,
            "Surface mark receiver needs an unscaled right-handed world transform.");
        // Camera and animation effects can move render geometry independently
        // of a static collider, including through the receiver's ancestors.
        for(auto current=&definition;;current=&topology->definitions.at(current->parent)) {
            require(!current->camera && !current->rig_node && !current->physical_rig,
                "Surface mark receiver cannot inherit camera or animation pose effects.");
            if(current->parent.empty())break;
        }
        return entity;
    }

    static void validate_surface_mark_fields(const SceneSurfaceMark& mark,bool triangle_mesh) {
        const auto bounded=[](float value,float minimum,float maximum) {
            return std::isfinite(value) && value>=minimum && value<=maximum;
        };
        const auto unit=[&](float value) { return bounded(value,0.f,1.f); };
        require(std::all_of(mark.half_extent_m.begin(),mark.half_extent_m.end(),
                [&](float value) { return bounded(value,.005f,4.f); }) &&
            bounded(mark.half_depth_m,.001f,.25f) && unit(mark.minimum_normal_dot) && unit(mark.edge_fraction) &&
            std::all_of(mark.linear_rgb.begin(),mark.linear_rgb.end(),unit) && unit(mark.opacity) &&
            bounded(mark.roughness,.045f,1.f),"Surface mark style is outside the bounded material ranges.");

        const float center_limit=triangle_mesh ? 10000.f : .501f;
        require(std::all_of(mark.center_m.begin(),mark.center_m.end(),
            [&](float value) { return bounded(value,-center_limit,center_limit); }),
            "Surface mark center is outside the supported receiver space.");
        const pose_frames::Vector normal{mark.normal[0],mark.normal[1],mark.normal[2]};
        const pose_frames::Vector tangent{mark.tangent[0],mark.tangent[1],mark.tangent[2]};
        require(std::abs(pose_frames::dot(normal,normal)-1)<1e-5 &&
            std::abs(pose_frames::dot(tangent,tangent)-1)<1e-5 && std::abs(pose_frames::dot(normal,tangent))<1e-5,
            "Surface mark projection axes must be finite and orthonormal.");
    }

    void validate_surface_mark(const RuntimeSurfaceMark& row) const {
        require(row.id>0 && row.id<max_surface_mark_id && row.tick>0 && row.tick<=tick,
            "Invalid surface mark identity or placement tick.");
        const auto receiver=surface_mark_receiver(row.receiver);
        validate_surface_mark_fields(row.mark,bool(registry.get<RuntimeMesh>(receiver).mesh));
    }

    void place_surface_marks(std::span<const RuntimeSurfaceMarkCommand> commands) {
        if(commands.empty())return;
        require(commands.size()<=max_surface_mark_commands,"Surface mark command budget exceeded.");
        auto next=surface_marks ? std::make_shared<SurfaceMarksState>(*surface_marks) : std::make_shared<SurfaceMarksState>();
        require(commands.size()<=max_scene_surface_marks-next->rows.size(),"Scene surface mark capacity exceeded.");
        next->rows.reserve(next->rows.size()+commands.size());
        for(const auto& command:commands) {
            const auto count=std::count_if(next->rows.begin(),next->rows.end(),[&](const auto& row) {
                return row.receiver==command.receiver;
            });
            require(count<static_cast<std::ptrdiff_t>(max_surface_marks_per_receiver),"Surface mark receiver capacity exceeded.");
            require(next->next_id<max_surface_mark_id,"Surface mark identity space exhausted.");
            require(command.ray.ignore.empty(),"Surface mark rays cannot ignore colliders.");

            const auto receiver=surface_mark_receiver(command.receiver);
            const auto& mesh=registry.get<RuntimeMesh>(receiver);
            const auto hit=physics_raycast(command.ray);
            require(hit && hit->entity==command.receiver && hit->normal,
                "Surface mark ray must first hit the expected receiver with a surface normal.");

            const auto& world=registry.get<Node>(receiver).world;
            const auto basis=pose_frames::axes(world);
            const pose_frames::Vector origin{world[12],world[13],world[14]};
            auto center=pose_frames::project(basis,surface_measure::subtract(hit->position,origin));
            auto normal=pose_frames::project(basis,*hit->normal);
            if(mesh.mesh) {
                require(hit->triangle && *hit->triangle<mesh.mesh->indices.size()/3,
                    "Surface mark ray must identify the matching cooked triangle.");
                // Use the frozen render triangle for the anchor. Physics uses
                // a quantized copy of the same vertices in its acceleration tree.
                SceneObject geometry{};
                geometry.world=identity_matrix();
                geometry.mesh=mesh.mesh;
                const auto triangle=surface_measure::triangle(geometry,*hit->triangle);
                require(triangle.normal.has_value(),"Surface mark triangle has no stable normal.");
                center=surface_measure::nearest(triangle,center).position;
                normal=*triangle.normal;
            }

            const auto facing=pose_frames::dot(normal,pose_frames::project(basis,command.ray.direction));
            require(facing<0 || (facing>0 && mesh.material->double_sided),
                "Surface mark ray requires a visible facing surface.");
            const auto normal_length=std::hypot(normal[0],normal[1],normal[2]);
            require(std::isfinite(normal_length) && normal_length>1e-8,"Surface mark hit normal is invalid.");
            for(auto& value:normal)value/=facing>0 ? -normal_length : normal_length;
            const std::size_t axis=std::abs(normal[0])<.9 ? 0 : 2;
            pose_frames::Vector tangent{};
            for(std::size_t k=0;k<3;++k)tangent[k]=(k==axis ? 1. : 0.)-normal[k]*normal[axis];
            const auto tangent_length=std::hypot(tangent[0],tangent[1],tangent[2]);

            auto mark=command.style;
            for(std::size_t k=0;k<3;++k) {
                mark.center_m[k]=static_cast<float>(center[k]);
                mark.normal[k]=static_cast<float>(normal[k]);
                mark.tangent[k]=static_cast<float>(tangent[k]/tangent_length);
            }
            validate_surface_mark_fields(mark,bool(mesh.mesh));
            next->rows.push_back({next->next_id++,tick,command.receiver,mark});
        }
        surface_marks=std::move(next);
    }

    void retire_surface_marks() {
        const auto retired=[&](const RuntimeSurfaceMark& row) {
            return !topology->identities.contains(row.receiver);
        };
        if(!surface_marks || std::none_of(surface_marks->rows.begin(),surface_marks->rows.end(),retired))return;
        auto next=std::make_shared<SurfaceMarksState>(*surface_marks);
        std::erase_if(next->rows,retired);
        surface_marks=std::move(next);
    }
