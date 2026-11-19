// SPDX-License-Identifier: Apache-2.0
// Included in Runtime::Impl.
    std::vector<RuntimeAttachmentCommand> game_attachment_commands;
    std::size_t game_attachment_reads=0,game_attachment_attempts=0;

    static int32_t POIMA_CALL get_game_attachment(void* context,const PoimaEntityId* entity,
        PoimaGameItemAttachmentStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Item attachment context is absent.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_attachment_reads<1024,"Gameplay exceeded 1024 item attachment reads.");
            ++self.game_attachment_reads;
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
                (source->version==2 && source->bytes==sizeof(PoimaGameItemAttachmentCommandV2))) && source->reserved==0 && source->mode<=2,
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
            if(source->version==2) {
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
                command.contact=contact;
            }
            require(std::none_of(self.game_attachment_commands.begin(),self.game_attachment_commands.end(),
                [&](const auto& prior){return prior.entity==command.entity;}),"Duplicate gameplay item attachment command in one Tick.");
            self.game_attachment_commands.push_back(std::move(command));
        });
    }
