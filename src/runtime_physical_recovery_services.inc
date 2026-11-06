// SPDX-License-Identifier: Apache-2.0
// Included in Runtime::Impl. Reads are committed observations; requests join
// the ordinary physical command transaction and do not guarantee handback.
    std::size_t game_physical_recovery_reads=0;
    static int32_t POIMA_CALL get_physical_recovery(void* context,const PoimaEntityId* entity,
        PoimaGamePhysicalRecoveryStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Physical recovery context is absent.");auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_physical_recovery_reads<1024,"Gameplay exceeded 1024 physical recovery reads.");++self.game_physical_recovery_reads;
            require(entity&&output,"Physical recovery query is absent.");
            require(output->version==1&&output->bytes==sizeof(*output)&&output->present==0&&output->flags==0&&output->support_mask==0&&output->reserved==0,
                "Physical recovery output needs version 1, exact 144 bytes and zero flags/reserved.");
            const auto id=gameplay_id(*entity);
            const auto state=self.pending_entity(id)?std::nullopt:self.owner->physical_recovery(id);
            PoimaGamePhysicalRecoveryStateV1 candidate{};candidate.version=1;candidate.bytes=sizeof(candidate);candidate.entity=*entity;
            if(state) {
                candidate.present=1;candidate.status=static_cast<std::uint32_t>(state->status);candidate.sampled_tick=state->tick;
                candidate.generation=state->generation;candidate.assessment_revision=state->assessment_revision;
                candidate.flags=(state->physical?1u:0u)|(state->transition_active?2u:0u);
                candidate.stable_ticks=state->stable_ticks;candidate.required_stable_ticks=state->required_stable_ticks;
                candidate.last_attempt_result=static_cast<std::uint32_t>(state->last_attempt_result);
                if(state->last_attempt_tick){candidate.flags|=4u;candidate.last_attempt_tick=*state->last_attempt_tick;}
                if(state->returned_tick){candidate.flags|=8u;candidate.returned_tick=*state->returned_tick;}
                candidate.maximum_linear_speed=state->maximum_linear_speed;candidate.maximum_angular_speed=state->maximum_angular_speed;
                candidate.minimum_upright_dot=state->minimum_upright_dot;candidate.wrapper_distance=state->wrapper_distance;candidate.wrapper_angle=state->wrapper_angle;
                for(std::size_t i=0;i<2;++i)if(state->support_bodies[i]){candidate.support_mask|=1u<<i;candidate.support_bodies[i]=*state->support_bodies[i];}
            }
            *output=candidate;
        });
    }
    static int32_t POIMA_CALL request_physical_recovery(void* context,const PoimaGamePhysicalRecoveryCommandV1* source,
        PoimaGameError* error) {
        return callback(error,[&] {
            require(context,"Physical recovery context is absent.");auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_physical_write_attempts<64,"Gameplay exceeded 64 physical command attempts.");++self.game_physical_write_attempts;
            require(self.game_physical_commands.size()+self.game_physical_caller_writes<64,"Caller and gameplay exceed 64 physical writes.");
            require(source&&source->version==1&&source->bytes==sizeof(*source)&&source->reserved==0,"Invalid physical recovery command header.");
            const auto id=gameplay_id(source->entity);const auto* definition=self.pending_entity(id);
            if(!definition)definition=&self.topology->definitions.at(self.registry.get<Node>(self.find(id)).id);
            require(definition->physical_rig&&definition->physical_rig->recovery,"Physical recovery requires an authored recovery profile.");
            PhysicalRigCommand command;command.entity=id;command.recover=true;self.validate_physical_commands({command});
            require(std::none_of(self.game_physical_commands.begin(),self.game_physical_commands.end(),[&](const auto& old){return old.entity==id;}),
                "Recovery cannot share a rig with another same-tick physical action.");
            self.game_physical_commands.push_back(std::move(command));
        });
    }
