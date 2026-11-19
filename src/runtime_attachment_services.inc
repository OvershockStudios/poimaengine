// SPDX-License-Identifier: Apache-2.0
// Included in Runtime::Impl.
    std::vector<RuntimeAttachmentCommand> game_attachment_commands;
    std::size_t game_attachment_reads=0,game_attachment_attempts=0;

    void finish_game_attachments() {
        attachment_outcomes.reset();
        if(game_attachment_commands.empty())return;
        try {
            if(std::none_of(game_attachment_commands.begin(),game_attachment_commands.end(),
                [](const auto& command){return command.recover_miss;})) {
                change_attachments(structure_revision,game_attachment_commands);
            } else {
                std::size_t checks=0;
                std::vector<RuntimeAttachmentCommand> strict;
                for(auto& command:game_attachment_commands)
                    if(!command.recover_miss)strict.push_back(std::move(command));
                if(!strict.empty())change_attachments(structure_revision,strict,&checks);
                auto outcomes=std::make_shared<AttachmentOutcomes>();
                for(const auto& command:game_attachment_commands)if(command.recover_miss) {
                    RuntimeAttachmentOutcome outcome{command.anchor,{}};
                    try {change_attachments(structure_revision,{&command,1},&checks,true);}
                    catch(const RuntimeAttachmentContactRejected& error) {
                        const auto& row=error.contact;
                        outcome.detail=RuntimeAttachmentFrameMiss{row.position_error_m,row.position_limit_m,
                            row.rotation_limit_deg,row.rotation_error_deg};
                    } catch(const RuntimeAttachmentSurfaceRejected& error) {
                        const auto& row=error.contact;
                        outcome.detail=RuntimeAttachmentSurfaceMiss{row.probe_index,row.measurement.target_triangle,
                            row.measurement.distance_m,row.distance_limit_m};
                    }
                    outcomes->emplace(command.entity,std::move(outcome));
                }
                attachment_outcomes=std::move(outcomes);
            }
        } catch(const std::invalid_argument& error) {throw std::runtime_error(error.what());}
        game_attachment_commands.clear();
    }

    void get_game_attachment_outcome(const PoimaEntityId& entity,PoimaGameItemAttachmentStateV1* output) const {
        require(game && game->requires_item_attachment_recovery() &&
            output->bytes==sizeof(PoimaGameItemAttachmentOutcomeV2) && output->present==0 && output->reserved==0,
            "Invalid gameplay attachment outcome query or missing recovery feature.");
        PoimaGameItemAttachmentOutcomeV2 result{};
        result.version=2;result.bytes=sizeof(result);result.entity=entity;result.sampled_tick=tick;
        if(attachment_outcomes) {
            const auto found=attachment_outcomes->find(gameplay_id(entity));
            if(found!=attachment_outcomes->end()) {
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

    static int32_t POIMA_CALL get_game_attachment(void* context,const PoimaEntityId* entity,
        PoimaGameItemAttachmentStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Item attachment context is absent.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_attachment_reads<1024,"Gameplay exceeded 1024 item attachment reads.");
            ++self.game_attachment_reads;
            if(entity && output && output->version==2) {
                self.get_game_attachment_outcome(*entity,output);
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
            require(source && ((source->version==1 && source->bytes==sizeof(*source)) ||
                (source->version==2 && source->bytes==sizeof(PoimaGameItemAttachmentCommandV2)) ||
                ((source->version==3 || source->version==4) && source->bytes==sizeof(PoimaGameItemAttachmentCommandV3))) &&
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
                command.recover_miss=source->version==4;
                require(!command.recover_miss || self.game->requires_item_attachment_recovery(),
                    "Gameplay must declare item_attachment_recovery_v1 to stage a recoverable transfer.");
                command.contact=std::move(contact);
            }
            require(std::none_of(self.game_attachment_commands.begin(),self.game_attachment_commands.end(),
                [&](const auto& prior){return prior.entity==command.entity;}),"Duplicate gameplay item attachment command in one Tick.");
            self.game_attachment_commands.push_back(std::move(command));
        });
    }
