// SPDX-License-Identifier: Apache-2.0
// Included in Runtime::Impl.
    static constexpr std::uint64_t max_surface_mark_id=9007199254740991ULL;
    struct SurfaceMarksState {
        std::uint64_t next_id=1;
        std::vector<RuntimeSurfaceMark> rows;
    };
    std::shared_ptr<const SurfaceMarksState> surface_marks;
    std::vector<RuntimeSurfaceMarkCommand> gameplay_surface_marks;
    std::size_t surface_mark_attempts=0;
    std::vector<SceneSurfaceMarkMask> surface_mark_masks;

    void initialize_surface_mark_profiles(const RuntimeDefinition& definition) {
        require(definition.surface_mark_profiles.size()<=max_surface_mark_profiles,"Surface mark profile budget exceeded.");
        std::map<std::string,std::shared_ptr<const TextureImage>> masks;
        for(const auto& [id,profile]:definition.surface_mark_profiles) {
            require(surface_mark_profiles::valid_id(id) && surface_mark_profiles::lowercase_hex(profile.mask_sha256,64) &&
                bool(profile.image),"Invalid frozen surface mark profile.");
            const auto [entry,inserted]=masks.try_emplace(profile.mask_sha256,profile.image);
            require(inserted || entry->second==profile.image,"Frozen surface mark aliases must share their cooked image.");
            if(inserted)surface_mark_profiles::validate_image(*profile.image);
        }
        require(masks.size()<=max_surface_mark_masks,"Surface mark mask budget exceeded.");
        surface_mark_masks.reserve(masks.size());
        for(const auto& [sha256,image]:masks)surface_mark_masks.push_back({sha256,image});
    }

    const RuntimeSurfaceMarkProfile& surface_mark_profile(const std::string& id) const {
        const auto& profiles=topology->live.surface_mark_profiles;
        const auto found=profiles.find(id);
        require(found!=profiles.end(),"Surface mark profile does not exist in the frozen catalog.");
        return found->second;
    }

    void check_surface_mark_request(const PoimaGameSurfaceMarkCommandV1* request,std::uint32_t version,std::size_t bytes) {
        require_tick();
        require(surface_mark_attempts<max_surface_mark_commands,"Gameplay exceeded 16 surface mark requests in one tick.");
        ++surface_mark_attempts;
        require(request && request->version==version && request->bytes==bytes &&
            (request->receiver.high || request->receiver.low),"Invalid gameplay surface mark request.");
    }

    static double travel_direction_length(const pose_frames::Vector& direction) {
        require(std::all_of(direction.begin(),direction.end(),[](double value) {
            return std::isfinite(value) && std::abs(value)<=1e6;
        }),"Surface mark travel direction must be finite and within 1e6.");
        const auto length=std::hypot(direction[0],direction[1],direction[2]);
        require(length>=1e-8,"Surface mark travel direction must have length at least 1e-8.");
        return length;
    }

    void queue_surface_mark(const PoimaGameSurfaceMarkCommandV1& request,
        std::optional<pose_frames::Vector> travel_direction={},std::string profile={}) {
        RuntimeSurfaceMarkCommand command;
        command.receiver=gameplay_id(request.receiver);
        if(request.source.high || request.source.low)command.source=gameplay_id(request.source);
        std::copy_n(request.origin,3,command.ray.origin.begin());
        std::copy_n(request.direction,3,command.ray.direction.begin());
        command.ray.distance=request.distance;
        command.travel_direction=travel_direction;
        command.profile=std::move(profile);
        auto& mark=command.style;
        std::copy_n(request.half_extent_m,2,mark.half_extent_m.begin());
        std::copy_n(request.linear_rgb,3,mark.linear_rgb.begin());
        mark.half_depth_m=request.half_depth_m;
        mark.minimum_normal_dot=request.minimum_normal_dot;
        mark.edge_fraction=request.edge_fraction;
        mark.opacity=request.opacity;
        mark.roughness=request.roughness;
        validate_surface_mark_fields(mark,false);
        gameplay_surface_marks.push_back(std::move(command));
    }

    void queue_directional_surface_mark(const PoimaGameSurfaceMarkCommandV2& request,std::string profile={}) {
        const auto& direction=request.travel_direction;
        const pose_frames::Vector travel{direction[0],direction[1],direction[2]};
        travel_direction_length(travel);
        queue_surface_mark(request.mark,travel,std::move(profile));
    }

    static int32_t POIMA_CALL stage_surface_mark(void* context,const PoimaGameSurfaceMarkCommandV1* request,
        PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Surface mark context is absent.");
            auto& self=*static_cast<Impl*>(context);
            self.check_surface_mark_request(request,1,sizeof(PoimaGameSurfaceMarkCommandV1));
            self.queue_surface_mark(*request);
        });
    }

    static int32_t POIMA_CALL stage_directional_surface_mark(void* context,const PoimaGameSurfaceMarkCommandV2* request,
        PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Surface mark context is absent.");
            auto& self=*static_cast<Impl*>(context);
            self.check_surface_mark_request(request ? &request->mark : nullptr,2,sizeof(PoimaGameSurfaceMarkCommandV2));
            self.queue_directional_surface_mark(*request);
        });
    }

    static int32_t POIMA_CALL stage_textured_surface_mark(void* context,const PoimaGameSurfaceMarkCommandV3* request,
        PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Surface mark context is absent.");
            auto& self=*static_cast<Impl*>(context);
            self.check_surface_mark_request(request ? &request->directional.mark : nullptr,3,sizeof(PoimaGameSurfaceMarkCommandV3));
            require(request->profile.high || request->profile.low,"Textured surface marks require a nonzero profile ID.");
            self.queue_directional_surface_mark(request->directional,gameplay_id(request->profile));
        });
    }

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
        require(row.profile.empty()==row.mark.mask_sha256.empty(),"Surface mark profile and mask must be paired.");
        if(!row.profile.empty())require(surface_mark_profile(row.profile).mask_sha256==row.mark.mask_sha256,
            "Saved surface mark mask does not match its frozen profile.");
    }

    RuntimeRay surface_mark_ray(const RuntimeSurfaceMarkCommand& command) const {
        const auto& requested=command.ray;
        require(requested.ignore.empty(),"Surface mark rays cannot ignore colliders.");
        if(command.source.empty())return requested;
        const auto bounded=[](double value){return std::isfinite(value) && std::abs(value)<=1e6;};
        require(std::all_of(requested.origin.begin(),requested.origin.end(),bounded) &&
            std::all_of(requested.direction.begin(),requested.direction.end(),bounded),
            "Surface mark source-local ray vectors must be finite and within 1e6.");
        const auto& world=registry.get<Node>(find(command.source)).world;
        require(rigid_transform(world) && pose_frames::axes(world).basis==pose_frames::Basis::regular,
            "Surface mark source needs an unscaled right-handed world transform.");
        RuntimeRay ray;
        ray.distance=requested.distance;
        for(std::size_t row=0;row<3;++row) {
            ray.origin[row]=world[12+row];
            ray.direction[row]=0;
            for(std::size_t axis=0;axis<3;++axis) {
                ray.origin[row]+=world[4*axis+row]*requested.origin[axis];
                ray.direction[row]+=world[4*axis+row]*requested.direction[axis];
            }
        }
        return ray;
    }

    pose_frames::Vector surface_mark_tangent(const RuntimeSurfaceMarkCommand& command,
        const pose_frames::Axes& receiver_basis,const pose_frames::Vector& normal) const {
        if(command.travel_direction) {
            auto travel=*command.travel_direction;
            const auto length=travel_direction_length(travel);
            for(auto& value:travel)value/=length;
            if(!command.source.empty()) {
                // The ray has already validated the same final source pose.
                const auto source_basis=pose_frames::axes(registry.get<Node>(find(command.source)).world);
                const auto local=travel;
                for(std::size_t row=0;row<3;++row) {
                    travel[row]=source_basis.direction[0][row]*local[0]+source_basis.direction[1][row]*local[1]+
                        source_basis.direction[2][row]*local[2];
                }
            }
            travel=pose_frames::project(receiver_basis,travel);
            const auto normal_dot=pose_frames::dot(normal,travel);
            for(std::size_t k=0;k<3;++k)travel[k]-=normal[k]*normal_dot;
            const auto projected_length=std::hypot(travel[0],travel[1],travel[2]);
            if(projected_length>1e-6) {
                for(auto& value:travel)value/=projected_length;
                return travel;
            }
        }
        const std::size_t axis=std::abs(normal[0])<.9 ? 0 : 2;
        pose_frames::Vector tangent{};
        for(std::size_t k=0;k<3;++k)tangent[k]=(k==axis ? 1. : 0.)-normal[k]*normal[axis];
        const auto length=std::hypot(tangent[0],tangent[1],tangent[2]);
        for(auto& value:tangent)value/=length;
        return tangent;
    }

    void place_surface_marks(std::span<const RuntimeSurfaceMarkCommand> caller) {
        const std::span<const RuntimeSurfaceMarkCommand> gameplay=gameplay_surface_marks;
        if(caller.empty() && gameplay.empty())return;
        require(caller.size()<=max_surface_mark_commands && gameplay.size()<=max_surface_mark_commands-caller.size(),
            "Surface mark command budget exceeded.");
        const auto requested=caller.size()+gameplay.size();
        auto next=surface_marks ? std::make_shared<SurfaceMarksState>(*surface_marks) : std::make_shared<SurfaceMarksState>();
        require(requested<=max_scene_surface_marks-next->rows.size(),"Scene surface mark capacity exceeded.");
        next->rows.reserve(next->rows.size()+requested);
        // Caller marks receive earlier IDs than gameplay marks on the same tick.
        for(const auto commands:{caller,gameplay})for(const auto& command:commands) {
            const auto count=std::count_if(next->rows.begin(),next->rows.end(),[&](const auto& row) {
                return row.receiver==command.receiver;
            });
            require(count<static_cast<std::ptrdiff_t>(max_surface_marks_per_receiver),"Surface mark receiver capacity exceeded.");
            require(next->next_id<max_surface_mark_id,"Surface mark identity space exhausted.");
            const auto receiver=surface_mark_receiver(command.receiver);
            const auto& mesh=registry.get<RuntimeMesh>(receiver);
            const auto ray=surface_mark_ray(command);
            const auto hit=physics_raycast(ray);
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

            const auto facing=pose_frames::dot(normal,pose_frames::project(basis,ray.direction));
            require(facing<0 || (facing>0 && mesh.material->double_sided),
                "Surface mark ray requires a visible facing surface.");
            const auto normal_length=std::hypot(normal[0],normal[1],normal[2]);
            require(std::isfinite(normal_length) && normal_length>1e-8,"Surface mark hit normal is invalid.");
            for(auto& value:normal)value/=facing>0 ? -normal_length : normal_length;
            const auto tangent=surface_mark_tangent(command,basis,normal);

            auto mark=command.style;
            mark.mask_sha256=command.profile.empty() ? std::string{} : surface_mark_profile(command.profile).mask_sha256;
            for(std::size_t k=0;k<3;++k) {
                mark.center_m[k]=static_cast<float>(center[k]);
                mark.normal[k]=static_cast<float>(normal[k]);
                mark.tangent[k]=static_cast<float>(tangent[k]);
            }
            validate_surface_mark_fields(mark,bool(mesh.mesh));
            next->rows.push_back({next->next_id++,tick,command.receiver,std::move(mark),command.profile});
        }
        surface_marks=std::move(next);
        gameplay_surface_marks.clear();
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
