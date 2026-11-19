// SPDX-License-Identifier: Apache-2.0
// Included in Runtime::Impl.
    std::vector<RuntimeAttachmentCommand> game_attachment_commands;
    std::vector<RuntimeAttachmentRelease> game_release_commands;
    std::map<std::string,StagedPickupClearance> game_pickup_checks;
    std::size_t game_attachment_reads=0,game_attachment_attempts=0;

    void finish_game_attachments() {
        attachment_outcomes.reset();
        if(game_attachment_commands.empty() && game_release_commands.empty())return;
        AttachmentOutcomes outcomes;
        try {
            if(std::none_of(game_attachment_commands.begin(),game_attachment_commands.end(),
                [](const auto& command){return command.recover_miss;})) {
                if(!game_attachment_commands.empty())change_attachments(structure_revision,game_attachment_commands);
            } else {
                std::size_t checks=0;
                std::vector<RuntimeAttachmentCommand> strict;
                for(auto& command:game_attachment_commands)
                    if(!command.recover_miss)strict.push_back(std::move(command));
                if(!strict.empty())change_attachments(structure_revision,strict,&checks);
                for(const auto& command:game_attachment_commands)if(command.recover_miss) {
                    RuntimeAttachmentOutcome outcome{command.anchor,{}};
                    const auto guard=game_pickup_checks.find(command.entity);
                    try {change_attachments(structure_revision,{&command,1},&checks,true,
                        guard==game_pickup_checks.end() ? nullptr : &guard->second);}
                    catch(const RuntimeAttachmentContactRejected& error) {
                        const auto& row=error.contact;
                        outcome.detail=RuntimeAttachmentFrameMiss{row.position_error_m,row.position_limit_m,
                            row.rotation_limit_deg,row.rotation_error_deg};
                    } catch(const RuntimeAttachmentSurfaceRejected& error) {
                        const auto& row=error.contact;
                        outcome.detail=RuntimeAttachmentSurfaceMiss{row.probe_index,row.measurement.target_triangle,
                            row.measurement.distance_m,row.distance_limit_m};
                    } catch(const PickupClearanceRejected& blocked) {
                        auto detail=blocked.detail;
                        if(guard->second.fallback)detail.pose=resolve_pickup_pose(command.entity,guard->second);
                        outcome.detail=std::move(detail);
                    }
                    outcomes.emplace(command.entity,std::move(outcome));
                }
            }
            for(const auto& command:game_release_commands) {
                try {
                    auto published=publish_attachment_release(structure_revision,command);
                    outcomes.emplace(command.item,RuntimeAttachmentOutcome{std::move(published.spawned.front()),RuntimeAttachmentReleased{}});
                } catch(const RuntimeAttachmentReleaseRejected& error) {
                    outcomes.emplace(command.item,RuntimeAttachmentOutcome{error.holder,error.witness});
                }
            }
            if(!outcomes.empty())attachment_outcomes=std::make_shared<const AttachmentOutcomes>(std::move(outcomes));
        } catch(const std::invalid_argument& error) {throw std::runtime_error(error.what());}
        game_attachment_commands.clear();
        game_release_commands.clear();
        game_pickup_checks.clear();
    }

    void get_game_attachment_outcome(const PoimaEntityId& entity,PoimaGameItemAttachmentStateV1* output) const {
        require(game && game->requires_item_attachment_recovery() &&
            output->bytes==sizeof(PoimaGameItemAttachmentOutcomeV2) && output->present==0 && output->reserved==0,
            "Invalid gameplay attachment outcome query or missing recovery feature.");
        PoimaGameItemAttachmentOutcomeV2 result{};
        result.version=2;result.bytes=sizeof(result);result.entity=entity;result.sampled_tick=tick;
        if(attachment_outcomes) {
            const auto found=attachment_outcomes->find(gameplay_id(entity));
            if(found!=attachment_outcomes->end() && found->second.detail.index()<3) {
                const auto& row=found->second;
                result.status=static_cast<std::uint32_t>(row.detail.index()+1);
                result.anchor=gameplay_id(row.anchor);
                if(const auto* frame=std::get_if<RuntimeAttachmentFrameMiss>(&row.detail)) {
                    result.position_error_m=frame->position_error_m;result.position_limit_m=frame->position_limit_m;
                    result.rotation_limit_deg=frame->rotation_limit_deg;
                    result.rotation_available=frame->rotation_error_deg.has_value();
                    result.rotation_error_deg=frame->rotation_error_deg.value_or(0.);
                }
                if(const auto* surface=std::get_if<RuntimeAttachmentSurfaceMiss>(&row.detail)) {
                    result.probe_index=static_cast<std::uint32_t>(surface->probe_index);result.target_triangle=surface->target_triangle;
                    result.distance_m=surface->distance_m;result.distance_limit_m=surface->distance_limit_m;
                }
            }
        }
        std::memcpy(output,&result,sizeof(result));
    }
    void get_game_release_outcome(const PoimaEntityId& item,PoimaGameItemAttachmentStateV1* output) const {
        require(game && game->requires_item_attachment_release() &&
            output->bytes==sizeof(PoimaGameItemReleaseOutcomeV3) && output->present==0 && output->reserved==0,
            "Invalid item release outcome query or missing release feature.");
        PoimaGameItemReleaseOutcomeV3 result{};
        result.version=3;result.bytes=sizeof(result);result.item=item;result.sampled_tick=tick;
        if(attachment_outcomes) {
            const auto found=attachment_outcomes->find(gameplay_id(item));
            if(found!=attachment_outcomes->end()) {
                const auto& row=found->second;
                if(std::holds_alternative<RuntimeAttachmentReleased>(row.detail)) {
                    result.status=1;result.anchor=gameplay_id(row.anchor);
                } else if(const auto* blocked=std::get_if<RuntimeAttachmentReleaseBlocked>(&row.detail)) {
                    result.status=2;result.anchor=gameplay_id(row.anchor);
                    result.obstacle=gameplay_id(blocked->obstacle);result.penetration_m=blocked->penetration_m;
                }
            }
        }
        std::memcpy(output,&result,sizeof(result));
    }
    void get_game_pickup_blocker(const PoimaEntityId& item,PoimaGameItemAttachmentStateV1* output) const {
        std::uint32_t header[4];
        std::memcpy(header,output,sizeof(header));
        require(game && game->requires_item_pickup_clearance() && header[1]==sizeof(PoimaGameItemPickupBlockerV5) &&
            header[2]==0 && header[3]==0,"Invalid pickup obstruction query or missing clearance feature.");
        PoimaGameItemPickupBlockerV5 result{};
        result.version=5;result.bytes=sizeof(result);result.item=item;result.sampled_tick=tick;
        if(attachment_outcomes) {
            const auto found=attachment_outcomes->find(gameplay_id(item));
            if(found!=attachment_outcomes->end())if(const auto* blocked=std::get_if<RuntimeAttachmentReachBlocked>(&found->second.detail)) {
                result.anchor=gameplay_id(found->second.anchor);result.obstacle=gameplay_id(blocked->obstacle);
                result.segment=blocked->segment;result.interval=blocked->interval;
                result.fraction=blocked->fraction;result.penetration_m=blocked->penetration_m;
            }
        }
        std::memcpy(output,&result,sizeof(result));
    }
    void stage_game_pickup_clearance(const RuntimeAttachmentCommand& command,const PoimaGameItemAttachmentCommandV1* source) {
        require(game && game->requires_item_pickup_clearance(),"Gameplay must declare item_attachment_pickup_clearance_v1.");
        PoimaGameItemPickupClearanceV7 request;
        std::memcpy(&request,source,sizeof(request));
        require(request.reserved==0 && request.slot>=1 && request.slot<=8,"Invalid pickup clearance slot or reserved field.");
        const bool with_fallback=source->version>=8;
        const bool blend=source->version>=9;
        const bool steer=source->version==10;
        require(!with_fallback || game->requires_item_pickup_pose(),"Gameplay must declare item_attachment_pickup_pose_v1.");
        require(!blend || game->requires_item_pickup_blend(),"Gameplay must declare item_attachment_pickup_blend_v1.");
        require(!steer || game->requires_item_pickup_steering(),"Gameplay must declare item_attachment_pickup_steering_v1.");
        const auto casts=2*ik_clearance_intervals+(with_fallback ? 4u : 0u)+
            (blend ? 2*pickup_blend_samples : 0u)+(steer ? 2*pickup_steer_samples : 0u);
        require(game_capsule_query_attempts<=capsule_query_tick_budget-casts,"Gameplay exceeded the capsule query budget.");
        const auto rig=gameplay_id(request.rig);
        (void)find(rig);
        require(!topology->definitions.at(rig).physical_rig,"Pickup clearance supports animated rigs without a physical pose owner.");
        const std::array radii{request.radii[0],request.radii[1]};
        validate_chain_radii(radii);
        const auto joints=animations->manual_ik_nodes(rig,request.slot);
        require(attachment_contains(attachment_graph.get(),joints.back(),command.anchor),
            "Pickup clearance must match the receiving hand's live manual chain.");
        const auto start=read_chain_points(joints);
        require(std::none_of(game_pickup_checks.begin(),game_pickup_checks.end(),[&](const auto& row) {
            const auto& prior=row.second;
            return prior.rig==rig && prior.slot==request.slot && (with_fallback || prior.fallback);
        }),"Pickup pose fallback requires an exclusive arm slot for this Tick.");
        std::optional<RuntimeIkCommand> fallback;
        if(with_fallback)fallback=animations->pickup_fallback_control(rig,request.slot);
        game_pickup_checks.emplace(command.entity,StagedPickupClearance{rig,request.slot,joints,radii,start,std::move(fallback),blend,steer});
        game_capsule_query_attempts+=casts;
    }

    void require_unique_game_attachment(const std::string& item) const {
        require(std::none_of(game_attachment_commands.begin(),game_attachment_commands.end(),
            [&](const auto& prior){return prior.entity==item;}) &&
            std::none_of(game_release_commands.begin(),game_release_commands.end(),
                [&](const auto& prior){return prior.item==item;}),
            "Duplicate gameplay item attachment command in one Tick.");
    }
    void stage_game_release(const PoimaGameItemAttachmentCommandV1* source) {
        require(source->bytes==sizeof(PoimaGameItemReleaseCommandV5) && game && game->requires_item_attachment_release(),
            "Invalid item release command size or missing release feature.");
        PoimaGameItemReleaseCommandV5 release;
        std::memcpy(&release,source,sizeof(release));
        require(release.reserved==0 && release.reserved_tail==0,"Item release reserved fields must be zero.");
        RuntimeAttachmentRelease command{};
        command.item=gameplay_id(release.item);
        if(!pending_entity(command.item))(void)find(command.item);
        command.body_template=gameplay_id(release.body_template);
        std::copy_n(release.body_position,3,command.body_in_item.position.begin());
        std::copy_n(release.body_rotation,4,command.body_in_item.rotation.begin());
        std::copy_n(release.linear_velocity_m_s,3,command.linear_velocity_m_s.begin());
        std::copy_n(release.angular_velocity_rad_s,3,command.angular_velocity_rad_s.begin());
        require_unique_game_attachment(command.item);
        game_release_commands.push_back(std::move(command));
    }

    std::vector<RuntimeSurfaceProbe> game_surface_probes(const PoimaGameItemAttachmentCommandV3& command) const {
        require(command.reserved==0 && command.probes && command.probe_count>0 &&
            command.probe_count<=max_attachment_surface_probes,"A gameplay grip requires 1..8 surface probes.");
        std::size_t total_probes=command.probe_count,explicit_triangles=0;
        for(const auto& prior:game_attachment_commands)if(prior.contact) {
            total_probes+=prior.contact->surface.size();
            for(const auto& sample:prior.contact->surface)explicit_triangles+=sample.target_triangles.size();
        }
        require(total_probes<=max_surface_pairs,"Gameplay exceeded 32 grip surface probes this tick.");
        std::vector<RuntimeSurfaceProbe> result;
        result.reserve(command.probe_count);
        for(std::uint32_t i=0;i<command.probe_count;++i) {
            const auto& sample=command.probes[i];
            require(sample.reserved==0 && sample.source_kind<=1 &&
                sample.target_count<=max_surface_selected_triangles &&
                (sample.target_triangles!=nullptr)==(sample.target_count!=0),"Invalid gameplay grip surface probe.");
            require(sample.distance_limit_m>=0 && sample.distance_limit_m<=1e9,"Invalid gameplay grip surface distance limit.");
            explicit_triangles+=sample.target_count;
            require(explicit_triangles<=max_surface_triangle_checks,"Gameplay exceeded 32768 explicitly selected grip triangles this tick.");
            RuntimeSurfaceProbe probe;
            probe.source=gameplay_id(sample.source);
            probe.target=gameplay_id(sample.target);
            probe.source_index=sample.source_index;
            probe.distance_limit_m=sample.distance_limit_m;
            if(sample.source_kind==1)
                probe.source_weights=std::array{sample.barycentric[0],sample.barycentric[1],sample.barycentric[2]};
            else require(std::all_of(sample.barycentric,sample.barycentric+3,[](double value){return value==0;}),
                "Vertex grip probes require zero barycentric fields.");
            if(sample.target_count)
                probe.target_triangles.assign(sample.target_triangles,sample.target_triangles+sample.target_count);
            result.push_back(std::move(probe));
        }
        return result;
    }

    void aim_game_released_item(const PoimaEntityId* entity,PoimaGameItemAttachmentStateV1* output) const {
        require(game && game->requires_item_attachment_aim() && !entity &&
            output->bytes==sizeof(PoimaGameReleasedItemAimV4),"Gameplay must declare item_attachment_aim_v1 and pass a 208-byte ray query.");
        PoimaGameReleasedItemAimV4 payload;
        std::memcpy(&payload,output,sizeof(payload));
        const auto& ray=payload.ray;
        require(payload.triangle_present==0 && payload.triangle==0 && ray.reserved==0 &&
            ray.ignore_count<=128 && (!ray.ignore_count || ray.ignore),"Invalid item aim ray header or ignore list.");
        RuntimeRay query;
        std::copy_n(ray.origin,3,query.origin.begin());std::copy_n(ray.direction,3,query.direction.begin());
        query.distance=ray.distance;
        for(std::uint32_t i=0;i<ray.ignore_count;++i)query.ignore.push_back(gameplay_id(ray.ignore[i]));
        const auto aimed=owner->aim_released_item(query);
        PoimaGameReleasedItemAimV4 response{};
        response.version=4;response.bytes=sizeof(response);response.sampled_tick=tick;response.structure_revision=structure_revision;
        if(aimed.item)response.item=gameplay_id(*aimed.item);
        if(aimed.hit) {
            const auto& hit=*aimed.hit;
            response.hit.hit=1;response.hit.entity=gameplay_id(hit.entity);
            response.hit.fraction=hit.fraction;response.hit.distance=hit.distance;
            std::copy(hit.position.begin(),hit.position.end(),response.hit.position);
            if(hit.normal) {
                response.hit.normal_valid=1;
                std::copy(hit.normal->begin(),hit.normal->end(),response.hit.normal);
            }
            if(hit.triangle) {response.triangle_present=1;response.triangle=*hit.triangle;}
        }
        std::memcpy(output,&response,sizeof(response));
    }
    static int32_t POIMA_CALL get_game_attachment(void* context,const PoimaEntityId* entity,
        PoimaGameItemAttachmentStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Item attachment context is absent.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_attachment_reads<1024,"Gameplay exceeded 1024 item attachment reads.");
            ++self.game_attachment_reads;
            if(output && output->version==4) {
                self.aim_game_released_item(entity,output);
                return;
            }
            if(entity && output && output->version==2) {
                self.get_game_attachment_outcome(*entity,output);
                return;
            }
            if(entity && output && output->version==3) {
                self.get_game_release_outcome(*entity,output);
                return;
            }
            if(entity && output && output->version==5) {
                self.get_game_pickup_blocker(*entity,output);
                return;
            }
            require(entity && output && output->version==1 && output->bytes==sizeof(*output) &&
                output->present==0 && output->reserved==0,"Invalid item attachment query header.");
            const auto id=gameplay_id(*entity);
            (void)self.find(id);
            PoimaGameItemAttachmentStateV1 state{};
            state.version=1;state.bytes=sizeof(state);state.entity=*entity;
            state.sampled_tick=self.tick;state.structure_revision=self.structure_revision;
            if(const auto* binding=attachment(self.attachment_graph.get(),id)) {
                state.present=1;
                if(!binding->anchor.empty())state.anchor=gameplay_id(binding->anchor);
            }
            *output=state;
        });
    }
    static int32_t POIMA_CALL set_game_attachment(void* context,const PoimaGameItemAttachmentCommandV1* source,
        PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Item attachment context is absent.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_attachment_attempts<max_runtime_attachments,"Gameplay exceeded 64 item attachment attempts.");
            ++self.game_attachment_attempts;
            if(source && source->version==5) {
                self.stage_game_release(source);
                return;
            }
            require(source && ((source->version==1 && source->bytes==sizeof(*source)) ||
                (source->version==2 && source->bytes==sizeof(PoimaGameItemAttachmentCommandV2)) ||
                ((source->version==3 || source->version==4 || source->version==6) && source->bytes==sizeof(PoimaGameItemAttachmentCommandV3)) ||
                ((source->version>=7 && source->version<=10) && source->bytes==sizeof(PoimaGameItemPickupClearanceV7))) &&
                source->reserved==0 && source->mode<=2,
                "Invalid item attachment command header.");
            RuntimeAttachmentCommand command;
            command.entity=gameplay_id(source->entity);
            if(!self.pending_entity(command.entity))(void)self.find(command.entity);
            if(source->anchor.high || source->anchor.low) {
                command.anchor=gameplay_id(source->anchor);
                if(!self.pending_entity(command.anchor))(void)self.find(command.anchor);
            }
            if(source->mode==1) {
                require(!command.anchor.empty(),"Snap requires an item attachment anchor.");
                command.mode=RuntimeAttachmentMode::Snap;
                RuntimeTransform offset;
                std::copy_n(source->position,3,offset.position.begin());
                std::copy_n(source->rotation,4,offset.rotation.begin());
                std::copy_n(source->scale,3,offset.scale.begin());
                command.offset=offset;
            } else {
                const auto zero=[](double value){return value==0;};
                require(std::all_of(source->position,source->position+3,zero) &&
                    std::all_of(source->rotation,source->rotation+4,zero) && std::all_of(source->scale,source->scale+3,zero),
                    "Keep-world and reset require zero item attachment offset fields.");
                command.mode=source->mode==0 ? RuntimeAttachmentMode::KeepWorld : RuntimeAttachmentMode::Reset;
                require(command.mode!=RuntimeAttachmentMode::Reset || command.anchor.empty(),"Reset requires a zero item attachment anchor.");
            }
            if(source->version>=2) {
                require(self.game && self.game->requires_item_attachment_contact(),
                    "Gameplay must declare item_attachment_contact_v1 to stage a guarded transfer.");
                require(command.mode==RuntimeAttachmentMode::KeepWorld && !command.anchor.empty(),
                    "Attachment contact requires a live keep-world destination anchor.");
                PoimaGameItemAttachmentCommandV2 guarded;
                std::memcpy(&guarded,source,sizeof(guarded));
                RuntimeAttachmentContact contact{};
                std::copy_n(guarded.item_position,3,contact.item.position.begin());
                std::copy_n(guarded.item_rotation,4,contact.item.rotation.begin());
                std::copy_n(guarded.anchor_position,3,contact.anchor.position.begin());
                std::copy_n(guarded.anchor_rotation,4,contact.anchor.rotation.begin());
                contact.position_limit_m=guarded.position_limit_m;contact.rotation_limit_deg=guarded.rotation_limit_deg;
                if(source->version>=3) {
                    require(self.game->requires_item_attachment_surface(),
                        "Gameplay must declare item_attachment_surface_v1 to stage surface probes.");
                    PoimaGameItemAttachmentCommandV3 sampled;
                    std::memcpy(&sampled,source,sizeof(sampled));
                    contact.surface=self.game_surface_probes(sampled);
                }
                if(source->version>=6) {
                    require(self.game->requires_item_attachment_pickup(),
                        "Gameplay must declare item_attachment_pickup_v1 to stage a pickup.");
                    command.mode=RuntimeAttachmentMode::Pickup;
                }
                command.recover_miss=source->version==4 || source->version>=6;
                require(!command.recover_miss || self.game->requires_item_attachment_recovery(),
                    "Gameplay must declare item_attachment_recovery_v1 to stage a recoverable transfer.");
                command.contact=std::move(contact);
            }
            self.require_unique_game_attachment(command.entity);
            if(source->version>=7)self.stage_game_pickup_clearance(command,source);
            self.game_attachment_commands.push_back(std::move(command));
        });
    }
