// SPDX-License-Identifier: Apache-2.0
// Included inside Runtime::Impl. Reads expose the committed requested control;
// replacements publish after Tick and do not grant manual ownership of a foot.
    static int32_t POIMA_CALL get_grounding_control(void* context,const PoimaEntityId* id,
        std::uint32_t slot,PoimaGameGroundingStateV1* output,PoimaGameError* error) {
        return callback(error,[&] {
            require(context&&id&&output,"Grounding control query is absent.");
            require(output->version==1&&output->bytes>=sizeof(PoimaGameGroundingStateV1)&&output->reserved==0,
                "Grounding control state requires version1, at least48 bytes and zero reserved.");
            require(slot>=1&&slot<=8,"Grounding slot must be1..8.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_grounding_read_calls<1024,"Gameplay exceeded1024 grounding reads in one Tick.");
            const auto state=self.owner->grounding(gameplay_id(*id),slot);
            PoimaGameGroundingStateV1 candidate{};candidate.version=1;candidate.bytes=sizeof(candidate);
            if(state) { candidate.present=1;candidate.entity=*id;candidate.slot=state->slot;
                candidate.enabled=state->enabled?1u:0u;candidate.weight=state->weight; }
            ++self.game_grounding_read_calls;*output=candidate;
        });
    }
    static int32_t POIMA_CALL set_grounding_control(void* context,const PoimaGameGroundingCommandV1* source,
        PoimaGameError* error) {
        return callback(error,[&] {
            require(context&&source,"Grounding command is absent.");
            require(source->version==1&&source->bytes>=sizeof(PoimaGameGroundingCommandV1)&&source->reserved==0,
                "Grounding command requires version1, at least48 bytes and zero reserved.");
            require(source->slot>=1&&source->slot<=8&&source->enabled<=1,"Invalid grounding slot or enabled flag.");
            require(std::isfinite(source->weight)&&source->weight>=0&&source->weight<=1,"Grounding weight must be0..1.");
            auto& self=*static_cast<Impl*>(context);self.require_tick();
            require(self.game_grounding_write_calls<64,"Gameplay exceeded64 grounding writes in one Tick.");
            RuntimeGroundingCommand command{gameplay_id(source->entity),source->slot,source->enabled!=0,source->weight};
            if(const auto* pending=self.pending_entity(command.entity)) {
                require(pending->animation_rig.has_value(),"Reserved grounding target needs an AnimationRig.");
                const auto& chains=pending->animation_rig->ik;
                require(std::any_of(chains.begin(),chains.end(),[&](const auto& chain) {
                    return chain.slot==command.slot&&chain.grounding.has_value();
                }),"Reserved grounding slot is not configured for automatic ownership.");
            } else require(self.animations->grounding_control(command.entity,command.slot).has_value(),
                "Grounding target slot is not configured for automatic ownership.");
            require(std::none_of(self.game_grounding_commands.begin(),self.game_grounding_commands.end(),[&](const auto& old) {
                return old.entity==command.entity&&old.slot==command.slot;
            }),"Duplicate gameplay grounding slot in one Tick.");
            self.game_grounding_commands.push_back(std::move(command));++self.game_grounding_write_calls;
        });
    }
